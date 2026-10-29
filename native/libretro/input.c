/*
 * hakuX native core — the RetroPad as the Duke, in the libretro core.
 *
 * Ports 1-4 are players 1-4, each with a controller plugged in unless the
 * port's Device Type in RetroArch is None: as RetroArch assumes, a RetroPad
 * on every port. RetroArch sets the ports after the game started to boot
 * (retro_set_controller_port_device after context_reset), and when its menu
 * changes one: the title sees the controller come or go, as when one is
 * plugged into the console.
 *
 * The buttons keep their places: the Xbox's A, B, X, Y are the RetroPad's
 * south, east, west and north (B, A, Y, X), White and Black its L and R.
 * The face buttons, White and Black are pressure-sensitive on the Duke:
 * fully pressed here. The triggers are analog where RetroArch reports them
 * so, else 0 or 255. Remapping is RetroArch's.
 */
#include "native/libretro/libretro_int.h"
#include "native/dev/usb.h"

#define NC_LR_PORTS 4

static unsigned g_ports = (1u << NC_LR_PORTS) - 1;  /* bit n: port n + 1 has a controller */

static int16_t clamp16(int v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

void nc_lr_input_describe(retro_environment_t env)
{
    static const struct retro_controller_description k_types[] = {
        {"Xbox Controller", RETRO_DEVICE_JOYPAD},
        {"None", RETRO_DEVICE_NONE},
    };
    static const struct retro_controller_info k_info[NC_LR_PORTS + 1] = {
        {k_types, 2}, {k_types, 2}, {k_types, 2}, {k_types, 2}, {NULL, 0},
    };
    static const struct retro_input_descriptor k_pad[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "White"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Black"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Left Trigger"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Right Trigger"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Left Thumb"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Right Thumb"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"},
    };
    enum { N = sizeof(k_pad) / sizeof(k_pad[0]) };
    static struct retro_input_descriptor d[NC_LR_PORTS * N + 1];   /* the last zeroed */
    for (unsigned port = 0; port < NC_LR_PORTS; port++) {
        for (unsigned i = 0; i < N; i++) {
            d[port * N + i] = k_pad[i];
            d[port * N + i].port = port;
        }
    }
    env(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)k_info);
    env(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, d);
}

void nc_lr_input_reset(void)
{
    g_ports = (1u << NC_LR_PORTS) - 1;
    for (unsigned port = 0; port < NC_LR_PORTS; port++) {
        nc_pad_plug(port, true);
    }
}

void nc_lr_input_set_device(unsigned port, unsigned device)
{
    if (port >= NC_LR_PORTS) {
        return;
    }
    const bool in = (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_NONE;
    g_ports = in ? g_ports | 1u << port : g_ports & ~(1u << port);
    nc_log("hakuX-libretro", "port %u: %s", port + 1, in ? "an Xbox controller" : "none");
    nc_pad_plug(port, in);
}

/* A trigger: RetroArch's analog value (0..0x7fff) where the pad has one,
 * else the button. */
static uint8_t trigger(retro_input_state_t state, unsigned port, unsigned id)
{
    const int v = state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);
    if (v > 0) {
        return (uint8_t)(v >> 7);
    }
    return state(port, RETRO_DEVICE_JOYPAD, 0, id) ? 0xFF : 0;
}

void nc_lr_input_run(retro_input_poll_t poll, retro_input_state_t state)
{
    static const unsigned k_analog[6] = {
        RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L,
    };  /* the Duke's A, B, X, Y, Black, White */
    static const struct {
        unsigned id;
        uint16_t bit;
    } k_digital[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, NC_PAD_DPAD_UP},     {RETRO_DEVICE_ID_JOYPAD_DOWN, NC_PAD_DPAD_DOWN},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, NC_PAD_DPAD_LEFT}, {RETRO_DEVICE_ID_JOYPAD_RIGHT, NC_PAD_DPAD_RIGHT},
        {RETRO_DEVICE_ID_JOYPAD_START, NC_PAD_START},    {RETRO_DEVICE_ID_JOYPAD_SELECT, NC_PAD_BACK},
        {RETRO_DEVICE_ID_JOYPAD_L3, NC_PAD_LTHUMB},      {RETRO_DEVICE_ID_JOYPAD_R3, NC_PAD_RTHUMB},
    };
    if (!poll || !state) {
        return;
    }
    poll();
    for (unsigned port = 0; port < NC_LR_PORTS; port++) {
        if (!(g_ports & 1u << port)) {
            continue;
        }
        NcPadState s = {0};
        for (unsigned i = 0; i < 6; i++) {
            s.analog[i] = state(port, RETRO_DEVICE_JOYPAD, 0, k_analog[i]) ? 0xFF : 0;
        }
        for (unsigned i = 0; i < sizeof(k_digital) / sizeof(k_digital[0]); i++) {
            if (state(port, RETRO_DEVICE_JOYPAD, 0, k_digital[i].id)) {
                s.buttons |= k_digital[i].bit;
            }
        }
        s.analog[6] = trigger(state, port, RETRO_DEVICE_ID_JOYPAD_L2);
        s.analog[7] = trigger(state, port, RETRO_DEVICE_ID_JOYPAD_R2);
        /* RetroArch's Y points down, the Duke's up */
        s.lx = clamp16(state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X));
        s.ly = clamp16(-state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y));
        s.rx = clamp16(state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
        s.ry = clamp16(-state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
        nc_pad_set(port, &s);
    }
}
