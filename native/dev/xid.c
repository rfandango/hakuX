/*
 * hakuX native core — the Xbox gamepad (Duke): an XID device (class 0x58)
 * with a 20-byte input report on interrupt IN 2 and the rumble report on
 * interrupt OUT 2.
 *
 * Input comes from nc_pad_set, and for headless runs from NC_PAD_SCRIPT:
 * "ms:input[+input...]:hold_ms,..." (inputs: a b x y black white lt rt up
 * down left right start back ls rs, or a stick's position lx= ly= rx= ry=
 * -32767..32767), times from the first report; "60f:a:2" counts frames
 * instead (frame 60, held for 2: the HLE GPU's swaps in this process, after
 * a savestate's load the frames since it), the same each run. "@path" reads
 * it from a file (debug.hakux.env holds 91 characters).
 */
#include "native/dev/usb.h"
#include "native/core/irq.h"
#include "hw/xbox/hle/vk/back.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XID_PLAYERS 4

typedef struct Xid {
    NcUsbDev dev;
    unsigned player;
    bool polled;    /* since the last bus reset (logged once per enumeration) */
} Xid;

static const uint8_t k_dev_desc[18] = {
    18, 1, 0x10, 0x01, /* USB 1.1 */
    0, 0, 0,           /* class per interface */
    0x40,
    0x5e, 0x04, 0x02, 0x02, /* Microsoft, controller (0x0202) */
    0x00, 0x01, 1, 2, 3, 1,
};

static const uint8_t k_cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x58, 0x42, 0, 0,  /* XID interface */
    7, 5, 0x82, 0x03, 0x20, 0, 4,     /* report: interrupt IN 2 */
    7, 5, 0x02, 0x03, 0x20, 0, 4,     /* rumble: interrupt OUT 2 */
};

static const uint8_t k_xid_desc[16] = {
    16, 0x42, 0x00, 0x01, /* XID 1.00 */
    1, 1,                 /* gamepad */
    20, 6,                /* input, output report sizes */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const char *const k_strings[] = {"hakuX", "Microsoft Xbox Controller", "1"};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static NcPadState g_pad[XID_PLAYERS];
static uint16_t g_rumble[XID_PLAYERS][2];

/* NC_PAD_SCRIPT. */
typedef struct ScriptStep {
    uint32_t at, hold;  /* ms, or frames */
    bool frames;
    uint16_t buttons;
    uint8_t analog;     /* bit n: analog[n] fully pressed */
    uint8_t sticks;     /* bit n: stick[n] (lx ly rx ry) set */
    int16_t stick[4];
} ScriptStep;
static ScriptStep g_script[64];
static unsigned g_nscript;
static uint64_t g_script_t0;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool script_button(const char *name, size_t n, ScriptStep *st)
{
    static const struct {
        const char *name;
        uint16_t buttons;
        uint8_t analog;
    } k_names[] = {
        {"a", 0, 1 << 0}, {"b", 0, 1 << 1}, {"x", 0, 1 << 2}, {"y", 0, 1 << 3},
        {"black", 0, 1 << 4}, {"white", 0, 1 << 5}, {"lt", 0, 1 << 6}, {"rt", 0, 1 << 7},
        {"up", NC_PAD_DPAD_UP, 0}, {"down", NC_PAD_DPAD_DOWN, 0}, {"left", NC_PAD_DPAD_LEFT, 0},
        {"right", NC_PAD_DPAD_RIGHT, 0}, {"start", NC_PAD_START, 0}, {"back", NC_PAD_BACK, 0},
        {"ls", NC_PAD_LTHUMB, 0}, {"rs", NC_PAD_RTHUMB, 0},
    };
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (strlen(k_names[i].name) == n && !strncmp(k_names[i].name, name, n)) {
            st->buttons |= k_names[i].buttons;
            st->analog |= k_names[i].analog;
            return true;
        }
    }
    static const char k_sticks[4][3] = {"lx", "ly", "rx", "ry"};
    for (unsigned i = 0; i < 4; i++) {
        if (n > 3 && !strncmp(name, k_sticks[i], 2) && name[2] == '=') {
            const long v = strtol(name + 3, NULL, 10);
            st->stick[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            st->sticks |= 1u << i;
            return true;
        }
    }
    return false;
}

static void script_load(void)
{
    const char *s = getenv("NC_PAD_SCRIPT");
    if (!s || g_nscript) {
        return;
    }
    static char file[2048];
    if (*s == '@') {
        FILE *f = fopen(s + 1, "r");
        const size_t n = f ? fread(file, 1, sizeof(file) - 1, f) : 0;
        if (f) {
            fclose(f);
        }
        file[n] = 0;
        s = file;
    }
    while (*s && g_nscript < sizeof(g_script) / sizeof(g_script[0])) {
        ScriptStep st = {0};
        char *end;
        st.at = (uint32_t)strtoul(s, &end, 10);
        st.frames = *end == 'f';
        end += st.frames;
        if (*end != ':') {
            break;
        }
        s = end + 1;
        while (*s && *s != ':' && *s != ',') {
            size_t n = strcspn(s, "+:,");
            if (!script_button(s, n, &st)) {
                nc_log("hakuX-native-usb", "pad script: unknown input '%.*s'", (int)n, s);
            }
            s += n;
            if (*s == '+') {
                s++;
            }
        }
        st.hold = st.frames ? 9 : 150;
        if (*s == ':') {
            st.hold = (uint32_t)strtoul(s + 1, &end, 10);
            s = end + (*end == 'f');
        }
        g_script[g_nscript++] = st;
        if (*s == ',') {
            s++;
        }
    }
    nc_log("hakuX-native-usb", "pad script: %u steps", g_nscript);
}

static void make_report(unsigned player, uint8_t r[20])
{
    NcPadState p;
    pthread_mutex_lock(&g_lock);
    p = g_pad[player];
    pthread_mutex_unlock(&g_lock);

    if (player == 0 && g_nscript) {
        if (!g_script_t0) {
            g_script_t0 = now_ms();
        }
        const uint64_t ms = now_ms() - g_script_t0, frame = hle_back_swap_count();
        for (unsigned i = 0; i < g_nscript; i++) {
            const ScriptStep *st = &g_script[i];
            const uint64_t t = st->frames ? frame : ms;
            if (t >= st->at && t < (uint64_t)st->at + st->hold) {
                p.buttons |= st->buttons;
                for (unsigned b = 0; b < 8; b++) {
                    if (st->analog & (1u << b)) {
                        p.analog[b] = 0xff;
                    }
                }
                int16_t *axis[4] = {&p.lx, &p.ly, &p.rx, &p.ry};
                for (unsigned a = 0; a < 4; a++) {
                    if (st->sticks & (1u << a)) {
                        *axis[a] = st->stick[a];
                    }
                }
            }
        }
    }

    r[0] = 0;  /* report id */
    r[1] = 20; /* length */
    nc_wr16(r + 2, p.buttons);
    memcpy(r + 4, p.analog, 8);
    nc_wr16(r + 12, (uint16_t)p.lx);
    nc_wr16(r + 14, (uint16_t)p.ly);
    nc_wr16(r + 16, (uint16_t)p.rx);
    nc_wr16(r + 18, (uint16_t)p.ry);
}

static void take_rumble(unsigned player, const uint8_t *r, int len)
{
    if (len < 6) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_rumble[player][0] = nc_rd16(r + 2);
    g_rumble[player][1] = nc_rd16(r + 4);
    pthread_mutex_unlock(&g_lock);
}

static int xid_control(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len)
{
    Xid *x = (Xid *)d;
    const uint8_t type = s[0], req = s[1];
    const uint16_t value = nc_rd16(s + 2);
    switch ((type << 8) | req) {
    case 0xA101: /* HID GET_REPORT */
        if (value != 0x0100) {
            return NC_USB_STALL;
        }
        {
            uint8_t r[20];
            make_report(x->player, r);
            memcpy(data, r, MIN(len, 20));
            return MIN(len, 20);
        }
    case 0x2109: /* HID SET_REPORT */
        if (value != 0x0200) {
            return NC_USB_STALL;
        }
        take_rumble(x->player, data, len);
        return 0;
    case 0xC106: /* XID GET_DESCRIPTOR */
        if (value != 0x4200) {
            return NC_USB_STALL;
        }
        memcpy(data, k_xid_desc, MIN(len, (int)sizeof(k_xid_desc)));
        return MIN(len, (int)sizeof(k_xid_desc));
    case 0xC101: /* XID GET_CAPABILITIES: every bit and axis is live */
        if (value == 0x0100) {
            memset(data, 0xff, MIN(len, 20));
            data[0] = 0;
            if (len > 1) {
                data[1] = 20;
            }
            return MIN(len, 20);
        }
        if (value == 0x0200) {
            memset(data, 0xff, MIN(len, 6));
            data[0] = 0;
            if (len > 1) {
                data[1] = 6;
            }
            return MIN(len, 6);
        }
        return NC_USB_STALL;
    default:
        nc_log("hakuX-native-usb", "xid: unhandled request %02x %02x value %04x", type, req, value);
        return NC_USB_STALL;
    }
}

static int xid_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Xid *x = (Xid *)d;
    if (ep != 2) {
        return NC_USB_STALL;
    }
    if (in) {
        if (!x->polled) {
            x->polled = true;
            nc_log("hakuX-native-usb", "gamepad %u: polled", x->player);
        }
        uint8_t r[20];
        make_report(x->player, r);
        {   /* button changes as the title reads them (a few a second at most) */
            static uint16_t s_last[XID_PLAYERS];
            static unsigned s_logged;
            const uint16_t b = (uint16_t)(r[2] | r[3] << 8);
            if (b != s_last[x->player] && s_logged < 200) {
                s_logged++;
                nc_log("hakuX-native-usb", "gamepad %u: buttons %04x -> %04x (A %02x)", x->player,
                       s_last[x->player], b, r[4]);
            }
            s_last[x->player] = b;
        }
        const int n = MIN(len, 20);
        memcpy(buf, r, n);
        return n;
    }
    take_rumble(x->player, buf, len);
    return len;
}

static void xid_reset(NcUsbDev *d)
{
    ((Xid *)d)->polled = false;
}

NcUsbDev *nc_xid_gamepad_new(unsigned player)
{
    Xid *x = calloc(1, sizeof(Xid));
    x->player = player % XID_PLAYERS;
    x->dev.name = "gamepad";
    x->dev.dev_desc = k_dev_desc;
    x->dev.dev_desc_len = sizeof(k_dev_desc);
    x->dev.cfg_desc = k_cfg_desc;
    x->dev.cfg_desc_len = sizeof(k_cfg_desc);
    x->dev.strings = k_strings;
    x->dev.nstrings = 3;
    x->dev.control = xid_control;
    x->dev.data = xid_data;
    x->dev.reset = xid_reset;
    x->dev.pending_addr = -1;
    script_load();
    return &x->dev;
}

void nc_pad_set(unsigned player, const NcPadState *s)
{
    if (player < XID_PLAYERS) {
        pthread_mutex_lock(&g_lock);
        g_pad[player] = *s;
        pthread_mutex_unlock(&g_lock);
    }
}

void nc_pad_rumble(unsigned player, uint16_t *left, uint16_t *right)
{
    if (player < XID_PLAYERS) {
        pthread_mutex_lock(&g_lock);
        *left = g_rumble[player][0];
        *right = g_rumble[player][1];
        pthread_mutex_unlock(&g_lock);
    }
    /* Paused (the menu open, the device asleep), the motors stop: the
     * title's last strengths went on shaking the controller meanwhile. Its
     * strengths come back with the emulation. */
    if (nc_paused()) {
        *left = *right = 0;
    }
}
