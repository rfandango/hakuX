/*
 * xbased (hakuX for PC) — the front end: an SDL3 window, the controllers and
 * the keyboard, the core on its own thread, the HLE GPU's frames presented
 * into the window once per display refresh (present.c), as the Android
 * app's NativeCoreActivity and app_main.c do.
 *
 *   hakux [disc image] [option...]
 *
 * With no disc image, a file dialog asks for one. Options are the Android
 * app's arguments: the core's (native/core/options.h: scale=, loadstate=,
 * video=, lang=, net=, mods=, uncap, ...), the window's (aspect=auto|4:3|
 * 16:9|stretch, fxaa=0|1, upscale=bilinear|sgsr), and
 *   hdd=DIR      the hard disk folder (default: hdd beside the executable)
 *   mod=ID       a mod of the game's on (its folder in mods\<TITLE ID>.<version>
 *                beside the executable; mod=* all of them), cheat=NAME a cheat
 *   fullscreen=0 start in a window (full screen, borderless, otherwise)
 *   console      a console window for the log (it always goes to kantaiX.log)
 * kantaiX.ini beside the executable holds options too, one a line ('#'
 * comments): the command line's come after, and win.
 *
 * Keys: F11 or Alt+Enter full screen, F2 pause, F5 save the state, F8 load
 * it (slot 1; Shift: slot 2), Esc leaves full screen. Player 1 plays on the
 * keyboard while no controller is plugged in (win_keys below).
 *
 * Loading a state, a title's relaunch (XLaunchNewImage) and its reboot
 * start the core again: the process runs itself again with the option the
 * Android app passes its activity (loadstate=, relaunch).
 */
#include "native/app/app.h"
#include "native/app/ui/config.h"
#include "native/app/ui/dash.h"
#include "native/app/ui/i18n.h"
#include "native/app/ui/input_map.h"
#include "hw/xbox/hle/vk/swapchain.h"
#include "native/app/ui/xbox_data.h"
#include "native/app/desk_auto.h"
#include "native/app/desk_mods.h"
#include "native/core/cheats.h"
#include "native/core/core.h"
#include "native/core/mods.h"
#include "native/core/options.h"
#include "native/dev/usb.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"
#include "native/state/save.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/vk/native_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "native/app/desk.h"   /* windows.h, or its few calls over POSIX */
#ifdef _WIN32
#include <shellapi.h>
#endif

/* native/dev/apu/out.c */
void nc_audio_set_paused(bool on);
void nc_audio_set_volume(float v);
/* native/dev/ohci.c */
void nc_dev_ohci_set_pads(unsigned mask);
/* hw/xbox/hle/pb/pbmode.h */
bool hle_pb_active(void);

#define MAX_ARGS 64

static NcCore g_core;
static int g_disc_fd = -1;
static SDL_AtomicInt g_core_state; /* 0 booting, 1 running, -1 failed or stopped */
static struct ANativeWindow g_nw;
static char g_exe_dir[MAX_PATH];
static char g_data_dir[MAX_PATH]; /* settings, saves, states, the log (cfg_data_dir) */
static char g_disc[1024];
static char *g_args[MAX_ARGS];     /* the options, for running again */
static int g_nargs;
static int g_ncfg_args;            /* how many of them kantaiX.ini gave */
static ImMap g_map;     /* the controls: the game's file over kantaiX.ini (input_map.h) */
static char g_key[24];  /* the game's "<TITLE ID>.<version>" */

/* ------------------------------------------------------------ helpers */

static void exe_dir(void)
{
    DWORD n = GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    while (n && g_exe_dir[n - 1] != '\\' && g_exe_dir[n - 1] != '/') {
        n--;
    }
    g_exe_dir[n] = 0;
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", cfg_data_dir());
}

static void add_arg(const char *a)
{
    if (g_nargs < MAX_ARGS && a && a[0]) {
        g_args[g_nargs++] = strdup(a);
    }
}

/* kantaiX.ini: one option a line (hakux.ini, an earlier build's, becomes it) */
static void read_ini(void)
{
    char path[MAX_PATH + 16], old[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%skantaiX.ini", g_data_dir);
    snprintf(old, sizeof(old), "%shakux.ini", g_data_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        MoveFileA(old, path);
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = 0;
        }
        if (n && p[0] != '#' && p[0] != ';') {
            add_arg(p);
        }
    }
    fclose(f);
}

static const char *opt_value(const char *name)
{
    const size_t n = strlen(name);
    const char *v = NULL;
    for (int i = 0; i < g_nargs; i++) {
        if (!strncmp(g_args[i], name, n) && g_args[i][n] == '=') {
            v = g_args[i] + n + 1; /* the last one wins */
        }
    }
    return v;
}

static bool opt_flag(const char *name)
{
    for (int i = 0; i < g_nargs; i++) {
        if (!strcmp(g_args[i], name)) {
            return true;
        }
    }
    return false;
}

/* Full screen (borderless, the desktop's mode) unless told otherwise
 * ("fullscreen=0"): a console's picture, not a window's. */
static bool want_fullscreen(void)
{
    const char *v = opt_value("fullscreen");
    return v ? strcmp(v, "0") != 0 : true;
}

/* The game's own options (games/<TITLE ID>.<version>.ini) after the global
 * ones and before the command line's (which come last and win); the
 * console's language from the interface's when none is set; the dashboard's
 * "uncap=1" as the flag the core takes, "fullscreen" (on unless "=0") once. */
static void game_options(const char *disc)
{
    XdGame g;
    xd_read_game(disc, &g);
    xd_free_game(&g);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g.key);
    Config c;
    cfg_load(&c, path);
    /* the command line's options after the game's: move them to the end */
    char *cmdline[MAX_ARGS];
    int ncmd = 0;
    for (int i = g_ncfg_args; i < g_nargs; i++) {
        cmdline[ncmd++] = g_args[i];
    }
    g_nargs = g_ncfg_args;
    for (int i = 0; i < c.n; i++) {
        char a[600];
        if (c.flag[i]) {
            snprintf(a, sizeof(a), "%s", c.key[i]);
        } else {
            snprintf(a, sizeof(a), "%s=%s", c.key[i], c.val[i]);
        }
        add_arg(a);
    }
    for (int i = 0; i < ncmd; i++) {
        if (g_nargs < MAX_ARGS) {
            g_args[g_nargs++] = cmdline[i];
        }
    }
    if (c.n) {
        nc_log("kantaiX", "%d options of the game's (%s)", c.n, path);
    }
    snprintf(g_key, sizeof(g_key), "%s", g.key);
    Config global;
    snprintf(path, sizeof(path), "%skantaiX.ini", g_data_dir);
    cfg_load(&global, path);
    im_resolve(&g_map, &global, &c);
    /* flags written as values */
    static const char *const k_flags[] = {"uncap", "fullscreen"};
    for (int f = 0; f < 2; f++) {
        const char *v = opt_value(k_flags[f]);
        bool on = opt_flag(k_flags[f]) || f == 1;  /* full screen by default */
        if (v) {
            on = strcmp(v, "0") != 0;
        }
        for (int i = 0; i < g_nargs; i++) {
            const size_t n = strlen(k_flags[f]);
            if (!strncmp(g_args[i], k_flags[f], n) && (g_args[i][n] == '=' || g_args[i][n] == 0)) {
                g_args[i][0] = 0;  /* gone; added back once below */
            }
        }
        if (on) {
            add_arg(k_flags[f]);
        } else if (f == 1) {
            add_arg("fullscreen=0");
        }
    }
    const char *lang = opt_value("lang");
    if (!lang || !strcmp(lang, "auto")) {
        i18n_set(opt_value("ui.lang"));
        char a[16];
        snprintf(a, sizeof(a), "lang=%d", i18n_console_lang());
        add_arg(a);
    }
}

static void make_hdd(const char *root)
{
    char p[1024];
    mkdir(root, 0775);
    for (int i = 0; i <= 5; i++) {
        snprintf(p, sizeof(p), "%s/Partition%d", root, i);
        mkdir(p, 0775);
    }
}

static void fatal_box(const char *title, const char *msg)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}

/* The process again, with the same disc and options plus extra (minus any
 * earlier option of extra's name). */
static void run_again(const char *extra)
{
    char cmd[8192];
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    int n = snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", exe, g_disc[0] ? g_disc : "none");
    const char *eq = extra ? strchr(extra, '=') : NULL;
    const size_t name_len = eq ? (size_t)(eq - extra) : extra ? strlen(extra) : 0;
    for (int i = 0; i < g_nargs && n < (int)sizeof(cmd) - 8; i++) {
        if (extra && !strncmp(g_args[i], extra, name_len) &&
            (g_args[i][name_len] == '=' || g_args[i][name_len] == 0)) {
            continue;
        }
        if (!strcmp(g_args[i], "relaunch") || !strncmp(g_args[i], "loadstate=", 10)) {
            continue; /* one run's */
        }
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " \"%s\"", g_args[i]);
    }
    if (extra) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " \"%s\"", extra);
    }
    nc_log("kantaiX", "running again: %s", cmd);
#ifdef _WIN32
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_exe_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        /* the dashboard waits for this process: the new one's id in the
         * exit code (bit 30 set) lets it follow the game on (dash.c) */
        fflush(stderr);
        _exit((int)(0x40000000u | (pi.dwProcessId & 0x3FFFFFFFu)));
    }
#else
    /* this process becomes the new run (the same arguments as above) */
    char *argv[MAX_ARGS + 4];
    int k = 0;
    argv[k++] = exe;
    argv[k++] = g_disc[0] ? g_disc : (char *)"none";
    for (int i = 0; i < g_nargs; i++) {
        if (extra && !strncmp(g_args[i], extra, name_len) && (g_args[i][name_len] == '=' || g_args[i][name_len] == 0)) {
            continue;
        }
        if (!strcmp(g_args[i], "relaunch") || !strncmp(g_args[i], "loadstate=", 10) || !g_args[i][0]) {
            continue;
        }
        argv[k++] = g_args[i];
    }
    if (extra) {
        argv[k++] = (char *)extra;
    }
    argv[k] = NULL;
    fflush(stderr);
    execv(exe, argv);
#endif
    _exit(0);
}

/* ------------------------------------------------------------- the core */

static void *core_thread(void *arg)
{
    nc_desk_thread_name("vcpu");
    nc_desk_thread_priority(1);
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("kantaiX", "core: boot failed");
        if (!nc_hle_fatal()) {
            nc_hle_set_fatal(NC_FATAL_BOOT, "The game stopped while starting.");
        }
        SDL_SetAtomicInt(&g_core_state, -1);
        return NULL;
    }
    SDL_SetAtomicInt(&g_core_state, 1);
    nc_core_run(&g_core);
    nc_log("kantaiX", "core: stopped");
    SDL_SetAtomicInt(&g_core_state, -1);
    return NULL;
}

/* ------------------------------------------------------------ the input */

/* Controllers by player: each one plugged in takes the first free port. */
static SDL_Gamepad *g_pads[4];
static uint16_t g_rumble[4][2];


/* Player p's gamepad: its device's (auto: the p-th plugged in, padK: the
 * K-th), NULL for the keyboard, none, or nothing plugged. */
static SDL_Gamepad *player_pad(int p)
{
    const int d = g_map.device[p];
    if (d < 0) {
        return NULL;
    }
    const int k = d == 0 ? p : d - 1;
    return k < 4 ? g_pads[k] : NULL;
}

static void pads_changed(void)
{
    unsigned mask = 0;
    for (int p = 0; p < 4; p++) {
        const int d = g_map.device[p];
        /* player 1 always (the keyboard, unless set to none); the others with a pad */
        if (d != -2 && (player_pad(p) || (p == 0 && d <= 0) || d == -1)) {
            mask |= 1u << p;
        }
    }
    nc_dev_ohci_set_pads(mask ? mask : 1);
}

static void pad_added(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            return;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            nc_log("kantaiX", "controller for player %u: %s", i + 1,
                   g_pads[i] ? SDL_GetGamepadName(g_pads[i]) : SDL_GetError());
            pads_changed();
            return;
        }
    }
}

/* A joystick SDL has no gamepad mapping for (the generic DirectInput pads):
 * the usual layout of those (buttons 0-3 the face buttons, 4-5 the
 * shoulders, 6-7 the triggers, 8-9 Back and Start, 10-11 the sticks'
 * clicks, the hat the d-pad, axes 0-1 and 2-3 the sticks), so it becomes a
 * gamepad. gamecontrollerdb.txt beside the executable, if there, has the
 * right one for many more. */
static void joystick_added(SDL_JoystickID id)
{
    if (SDL_IsGamepad(id)) {
        return;
    }
    char guid[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    const char *name = SDL_GetJoystickNameForID(id);
    char map[512];
    snprintf(map, sizeof(map),
             "%s,%s,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
             "back:b8,start:b9,leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,",
             guid, name ? name : "Joystick");
    nc_log("kantaiX", "joystick without a gamepad mapping: \"%s\" (%s); the generic layout given", name ? name : "?",
           guid);
    SDL_AddGamepadMapping(map); /* a gamepad now: SDL_EVENT_GAMEPAD_ADDED follows */
}

static void pad_removed(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
            nc_log("kantaiX", "player %u's controller unplugged", i + 1);
            pads_changed();
        }
    }
}

static void update_pads(bool focused)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    for (unsigned i = 0; i < 4; i++) {
        NcPadState s;
        memset(&s, 0, sizeof(s));
        SDL_Gamepad *pad = player_pad((int)i);
        /* the keyboard: player 1 on auto, any player set to it */
        const bool kb = focused && (g_map.device[i] == -1 || (i == 0 && g_map.device[i] == 0));
        ImPad m;
        im_read(&g_map, (int)i, kb ? keys : NULL, pad, &m);
        s.buttons = m.buttons;
        memcpy(s.analog, m.analog, sizeof(s.analog));
        s.lx = m.lx;
        s.ly = m.ly;
        s.rx = m.rx;
        s.ry = m.ry;
        if (i == 0) {
            nc_auto_pad(&s);
        }
        nc_pad_set(i, &s);
        uint16_t l = 0, r = 0;
        nc_pad_rumble(i, &l, &r);
        if (!g_map.rumble) {
            l = r = 0;
        }
        if (pad && (l != g_rumble[i][0] || r != g_rumble[i][1])) {
            SDL_RumbleGamepad(pad, l, r, l || r ? 1000 : 0);
            g_rumble[i][0] = l;
            g_rumble[i][1] = r;
        }
    }
}

/* ------------------------------------------------------------ states */

/* states\<TITLE ID>.<version>\slot<N>.hxs, its picture slot<N>.bmp beside */
static void state_path(int slot, char *out, size_t n)
{
    char dir[MAX_PATH + 64];
    snprintf(dir, sizeof(dir), "%sstates", g_data_dir);
    mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%sstates/%s", g_data_dir, g_key);
    mkdir(dir, 0775);
    snprintf(out, n, "%s/slot%d.hxs", dir, slot);
}

/* The save runs on a thread of its own, as NativeCoreActivity's does: it
 * waits for the GPU to park at a frame, which the window's thread presents
 * meanwhile (from that thread, it waited on itself). */
static SDL_AtomicInt g_saving;

static void *save_thread(void *arg)
{
    const int slot = (int)(intptr_t)arg;
    char path[MAX_PATH * 2];
    state_path(slot, path, sizeof(path));
    const char *err = nc_state_save(path);
    nc_log("kantaiX", "savestate %d: %s", slot, err ? err : "saved");
    if (!err) {
        /* its picture: the frame shown, the dashboard's thumbnail */
        char pic[MAX_PATH * 2];
        snprintf(pic, sizeof(pic), "%.*s.bmp", (int)(strlen(path) - 4), path);
        hle_present_shot(pic);
    }
    SDL_SetAtomicInt(&g_saving, 0);
    return NULL;
}

static void save_state(int slot)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1 || !hle_pb_active()) {
        nc_log("kantaiX", "savestate: not available now");
        return;
    }
    if (!SDL_CompareAndSwapAtomicInt(&g_saving, 0, 1)) {
        return; /* one at a time */
    }
    pthread_t t;
    if (pthread_create(&t, NULL, save_thread, (void *)(intptr_t)slot) == 0) {
        pthread_detach(t);
    } else {
        SDL_SetAtomicInt(&g_saving, 0);
    }
}

static void load_state(int slot)
{
    char path[MAX_PATH * 2], opt[MAX_PATH * 2 + 16];
    state_path(slot, path, sizeof(path));
    const char *err = nc_state_compat(path);
    if (err) {
        nc_log("kantaiX", "savestate %d: %s", slot, err);
        return;
    }
    snprintf(opt, sizeof(opt), "loadstate=%s", path);
    run_again(opt);
}

static bool g_uncap;    /* the run's frame rate setting (fast forward goes back to it) */
static float g_volume = 1.0f;
static bool g_mute_bg = true;   /* silent while the window isn't the active one */

/* screenshots\<title>-<date>.bmp beside the executable */
static void screenshot(void)
{
    char dir[MAX_PATH + 32], p[MAX_PATH + 128];
    snprintf(dir, sizeof(dir), "%sscreenshots", g_data_dir);
    mkdir(dir, 0775);
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(p, sizeof(p), "%s/%s-%04d%02d%02d-%02d%02d%02d.bmp", dir, g_key, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    hle_present_shot(p);
}

/* The in-game menu: the emulation paused, the presenter off the window (its
 * Vulkan surface dropped) while the dashboard's renderer draws the menu
 * over the last frame; then the presenter takes the window back. */
static void ingame_menu(SDL_Window *win, bool *paused, bool minimized)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1) {
        return;
    }
    nc_kernel_set_paused(&g_core.kernel, true);
    nc_audio_set_paused(true);
    char pic[MAX_PATH + 32];
    snprintf(pic, sizeof(pic), "%smenu_frame.bmp", g_data_dir);
    remove(pic);
    hle_present_shot(pic);
    for (int i = 0; i < 30 && GetFileAttributesA(pic) == INVALID_FILE_ATTRIBUTES; i++) {
        nc_present_frame(&g_nw);
        SDL_Delay(5);
    }
    nc_present_frame(NULL);     /* the surface goes */
    int slot = 0;
    const char *title = strrchr(g_disc, '\\');
    const int act = dash_ingame(win, g_key, title ? title + 1 : g_disc, pic, &slot);
    remove(pic);
    nc_auto_resync();
    /* the presenter back on the window before anything is shown or saved */
    SDL_Delay(50);
    nc_present_frame(&g_nw);
    nc_kernel_set_paused(&g_core.kernel, *paused || minimized);
    nc_audio_set_paused(*paused || minimized);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%skantaiX.ini", g_data_dir);
    Config global, game;
    cfg_load(&global, path);
    snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g_key);
    cfg_load(&game, path);
    im_resolve(&g_map, &global, &game);
    pads_changed();
    /* what applies at once: the picture's shape and filters, the volume */
    const Config *pick[2] = {&game, &global};
    for (int k = 1; k >= 0; k--) {
        const Config *c = pick[k];
        const char *v;
        if ((v = cfg_get(c, "aspect"))) {
            nc_present_set_aspect(!strcmp(v, "4:3") ? NC_ASPECT_4_3 : !strcmp(v, "16:9") ? NC_ASPECT_16_9
                                  : !strcmp(v, "stretch") ? NC_ASPECT_STRETCH : NC_ASPECT_AUTO);
        }
        if ((v = cfg_get(c, "fxaa"))) {
            nc_present_set_fxaa(atoi(v) != 0);
        }
        if ((v = cfg_get(c, "upscale"))) {
            nc_present_set_upscale(!strcmp(v, "sgsr") ? NC_UPSCALE_SGSR : NC_UPSCALE_BILINEAR);
        }
        if ((v = cfg_get(c, "crt"))) {
            hle_present_set_crt(atoi(v));
        }
        if ((v = cfg_get(c, "volume"))) {
            g_volume = atoi(v) / 100.0f;
            nc_audio_set_volume(g_volume);
        }
        if ((v = cfg_get(c, "showfps"))) {
            nc_present_set_show_fps(atoi(v) != 0);
        }
        if ((v = cfg_get(c, "mutebg"))) {
            g_mute_bg = atoi(v) != 0;
        }
    }
    switch (act) {
    case DASH_INGAME_SAVE:
        save_state(slot);
        break;
    case DASH_INGAME_LOAD:
        load_state(slot);
        break;
    case DASH_INGAME_RESTART:
        run_again(NULL);
        break;
    case DASH_INGAME_QUIT:
        nc_log("kantaiX", "quit to the dashboard");
        _exit(0);
    default:
        break;
    }
}

/* debug.hakux.state ("save:<slot>", "load:<slot>"), as the app's menu would */
static void state_request_poll(void)
{
    static char last[PROP_VALUE_MAX];
    static bool primed;
    static unsigned n;
    if (n++ % 30) {
        return;
    }
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hakux.state", v);
    if (!primed || !strcmp(v, last)) {
        primed = true;
        snprintf(last, sizeof(last), "%s", v);
        return;
    }
    snprintf(last, sizeof(last), "%s", v);
    if (!strncmp(v, "save:", 5)) {
        save_state(atoi(v + 5));
    } else if (!strncmp(v, "load:", 5)) {
        load_state(atoi(v + 5));
    }
}

/* ------------------------------------------------------------- main */

/* kantaiX.log beside the executable: a run the process started again (a
 * title's relaunch, a state loaded) goes on in the same log. */
static void log_setup(bool console, bool again)
{
    char path[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%skantaiX.log", g_data_dir);
#ifdef _WIN32
    if (console && AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else {
        freopen(path, again ? "a" : "w", stderr);
    }
#else
    (void)console;
    freopen(path, again ? "a" : "w", stderr);
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* The file dialog's answer (SDL calls back on a thread of its own). */
static SDL_AtomicInt g_dialog_done;
static char g_dialog_path[1024];

static void SDLCALL dialog_cb(void *ud, const char *const *list, int filter)
{
    if (list && list[0]) {
        snprintf(g_dialog_path, sizeof(g_dialog_path), "%s", list[0]);
    }
    SDL_SetAtomicInt(&g_dialog_done, 1);
}

static const char *pick_disc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Xbox disc images (*.iso, *.xiso)", "iso;xiso"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(dialog_cb, NULL, NULL, filters, 2, NULL, false);
    while (!SDL_GetAtomicInt(&g_dialog_done)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        SDL_Delay(20);
    }
    return g_dialog_path[0] ? g_dialog_path : NULL;
}

#ifndef KANTAIX_VERSION
#define KANTAIX_VERSION "1.0.2"
#endif
#ifdef _WIN32
#define NC_PLATFORM_NAME "Windows"
#else
#define NC_PLATFORM_NAME "Linux"
#endif

int main(int argc, char *argv[])
{
    exe_dir();
    read_ini();
    g_ncfg_args = g_nargs;
    const char *disc = NULL;
    for (int i = 1; i < argc; i++) {
        if (!disc && !strchr(argv[i], '=') && strcmp(argv[i], "uncap") && strcmp(argv[i], "relaunch") &&
            strcmp(argv[i], "fullscreen") && strcmp(argv[i], "console")) {
            disc = argv[i];
        } else {
            add_arg(argv[i]);
        }
    }
    bool again = opt_flag("relaunch");
    for (int i = 0; i < g_nargs; i++) {
        again |= !strncmp(g_args[i], "loadstate=", 10);
    }
    log_setup(opt_flag("console"), again);
    if (again) {
        nc_log("kantaiX", "---- started again (%s)", opt_flag("relaunch") ? "the title's relaunch" : "a state loaded");
    }
    nc_log("kantaiX", "kantaiX " KANTAIX_VERSION " (based on hakuX + xbased), " NC_PLATFORM_NAME);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifndef _WIN32
    /* PulseAudio's API first: PipeWire desktops serve it too, and where
     * PipeWire's own is missing (WSLg) SDL waits half a minute on it */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fatal_box("kantaiX", SDL_GetError());
        return 1;
    }
    {
        char db[MAX_PATH + 32];
        snprintf(db, sizeof(db), "%sgamecontrollerdb.txt", g_exe_dir);
        const int n = SDL_AddGamepadMappingsFromFile(db);
        if (n > 0) {
            nc_log("kantaiX", "%d gamepad mappings from %s", n, db);
        }
    }
    if (!disc || !strcmp(disc, "none")) {
        if (opt_flag("pick")) {
            disc = pick_disc();
            if (!disc) {
                return 0;
            }
        } else {
            /* the dashboard: the library and settings; each game in a process of its own */
            SDL_Window *dw = SDL_CreateWindow("kantaiX", 1280, 720,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                  (want_fullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
            if (!dw) {
                fatal_box("kantaiX", SDL_GetError());
                return 1;
            }
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            const int r = dash_run(dw, exe);
            SDL_DestroyWindow(dw);
            SDL_Quit();
            return r;
        }
    }
    snprintf(g_disc, sizeof(g_disc), "%s", disc);
    game_options(g_disc);

    g_disc_fd = open(g_disc, O_RDONLY | O_BINARY);
    if (g_disc_fd < 0) {
        nc_log("kantaiX", "can't open the disc %s", g_disc);
    }
    char hdd[1024];
    const char *hv = opt_value("hdd");
    if (hv) {
        snprintf(hdd, sizeof(hdd), "%s", hv);
    } else {
        snprintf(hdd, sizeof(hdd), "%shdd", g_data_dir);
    }
    make_hdd(hdd);
    nc_io_set_hdd_root(hdd);
    setenv("HAKUX_HLE_MODE", "replace", 0);
    {
        char base[1100];
        snprintf(base, sizeof(base), "%s/../hle", hdd);
        mkdir(base, 0775);
        setenv("HAKUX_NATIVE_BASE", base, 0);
    }

    NcOptions opt;
    nc_options_init(&opt);
    for (int i = 0; i < g_nargs; i++) {
        const char *a = g_args[i];
        if (nc_option(&opt, a)) {
            continue; /* the core's */
        }
        if (!strncmp(a, "aspect=", 7)) {
            const char *v = a + 7;
            nc_present_set_aspect(!strcmp(v, "4:3")       ? NC_ASPECT_4_3
                                  : !strcmp(v, "16:9")    ? NC_ASPECT_16_9
                                  : !strcmp(v, "stretch") ? NC_ASPECT_STRETCH
                                                          : NC_ASPECT_AUTO);
        } else if (!strncmp(a, "fxaa=", 5)) {
            nc_present_set_fxaa(atoi(a + 5) != 0);
        } else if (!strncmp(a, "upscale=", 8)) {
            nc_present_set_upscale(!strcmp(a + 8, "sgsr") ? NC_UPSCALE_SGSR : NC_UPSCALE_BILINEAR);
        } else if (!strncmp(a, "crt=", 4)) {
            hle_present_set_crt(atoi(a + 4));
        } else if (!strncmp(a, "volume=", 7)) {
            g_volume = atoi(a + 7) / 100.0f;
            nc_audio_set_volume(g_volume);
        } else if (!strncmp(a, "showfps=", 8)) {
            nc_present_set_show_fps(atoi(a + 8) != 0);
        } else if (!strncmp(a, "mutebg=", 7)) {
            g_mute_bg = atoi(a + 7) != 0;
        }
    }
    nc_desk_mods_prepare(g_exe_dir, g_disc_fd, g_args, g_nargs);
    nc_options_apply(&opt);
    g_uncap = opt.uncap;
    nc_present_set_widescreen(opt.video >= 0 && (opt.video & NC_VIDEO_WIDESCREEN));
    nc_log("kantaiX", "native core: disc %s, hdd %s%s", g_disc, hdd, opt.uncap ? ", frame rate uncapped" : "");

    const char *title = strrchr(g_disc, '\\');
    const char *title2 = strrchr(g_disc, '/');
    title = title2 > title ? title2 : title;
    char wtitle[512];
    snprintf(wtitle, sizeof(wtitle), "kantaiX - %s", title ? title + 1 : g_disc);
#ifdef _WIN32
    const SDL_WindowFlags vk_flag = 0;            /* the presenter makes its own Win32 surface */
#else
    const SDL_WindowFlags vk_flag = SDL_WINDOW_VULKAN; /* SDL_Vulkan_CreateSurface's (swapchain.c) */
#endif
    SDL_Window *win = SDL_CreateWindow(wtitle, 1280, 960, vk_flag | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                              (want_fullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) {
        fatal_box("kantaiX", SDL_GetError());
        return 1;
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    g_nw.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    g_nw.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);
    g_nw.sdl_window = win;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    g_nw.width = pw;
    g_nw.height = ph;

    if (!nc_present_init()) {
        return 1;
    }
    nc_vsync_start();
    nc_auto_init(g_data_dir, save_state, load_state);
#if defined(__x86_64__) || (defined(__aarch64__) && defined(__linux__))
    nc_core_fault_install(&g_core);
#endif
    pads_changed();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t core;
    pthread_create(&core, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);

    nc_desk_thread_name("window");
    bool quit = false, paused = false, minimized = false, focused = true, told = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.repeat) {
                    break;
                }
                /* the shortcuts (Settings > Shortcuts); Shift with quick save/load: slot 2 */
                if (ev.key.key == g_map.hotkey[IM_HK_FULL] ||
                    (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                } else if (ev.key.key == g_map.hotkey[IM_HK_PAUSE]) {
                    paused = !paused;
                    nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                    nc_audio_set_paused(paused || minimized);
                    nc_log("kantaiX", "%s", paused ? "paused" : "resumed");
                } else if (ev.key.key == g_map.hotkey[IM_HK_SAVE]) {
                    save_state(ev.key.mod & SDL_KMOD_SHIFT ? 2 : 1);
                } else if (ev.key.key == g_map.hotkey[IM_HK_LOAD]) {
                    load_state(ev.key.mod & SDL_KMOD_SHIFT ? 2 : 1);
                } else if (ev.key.key == g_map.hotkey[IM_HK_TURBO]) {
                    nc_desk_prop_set("debug.hakux.uncap", "1");  /* frames as fast as they come, while held */
                } else if (ev.key.key == g_map.hotkey[IM_HK_SHOT]) {
                    screenshot();
                } else if (ev.key.key == g_map.hotkey[IM_HK_MENU]) {
                    ingame_menu(win, &paused, minimized);
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (ev.key.key == g_map.hotkey[IM_HK_TURBO]) {
                    nc_desk_prop_set("debug.hakux.uncap", g_uncap ? "1" : "0");
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                /* the menu: the Guide button, or Back with Start */
                SDL_Gamepad *gp = SDL_GetGamepadFromID(ev.gbutton.which);
                if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE ||
                    (gp && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK) &&
                     SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START))) {
                    ingame_menu(win, &paused, minimized);
                }
                break;
            }
            case SDL_EVENT_JOYSTICK_ADDED:
                joystick_added(ev.jdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                pad_added(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                pad_removed(ev.gdevice.which);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_nw.width = ev.window.data1;
                g_nw.height = ev.window.data2;
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                minimized = ev.type == SDL_EVENT_WINDOW_MINIMIZED;
                nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                nc_audio_set_paused(paused || minimized);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                focused = true;
                nc_audio_set_volume(g_volume);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                focused = false;
                if (g_mute_bg) {
                    nc_audio_set_volume(0);
                }
                break;
            default:
                break;
            }
        }
        nc_auto_tick();
        if (nc_auto_take_menu()) {
            ingame_menu(win, &paused, minimized);
        }
        update_pads(focused);
        state_request_poll();

        const int state = SDL_GetAtomicInt(&g_core_state);
        if (state < 0) {
            if (g_core.kernel.relaunch) {
                run_again("relaunch");
            }
            if (g_core.kernel.quit) {
                break;
            }
            if (!told) {
                told = true;
                const char *why = nc_hle_fatal();
                if (why) {
                    fatal_box("kantaiX", why);
                    break;
                }
            }
        }
        if (minimized || g_nw.width <= 0 || !nc_present_frame(&g_nw)) {
            SDL_Delay(8); /* nothing to wait for the display on */
        }
    }
    nc_log("kantaiX", "native core: leaving");
    _exit(0); /* the core thread has no stop yet */
}
