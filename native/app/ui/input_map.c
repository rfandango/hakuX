/*
 * hakuX front end — controller mapping (input_map.h).
 */
#include "native/app/ui/input_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_name[IM_INPUTS] = {"a", "b", "x", "y", "black", "white", "lt", "rt", "start", "back",
                                              "ls", "rs", "up", "down", "left", "right", "lup", "ldown", "lleft",
                                              "lright", "rup", "rdown", "rleft", "rright"};
static const char *const k_label[IM_INPUTS] = {
    "A", "B", "X", "Y", "Black", "White", "Left trigger", "Right trigger", "Start", "Back", "Left stick (click)",
    "Right stick (click)", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick up", "Left stick down",
    "Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left",
    "Right stick right"};
/* the keyboard's defaults (player 1) */
static const SDL_Keycode k_kb_def[IM_INPUTS] = {
    SDLK_SPACE, SDLK_LSHIFT, SDLK_Q, SDLK_E, SDLK_C, SDLK_Z, SDLK_1, SDLK_3, SDLK_RETURN, SDLK_BACKSPACE, SDLK_F, SDLK_H,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_W, SDLK_S, SDLK_A, SDLK_D, SDLK_I, SDLK_K, SDLK_J, SDLK_L};
/* the gamepad's: 1 + button, 1000 + axis * 2 + negative */
#define B(x) (1 + (x))
#define AX(a, neg) (1000 + (a) * 2 + (neg))
static const int k_pad_def[IM_INPUTS] = {
    B(SDL_GAMEPAD_BUTTON_SOUTH), B(SDL_GAMEPAD_BUTTON_EAST), B(SDL_GAMEPAD_BUTTON_WEST), B(SDL_GAMEPAD_BUTTON_NORTH),
    B(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), B(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), AX(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0),
    AX(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0), B(SDL_GAMEPAD_BUTTON_START), B(SDL_GAMEPAD_BUTTON_BACK),
    B(SDL_GAMEPAD_BUTTON_LEFT_STICK), B(SDL_GAMEPAD_BUTTON_RIGHT_STICK), B(SDL_GAMEPAD_BUTTON_DPAD_UP),
    B(SDL_GAMEPAD_BUTTON_DPAD_DOWN), B(SDL_GAMEPAD_BUTTON_DPAD_LEFT), B(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
    AX(SDL_GAMEPAD_AXIS_LEFTY, 1), AX(SDL_GAMEPAD_AXIS_LEFTY, 0), AX(SDL_GAMEPAD_AXIS_LEFTX, 1),
    AX(SDL_GAMEPAD_AXIS_LEFTX, 0), AX(SDL_GAMEPAD_AXIS_RIGHTY, 1), AX(SDL_GAMEPAD_AXIS_RIGHTY, 0),
    AX(SDL_GAMEPAD_AXIS_RIGHTX, 1), AX(SDL_GAMEPAD_AXIS_RIGHTX, 0)};
static const char *const k_hk_name[IM_HOTKEYS] = {"menu", "save", "load", "turbo", "pause", "shot", "full"};
static const SDL_Keycode k_hk_def[IM_HOTKEYS] = {SDLK_F1, SDLK_F5, SDLK_F8, SDLK_TAB, SDLK_F2, SDLK_F12, SDLK_F11};
static const enum UiStr k_hk_label[IM_HOTKEYS] = {S_HK_MENU, S_HK_SAVE, S_HK_LOAD, S_HK_TURBO,
                                                  S_HK_PAUSE, S_HK_SHOT, S_HK_FULL};

const char *im_input_label(int i)
{
    /* the face buttons and Start/Back as the controller prints them; the rest in the interface's language */
    static char buf[96];
    static const enum UiStr k_dir[4] = {S_DIR_UP, S_DIR_DOWN, S_DIR_LEFT, S_DIR_RIGHT};
    switch (i) {
    case IM_BLACK: return T(S_IN_BLACK);
    case IM_WHITE: return T(S_IN_WHITE);
    case IM_LT: return T(S_IN_LT);
    case IM_RT: return T(S_IN_RT);
    case IM_LS: return T(S_IN_LS);
    case IM_RS: return T(S_IN_RS);
    default: break;
    }
    if (i >= IM_DUP && i <= IM_DRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_DPAD), T(k_dir[i - IM_DUP]));
        return buf;
    }
    if (i >= IM_LUP && i <= IM_LRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_LSTICK), T(k_dir[i - IM_LUP]));
        return buf;
    }
    if (i >= IM_RUP && i <= IM_RRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_RSTICK), T(k_dir[i - IM_RUP]));
        return buf;
    }
    return k_label[i];
}

enum UiStr im_hotkey_label(int h)
{
    return k_hk_label[h];
}

static void map_key(int player, int input, char *out, size_t n)
{
    snprintf(out, n, "map.p%d.%s", player + 1, k_name[input]);
}

/* "<key name>|<pad element>" -> its parts, the defaults where absent */
static void parse(const Config *c, int player, int input, SDL_Keycode *key, int *pad)
{
    char k[48];
    map_key(player, input, k, sizeof(k));
    const char *v = cfg_get(c, k);
    *key = player == 0 ? k_kb_def[input] : SDLK_UNKNOWN;
    *pad = k_pad_def[input];
    if (!v) {
        return;
    }
    char kb[64];
    const char *bar = strchr(v, '|');
    snprintf(kb, sizeof(kb), "%.*s", bar ? (int)(bar - v) : (int)strlen(v), v);
    *key = kb[0] ? SDL_GetKeyFromName(kb) : SDLK_UNKNOWN;
    const char *p = bar ? bar + 1 : "";
    if (p[0] == 'b') {
        *pad = 1 + atoi(p + 1);
    } else if (p[0] == 'a') {
        const int a = atoi(p + 1);
        *pad = 1000 + a * 2 + (strchr(p, '-') != NULL);
    } else {
        *pad = 0;
    }
}

static void store(Config *c, int player, int input, SDL_Keycode key, int pad)
{
    char k[48], v[96], pe[16] = "";
    map_key(player, input, k, sizeof(k));
    if (pad >= 1000) {
        snprintf(pe, sizeof(pe), "a%d%c", (pad - 1000) / 2, (pad & 1) ? '-' : '+');
    } else if (pad > 0) {
        snprintf(pe, sizeof(pe), "b%d", pad - 1);
    }
    snprintf(v, sizeof(v), "%s|%s", key ? SDL_GetKeyName(key) : "", pe);
    cfg_set(c, k, v);
}

/* a pad's input as its face says it: A, LB, RT, Left stick X+, D-pad up */
static void pad_name(int pad, char *out, size_t n)
{
    static const char *const buttons[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "LS", "RS", "LB", "RB",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    static const char *const axes[] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "LT", "RT"};
    if (!pad) {
        snprintf(out, n, "-");
    } else if (pad >= 1000) {
        const int a = (pad - 1000) / 2;
        const char *nm = a < 6 ? axes[a] : SDL_GetGamepadStringForAxis((SDL_GamepadAxis)a);
        snprintf(out, n, "%s%s", nm ? nm : "axis", a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "" : (pad & 1) ? "-" : "+");
    } else {
        const int b = pad - 1;
        const char *nm = b < 15 ? buttons[b] : SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
        snprintf(out, n, "%s", nm ? nm : "?");
    }
}

void im_binding_name(const Config *c, int player, int input, char *out, size_t n)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    char pn[48];
    pad_name(p, pn, sizeof(pn));
    snprintf(out, n, "%s  /  %s", k ? SDL_GetKeyName(k) : "-", pn);
}

void im_bind_key(Config *c, int player, int input, SDL_Keycode key)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    store(c, player, input, key, p);
}

void im_bind_button(Config *c, int player, int input, int button)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    store(c, player, input, k, 1 + button);
}

void im_bind_axis(Config *c, int player, int input, int sa)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    const int a = (sa < 0 ? -sa : sa) - 1;
    store(c, player, input, k, 1000 + a * 2 + (sa < 0));
}

void im_unbind(Config *c, int player, int input)
{
    store(c, player, input, SDLK_UNKNOWN, 0);
}

void im_clear_map(Config *c, int player)
{
    for (int i = 0; i < IM_INPUTS; i++) {
        char k[48];
        map_key(player, i, k, sizeof(k));
        cfg_unset(c, k);
    }
}

void im_copy_map(Config *to, const Config *from, int player)
{
    for (int i = 0; i < IM_INPUTS; i++) {
        SDL_Keycode k;
        int p;
        parse(from, player, i, &k, &p);
        store(to, player, i, k, p);
    }
}

static int device_of(const Config *c, int player)
{
    char k[16];
    snprintf(k, sizeof(k), "pad.p%d", player + 1);
    const char *v = cfg_get(c, k);
    if (!v || !strcmp(v, "auto")) {
        return 0;
    }
    if (!strcmp(v, "none")) {
        return -2;
    }
    if (!strcmp(v, "keyboard")) {
        return -1;
    }
    return !strncmp(v, "pad", 3) ? atoi(v + 3) : 0;
}

void im_device_name(const Config *c, int player, char *out, size_t n)
{
    const int d = device_of(c, player);
    if (d == -2) {
        snprintf(out, n, "%s", T(S_NONE));
    } else if (d == -1) {
        snprintf(out, n, "%s", T(S_KEYBOARD));
    } else if (d == 0) {
        snprintf(out, n, "%s", T(S_AUTO));
    } else {
        int cnt = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&cnt);
        const char *nm = ids && d <= cnt ? SDL_GetGamepadNameForID(ids[d - 1]) : NULL;
        snprintf(out, n, "%d: %s", d, nm ? nm : "-");
        SDL_free(ids);
    }
}

void im_cycle_device(Config *c, int player, int delta)
{
    int cnt = 0;
    SDL_free(SDL_GetGamepads(&cnt));
    /* auto, keyboard, pad1..padN, none */
    const int slots = cnt + 3;
    const int d = device_of(c, player);
    int pos = d == 0 ? 0 : d == -1 ? 1 : d == -2 ? slots - 1 : 1 + d;
    pos = (pos + delta + slots) % slots;
    char k[16], v[16];
    snprintf(k, sizeof(k), "pad.p%d", player + 1);
    if (pos == 0) {
        snprintf(v, sizeof(v), "auto");
    } else if (pos == 1) {
        snprintf(v, sizeof(v), "keyboard");
    } else if (pos == slots - 1) {
        snprintf(v, sizeof(v), "none");
    } else {
        snprintf(v, sizeof(v), "pad%d", pos - 1);
    }
    cfg_set(c, k, v);
}

void im_hotkey_name(const Config *c, int h, char *out, size_t n)
{
    char k[24];
    snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
    const char *v = cfg_get(c, k);
    snprintf(out, n, "%s", v && *v ? v : SDL_GetKeyName(k_hk_def[h]));
}

void im_set_hotkey(Config *c, int h, SDL_Keycode key)
{
    char k[24];
    snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
    cfg_set(c, k, SDL_GetKeyName(key));
}

void im_resolve(ImMap *m, const Config *global, const Config *game)
{
    memset(m, 0, sizeof(*m));
    const bool custom = game && cfg_bool(game, "map.custom", false);
    for (int p = 0; p < 4; p++) {
        m->device[p] = device_of(global, p);
        for (int i = 0; i < IM_INPUTS; i++) {
            parse(custom ? game : global, p, i, &m->key[p][i], &m->pad[p][i]);
        }
    }
    for (int h = 0; h < IM_HOTKEYS; h++) {
        char k[24];
        snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
        const char *v = cfg_get(global, k);
        m->hotkey[h] = v && *v ? SDL_GetKeyFromName(v) : k_hk_def[h];
    }
    const Config *dz = game && cfg_get(game, "deadzone") ? game : global;
    m->deadzone = cfg_int(dz, "deadzone", 10) / 100.0f;
    m->rumble = cfg_bool(global, "rumble", true);
}

static float pad_value(SDL_Gamepad *g, int pad)
{
    if (!g || !pad) {
        return 0;
    }
    if (pad < 1000) {
        return SDL_GetGamepadButton(g, (SDL_GamepadButton)(pad - 1)) ? 1.0f : 0.0f;
    }
    const float v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)((pad - 1000) / 2)) / 32767.0f;
    return (pad & 1) ? (v < 0 ? -v : 0) : (v > 0 ? v : 0);
}

static int16_t stick(float pos, float neg, float dz)
{
    float v = pos - neg;
    const float a = fabsf(v);
    if (a < dz) {
        return 0;
    }
    v = (v < 0 ? -1 : 1) * (a - dz) / (1 - dz);
    v = v > 1 ? 1 : v < -1 ? -1 : v;
    return (int16_t)(v * 32767);
}

void im_read(const ImMap *m, int p, const bool *keys, SDL_Gamepad *pad, ImPad *out)
{
    float v[IM_INPUTS];
    for (int i = 0; i < IM_INPUTS; i++) {
        const SDL_Keycode k = m->key[p][i];
        float kv = 0;
        if (keys && k) {
            const SDL_Scancode sc = SDL_GetScancodeFromKey(k, NULL);
            kv = keys[sc] ? 1.0f : 0.0f;
        }
        const float pv = pad_value(pad, m->pad[p][i]);
        v[i] = kv > pv ? kv : pv;
    }
    memset(out, 0, sizeof(*out));
    static const int k_bits[] = {IM_DUP, IM_DDOWN, IM_DLEFT, IM_DRIGHT, IM_START, IM_BACK, IM_LS, IM_RS};
    for (int b = 0; b < 8; b++) {
        if (v[k_bits[b]] > 0.5f) {
            out->buttons |= (uint16_t)(1u << b);
        }
    }
    for (int i = 0; i < 8; i++) {
        const float a = v[IM_A + i];
        out->analog[i] = (uint8_t)(a >= 1 ? 255 : a <= 0.1f ? 0 : a * 255);
    }
    out->lx = stick(v[IM_LRIGHT], v[IM_LLEFT], m->deadzone);
    out->ly = stick(v[IM_LUP], v[IM_LDOWN], m->deadzone);
    out->rx = stick(v[IM_RRIGHT], v[IM_RLEFT], m->deadzone);
    out->ry = stick(v[IM_RUP], v[IM_RDOWN], m->deadzone);
}
