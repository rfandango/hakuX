/*
 * xboxrun — runs an Xbox title in the native core from the command line
 * (adb shell, /data/local/tmp), without the app: no window, no sound, on
 * the HLE GPU as the app runs it (HAKUX_HLE_MODE=replace unless set).
 *
 *   xboxrun <disc image> [option...]
 *
 * Options: the app's for the core (native/core/options.h: loadstate=,
 * scale=, asyncpipe=, mods=, video=, clock=, ...), and
 *   hdd=DIR          the hard disk folder (Partition0-5; made as needed),
 *                    default /data/local/tmp/native/hdd
 *   vkdriver=DIR|LIB|HOOKS  a custom Vulkan driver through adrenotools, as
 *                    the app's (native/app/vkdriver.c)
 *   capture=N[,N...] frames N written to frames/fN.png in the working
 *                    directory, as presented (frames count the HLE GPU's
 *                    swaps in this process: after loadstate=, since the load)
 *   frames=N         the run ends after frame N (exit 0), the shader and
 *                    pipeline caches written first
 *   hang=S           no new frame for S seconds: the guest's state reported
 *                    and exit 6 (from the start: a load's first frame too)
 * NC_TIMEOUT=S (default 30, 0 none) ends any run after S seconds.
 *
 * Exit status: 0 the run ended (frames=, or the guest halted), 1 boot
 * failed, 2 usage or the disc can't be opened, 3 a host fault, 5 NC_TIMEOUT,
 * 6 hang=, 7 the guest stopped (a bugcheck, an unhandled exception: its
 * "stop:" line says why), 8 the savestate couldn't be loaded, 9 the GPU
 * device was lost, 10 the title left (to the dashboard, a reboot, another
 * XBE). xboxrun.sh adds 4: the app is running.
 */
#include "native/app/app.h"
#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/core/options.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/vk/back.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static NcCore g_core;

/* capture.c: NC_CAPTURE_EVERY=<s> writes the presented frame to frames/;
 * capture= and frames= by frame. */
void nc_capture_start(void);
void nc_capture_frames(const char *list, uint64_t stop);
static void dump_ranges(const char *spec);

/* Hex dump of mapped guest bytes (unmapped pages show as ..). */
static void dump_guest(const NcVm *vm, const char *what, uint32_t va, unsigned len)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   %s %08x:", what, va);
    for (unsigned i = 0; i < len && n < (int)sizeof(buf) - 4; i++) {
        if (nc_vm_phys(vm, va + i) == UINT32_MAX) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " ..");
        } else {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %02x", *(const uint8_t *)nc_vm_host(vm, va + i));
        }
    }
    buf[n++] = '\n';
    write(2, buf, (size_t)n);
}

/* Guest state at a signal: registers live in host registers inside JIT code
 * (FEX's 32-bit static mapping: eax x4, ecx x7, edx x5, ebx x6, esp x8, ebp x9,
 * esi x10, edi x11). */
static void report_guest(const mcontext_t *mc, uint32_t eip)
{
    if (eip) {
        static const char *const names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        static const int regs[8] = {4, 7, 5, 6, 8, 9, 10, 11};
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:  ");
        for (int i = 0; i < 8; i++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%08x", names[i], (uint32_t)mc->regs[regs[i]]);
        }
        buf[n++] = '\n';
        write(2, buf, (size_t)n);
        dump_guest(&g_core.vm, "code", eip - 32, 64);
        dump_guest(&g_core.vm, "stack", (uint32_t)mc->regs[8], 64);
    }
    nc_mmio_dump_stats(2);
    nc_kernel_dump_recent(2);
    nc_kernel_dump_threads(&g_core.kernel, 2);
}

/* The faulting host thread and the mappings around it (a crash path: not
 * async-signal-safe, but the process is going down). */
static void report_host_thread(uintptr_t pc, uintptr_t sp, uintptr_t addr)
{
    char buf[512], comm[64] = "?";
    const int tid = (int)gettid();
    snprintf(buf, sizeof(buf), "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(buf, "r");
    if (f) {
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;
        }
        fclose(f);
    }
    int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   thread %d (%s) sp=%llx (main %d)\n", tid, comm,
                     (unsigned long long)sp, (int)getpid());
    write(2, buf, (size_t)n);
    f = fopen("/proc/self/maps", "r");
    if (!f) {
        return;
    }
    char line[400];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) != 2) {
            continue;
        }
        const char *what = pc >= lo && pc < hi ? "pc" : sp >= lo && sp < hi ? "sp" : addr >= lo && addr < hi ? "addr"
                         : addr >= hi && addr - hi < 0x10000 ? "below addr" : addr < lo && lo - addr < 0x10000 ? "above addr"
                                                                                                                   : NULL;
        if (what) {
            n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   map %-10s %s", what, line);
            write(2, buf, (size_t)n);
        }
    }
    fclose(f);
}

static void fault(int sig, siginfo_t *info, void *ctx)
{
    if (nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    const mcontext_t *mc = &((ucontext_t *)ctx)->uc_mcontext;
    uint64_t guest = nc_vm_guest_of(&g_core.vm, info->si_addr);
    uint32_t eip = nc_cpu_guest_eip_at(g_core.cpu, (uintptr_t)mc->pc);
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "hakuX-native-frontier: fault sig=%d host_pc=%llx addr=%p guest_addr=%s%llx "
                     "guest_eip=%08x\n",
                     sig, (unsigned long long)mc->pc, info->si_addr,
                     guest == UINT64_MAX ? "host:" : "", (unsigned long long)(guest == UINT64_MAX ? 0 : guest),
                     eip);
    write(2, buf, (size_t)n);
    report_host_thread((uintptr_t)mc->pc, (uintptr_t)mc->sp, (uintptr_t)info->si_addr);
    if (eip) {
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   host insn %08x\n", *(const uint32_t *)(uintptr_t)mc->pc);
        write(2, buf, (size_t)n);
    }
    /* Host code: where in which image (llvm-addr2line on the unstripped build). */
    Dl_info dl;
    if (!eip && dladdr((void *)(uintptr_t)mc->pc, &dl) && dl.dli_fbase) {
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   host %s+0x%llx (%s)\n", dl.dli_fname,
                     (unsigned long long)((uintptr_t)mc->pc - (uintptr_t)dl.dli_fbase),
                     dl.dli_sname ? dl.dli_sname : "?");
        write(2, buf, (size_t)n);
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   host lr +0x%llx\n",
                     (unsigned long long)((uintptr_t)mc->regs[30] - (uintptr_t)dl.dli_fbase));
        write(2, buf, (size_t)n);
    }
    if (g_core.kernel.in_call && g_core.kernel.frame) {
        /* Inside a kernel function: the guest state is the call's frame. */
        NcFrame *f = g_core.kernel.frame;
        static const char *const names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   in a kernel call; guest");
        for (int i = 0; i < 8; i++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%08x", names[i], nc_frame_gpr(f, i));
        }
        buf[n++] = '\n';
        write(2, buf, (size_t)n);
        const uint32_t esp = nc_frame_gpr(f, NC_ESP);
        dump_guest(&g_core.vm, "stack", esp, 64);
        dump_guest(&g_core.vm, "stack", esp + 64, 64);
        const uint32_t ret = nc_vm_phys(&g_core.vm, esp) != UINT32_MAX ? nc_vm_rd32(&g_core.vm, esp) : 0;
        dump_guest(&g_core.vm, "caller", ret - 48, 64);
        dump_ranges(getenv("NC_FAULT_DUMP"));
        report_guest(mc, 0);
    } else {
        report_guest(mc, eip);
        dump_ranges(getenv("NC_FAULT_DUMP"));
    }
    _exit(3);
}

/* NC_TIMEOUT (seconds, default 30): a run that neither stops nor faults by
 * then is reported where it is (a guest spin shows its EIP and MMIO sites). */
static unsigned g_timeout;

static void timeout(int sig, siginfo_t *info, void *ctx)
{
    const mcontext_t *mc = &((ucontext_t *)ctx)->uc_mcontext;
    uint32_t eip = nc_cpu_guest_eip_at(g_core.cpu, (uintptr_t)mc->pc);
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier: timeout after %u s: host_pc=%llx guest_eip=%08x%s\n",
                     g_timeout, (unsigned long long)mc->pc, eip, eip ? "" : " (not in guest code)");
    write(2, buf, (size_t)n);
    report_guest(mc, eip);
    dump_ranges(getenv("NC_FAULT_DUMP"));
    _exit(5);
}

/* NC_DUMP="va,len;va,len": after the title loads, writes guest bytes to
 * dump_<va>.bin in the working directory (for disassembly on the host). */
static void dump_ranges(const char *spec)
{
    while (spec && *spec) {
        char *end;
        uint32_t va = (uint32_t)strtoul(spec, &end, 16);
        uint32_t len = *end == ',' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
        char name[64];
        snprintf(name, sizeof(name), "dump_%08x.bin", va);
        FILE *out = fopen(name, "wb");
        for (uint32_t i = 0; out && i < len; i++) {
            const uint8_t b = nc_vm_phys(&g_core.vm, va + i) == UINT32_MAX ? 0 : *(const uint8_t *)nc_vm_host(&g_core.vm, va + i);
            fputc(b, out);
        }
        if (out) {
            fclose(out);
            nc_log("hakuX-native", "dumped %08x+%x to %s", va, len, name);
        }
        /* The physical page behind each page of the range (u32 each; ~0: none). */
        snprintf(name, sizeof(name), "dump_%08x.pa", va);
        if ((out = fopen(name, "wb")) != NULL) {
            for (uint32_t p = va & ~(NC_PAGE - 1); p < va + len; p += NC_PAGE) {
                const uint32_t pa = nc_vm_phys(&g_core.vm, p);
                fwrite(&pa, sizeof(pa), 1, out);
            }
            fclose(out);
        }
        spec = *end == ';' ? end + 1 : "";
    }
}

/* hang=: the watchdog over the frames (and a lost GPU, which stops them for
 * good: the run would only reach NC_TIMEOUT). */
static unsigned g_hang_s;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void *watchdog(void *arg)
{
    uint64_t frames = hle_back_swap_count(), since = mono_ms();
    for (;;) {
        usleep(100 * 1000);
        if (nc_hle_fatal_kind() == NC_FATAL_GPU_LOST) {
            nc_log("hakuX-native-frontier", "GPU lost: %s", nc_hle_fatal() ? nc_hle_fatal() : "?");
            _exit(9);
        }
        const uint64_t n = hle_back_swap_count(), now = mono_ms();
        if (n != frames) {
            frames = n;
            since = now;
        } else if (g_hang_s && now - since >= g_hang_s * 1000ull) {
            nc_log("hakuX-native-frontier", "hang: no frame for %u s after frame %llu", g_hang_s,
                   (unsigned long long)n);
            report_guest(NULL, 0);
            _exit(6);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
#ifndef __ANDROID__
    /* --render: the HLE GPU draws, with no display; NC_CAPTURE_EVERY's frames
     * (every 2 s unless set) go to <data>/frames. */
    const bool render = argc >= 4 && !strcmp(argv[1], "--render");
    if (argc < 4 || (strcmp(argv[1], "--headless") && !render)) {
        fprintf(stderr, "usage: xboxrun --headless|--render <disc image> <diagnostic data directory> [option...]\n");
        return 2;
    }
    char data[PATH_MAX], hdd[PATH_MAX + 8], cache[PATH_MAX + 8];
    struct stat st;
    if ((mkdir(argv[3], 0700) != 0 && errno != EEXIST) || stat(argv[3], &st) != 0 || !S_ISDIR(st.st_mode) ||
        !realpath(argv[3], data)) {
        fprintf(stderr, "invalid diagnostic data directory: %s\n", argv[3]);
        return 2;
    }
    snprintf(hdd, sizeof(hdd), "%s/hdd", data);
    snprintf(cache, sizeof(cache), "%s/cache", data);
    if ((mkdir(hdd, 0700) != 0 && errno != EEXIST) ||
        (mkdir(cache, 0700) != 0 && errno != EEXIST)) {
        perror("diagnostic data directories");
        return 2;
    }
    nc_io_set_hdd_root(hdd);
    setenv("HAKUX_NATIVE_BASE", cache, 1);
    setenv("HAKUX_HLE_MODE", render ? "replace" : "off", 1);
    if (render) {
        setenv("NC_CAPTURE_EVERY", "2", 0);
        nc_log("hakuX-native", "explicit render diagnostics: renderer ON without a display, "
               "frames to %s/frames, paced silent audio", data);
    } else {
        nc_log("hakuX-native", "explicit headless diagnostics: renderer OFF, paced silent audio");
    }
    const char *disc = argv[2];
#else
    if (argc < 2) {
        fprintf(stderr, "usage: xboxrun <disc image> [option...]\n");
        return 2;
    }
    const char *disc = argv[1];
#endif
    int fd = open(disc, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(disc);
        return 2;
    }
#ifndef __ANDROID__
    if (render && chdir(data) != 0) {
        perror(data);
        return 2;
    }
#endif

    struct sigaction sa = {0};
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sa.sa_sigaction = timeout;
    sigaction(SIGALRM, &sa, NULL);
    g_timeout = getenv("NC_TIMEOUT") ? (unsigned)atoi(getenv("NC_TIMEOUT")) : 30;
    nc_log("hakuX-native", "xboxrun pid %d", (int)getpid()); /* the HLE layer logs to logcat under this pid */
    setenv("HAKUX_HLE_MODE", "replace", 0);     /* the GPU as the app runs it */

    NcOptions opt;
    nc_options_init(&opt);
    /* The console's video settings and language also from NC_VIDEO (hex),
     * NC_AVPACK (the NC_AV_PACK_* number) and NC_LANGUAGE, as video=,
     * avpack= and lang=; HAKUX_HLE_UNCAP as uncap */
    opt.uncap = getenv("HAKUX_HLE_UNCAP") != NULL;
    if (getenv("NC_VIDEO")) {
        opt.video = (int64_t)strtoul(getenv("NC_VIDEO"), NULL, 16);
    }
    if (getenv("NC_AVPACK")) {
        opt.av_pack = atoi(getenv("NC_AVPACK"));
    }
    if (getenv("NC_LANGUAGE")) {
        nc_kernel_set_language(atoi(getenv("NC_LANGUAGE")));
    }
    const char *capture = NULL;
    uint64_t stop = 0;
#ifdef __ANDROID__
    const int first_option = 2;
#else
    const int first_option = 4;
#endif
    for (int i = first_option; i < argc; i++) {
        if (nc_option(&opt, argv[i])) {
            continue;
        }
        if (!strncmp(argv[i], "hdd=", 4)) {
            nc_io_set_hdd_root(argv[i] + 4);
        } else if (!strncmp(argv[i], "vkdriver=", 9)) {
            nc_vk_driver_load(argv[i] + 9);
        } else if (!strncmp(argv[i], "capture=", 8)) {
            capture = argv[i] + 8;
        } else if (!strncmp(argv[i], "frames=", 7)) {
            stop = strtoull(argv[i] + 7, NULL, 10);
        } else if (!strncmp(argv[i], "hang=", 5)) {
            g_hang_s = (unsigned)atoi(argv[i] + 5);
        } else {
            fprintf(stderr, "xboxrun: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    nc_options_apply(&opt);
    nc_log("hakuX-native", "xboxrun: disc %s, hdd %s", disc, nc_io_hdd_root());

    if (!nc_core_boot(&g_core, fd)) {
        nc_log("hakuX-native-frontier", "boot failed%s%s", nc_hle_fatal() ? ": " : "",
               nc_hle_fatal() ? nc_hle_fatal() : "");
        return nc_hle_fatal_kind() == NC_FATAL_STATE ? 8 : 1;
    }
    dump_ranges(getenv("NC_DUMP"));
    if (getenv("NC_DUMP_ONLY")) {
        return 0;
    }
    nc_capture_start();
    nc_capture_frames(capture, stop);
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    pthread_detach(wd);
    alarm(g_timeout);
    nc_core_run(&g_core);
    const NcKernel *k = &g_core.kernel;
    return k->quit || k->relaunch ? 10 : k->stop ? 7 : 0;
}
