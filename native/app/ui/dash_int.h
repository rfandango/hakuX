/*
 * hakuX front end — the dashboard's pieces (dash*.c): the input, the
 * screen stack, the lists every screen is made of.
 */
#ifndef NATIVE_APP_UI_DASH_INT_H
#define NATIVE_APP_UI_DASH_INT_H

#include "native/app/ui/config.h"
#include "native/app/ui/i18n.h"
#include "native/app/ui/library.h"
#include "native/app/ui/ui.h"

#include <stdbool.h>

typedef struct In {
    bool up, down, left, right, ok, back, x, y, lb, rb, start;
    float mx, my, wheel;    /* the mouse, in 720p units */
    bool click, moved, rclick;
    bool any;               /* something was pressed (screensaver, hints) */
    bool pad;               /* the last input came from a gamepad (the hints' glyphs) */
    SDL_Keycode key;        /* the key pressed (remapping), 0 none */
    int pad_button;         /* the gamepad button pressed, -1 none */
    int pad_axis;           /* an axis pushed past half: 1 + axis, negated for negative; 0 none */
    const char *text;       /* text typed (text fields) */
} In;

enum Screen {
    SCR_MAIN, SCR_GAMES, SCR_GAME, SCR_GAME_SETTINGS, SCR_MODS, SCR_STATES, SCR_SETTINGS, SCR_CONTROLS,
    SCR_REMAP, SCR_MEMORY, SCR_MEMORY_TITLE, SCR_ONLINE, SCR_ABOUT, SCR_INGAME, SCR_INGAME_SLOTS, SCR_COUNT
};

typedef struct Dash {
    In in;
    enum Screen stack[16];
    int sp;
    int sel[SCR_COUNT], scroll[SCR_COUNT];
    float anim;                 /* the current screen's age, s */
    Config global, game;        /* xbased.ini, the selected game's */
    LibGame *cur;               /* the game the game screens are about */
    int tab;                    /* the settings' tab */
    int player;                 /* the controls' player (0-3) */
    bool editing_game;          /* lists edit the game's file over the global one */
    /* a question (yes/no), its callback */
    const char *ask;
    void (*on_yes)(struct Dash *);
    char toast[256];
    double toast_until;
    char exe[1024];
    bool quit;
    /* remapping: the input being bound */
    int remap_target;
    double remap_since;
    /* text entry */
    char *edit_buf;
    size_t edit_cap;
    const char *edit_key;
    char edit_tmp[256];
} Dash;

extern Dash D;

/* screens */
void dash_push(enum Screen s);
void dash_pop(void);
enum Screen dash_top(void);
void dash_toast(const char *msg);
void dash_ask(const char *question, void (*on_yes)(Dash *));
void dash_save_cfg(void);

/* the lists: a panel of rows, the current screen's selection and scroll */
typedef struct List {
    float x, y, w, h, row_h;
    int count;      /* rows so far this frame */
    int *sel, *scroll;
    int visible;
    bool focused;
} List;
void list_begin(List *l, float x, float y, float w, float h, float row_h);
/* A row: its label and value; true when chosen (A, Enter, a click). delta:
 * left/right on it (-1, +1), may be NULL. */
bool list_row(List *l, const char *label, const char *value, int *delta);
bool list_row_icon(List *l, SDL_Texture *icon, const char *label, const char *sub, const char *value, int *delta);
void list_end(List *l);
bool list_selected(const List *l);   /* the last row is the selected one */

/* settings rows over D.global or (editing a game) D.game */
void opt_choice(List *l, enum UiStr label, const char *key, const char *const *vals, const char *const *names, int n,
                const char *def);
void opt_bool(List *l, enum UiStr label, const char *key, bool def, bool flag_style);
void opt_int(List *l, enum UiStr label, const char *key, int lo, int hi, int step, int def, const char *unit);
void opt_videoflag(List *l, enum UiStr label, unsigned bit);

/* drawing helpers */
void draw_header(const char *title, const char *sub);
void draw_hints(const char *a, const char *b, const char *x, const char *y);
void draw_button_glyph(float x, float y, int which, float size);   /* 0 A, 1 B, 2 X, 3 Y */
float ease(float t);

/* the screens */
void scr_settings(void);
void scr_controls(void);
void scr_remap(void);
void scr_memory(void);
void scr_memory_title(void);
void scr_online(void);
void scr_game_settings(void);
void scr_mods(void);
void scr_states(void);
/* the folder picker's answer: a game folder added (SDL_ShowOpenFolderDialog) */
void add_folder_cb(void *ud, const char *const *files, int filter);
void dash_take_folder(void);    /* each frame: a folder picked meanwhile goes in */

/* the game's files */
void game_cfg_path(const LibGame *g, char *out, size_t n);
void states_dir(const LibGame *g, char *out, size_t n);
void launch_game(LibGame *g, const char *extra);

#endif
