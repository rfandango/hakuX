/*
 * hakuX front end — controller mapping (input_map.c): which key and which
 * gamepad button or axis drive each of the Duke's inputs, per player, as
 * the settings files keep it ("map.p1.a=Space|b0"), the devices players get
 * ("pad.p2=auto|keyboard|none|pad2"), and the keyboard shortcuts.
 */
#ifndef NATIVE_APP_UI_INPUT_MAP_H
#define NATIVE_APP_UI_INPUT_MAP_H

#include "native/app/ui/config.h"
#include "native/app/ui/i18n.h"

#include <SDL3/SDL.h>
#include <stdint.h>

enum {
    IM_A, IM_B, IM_X, IM_Y, IM_BLACK, IM_WHITE, IM_LT, IM_RT, IM_START, IM_BACK, IM_LS, IM_RS,
    IM_DUP, IM_DDOWN, IM_DLEFT, IM_DRIGHT,
    IM_LUP, IM_LDOWN, IM_LLEFT, IM_LRIGHT, IM_RUP, IM_RDOWN, IM_RLEFT, IM_RRIGHT,
    IM_INPUTS
};
enum { IM_HK_MENU, IM_HK_SAVE, IM_HK_LOAD, IM_HK_TURBO, IM_HK_PAUSE, IM_HK_SHOT, IM_HK_FULL, IM_HOTKEYS };

/* The Duke's state, as the core's NcPadState (usb.h) holds it. */
typedef struct ImPad {
    uint16_t buttons;       /* d-pad 1 2 4 8, Start 0x10, Back 0x20, LS 0x40, RS 0x80 */
    uint8_t analog[8];      /* A B X Y black white LT RT */
    int16_t lx, ly, rx, ry;
} ImPad;

const char *im_input_label(int i);
enum UiStr im_hotkey_label(int h);

/* dashboard: names and edits */
void im_binding_name(const Config *c, int player, int input, char *out, size_t n);
void im_bind_key(Config *c, int player, int input, SDL_Keycode k);
void im_bind_button(Config *c, int player, int input, int button);
void im_bind_axis(Config *c, int player, int input, int signed_axis);   /* (axis + 1) * sign */
void im_unbind(Config *c, int player, int input);
void im_clear_map(Config *c, int player);
void im_copy_map(Config *to, const Config *from, int player);
void im_device_name(const Config *c, int player, char *out, size_t n);
void im_cycle_device(Config *c, int player, int delta);
void im_hotkey_name(const Config *c, int h, char *out, size_t n);
void im_set_hotkey(Config *c, int h, SDL_Keycode k);

/* the game: the mapping resolved (the game's file over the global one) */
typedef struct ImMap {
    SDL_Keycode key[4][IM_INPUTS];
    int pad[4][IM_INPUTS];      /* 0 none; 1 + button; 1000 + axis * 2 + (negative) */
    int device[4];              /* -2 none, -1 keyboard only, 0 auto, k: the k-th gamepad */
    SDL_Keycode hotkey[IM_HOTKEYS];
    float deadzone;
    float stick_range;          /* stick.range: the output's scale, 1 = the stick's own */
    bool stick_round;           /* stick.shape=round: the stick's circle kept, not the Duke's square */
    bool rumble;
} ImMap;
void im_resolve(ImMap *m, const Config *global, const Config *game);
/* A stick's position (x, y: -1..1, up positive) as the title gets it,
 * -1..1 on each axis: the dead zone, the shape and the range applied. */
void im_stick(float x, float y, float deadzone, float range, bool round, float *ox, float *oy);
/* Player p's state from its keyboard keys (keys: SDL_GetKeyboardState) and gamepad. */
void im_read(const ImMap *m, int p, const bool *keys, SDL_Gamepad *pad, ImPad *out);

#endif
