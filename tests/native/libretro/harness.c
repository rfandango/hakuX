/*
 * hakuX libretro core — a minimal frontend for its checks (check.py).
 *
 *   libretro-harness <core.so> [<disc image> <frames> <save directory>]
 *
 * Loads the core as RetroArch does (dlopen, every retro_* symbol), reports
 * its system and AV info, and with a disc loads it and calls retro_run at
 * the core's rate, checking that each call hands over exactly one video
 * frame. Core options are the core's defaults, or HARNESS_OPTIONS's
 * ("key=value;..."), and from the 30th call HARNESS_OPTIONS_LATER's, as
 * if changed in RetroArch's menu; RetroArch's language is HARNESS_LANGUAGE's
 * (RETRO_LANGUAGE_*, English without it). It stops after <frames>, or when
 * the core asks to shut down; the summary is printed before
 * retro_unload_game (which with HAKUX_LR_STOP=0 in PortareOS's build ends
 * the process, or after a title's relaunch starts the harness again, as it
 * would RetroArch). HARNESS_LOAD_AGAIN: the core is opened again after it and
 * a second game loaded, as RetroArch would.
 *
 * HARNESS_VULKAN=1: the core renders on a Vulkan device the harness's
 * frontend (vk.c) negotiates with it, as RetroArch does, without a window.
 * HARNESS_RESET_AT=N: retro_reset before the Nth retro_run.
 * HARNESS_REINIT_AT=N (with HARNESS_VULKAN): before the Nth, the video
 * driver rebuilt as RetroArch does it uncached: context_destroy, another
 * device negotiated, context_reset.
 * HARNESS_PORTS=d1,d2,...: each port's device (RETRO_DEVICE_*: 1 a
 * controller, 0 none) set as RetroArch sets them, after the video driver
 * started (context_reset); HARNESS_PORTS_AT="N:d1,d2,...;M:..." sets them
 * again before the Nth (Mth) retro_run, as RetroArch's menu does.
 * HARNESS_PRESS="port:id:from:to,...": the RetroPad button id (0-based port,
 * RETRO_DEVICE_ID_JOYPAD_*) held from the from'th retro_run until the to'th.
 * HARNESS_CYCLES=N: the game loaded, run and unloaded N times, the core
 * closed and opened again between, as RetroArch does between games; after
 * each, the core closed, the threads, descriptors, memory and signal
 * handlers left are printed, and whether the library is still mapped. HARNESS_MAPS=<path>
 * (debug): after each cycle the mappings to <path>.<cycle>, and glibc's heap
 * arenas to stderr.
 */
#include "native/libretro/libretro.h"
#include "tests/native/libretro/vk.h"

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SYMS(X)                                                                                              \
    X(retro_api_version, unsigned (*)(void))                                                                 \
    X(retro_set_environment, void (*)(retro_environment_t))                                                  \
    X(retro_set_video_refresh, void (*)(retro_video_refresh_t))                                              \
    X(retro_set_audio_sample, void (*)(retro_audio_sample_t))                                                \
    X(retro_set_audio_sample_batch, void (*)(retro_audio_sample_batch_t))                                    \
    X(retro_set_input_poll, void (*)(retro_input_poll_t))                                                    \
    X(retro_set_input_state, void (*)(retro_input_state_t))                                                  \
    X(retro_init, void (*)(void))                                                                            \
    X(retro_deinit, void (*)(void))                                                                          \
    X(retro_get_system_info, void (*)(struct retro_system_info *))                                           \
    X(retro_get_system_av_info, void (*)(struct retro_system_av_info *))                                     \
    X(retro_set_controller_port_device, void (*)(unsigned, unsigned))                                        \
    X(retro_reset, void (*)(void))                                                                           \
    X(retro_run, void (*)(void))                                                                             \
    X(retro_serialize_size, size_t (*)(void))                                                                \
    X(retro_serialize, bool (*)(void *, size_t))                                                             \
    X(retro_unserialize, bool (*)(const void *, size_t))                                                     \
    X(retro_cheat_reset, void (*)(void))                                                                     \
    X(retro_cheat_set, void (*)(unsigned, bool, const char *))                                               \
    X(retro_load_game, bool (*)(const struct retro_game_info *))                                             \
    X(retro_load_game_special, bool (*)(unsigned, const struct retro_game_info *, size_t))                   \
    X(retro_unload_game, void (*)(void))                                                                     \
    X(retro_get_region, unsigned (*)(void))                                                                  \
    X(retro_get_memory_data, void *(*)(unsigned))                                                            \
    X(retro_get_memory_size, size_t (*)(unsigned))

#define DECLARE(name, type) static __typeof__(type) p_##name;
SYMS(DECLARE)

static const char *g_save_dir;
static bool g_shutdown;
static unsigned g_video_calls, g_new_frames, g_dupes, g_frame_w, g_frame_h;
static size_t g_audio_frames;
static const struct retro_core_option_v2_definition *g_options;
static bool g_vulkan;           /* HARNESS_VULKAN */
static struct retro_hw_render_callback g_hw;
static const struct retro_hw_render_context_negotiation_interface_vulkan *g_neg;
static long g_ran;              /* retro_run calls so far */
static bool g_options_changed;  /* HARNESS_OPTIONS_LATER's, once */

/* The core option's value: HARNESS_OPTIONS's ("key=value;..."), else its
 * default. */
static const char *option(const char *key)
{
    static char value[256];
    const char *set = getenv(g_options_changed ? "HARNESS_OPTIONS_LATER" : "HARNESS_OPTIONS");
    const size_t n = strlen(key);
    for (const char *p = set; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : "") {
        if (!strncmp(p, key, n) && p[n] == '=') {
            const char *end = strchr(p + n + 1, ';');
            snprintf(value, sizeof(value), "%.*s", (int)(end ? end - (p + n + 1) : (long)strlen(p + n + 1)),
                     p + n + 1);
            return value;
        }
    }
    for (const struct retro_core_option_v2_definition *d = g_options; d && d->key; d++) {
        if (!strcmp(d->key, key)) {
            return d->default_value;
        }
    }
    return NULL;
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        g_options = ((const struct retro_core_options_v2 *)data)->definitions;
        for (const struct retro_core_option_v2_definition *d = g_options; d->key; d++) {
            printf("option: %s (%s), default %s\n", d->key, d->desc, d->default_value);
        }
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = option(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = !g_options_changed && g_ran >= 30 && getenv("HARNESS_OPTIONS_LATER");
        g_options_changed |= *(bool *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        printf("message: %s\n", ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:   /* HARNESS_LANGUAGE: RetroArch's (RETRO_LANGUAGE_*) */
        *(unsigned *)data = getenv("HARNESS_LANGUAGE") ? (unsigned)atoi(getenv("HARNESS_LANGUAGE")) : 0;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_save_dir;
        return g_save_dir != NULL;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {   /* the ports, and port 1's types if all have them */
        const struct retro_controller_info *info = data;
        char types[256] = "";
        unsigned n = 0, same = 0;
        for (; info[n].types; n++) {
            char t[256] = "";
            for (unsigned i = 0; i < info[n].num_types; i++) {
                const size_t len = strlen(t);
                snprintf(t + len, sizeof(t) - len, "%s%s (%u)", i ? ", " : "", info[n].types[i].desc,
                         info[n].types[i].id);
            }
            if (!n) {
                snprintf(types, sizeof(types), "%s", t);
            }
            same += !strcmp(t, types);
        }
        printf("controller info: %u ports, %s: %s\n", n, same == n ? "each" : "port 1", types);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        g_shutdown = true;
        return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT: {
        struct retro_hw_render_context_negotiation_interface *n = data;
        if (!g_vulkan || n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) {
            return false;
        }
        n->interface_version = 2;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        if (!g_vulkan || ((struct retro_hw_render_callback *)data)->context_type != RETRO_HW_CONTEXT_VULKAN) {
            return false;
        }
        g_hw = *(struct retro_hw_render_callback *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        g_neg = data;
        return g_vulkan;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        *(const struct retro_hw_render_interface **)data = (const struct retro_hw_render_interface *)hv_interface();
        return hv_interface() != NULL;
    default:
        return false;
    }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    g_video_calls++;
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        hv_frame(w, h);
    }
    if (data) {
        g_new_frames++;
        g_frame_w = w;
        g_frame_h = h;
    } else {
        g_dupes++;
    }
}

static void audio_sample(int16_t l, int16_t r)
{
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    g_audio_frames += frames;
    return frames;
}

static void input_poll(void)
{
}

/* HARNESS_PRESS's buttons, held for this retro_run. */
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    const char *p = getenv("HARNESS_PRESS");
    if (device != RETRO_DEVICE_JOYPAD || !p) {
        return 0;
    }
    unsigned pp, pid;
    long from, to;
    for (; *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : "") {
        if (sscanf(p, "%u:%u:%ld:%ld", &pp, &pid, &from, &to) == 4 && pp == port && pid == id && g_ran >= from &&
            g_ran < to) {
            return 1;
        }
    }
    return 0;
}

/* The ports' devices, "d1,d2,...", as RetroArch sets them. */
static void set_ports(const char *spec)
{
    printf("ports:");
    unsigned port = 0;
    for (const char *p = spec; *p && *p != ';'; port++) {
        char *end;
        const unsigned device = (unsigned)strtoul(p, &end, 10);
        printf(" %u", device);
        p_retro_set_controller_port_device(port, device);
        p = *end == ',' ? end + 1 : end;
    }
    printf("\n");
    fflush(stdout);
}

static const char *g_core_path;
static void *g_core;

#define RESOLVE(name, type)                                                                                  \
    if (!(p_##name = (type)dlsym(g_core, #name))) {                                                          \
        fprintf(stderr, "FAIL: missing %s\n", #name);                                                        \
        return false;                                                                                        \
    }

/* The core opened, as RetroArch opens it: every callback set, retro_init. */
static bool open_core(void)
{
    if (!(g_core = dlopen(g_core_path, RTLD_NOW | RTLD_LOCAL))) {
        fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
        return false;
    }
    SYMS(RESOLVE)
    if (p_retro_api_version() != RETRO_API_VERSION) {
        fprintf(stderr, "FAIL: API version %u\n", p_retro_api_version());
        return false;
    }
    p_retro_set_environment(environment);
    p_retro_set_video_refresh(video);
    p_retro_set_audio_sample(audio_sample);
    p_retro_set_audio_sample_batch(audio_batch);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);
    p_retro_init();
    return true;
}

static void close_core(void)
{
    p_retro_deinit();
    dlclose(g_core);
    g_core = NULL;
}

static long count_dir(const char *path)
{
    DIR *d = opendir(path);
    long n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* The process's threads once the count holds for 50 ms (up to a second): a
 * joined thread can still be listed for a while (the kernel wakes the joiner
 * before it reaps the thread, later still for one of low priority on a busy
 * CPU). */
static long settled_threads(void)
{
    long n = count_dir("/proc/self/task");
    for (int i = 0, same = 0; i < 200 && same < 10; i++) {
        const struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
        const long m = count_dir("/proc/self/task");
        same = m == n ? same + 1 : 0;
        n = m;
    }
    return n;
}

static long status_kib(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, strlen(key))) {
            v = atol(line + strlen(key));
        }
    }
    if (f) {
        fclose(f);
    }
    return v;
}

/* Whether the core's library is still mapped (after dlclose: unloaded). */
static bool core_mapped(void)
{
    char real[PATH_MAX], line[PATH_MAX + 128];
    if (!realpath(g_core_path, real)) {
        return true;
    }
    FILE *f = fopen("/proc/self/maps", "r");
    bool found = false;
    while (f && !found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        const char *p = strchr(line, '/');
        found = p && !strcmp(p, real);
    }
    if (f) {
        fclose(f);
    }
    return found;
}

static void *no_op(void *arg)
{
    return arg;
}

/* The threads besides this one, by name, to stderr. */
static void name_threads(int cycle)
{
    DIR *d = opendir("/proc/self/task");
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.' || atoi(e->d_name) == getpid()) {
            continue;
        }
        char path[64], name[32] = "?";
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(name, sizeof(name), f)) {
                name[strcspn(name, "\n")] = 0;
            }
            fclose(f);
        }
        fprintf(stderr, "cycle %d: thread %s (%s) still there\n", cycle, e->d_name, name);
    }
    if (d) {
        closedir(d);
    }
}

/* The signals the process has handlers for (/proc's SigCgt mask, hex). */
static const char *caught_signals(void)
{
    static char mask[32];
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    snprintf(mask, sizeof(mask), "?");
    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "SigCgt:", 7)) {
            sscanf(line + 7, "%31s", mask);
        }
    }
    if (f) {
        fclose(f);
    }
    return mask;
}

/* One game: loaded, run for frames retro_run calls at the core's rate (or
 * until it asks to shut down), unloaded. 0, or 3 when refused. */
static int play(const struct retro_game_info *game, long frames)
{
    g_shutdown = false;
    g_ran = 0;
    g_options_changed = false;
    g_video_calls = g_new_frames = g_dupes = g_frame_w = g_frame_h = 0;
    g_audio_frames = 0;
    memset(&g_hw, 0, sizeof(g_hw));
    g_neg = NULL;
    if (!p_retro_load_game(game)) {
        printf("load: refused\n");
        return 3;
    }
    struct retro_system_av_info av;
    p_retro_get_system_av_info(&av);
    printf("loaded: %ux%u, aspect %.3f\n", av.geometry.base_width, av.geometry.base_height,
           av.geometry.aspect_ratio);
    if (g_hw.context_reset) {               /* as RetroArch's video driver starts */
        if (!hv_load() || !hv_create(g_neg)) {
            fprintf(stderr, "FAIL: no Vulkan device for the core\n");
            return 1;
        }
        g_hw.context_reset();
    }
    if (getenv("HARNESS_PORTS")) {          /* RetroArch: after its drivers started */
        set_ports(getenv("HARNESS_PORTS"));
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const double t0 = (double)start.tv_sec * 1e9 + (double)start.tv_nsec;
    long ran = 0;
    const long reset_at = getenv("HARNESS_RESET_AT") ? atol(getenv("HARNESS_RESET_AT")) : -1;
    const long reinit_at = getenv("HARNESS_REINIT_AT") ? atol(getenv("HARNESS_REINIT_AT")) : -1;
    for (; ran < frames && !g_shutdown; ran++) {
        for (const char *p = getenv("HARNESS_PORTS_AT"); p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : "") {
            char *end;
            if (strtol(p, &end, 10) == ran && *end == ':') {   /* RetroArch's menu: a port's Device Type */
                printf("frame %ld: ", ran);
                set_ports(end + 1);
            }
        }
        if (ran == reset_at) {              /* RetroArch's Reset (Restart) */
            printf("reset at frame %ld\n", ran);
            fflush(stdout);
            p_retro_reset();
        }
        if (ran == reinit_at && g_hw.context_reset) {   /* RetroArch's video driver rebuilt, uncached */
            printf("video driver rebuilt at frame %ld\n", ran);
            fflush(stdout);
            if (g_hw.context_destroy) {
                g_hw.context_destroy();
            }
            hv_destroy();
            if (!hv_create(g_neg)) {
                fprintf(stderr, "FAIL: no Vulkan device for the core after the rebuild\n");
                return 1;
            }
            g_hw.context_reset();
        }
        const unsigned before = g_video_calls;
        p_retro_run();
        g_ran = ran + 1;
        if (g_video_calls != before + 1) {
            fprintf(stderr, "FAIL: retro_run %ld made %u video calls\n", ran, g_video_calls - before);
            return 1;
        }
        /* at the core's rate exactly: the next call's time from the first's */
        const double t = t0 + (double)(ran + 1) * 1e9 / av.timing.fps;
        const struct timespec next = {(time_t)(t / 1e9), (long)(t - (double)(time_t)(t / 1e9) * 1e9)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    printf("ran %ld frames: %u video calls, %u new frames (last %ux%u), %u dupes, %zu audio frames%s\n", ran,
           g_video_calls, g_new_frames, g_frame_w, g_frame_h, g_dupes, g_audio_frames,
           g_shutdown ? ", the core asked to shut down" : "");
    fflush(stdout);
    if (g_hw.context_destroy) {             /* RetroArch: the context goes before the game */
        g_hw.context_destroy();
    }
    p_retro_unload_game();
    hv_destroy();
    printf("unloaded\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 5) {
        fprintf(stderr, "usage: libretro-harness <core.so> [<disc image> <frames> <save directory>]\n");
        return 2;
    }
    g_core_path = argv[1];
    g_vulkan = getenv("HARNESS_VULKAN") && atoi(getenv("HARNESS_VULKAN"));
    g_save_dir = argc == 5 ? argv[4] : NULL;
    pthread_t t;                    /* glibc's own handler (SIGSETXID) comes with a first thread */
    if (pthread_create(&t, NULL, no_op, NULL) == 0) {
        pthread_join(t, NULL);
    }
    printf("signals caught before the core: %s\n", caught_signals());
    if (!open_core()) {
        return 1;
    }
    struct retro_system_info si;
    p_retro_get_system_info(&si);
    printf("core: %s %s, extensions %s, need_fullpath %d, block_extract %d\n", si.library_name,
           si.library_version, si.valid_extensions, si.need_fullpath, si.block_extract);
    struct retro_system_av_info av;
    p_retro_get_system_av_info(&av);
    printf("av: %.6f fps, %.0f Hz, %ux%u (max %ux%u), aspect %.3f\n", av.timing.fps, av.timing.sample_rate,
           av.geometry.base_width, av.geometry.base_height, av.geometry.max_width, av.geometry.max_height,
           av.geometry.aspect_ratio);
    printf("serialize size: %zu\n", p_retro_serialize_size());

    if (argc == 2) {
        close_core();
        printf("unloaded without a game\n");
        return 0;
    }
    const struct retro_game_info game = {.path = argv[2]};
    const long frames = atol(argv[3]);
    const int cycles = getenv("HARNESS_CYCLES") ? atoi(getenv("HARNESS_CYCLES")) : 1;
    for (int c = 0; c < cycles; c++) {
        if (c > 0 && !open_core()) {
            return 1;
        }
        const int r = play(&game, frames);
        if (r) {
            close_core();
            return r;
        }
        if (cycles > 1) {               /* what the core left in the process once closed */
            close_core();
            const long threads = settled_threads();
            if (threads > 1) {
                name_threads(c + 1);
            }
            printf("cycle %d: %ld threads, %ld descriptors, VmSize %ld KiB, VmRSS %ld KiB, signals caught %s, %s\n",
                   c + 1, threads, count_dir("/proc/self/fd"), status_kib("VmSize:"), status_kib("VmRSS:"),
                   caught_signals(), core_mapped() ? "the core still mapped" : "the core unloaded");
            fflush(stdout);
            if (getenv("HARNESS_MAPS")) {   /* debug: the mappings after each cycle, to a file */
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "cat /proc/%d/maps > %s.%d", (int)getpid(), getenv("HARNESS_MAPS"), c + 1);
                if (system(cmd) != 0) {
                    fprintf(stderr, "maps: %s failed\n", cmd);
                }
#ifdef __GLIBC__
                malloc_stats();             /* and the heap's arenas, to stderr */
#endif
            }
        }
    }
    if (cycles <= 1) {
        close_core();
    }
    if (getenv("HARNESS_LOAD_AGAIN")) {     /* a second game, as RetroArch loads one: the core opened again */
        fflush(stdout);
        if (!open_core()) {
            return 1;
        }
        printf("load again: %s\n", p_retro_load_game(&game) ? "loaded" : "refused");
        close_core();
    }
    return 0;
}
