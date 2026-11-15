/*
 * hakuX native core, desktop front end (Linux) — the hakux executable.
 *
 *   hakux <disc image | none> [options]
 *
 * Options: the core's (native/core/options.h: "uncap", "loadstate=",
 * "scale=", "video=", ...), and "hdd=<folder>" the hard disk (default
 * $XDG_DATA_HOME/hakuX/hdd), "fullscreen", "aspect=auto|4:3|16:9|stretch",
 * "fxaa=0|1", "upscale=bilinear|sgsr" as on Android (app_main.c).
 *
 * As app_main.c, the core boots and runs on its own thread and this one owns
 * the window: up to four SDL gamepads are players 1-4 in the order they
 * come, the keyboard adds to player 1 (k_keys), and the HLE GPU's frames are
 * presented once per refresh. F11 toggles full screen. A title's reboot into
 * another XBE (XLaunchNewImage) runs this again with "relaunch".
 */
#include "native/app/app.h"
#include "native/core/core.h"
#include "native/core/options.h"
#include "native/kernel/kernel.h"
#include "native/hle/hle_native.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLAYERS 4

static NcCore g_core;
static int g_disc_fd = -1;
static SDL_AtomicInt g_core_state; /* 0 booting, 1 running, -1 stopped */

/* The keyboard as player 1: the left stick on WASD, the d-pad on the
 * arrows, the face buttons as a diamond on IJKL. */
static const struct {
    SDL_Scancode key;
    int analog;         /* an analog button (NcPadState.analog), or -1 */
    uint16_t bit;       /* else a digital one */
} k_keys[] = {
    {SDL_SCANCODE_K, 0, 0},      /* A */
    {SDL_SCANCODE_L, 1, 0},      /* B */
    {SDL_SCANCODE_J, 2, 0},      /* X */
    {SDL_SCANCODE_I, 3, 0},      /* Y */
    {SDL_SCANCODE_O, 4, 0},      /* black */
    {SDL_SCANCODE_U, 5, 0},      /* white */
    {SDL_SCANCODE_Q, 6, 0},      /* left trigger */
    {SDL_SCANCODE_E, 7, 0},      /* right trigger */
    {SDL_SCANCODE_UP, -1, NC_PAD_DPAD_UP},
    {SDL_SCANCODE_DOWN, -1, NC_PAD_DPAD_DOWN},
    {SDL_SCANCODE_LEFT, -1, NC_PAD_DPAD_LEFT},
    {SDL_SCANCODE_RIGHT, -1, NC_PAD_DPAD_RIGHT},
    {SDL_SCANCODE_RETURN, -1, NC_PAD_START},
    {SDL_SCANCODE_BACKSPACE, -1, NC_PAD_BACK},
};

static void read_keyboard(NcPadState *s)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    for (unsigned i = 0; i < SDL_arraysize(k_keys); i++) {
        if (!k[k_keys[i].key]) {
            continue;
        }
        if (k_keys[i].analog >= 0) {
            s->analog[k_keys[i].analog] = 0xFF;
        } else {
            s->buttons |= k_keys[i].bit;
        }
    }
    s->lx = (int16_t)((k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]) * 32767);
    s->ly = (int16_t)((k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S]) * 32767);
}

/* HleWindow's: the SDL window's Vulkan surface and size */
static bool win_surface(void *ctx, VkInstance inst, VkSurfaceKHR *surf)
{
    if (!SDL_Vulkan_CreateSurface(ctx, inst, NULL, surf)) {
        nc_log("hakuX-app", "window: no Vulkan surface: %s", SDL_GetError());
        return false;
    }
    return true;
}

static void win_size(void *ctx, uint32_t *w, uint32_t *h)
{
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(ctx, &iw, &ih);
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
}

static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    if (nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    signal(sig, SIG_DFL);       /* not the core's: it faults again, and dumps */
}

static void *core_thread(void *arg)
{
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("hakuX-app", "core: boot failed");
        if (!nc_hle_fatal()) {
            nc_hle_set_fatal(NC_FATAL_BOOT, "The game stopped while starting.");
        }
        SDL_SetAtomicInt(&g_core_state, -1);
        return NULL;
    }
    SDL_SetAtomicInt(&g_core_state, 1);
    nc_core_run(&g_core);
    nc_log("hakuX-app", "core: stopped");
    SDL_SetAtomicInt(&g_core_state, -1);
    return NULL;
}

/* $XDG_DATA_HOME/hakuX, else ~/.local/share/hakuX, made if missing */
static void data_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') {
        snprintf(out, n, "%s/hakuX", xdg);
    } else {
        const char *home = getenv("HOME");
        char share[512];
        snprintf(share, sizeof(share), "%s/.local", home ? home : ".");
        mkdir(share, 0775);
        snprintf(share, sizeof(share), "%s/.local/share", home ? home : ".");
        mkdir(share, 0775);
        snprintf(out, n, "%s/hakuX", share);
    }
    mkdir(out, 0775);
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

/* The core stopped for a title's reboot into another XBE: this program again
 * with "relaunch" (once in its arguments). */
static void relaunch(int argc, char *argv[])
{
    char **args = calloc((size_t)argc + 2, sizeof(*args));
    bool has = false;
    for (int i = 0; i < argc; i++) {
        args[i] = argv[i];
        has |= !strcmp(argv[i], "relaunch");
    }
    if (!has) {
        args[argc] = "relaunch";
    }
    execv("/proc/self/exe", args);
    nc_log("hakuX-app", "relaunch: %s", strerror(errno));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        static const char usage[] = "usage: hakux <disc image | none> [hdd=<folder>] [fullscreen] [core options]";
        fprintf(stderr, "%s\n", usage);
        if (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "hakuX", usage, NULL); /* from a launcher */
        }
        return 1;
    }
    const char *disc = argv[1];
    if (strcmp(disc, "none")) {
        g_disc_fd = open(disc, O_RDONLY | O_CLOEXEC);
        if (g_disc_fd < 0) {
            fprintf(stderr, "%s: %s\n", disc, strerror(errno));
            return 1;
        }
    }
    char data[512], hdd[1024], base[1024];
    data_dir(data, sizeof(data));
    snprintf(hdd, sizeof(hdd), "%s/hdd", data);
    snprintf(base, sizeof(base), "%s/hle", data);
    setenv("HAKUX_NATIVE_BASE", base, 1);       /* the shader and pipeline caches */
    setenv("HAKUX_HLE_MODE", "replace", 0);

    NcOptions opt;
    nc_options_init(&opt);
    bool fullscreen = false;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (nc_option(&opt, a)) {
            continue;                           /* the core's (native/core/options.h) */
        }
        if (!strncmp(a, "hdd=", 4)) {
            snprintf(hdd, sizeof(hdd), "%s", a + 4);
        } else if (!strcmp(a, "fullscreen")) {
            fullscreen = true;
        } else if (!strncmp(a, "aspect=", 7)) {
            a += 7;
            nc_present_set_aspect(!strcmp(a, "4:3")       ? NC_ASPECT_4_3
                                  : !strcmp(a, "16:9")    ? NC_ASPECT_16_9
                                  : !strcmp(a, "stretch") ? NC_ASPECT_STRETCH
                                                          : NC_ASPECT_AUTO);
        } else if (!strncmp(a, "fxaa=", 5)) {
            nc_present_set_fxaa(atoi(a + 5) != 0);
        } else if (!strncmp(a, "upscale=", 8)) {
            nc_present_set_upscale(!strcmp(a + 8, "sgsr") ? NC_UPSCALE_SGSR : NC_UPSCALE_BILINEAR);
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
            return 1;
        }
    }
    make_hdd(hdd);
    nc_io_set_hdd_root(hdd);
    nc_options_apply(&opt);
    nc_present_set_widescreen(opt.video >= 0 && (opt.video & NC_VIDEO_WIDESCREEN));
    nc_log("hakuX-app", "native core: disc %s, hdd %s%s", disc, hdd, opt.uncap ? ", frame rate uncapped" : "");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        nc_log("hakuX-app", "SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("hakuX", 1280, 960,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                           (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) {
        nc_log("hakuX-app", "window: %s", SDL_GetError());
        return 1;
    }
    HleWindow hw = { .ctx = win, .surface = win_surface, .size = win_size };
    Uint32 nexts = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&nexts);
    if (!exts) {
        nc_log("hakuX-app", "window: no Vulkan: %s", SDL_GetError());
        return 1;
    }
    hle_window_instance_extensions(exts, nexts);
    if (!nc_present_init()) {
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t core;
    pthread_create(&core, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);

    SDL_Gamepad *pads[PLAYERS] = {0};
    uint16_t rumble[PLAYERS][2] = {0};
    unsigned pad_mask = 0;
    nc_dev_ohci_set_pads(1);                    /* player 1: the keyboard, if nothing else */
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                goto leave;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.scancode == SDL_SCANCODE_F11 && !ev.key.repeat) {
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                for (unsigned p = 0; p < PLAYERS; p++) {
                    if (!pads[p]) {
                        pads[p] = SDL_OpenGamepad(ev.gdevice.which);
                        nc_log("hakuX-app", "player %u: %s", p + 1, pads[p] ? SDL_GetGamepadName(pads[p])
                                                                            : SDL_GetError());
                        break;
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (unsigned p = 0; p < PLAYERS; p++) {
                    if (pads[p] && SDL_GetGamepadID(pads[p]) == ev.gdevice.which) {
                        SDL_CloseGamepad(pads[p]);
                        pads[p] = NULL;
                    }
                }
                break;
            default:
                break;
            }
        }

        unsigned mask = 1;
        for (unsigned p = 0; p < PLAYERS; p++) {
            NcPadState s = {0};
            if (p == 0) {
                read_keyboard(&s);
            }
            if (pads[p]) {
                mask |= 1u << p;
                nc_read_gamepad(pads[p], &s);
                uint16_t l = 0, r = 0;
                nc_pad_rumble(p, &l, &r);
                if (l != rumble[p][0] || r != rumble[p][1]) {
                    SDL_RumbleGamepad(pads[p], l, r, l || r ? 1000 : 0);
                    rumble[p][0] = l;
                    rumble[p][1] = r;
                }
            }
            nc_pad_set(p, &s);
        }
        if (mask != pad_mask) {
            nc_dev_ohci_set_pads(mask);
            pad_mask = mask;
        }

        if (!nc_present_frame(&hw)) {
            SDL_Delay(8);               /* nothing to wait for the panel on */
        }
        if (SDL_GetAtomicInt(&g_core_state) < 0) {
            if (g_core.kernel.relaunch) {
                relaunch(argc, argv);
            }
            if (!g_core.kernel.quit) {
                const char *why = nc_hle_fatal();
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "hakuX", why ? why : "The game stopped.", win);
            }
            break;
        }
    }
leave:
    nc_log("hakuX-app", "native core: leaving");
    _exit(0); /* the core thread has no stop yet */
}
