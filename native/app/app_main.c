/*
 * hakuX native core, app front end — SDL_main for NativeCoreActivity.
 *
 *   argv[1]  the disc image: a path, or "fd:<n>" for a descriptor the activity opened
 *   argv[2]  the hard disk folder (Partition0-5 and the raw sector files)
 *   argv[3+] options: the core's (native/core/options.h: "uncap",
 *            "loadstate=", "scale=", "video=", "asyncpipe=", "mods=", ...),
 *            and the window's: "aspect=auto|4:3|16:9|stretch" the picture's
 *            shape, "fxaa=0|1" FXAA over each frame and
 *            "upscale=bilinear|sgsr" how a frame smaller than its place on
 *            screen is scaled up; "padinput=sdl" the legacy input (the first
 *            SDL gamepad as player 1); "vkdriver=" the custom Vulkan driver
 *            (vkdriver.c)
 *
 * The core boots and runs on its own thread (FEX keeps per-thread state, so
 * boot and run share it). This thread owns the window: it hands the
 * controllers' states (the activity's, by player; the on-screen controls
 * added to player 1) to the core, and presents the HLE GPU's frames once
 * per display refresh.
 */
#include "native/app/app.h"
#include "native/core/cheats.h"
#include "native/core/core.h"
#include "native/core/mods.h"
#include "native/core/options.h"
#include "native/dev/usb.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"
#include "native/state/save.h"
#include "native/hle/hle_native.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#include <sys/syscall.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static NcCore g_core;
static int g_disc_fd = -1;
static SDL_AtomicInt g_core_state; /* 0 booting, 1 running, -1 failed */

/* The on-screen controller's state (NativeCoreActivity.nativeSetOverlayPad). */
static pthread_mutex_t g_overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static NcPadState g_overlay;

/* The physical controllers' states, by player (NativeCoreActivity's
 * controller router: each device's events through its mapping); the
 * overlay adds to player 1's. "padinput=sdl" (the app's legacy input) reads
 * the first SDL gamepad into player 1 instead, as before the router. */
static NcPadState g_phys[4];
static bool g_sdl_pads;

/* The overlay o over the controller state s: buttons and analog buttons
 * either, each stick the one pushed furthest. */
static void merge_pad(NcPadState *s, const NcPadState *o)
{
    s->buttons |= o->buttons;
    for (unsigned i = 0; i < 8; i++) {
        if (o->analog[i] > s->analog[i]) {
            s->analog[i] = o->analog[i];
        }
    }
    if (abs(o->lx) + abs(o->ly) > abs(s->lx) + abs(s->ly)) {
        s->lx = o->lx;
        s->ly = o->ly;
    }
    if (abs(o->rx) + abs(o->ry) > abs(s->rx) + abs(s->ry)) {
        s->rx = o->rx;
        s->ry = o->ry;
    }
}

/* Router input goes to the pad as it comes, not at the SDL loop's next pass
 * (a refresh later). Under g_overlay_lock: two setters publish in the order
 * they stored. */
static void publish_pad(unsigned player)
{
    NcPadState s = g_phys[player];
    if (player == 0) {
        merge_pad(&s, &g_overlay);
    }
    nc_pad_set(player, &s);
}

EXPORT JNIEXPORT void JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeSetOverlayPad(
    JNIEnv *env, jclass cls, jint buttons, jint analog_mask, jfloat lx, jfloat ly, jfloat rx, jfloat ry,
    jfloat lt, jfloat rt)
{
    NcPadState s = {0};
    s.buttons = (uint16_t)buttons;
    for (unsigned i = 0; i < 6; i++) {
        s.analog[i] = analog_mask & (1 << i) ? 0xFF : 0;
    }
    s.analog[6] = (uint8_t)(lt * 255.0f);
    s.analog[7] = (uint8_t)(rt * 255.0f);
    s.lx = (int16_t)(lx * 32767.0f);
    s.ly = (int16_t)(-ly * 32767.0f); /* screen down is stick down */
    s.rx = (int16_t)(rx * 32767.0f);
    s.ry = (int16_t)(-ry * 32767.0f);
    pthread_mutex_lock(&g_overlay_lock);
    g_overlay = s;
    if (!g_sdl_pads) {
        publish_pad(0);
    }
    pthread_mutex_unlock(&g_overlay_lock);
}

/* The players with a controller (bit n: player n + 1): plugged in and pulled
 * out as the activity's controllers come and go (native/dev/ohci.c). */
EXPORT JNIEXPORT void JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeSetPads(JNIEnv *env, jclass cls,
                                                                                       jint mask)
{
    nc_dev_ohci_set_pads((unsigned)mask & 0xF);
}

EXPORT JNIEXPORT void JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeSetPad(
    JNIEnv *env, jclass cls, jint player, jint buttons, jlong analog, jint lx, jint ly, jint rx, jint ry)
{
    if (player < 0 || player >= 4) {
        return;
    }
    NcPadState s = {0};
    s.buttons = (uint16_t)buttons;
    for (unsigned i = 0; i < 8; i++) {
        s.analog[i] = (uint8_t)((uint64_t)analog >> (8 * i));
    }
    s.lx = (int16_t)lx;
    s.ly = (int16_t)ly;
    s.rx = (int16_t)rx;
    s.ry = (int16_t)ry;
    pthread_mutex_lock(&g_overlay_lock);
    g_phys[player] = s;
    if (!g_sdl_pads) {
        publish_pad((unsigned)player);
    }
    pthread_mutex_unlock(&g_overlay_lock);
}

/* native/dev/apu/out.c: the output stream paused with the emulation */
void nc_audio_set_paused(bool on);

/* NativeCoreActivity: the emulation pauses while the activity is paused (the
 * device suspends, another app in front) or its menu is open, and resumes
 * where it stopped, its sound output with it. */
EXPORT JNIEXPORT void JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeSetPaused(JNIEnv *env,
                                                                                         jclass cls, jboolean on)
{
    nc_kernel_set_paused(&g_core.kernel, on != JNI_FALSE);
    nc_audio_set_paused(on != JNI_FALSE);
}

/* Savestates (native/state): the running game saved to path, on the
 * caller's thread (not the UI's: a frame and the write). Null: saved; else
 * why not. */
EXPORT JNIEXPORT jstring JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeSaveState(JNIEnv *env,
                                                                                         jclass cls, jstring jpath)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1) {
        return (*env)->NewStringUTF(env, "the game isn't running");
    }
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    const char *err = nc_state_save(path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return err ? (*env)->NewStringUTF(env, err) : NULL;
}

/* debug: a new value of debug.hakux.state ("save:<slot>", "load:<slot>")
 * for the activity to act on as its menu would (scripted savestate runs);
 * the value found at the first call, "none" and "" do nothing. Null: none. */
EXPORT JNIEXPORT jstring JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeTakeStateRequest(JNIEnv *env,
                                                                                                jclass cls)
{
    static char last[PROP_VALUE_MAX];
    static bool primed;
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.state", v);
    if (!primed || !strcmp(v, last)) {
        primed = true;
        snprintf(last, sizeof(last), "%s", v);
        return NULL;
    }
    snprintf(last, sizeof(last), "%s", v);
    if (!v[0] || !strcmp(v, "none")) {
        return NULL;
    }
    nc_log("hakuX-app", "debug.hakux.state: %s", v);
    return (*env)->NewStringUTF(env, v);
}

/* hw/xbox/hle/pb/pbmode.h: the title runs in push-buffer mode */
bool hle_pb_active(void);

/* Savestates work for the running game: push-buffer mode only. */
EXPORT JNIEXPORT jboolean JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeStatesAvailable(JNIEnv *env,
                                                                                                jclass cls)
{
    return SDL_GetAtomicInt(&g_core_state) == 1 && hle_pb_active();
}

/* Whether this core can load the state at path into the running game:
 * null, or why not (another game, another disc, a section this core can't
 * read: the state's own core named). */
EXPORT JNIEXPORT jstring JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeStateCheck(JNIEnv *env,
                                                                                          jclass cls, jstring jpath)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    const char *err = nc_state_compat(path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return err ? (*env)->NewStringUTF(env, err) : NULL;
}

/* A cheat of the run's manifest (its index there) turned on or off: from the
 * next vblank (native/core/cheats.h). */
EXPORT JNIEXPORT void JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeSetCheat(JNIEnv *env, jclass cls,
                                                                                        jint index, jboolean on)
{
    nc_cheat_set(index, on);
}

/* The rumble the title last sent to that player: left << 16 | right. */
EXPORT JNIEXPORT jint JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetRumble(JNIEnv *env,
                                                                                         jclass cls, jint player)
{
    uint16_t l = 0, r = 0;
    if (player >= 0 && player < 4) {
        nc_pad_rumble((unsigned)player, &l, &r);
    }
    return (jint)((uint32_t)l << 16 | r);
}

/* SIGSEGV/SIGBUS: the core's own faults first (MMIO, dirty tracking, the
 * call-ret stack, a title's pager), anything else to whoever handled them
 * before (ART's handlers, then the crash reporter).
 *
 * The handler goes to the kernel directly (rt_sigaction), in front of ART's
 * signal chain, which would take every guest device-register access first
 * and add two sigprocmask calls to each (a tenth of the guest thread with
 * DirectSound at work). Faults that aren't the core's go to the handler the
 * kernel had, ART's chain, as the kernel would have called it. */
struct k_sigaction {
    void (*handler)(int, siginfo_t *, void *);
    unsigned long flags;
    void (*restorer)(void);
    uint64_t mask;
};
static struct k_sigaction g_old_segv, g_old_bus;

static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    if (nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    struct k_sigaction *old = sig == SIGBUS ? &g_old_bus : &g_old_segv;
    void (*h)(int, siginfo_t *, void *) = old->handler;
    if (h == (void *)SIG_DFL || h == (void *)SIG_IGN) {
        struct k_sigaction dfl = {0};
        syscall(__NR_rt_sigaction, sig, &dfl, NULL, sizeof(uint64_t)); /* the fault repeats: default action */
        return;
    }
    if (old->flags & SA_SIGINFO) {
        h(sig, info, ctx);
    } else {
        ((void (*)(int))(void *)h)(sig);
    }
}

static void install_fault_handler(void)
{
    struct k_sigaction sa = {0};
    sa.handler = on_fault;
    sa.flags = SA_SIGINFO | SA_ONSTACK;
    syscall(__NR_rt_sigaction, SIGSEGV, &sa, &g_old_segv, sizeof(uint64_t));
    syscall(__NR_rt_sigaction, SIGBUS, &sa, &g_old_bus, sizeof(uint64_t));
}

/* A title frame has been on screen: the relaunch's held frame can go (NativeCoreActivity). */
EXPORT JNIEXPORT jboolean JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetFrameShown(JNIEnv *env,
                                                                                                jclass cls)
{
    return nc_present_shown() ? JNI_TRUE : JNI_FALSE;
}

/* The FPS overlay (NativeCoreActivity): the title's frame rate and pacing,
 * and 1 when it renders by push-buffer decoding (else 0: D3D's hooks). */
EXPORT JNIEXPORT jfloatArray JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetFrameStats(JNIEnv *env,
                                                                                                    jclass cls)
{
    float v[4];
    nc_present_stats(&v[0], &v[1], &v[2]);
    v[3] = nc_hle_pb_active() ? 1.0f : 0.0f;
    jfloatArray a = (*env)->NewFloatArray(env, 4);
    if (a) {
        (*env)->SetFloatArrayRegion(env, a, 0, 4, v);
    }
    return a;
}

/* NativeCoreActivity: why the game can't run here (the GPU driver lacks
 * what the HLE GPU needs), or null. */
const char *nc_hle_fatal(void);

EXPORT JNIEXPORT jstring JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetFatal(JNIEnv *env,
                                                                                         jclass cls)
{
    const char *why = nc_hle_fatal();
    return why ? (*env)->NewStringUTF(env, why) : NULL;
}

/* Whose fault that is: 1 the GPU driver's, 2 the game's (the HLE GPU does
 * not support it), 3 a savestate's, 4 the GPU driver lost the device while
 * the game ran, 0 none (native/hle/hle_native.h NC_FATAL_*). */
int nc_hle_fatal_kind(void);

EXPORT JNIEXPORT jint JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetFatalKind(JNIEnv *env,
                                                                                          jclass cls)
{
    return nc_hle_fatal_kind();
}

/* The HLE GPU's stutter and hang monitor for the overlay, under the FPS line
 * (as the xemu-based app shows it): counts, the ongoing or last hang, the
 * last stutter; "" with nothing to show. */
bool hle_hitch_overlay(char *buf, size_t n);

EXPORT JNIEXPORT jstring JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetHleHitches(JNIEnv *env,
                                                                                              jclass cls)
{
    char buf[512] = "";
    if (!hle_hitch_overlay(buf, sizeof(buf))) {
        buf[0] = 0;
    }
    return (*env)->NewStringUTF(env, buf);
}

/* The window's overlays and the game's GPU work (NativeCoreActivity's
 * OverlayPacer): how long the GPU has been on the game's work without
 * running out of it, in ms (0: it's out now), and a moment of idle GPU for a
 * frame of the window waiting on the GPU queue below the game's. Only where
 * the game's queue is above the window's (the driver's kgsl contexts raised
 * above the default 8: Turnip, vkdriver.c); elsewhere the window's frames
 * take turns with the game's, never busy, never a gap. */
int64_t hle_tl_busy_ns(void);
void hle_tl_gap(void);

static bool above_window(void)
{
    const int prio = nc_vk_driver_kgsl_priority();
    return prio > 0 && prio < 8;
}

EXPORT JNIEXPORT jint JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGpuBusyMs(JNIEnv *env, jclass cls)
{
    if (!above_window()) {
        return 0;
    }
    const int64_t ms = hle_tl_busy_ns() / 1000000;
    return (jint)(ms > INT32_MAX ? INT32_MAX : ms);
}

EXPORT JNIEXPORT void JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGpuGap(JNIEnv *env, jclass cls)
{
    if (above_window()) {
        hle_tl_gap();
    }
}

/* The start screen's "Building shaders n / N" line: the pipeline prebuild's
 * progress, {built, to build} ({0, 0} when none runs). */
EXPORT JNIEXPORT jintArray JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetShaderBuild(JNIEnv *env,
                                                                                                jclass cls)
{
    uint32_t done, total;
    nc_hle_pipeline_progress(&done, &total);
    jint v[2] = { (jint)done, (jint)total };
    jintArray a = (*env)->NewIntArray(env, 2);
    if (a) {
        (*env)->SetIntArrayRegion(env, a, 0, 2, v);
    }
    return a;
}

/* NativeCoreActivity: the core stopped for a quick reboot into another XBE
 * (XLaunchNewImage); the activity starts it again with "relaunch". */
EXPORT JNIEXPORT jboolean JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetRelaunch(JNIEnv *env,
                                                                                            jclass cls)
{
    return SDL_GetAtomicInt(&g_core_state) == -1 && g_core.kernel.relaunch;
}

/* NativeCoreActivity: a console on the network with this one's identity (the
 * hard disk copied from device to device), its address; once each, null when
 * no new one (native/net/net.h nc_net_take_twin). */
EXPORT JNIEXPORT jstring JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeTakeTwinConsole(JNIEnv *env,
                                                                                               jclass cls)
{
    char addr[64];
    return nc_net_take_twin(addr, sizeof(addr)) ? (*env)->NewStringUTF(env, addr) : NULL;
}

/* NativeCoreActivity: the core stopped as the title left (to the dashboard, a
 * reboot, a shutdown); the activity closes the game. */
EXPORT JNIEXPORT jboolean JNICALL Java_com_rfandango_haku_1x_NativeCoreActivity_nativeGetQuit(JNIEnv *env,
                                                                                        jclass cls)
{
    return SDL_GetAtomicInt(&g_core_state) == -1 && g_core.kernel.quit;
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

static void make_hdd(const char *root)
{
    char p[512];
    mkdir(root, 0775);
    for (int i = 0; i <= 5; i++) {
        snprintf(p, sizeof(p), "%s/Partition%d", root, i);
        mkdir(p, 0775);
    }
}

EXPORT int SDL_main(int argc, char *argv[])
{
    if (argc < 3) {
        nc_log("hakuX-app", "usage: <disc path | fd:N> <hdd folder>");
        return 1;
    }
    const char *disc = argv[1];
    g_disc_fd = !strncmp(disc, "fd:", 3) ? atoi(disc + 3) : open(disc, O_RDONLY | O_CLOEXEC);
    if (g_disc_fd < 0) {
        /* the boot says so (NC_FATAL_BOOT), and the activity goes back */
        nc_log("hakuX-app", "can't open the disc %s", disc);
    }
    /* debug: debug.hakux.hdd=NAME runs on the hard disk <hdd>-NAME instead,
     * made empty the first time: a title's first boot (its cache partition
     * formatted and filled) without touching the usual disk. Unset or
     * "none": the usual one. */
    char hdd[512];
    snprintf(hdd, sizeof(hdd), "%s", argv[2]);
    {
        char name[PROP_VALUE_MAX] = "";
        if (hle_prop_get("debug.hakux.hdd", name) > 0 && strcmp(name, "none") && !strchr(name, '/') &&
            name[0] != '.') {
            snprintf(hdd, sizeof(hdd), "%s-%s", argv[2], name);
            nc_log("hakuX-app", "debug.hakux.hdd: the hard disk is %s", hdd);
        }
    }
    make_hdd(hdd);
    nc_io_set_hdd_root(hdd);
    setenv("HAKUX_HLE_MODE", "replace", 0);
    /* The HLE GPU's shader and pipeline caches beside the hard disk, in the
     * app's own storage: the default (/data/local/tmp, xboxrun's) is the
     * shell's, where the app can't keep its SPIR-V cache, and every run
     * compiled every shader again (half-second hitches in new places). */
    {
        char base[512];
        snprintf(base, sizeof(base), "%s/../hle", argv[2]);
        setenv("HAKUX_NATIVE_BASE", base, 1);
    }
    /* debug: debug.hakux.env="NAME=value;NAME=value" sets the runner's
     * environment switches (NC_TRACE_*, HAKUX_HLE_*) for the next game, and
     * debug.hakux.env2 more of them (a property holds 91 characters) */
    for (unsigned p = 0; p < 2; p++) {
        char env[PROP_VALUE_MAX] = "";
        if (hle_prop_get(p ? "debug.hakux.env2" : "debug.hakux.env", env) > 0) {
            for (char *save = NULL, *t = strtok_r(env, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
                char *eq = strchr(t, '=');
                if (eq && eq != t) {
                    *eq = 0;
                    setenv(t, eq + 1, 1);
                    nc_log("hakuX-app", "debug.hakux.env: %s=%s", t, eq + 1);
                }
            }
        }
    }
    NcOptions opt;
    nc_options_init(&opt);
    for (int i = 3; i < argc; i++) {
        if (nc_option(&opt, argv[i])) {
            continue;                           /* the core's (native/core/options.h) */
        }
        if (!strcmp(argv[i], "padinput=sdl")) {
            g_sdl_pads = true;
        }
        if (!strncmp(argv[i], "aspect=", 7)) {
            const char *a = argv[i] + 7;
            nc_present_set_aspect(!strcmp(a, "4:3")       ? NC_ASPECT_4_3
                                  : !strcmp(a, "16:9")    ? NC_ASPECT_16_9
                                  : !strcmp(a, "stretch") ? NC_ASPECT_STRETCH
                                                          : NC_ASPECT_AUTO);
            nc_log("hakuX-app", "aspect ratio %s", a);
        }
        if (!strncmp(argv[i], "fxaa=", 5)) {
            nc_present_set_fxaa(atoi(argv[i] + 5) != 0);
        }
        if (!strncmp(argv[i], "upscale=", 8)) {
            nc_present_set_upscale(!strcmp(argv[i] + 8, "sgsr") ? NC_UPSCALE_SGSR : NC_UPSCALE_BILINEAR);
        }
        /* vkdriver=<driver dir>|<library>|<hook library dir>: the custom
         * Vulkan driver chosen in the settings (Turnip), via adrenotools. */
        if (!strncmp(argv[i], "vkdriver=", 9)) {
            nc_vk_driver_load(argv[i] + 9);
        }
    }
    nc_options_apply(&opt);
    nc_present_set_widescreen(opt.video >= 0 && (opt.video & NC_VIDEO_WIDESCREEN));
    nc_log("hakuX-app", "native core: disc %s, hdd %s%s", disc, argv[2], opt.uncap ? ", frame rate uncapped" : "");

    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* the gamepads are the activity's (its controller router), unless the
     * legacy SDL input was asked for: SDL then doesn't open them either */
    if (!SDL_Init(SDL_INIT_VIDEO | (g_sdl_pads ? SDL_INIT_GAMEPAD : 0))) {
        nc_log("hakuX-app", "SDL_Init: %s", SDL_GetError());
        return 1;
    }
    /* no GL or Vulkan flag: the HLE GPU makes its own Vulkan surface of the
     * window's ANativeWindow (hw/xbox/hle/vk/swapchain.c) */
    SDL_Window *win = SDL_CreateWindow("hakuX", 0, 0, SDL_WINDOW_FULLSCREEN);
    if (!win) {
        nc_log("hakuX-app", "window: %s", SDL_GetError());
        return 1;
    }
    if (!nc_present_init()) {
        return 1;
    }
    nc_vsync_start();

    install_fault_handler();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t core;
    pthread_create(&core, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);

    SDL_Gamepad *pad = NULL;
    uint16_t rumble_l = 0, rumble_r = 0;
    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_AC_BACK) {
                    quit = true;
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) {
                    pad = SDL_OpenGamepad(ev.gdevice.which);
                    nc_log("hakuX-app", "gamepad: %s", pad ? SDL_GetGamepadName(pad) : SDL_GetError());
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == ev.gdevice.which) {
                    SDL_CloseGamepad(pad);
                    pad = NULL;
                }
                break;
            default:
                break;
            }
        }

        if (g_sdl_pads) {
            NcPadState s;
            pthread_mutex_lock(&g_overlay_lock);
            s = g_overlay;
            pthread_mutex_unlock(&g_overlay_lock);
            if (pad) {
                nc_read_gamepad(pad, &s);
            }
            nc_pad_set(0, &s);
            uint16_t l = 0, r = 0;
            nc_pad_rumble(0, &l, &r);
            if (pad && (l != rumble_l || r != rumble_r)) {
                SDL_RumbleGamepad(pad, l, r, l || r ? 1000 : 0);
                rumble_l = l;
                rumble_r = r;
            }
        }
        /* else the activity's router sets each player's pad as its input
         * comes (publish_pad), and takes rumble back (nativeGetRumble) */

        /* the activity's surface: replaced when it is made again, NULL while
         * it's gone */
        struct ANativeWindow *nw =
            SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
        if (!nc_present_frame(nw)) {
            SDL_Delay(8);               /* nothing to wait for the panel on */
        }
        if (SDL_GetAtomicInt(&g_core_state) < 0) {
            SDL_Delay(16);
        }
    }
    nc_log("hakuX-app", "native core: leaving");
    _exit(0); /* the core thread has no stop yet; the activity's process ends with it */
}
