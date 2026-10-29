/*
 * hakuX native core, app front end — a modern gamepad read as the Duke
 * (app_main.c's).
 */
#include "native/app/app.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

static int16_t clamp16(int v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* The Duke from a modern gamepad: face buttons and black/white (the shoulder
 * buttons) are analog on the Xbox, fully pressed here; stick Y points up. */
void nc_read_gamepad(SDL_Gamepad *p, NcPadState *s)
{
    static const SDL_GamepadButton k_analog[6] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    };
    static const struct {
        SDL_GamepadButton b;
        uint16_t bit;
    } k_digital[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, NC_PAD_DPAD_UP},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, NC_PAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, NC_PAD_DPAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, NC_PAD_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, NC_PAD_START},         {SDL_GAMEPAD_BUTTON_BACK, NC_PAD_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, NC_PAD_LTHUMB},   {SDL_GAMEPAD_BUTTON_RIGHT_STICK, NC_PAD_RTHUMB},
    };
    for (unsigned i = 0; i < 6; i++) {
        if (SDL_GetGamepadButton(p, k_analog[i])) {
            s->analog[i] = 0xFF;
        }
    }
    for (unsigned i = 0; i < sizeof(k_digital) / sizeof(k_digital[0]); i++) {
        if (SDL_GetGamepadButton(p, k_digital[i].b)) {
            s->buttons |= k_digital[i].bit;
        }
    }
    const int lt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7;
    const int rt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7;
    if (lt > s->analog[6]) {
        s->analog[6] = (uint8_t)lt;
    }
    if (rt > s->analog[7]) {
        s->analog[7] = (uint8_t)rt;
    }
    const int lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = -SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    const int rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = -SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
    /* The stick pushed furthest wins between the pad and the overlay. */
    if (abs(lx) + abs(ly) > abs(s->lx) + abs(s->ly)) {
        s->lx = clamp16(lx);
        s->ly = clamp16(ly);
    }
    if (abs(rx) + abs(ry) > abs(s->rx) + abs(s->ry)) {
        s->rx = clamp16(rx);
        s->ry = clamp16(ry);
    }
}
