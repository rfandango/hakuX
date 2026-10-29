/*
 * hakuX native core, libretro front end (hakux_libretro.so): RetroArch runs
 * the game.
 *
 * Frames are phase-locked, not stepped: the emulation keeps its own threads
 * (the CPU, the renderer, the APU), and each retro_run is the display's vsync
 * for the NV2A's vblank clock (nc_dev_nv2a_display_vsync), which locks to it
 * as it does to Android's Choreographer. retro_run hands over the newest
 * frame and one period's samples; it never waits for the guest.
 *
 * When RetroArch stops calling (its menu, a stall), the emulation pauses
 * after three periods (nc_pause_set: the CPU parks at its next kernel entry,
 * the devices hold off, the guest's time leaves the pause out), and the next
 * retro_run resumes it.
 *
 * Unloading, a reset, a title's relaunch or RetroArch letting its Vulkan
 * context go stops the core in RetroArch's process (stop_core, M2): the
 * guest, its devices, then all of it freed and reset, so that another game,
 * the reset or the relaunch boots afresh. In PortareOS's build
 * (HAKUX_LR_END_PROCESS) a stop that hangs ends RetroArch's process (the
 * watchdog, wd_start), and HAKUX_LR_STOP=0 (can_stop) brings back the way
 * before M2: the process ends when the game closes (nc_lr_end), and a
 * title's relaunch starts RetroArch again in its place (relaunch).
 */
#include "native/libretro/libretro_int.h"
#include "native/core/core.h"
#include "native/core/irq.h"
#include "native/dev/dev.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HAKUX_VERSION
#define HAKUX_VERSION "dev"
#endif
#ifndef HAKUX_LR_END_PROCESS
#define HAKUX_LR_END_PROCESS 0
#endif

#define GAP_NS (3 * NC_LR_FRAME_NS)   /* no retro_run this long: paused */

static retro_environment_t g_env;
static retro_video_refresh_t g_video;
static retro_audio_sample_batch_t g_audio_batch;
static retro_input_poll_t g_input_poll;
static retro_input_state_t g_input_state;

static NcCore g_core;
static int g_disc_fd = -1;
static int g_core_state;           /* 0 none, 1 booting, 2 running, -1 stopped */
static bool g_started;
static const char *g_fail;         /* nc_lr_fail's: the content stops */
static bool g_shutdown_sent;
static char g_game[PATH_MAX];      /* the content's real path */
static char g_hdd[PATH_MAX], g_cache[PATH_MAX];    /* the save directory's (data_dirs) */
static pthread_t g_core_thread, g_gap_thread;
static int g_gap_quit;             /* stop_core */
static bool g_handlers;            /* on_fault installed (g_old_segv, g_old_bus) */

static void prepare_boot(bool relaunch);
static bool can_stop(void);
static bool boot_again(const char *why, bool relaunch);

static struct {
    pthread_mutex_t mu;
    uint64_t last_run_ns;          /* 0: no retro_run yet */
    bool paused;                   /* by a gap, not by the guest */
} g_gap = {.mu = PTHREAD_MUTEX_INITIALIZER};

static struct sigaction g_old_segv, g_old_bus;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Guest RAM tracking, MMIO and the JIT's call-ret stack are faults the core
 * resolves; any other is passed on to the handler before ours (RetroArch's,
 * another core's), or to the default action as the fault repeats. */
static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    if (__atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) != 0 && nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    const struct sigaction *old = sig == SIGSEGV ? &g_old_segv : &g_old_bus;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(sig, info, ctx);
        return;
    }
    if (!(old->sa_flags & SA_SIGINFO) && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        return;
    }
    struct sigaction dfl = {.sa_handler = SIG_DFL};
    sigaction(sig, &dfl, NULL);
}

bool nc_lr_env(unsigned cmd, void *data)
{
    return g_env && g_env(cmd, data);
}

void nc_lr_fail(const char *why)
{
    nc_log("hakuX-libretro", "failed: %s", why);
    __atomic_store_n(&g_fail, why, __ATOMIC_RELEASE);
}

static void *core_thread(void *arg)
{
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("hakuX-libretro", "core: boot failed");
        __atomic_store_n(&g_core_state, -1, __ATOMIC_RELEASE);
        nc_hle_cpu_leave();
        nc_cpu_thread_done();
        return NULL;
    }
    __atomic_store_n(&g_core_state, 2, __ATOMIC_RELEASE);
    nc_core_run(&g_core);
    nc_hle_cpu_leave();
    nc_log("hakuX-libretro", "core: stopped%s", g_core.kernel.quit ? " (the title left)" : "");
    __atomic_store_n(&g_core_state, -1, __ATOMIC_RELEASE);
    nc_cpu_thread_done();
    return NULL;
}

/* RetroArch stopped calling: pause the emulation until it calls again. */
static void *gap_thread(void *arg)
{
    while (!__atomic_load_n(&g_gap_quit, __ATOMIC_ACQUIRE)) {
        const struct timespec ts = {0, (long)NC_LR_FRAME_NS};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_gap.mu);
        if (g_gap.last_run_ns && !g_gap.paused && now_ns() - g_gap.last_run_ns > GAP_NS &&
            __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) == 2) {
            g_gap.paused = true;
            nc_pause_set(true);
        }
        pthread_mutex_unlock(&g_gap.mu);
    }
    return NULL;
}

static void gap_tick(uint64_t now)
{
    pthread_mutex_lock(&g_gap.mu);
    g_gap.last_run_ns = now;
    if (g_gap.paused) {
        g_gap.paused = false;
        nc_pause_set(false);
    }
    pthread_mutex_unlock(&g_gap.mu);
}

static bool make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

/* The save directory (PortareOS: /storage/saves/xbox): the console's hard
 * disk (hdd/, its partitions) and the renderer's caches (cache/). */
static bool data_dirs(char *hdd, char *cache, size_t n)
{
    const char *root = NULL;
    if (!g_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &root) || !root || !*root) {
        nc_log("hakuX-libretro", "no save directory from the frontend");
        return false;
    }
    snprintf(hdd, n, "%s/hdd", root);
    snprintf(cache, n, "%s/cache", root);
    bool ok = make_dir(root) && make_dir(hdd) && make_dir(cache);
    for (int i = 0; ok && i <= 5; i++) {
        char p[PATH_MAX + 32];
        snprintf(p, sizeof(p), "%s/Partition%d", hdd, i);
        ok = make_dir(p);
    }
    if (!ok) {
        nc_log("hakuX-libretro", "data directories under %s: %s", root, strerror(errno));
    }
    return ok;
}

/* PortareOS's build (HAKUX_LR_END_PROCESS) may end RetroArch's process: when
 * a stop hangs, and with HAKUX_LR_STOP=0 when the game closes.
 * HAKUX_LR_END_PROCESS=0 or 1 in the environment chooses for a run (the
 * tests). */
static bool end_process(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HAKUX_LR_END_PROCESS");
        on = e ? atoi(e) != 0 : HAKUX_LR_END_PROCESS;
    }
    return on;
}

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    g_env = cb;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    nc_lr_options_declare(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
    g_video = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    g_audio_batch = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
    g_input_poll = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
    g_input_state = cb;
}

RETRO_API void retro_init(void)
{
    /* RetroArch's dlclose leaves the library mapped. FEX's allocator
     * (rpmalloc) keeps its spans for the process, its state in the library:
     * unloaded, each load would leave them behind (about 1 GiB of address
     * space); kept, the next load reuses them and finds the statics as the
     * stop reset them. */
    Dl_info self;
    if (dladdr((void *)retro_init, &self) && self.dli_fname) {
        dlopen(self.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    }
    nc_lr_input_reset();    /* the library stays: its ports, not the last session's */
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "hakuX";
    info->library_version = HAKUX_VERSION;
    info->valid_extensions = "iso|chd";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    nc_lr_video_geometry(&info->geometry);
    info->timing.fps = (double)NC_LR_FPS_NUM / (double)NC_LR_FPS_DEN;   /* the Xbox's, exactly */
    info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    nc_lr_input_set_device(port, device);
}

RETRO_API void retro_reset(void)
{
    if (!g_started || !can_stop()) {
        nc_log("hakuX-libretro", "reset: not yet, while the renderer runs (M2's next part)");
        return;
    }
    boot_again("reset", false);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path) {
        return false;
    }
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!g_env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        nc_log("hakuX-libretro", "the frontend has no XRGB8888");
        return false;
    }
    if (!data_dirs(g_hdd, g_cache, sizeof(g_hdd))) {
        return false;
    }
    g_disc_fd = open(game->path, O_RDONLY | O_CLOEXEC);
    if (g_disc_fd < 0) {
        nc_log("hakuX-libretro", "%s: %s", game->path, strerror(errno));
        return false;
    }
    if (!realpath(game->path, g_game)) {
        snprintf(g_game, sizeof(g_game), "%s", game->path);
    }
    /* RetroArch started again for this game's relaunch (relaunch, where the
     * core couldn't stop): boot the XBE it named. */
    bool again = false;
    if (end_process()) {
        const char *e = getenv("HAKUX_RELAUNCH");
        again = e && !strcmp(e, g_game);
        unsetenv("HAKUX_RELAUNCH");
    }
    nc_lr_input_describe(g_env);
    prepare_boot(again);

    /* With Vulkan the guest boots in context_reset, on RetroArch's device. */
    const bool vulkan = nc_lr_vk_negotiate();
    if (__atomic_load_n(&g_fail, __ATOMIC_ACQUIRE)) {
        return false;
    }
    nc_log("hakuX-libretro", "loaded %s; data in %s", game->path, g_hdd);
    return vulkan || nc_lr_start();
}

bool nc_lr_start(void)
{
    if (g_started) {
        return true;
    }
    if (g_disc_fd < 0 && g_game[0]) {
        /* stopped with the game loaded: RetroArch rebuilt its video driver
         * (context_destroy, then context_reset on its new device) */
        static const struct retro_message msg = {"RetroArch rebuilt its video driver: the game starts again",
                                                 300};
        g_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
        return boot_again("video driver rebuilt", false);
    }
    g_started = true;
    __atomic_store_n(&g_core_state, 1, __ATOMIC_RELEASE);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    const int err = pthread_create(&g_core_thread, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);
    if (err) {
        nc_log("hakuX-libretro", "core thread: %s", strerror(err));
        __atomic_store_n(&g_core_state, -1, __ATOMIC_RELEASE);
        g_started = false;
        return false;
    }
    __atomic_store_n(&g_gap_quit, 0, __ATOMIC_RELEASE);
    if (pthread_create(&g_gap_thread, NULL, gap_thread, NULL) != 0) {
        g_gap_thread = 0;
    }
    return true;
}

/* The core's process-wide fault handler, while a game is loaded. */
static void handlers_install(void)
{
    if (g_handlers) {
        return;
    }
    struct sigaction sa = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    g_handlers = true;
}

/* RetroArch's own back, if it is still ours (nobody installed one since). */
static void handlers_restore(void)
{
    if (!g_handlers) {
        return;
    }
    struct sigaction now;
    if (sigaction(SIGSEGV, NULL, &now) == 0 && now.sa_sigaction == on_fault) {
        sigaction(SIGSEGV, &g_old_segv, NULL);
    }
    if (sigaction(SIGBUS, NULL, &now) == 0 && now.sa_sigaction == on_fault) {
        sigaction(SIGBUS, &g_old_bus, NULL);
    }
    g_handlers = false;
}

/* What each boot is given before the guest runs: the data folders, the
 * renderer's mode, the options (read again: a reset or a relaunch takes a
 * changed one), the vblank's period, a relaunch. */
static void prepare_boot(bool relaunch)
{
    nc_core_set_relaunch(relaunch);
    nc_io_set_hdd_root(g_hdd);
    nc_hle_set_mode("replace");
    nc_hle_set_base_path(g_cache);
    nc_lr_options_apply();                          /* the render scale, the console's settings */
    nc_dev_nv2a_display_period(NC_LR_FRAME_NS);    /* retro_run's: the vblank locks one to one */
    nc_hle_set_pipeline_options(true, true);   /* the app's defaults */
    handlers_install();
}

/* The core stops in RetroArch's process (M2). In PortareOS's build
 * HAKUX_LR_STOP=0 asks for the way before it, should a stop misbehave
 * somewhere: the process ends instead (nc_lr_end). */
static bool can_stop(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HAKUX_LR_STOP");
        on = !(end_process() && e && !strcmp(e, "0"));
    }
    return on;
}

/* PortareOS's build: a stop that takes longer than this ends RetroArch's
 * process, as a closed game did before M2, rather than leave the handheld
 * frozen; elsewhere unloading waits.
 * HAKUX_LR_STOP_TIMEOUT_MS sets it (the tests). */
#define STOP_TIMEOUT_MS 10000u

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;              /* CLOCK_MONOTONIC's */
    bool done;
    const char *why, *stage;        /* the stop's, and what it waits for */
    unsigned ms;
    uint64_t deadline_ns;
    pthread_t th;
} g_wd = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void *wd_thread(void *arg)
{
    const struct timespec until = {(time_t)(g_wd.deadline_ns / 1000000000ull),
                                   (long)(g_wd.deadline_ns % 1000000000ull)};
    pthread_mutex_lock(&g_wd.mu);
    while (!g_wd.done) {
        if (pthread_cond_timedwait(&g_wd.cv, &g_wd.mu, &until) == ETIMEDOUT && !g_wd.done) {
            nc_log("hakuX-libretro", "%s: the core did not stop in %u ms (%s): ending the process", g_wd.why,
                   g_wd.ms, g_wd.stage);
            fflush(NULL);
            _exit(0);
        }
    }
    pthread_mutex_unlock(&g_wd.mu);
    return arg;
}

/* The watchdog over a stop from t0 (true: started, wd_end ends it). */
static bool wd_start(const char *why, uint64_t t0)
{
    if (!end_process()) {
        return false;
    }
    const char *e = getenv("HAKUX_LR_STOP_TIMEOUT_MS");
    g_wd.ms = e && atoi(e) > 0 ? (unsigned)atoi(e) : STOP_TIMEOUT_MS;
    g_wd.deadline_ns = t0 + g_wd.ms * 1000000ull;
    g_wd.why = why;
    g_wd.stage = "starting";
    g_wd.done = false;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_wd.cv, &ca);
    pthread_condattr_destroy(&ca);
    const int err = pthread_create(&g_wd.th, NULL, wd_thread, NULL);
    if (err) {
        nc_log("hakuX-libretro", "%s: no watchdog over the stop: %s", why, strerror(err));
        pthread_cond_destroy(&g_wd.cv);
        return false;
    }
    return true;
}

static void wd_stage(const char *stage)
{
    pthread_mutex_lock(&g_wd.mu);
    g_wd.stage = stage;
    pthread_mutex_unlock(&g_wd.mu);
}

static void wd_end(void)
{
    pthread_mutex_lock(&g_wd.mu);
    g_wd.done = true;
    pthread_cond_signal(&g_wd.cv);
    pthread_mutex_unlock(&g_wd.mu);
    pthread_join(g_wd.th, NULL);
    pthread_cond_destroy(&g_wd.cv);
}

/* M2: the guest stopped (its CPU, at its next block start, idle or paused),
 * its devices and I/O, then everything the boot made freed and the core's
 * state reset (nc_core_destroy), the fault handler given back: the next
 * nc_lr_start boots afresh. RetroArch's thread. */
static void stop_core(const char *why)
{
    if (!g_started) {
        return;
    }
    const uint64_t t0 = now_ns();
    const bool wd = wd_start(why, t0);
    wd_stage("the guest CPU");
    nc_core_request_stop(&g_core);
    pthread_join(g_core_thread, NULL);
    wd_stage("the frame gap's thread");
    __atomic_store_n(&g_gap_quit, 1, __ATOMIC_RELEASE);
    if (g_gap_thread) {
        pthread_join(g_gap_thread, NULL);
        g_gap_thread = 0;
    }
    wd_stage("the devices and the renderer");
    nc_core_halt(&g_core);
    wd_stage("freeing the console");
    nc_core_destroy(&g_core);
#ifdef __GLIBC__
    /* What the boot's threads freed back to the kernel: glibc gave them arenas
     * of their own, which keep it, and the next boot's threads get others. */
    malloc_trim(0);
#endif
    if (wd) {
        wd_end();
    }
    handlers_restore();
    g_disc_fd = -1;                 /* the core's: closed with the disc */
    __atomic_store_n(&g_core_state, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fail, NULL, __ATOMIC_RELEASE);
    g_started = false;
    g_shutdown_sent = false;
    pthread_mutex_lock(&g_gap.mu);
    g_gap.last_run_ns = 0;
    g_gap.paused = false;
    pthread_mutex_unlock(&g_gap.mu);
    nc_log("hakuX-libretro", "%s: the core stopped in %.0f ms", why, (double)(now_ns() - t0) / 1e6);
}

/* Stopped, then the same game booted again in the process: a reset, or a
 * title's relaunch into the XBE its launch data page names. */
static bool boot_again(const char *why, bool relaunch)
{
    stop_core(why);
    g_disc_fd = open(g_game, O_RDONLY | O_CLOEXEC);
    if (g_disc_fd < 0) {
        nc_log("hakuX-libretro", "%s: %s: %s", why, g_game, strerror(errno));
        nc_lr_fail("the game's file can't be opened again");
        return false;
    }
    prepare_boot(relaunch);
    return nc_lr_start();
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    return false;
}

/* Every descriptor but stdin, stdout and stderr closes at the exec, as at an
 * exit: the display, the input devices, the audio server. */
static void close_on_exec(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        return;
    }
    for (const struct dirent *e; (e = readdir(d));) {
        const int fd = atoi(e->d_name);
        const int flags = fd > 2 && fd != dirfd(d) ? fcntl(fd, F_GETFD) : -1;
        if (flags >= 0) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    closedir(d);
}

/* A title's XLaunchNewImage (kernel.relaunch; the launch data page is in
 * relaunch.bin, beside the console disk): the core cannot boot again in this
 * process, so RetroArch starts again in its place, on the same command line,
 * with HAKUX_RELAUNCH naming the game. Its load boots the XBE the page names,
 * as Android's activity does with "relaunch". The process stays the same one
 * to its parent (runemu.sh), so the game does not go back to the launcher.
 * Returns if RetroArch can't start again for the game. */
static void relaunch(void)
{
    static char cmd[64 * 1024];
    static char *argv[1024];
    static char mark[PATH_MAX + 32];
    extern char **environ;
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    size_t n = 0;
    if (fd >= 0) {
        ssize_t r;
        while (n < sizeof(cmd) - 1 && (r = read(fd, cmd + n, sizeof(cmd) - 1 - n)) > 0) {
            n += (size_t)r;
        }
        close(fd);
    }
    unsigned argc = 0;
    bool on_game = false;
    for (size_t i = 0; i < n && argc < sizeof(argv) / sizeof(*argv) - 1; i += strlen(cmd + i) + 1) {
        char real[PATH_MAX];
        argv[argc++] = cmd + i;
        on_game = on_game || (realpath(cmd + i, real) && !strcmp(real, g_game));
    }
    argv[argc] = NULL;
    if (!on_game) {
        nc_log("hakuX-libretro", "relaunch: RetroArch was not started on the game; it can't start again for it");
        return;
    }
    size_t ne = 0;
    while (environ && environ[ne]) {
        ne++;
    }
    char **envp = calloc(ne + 2, sizeof(*envp));
    if (!envp) {
        return;
    }
    size_t m = 0;
    for (size_t i = 0; i < ne; i++) {
        if (strncmp(environ[i], "HAKUX_RELAUNCH=", 15)) {
            envp[m++] = environ[i];
        }
    }
    snprintf(mark, sizeof(mark), "HAKUX_RELAUNCH=%s", g_game);
    envp[m] = mark;
    nc_log("hakuX-libretro", "relaunch: starting RetroArch again for %s", g_game);
    fflush(NULL);
    close_on_exec();
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    execve("/proc/self/exe", argv, envp);
    nc_log("hakuX-libretro", "relaunch: RetroArch can't start again: %s", strerror(errno));
    free(envp);
}

void nc_lr_end(const char *why)
{
    const int state = __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE);
    if (state == 0) {
        return;
    }
    if (can_stop()) {
        stop_core(why);
        return;
    }
    if (state < 0 && g_core.kernel.relaunch) {
        relaunch();
    }
    nc_log("hakuX-libretro", "%s: the core does not stop (HAKUX_LR_STOP=0); ending the process", why);
    fflush(NULL);
    _exit(0);
}

RETRO_API void retro_unload_game(void)
{
    nc_lr_end("unload");
    if (g_disc_fd >= 0) {           /* loaded, never booted (no context_reset came) */
        close(g_disc_fd);
        g_disc_fd = -1;
    }
    handlers_restore();
    nc_lr_video_reset();
    nc_lr_vk_reset();
    g_game[0] = 0;
}

/* HAKUX_LR_STATS=1: every 10 s, how long retro_run takes RetroArch's thread
 * (avg, max) and its video part. */
static void run_stat(uint64_t t0, uint64_t tv, uint64_t t1)
{
    static int on = -1;
    static uint64_t start, n, run_sum, run_max, vid_sum, vid_max;
    if (on < 0) {
        const char *e = getenv("HAKUX_LR_STATS");
        on = e && atoi(e) != 0;
    }
    if (!on) {
        return;
    }
    start = start ? start : t0;
    n++;
    run_sum += t1 - t0;
    run_max = t1 - t0 > run_max ? t1 - t0 : run_max;
    vid_sum += t1 - tv;
    vid_max = t1 - tv > vid_max ? t1 - tv : vid_max;
    if (t1 - start >= 10000000000ull) {
        nc_log("hakuX-libretro", "stats: %.2f retro_run/s, %.2f ms each (max %.2f), video %.2f ms (max %.2f)",
               (double)n * 1e9 / (double)(t1 - start), (double)run_sum / (double)n / 1e6, (double)run_max / 1e6,
               (double)vid_sum / (double)n / 1e6, (double)vid_max / 1e6);
        start = t1;
        n = run_sum = run_max = vid_sum = vid_max = 0;
    }
}

RETRO_API void retro_run(void)
{
    const uint64_t now = now_ns();
    nc_lr_input_run(g_input_poll, g_input_state);
    nc_dev_nv2a_display_vsync(now);         /* RetroArch's frame tick is the display's vsync */
    gap_tick(now);
    nc_lr_audio_run(g_audio_batch);
    const uint64_t tv = now_ns();
    nc_lr_video_run(g_video);
    run_stat(now, tv, now_ns());
    nc_lr_options_run();

    /* A title's relaunch (XLaunchNewImage): the same game boots again, into
     * the XBE its launch data page names, in the process. */
    if (__atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) < 0 && g_core.kernel.relaunch && can_stop() &&
        !__atomic_load_n(&g_fail, __ATOMIC_ACQUIRE)) {
        nc_log("hakuX-libretro", "relaunch: the game boots again in RetroArch's process");
        boot_again("relaunch", true);
        return;
    }
    const char *why = __atomic_load_n(&g_fail, __ATOMIC_ACQUIRE);
    why = why ? why : __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) < 0 ? "the core stopped" : nc_hle_fatal();
    if (why && !g_shutdown_sent) {
        g_shutdown_sent = true;
        nc_log("hakuX-libretro", "shutting down: %s", why);
        g_env(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

RETRO_API unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

RETRO_API size_t retro_serialize_size(void)
{
    return 0;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    return false;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
    return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    return 0;
}
