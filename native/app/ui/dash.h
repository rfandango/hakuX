/*
 * hakuX front end — the dashboard (dash.c): the console-style menus
 * before a game: the library, settings, saves, online. Runs until the user
 * leaves; each game it starts runs in a process of its own.
 */
#ifndef NATIVE_APP_UI_DASH_H
#define NATIVE_APP_UI_DASH_H

#include <SDL3/SDL.h>

/* The dashboard in win until the user leaves. exe: this executable (the
 * games' process). */
int dash_run(SDL_Window *win, const char *exe);

/* The in-game menu over the paused game (frame_bmp: its last frame) in
 * win, whose presenter let it go. What the player chose, and for a state
 * the slot. Settings changed are saved to the files. */
enum { DASH_INGAME_RESUME, DASH_INGAME_SAVE, DASH_INGAME_LOAD, DASH_INGAME_RESTART, DASH_INGAME_QUIT };
int dash_ingame(SDL_Window *win, const char *key, const char *title, const char *frame_bmp, int *slot);

#endif
