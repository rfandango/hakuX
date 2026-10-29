/*
 * hakuX native core — the HLE GPU (hw/xbox/hle) on the native core: boot,
 * the debug probes and the PC samplers. What the HLE asks of its host
 * (host.h) is host_native.c's.
 *
 * A probe's address is a FEX hook (NcCpuConfig.hook_query, a system op
 * before the instruction runs); the guest's registers go from the JIT frame
 * to the HLE's HleCpu for it. The title starts at boot, before any guest
 * code is translated.
 */
#include "native/hle/hle_native.h"

#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/core/probe.h"
#include "hw/xbox/hle/vk/pipeasync.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/hitch.h"
#include "native/core/irq.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "hw/xbox/hle/pb/pbdraw.h"
#include "hw/xbox/hle/host.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>

void nc_hle_dirty_init(NcVm *vm);

static NcCpu *g_cpu;
static uint32_t g_fs_base;
static bool g_active;

void nc_hle_set_pipeline_options(bool async, bool prebuild)
{
    hle_pipe_set_options(async, prebuild);
}

void nc_hle_pipeline_progress(uint32_t *done, uint32_t *total)
{
    hle_pipe_progress(done, total);
}

/* ------------------------------------------------------------------ boot */

static char g_fatal[384];  /* why the title can't run here (nc_hle_fatal) */
static int g_fatal_kind;   /* NC_FATAL_* (nc_hle_fatal_kind) */

/* The activity polls nc_hle_fatal from its own thread while this is set
 * (a lost GPU device: the render thread's): the text goes first, its first
 * byte last. */
void nc_hle_set_fatal(int kind, const char *why)
{
    char s[sizeof(g_fatal)];
    snprintf(s, sizeof(s), "%s", why && why[0] ? why : "?");
    g_fatal_kind = kind;
    memcpy(g_fatal + 1, s + 1, sizeof(g_fatal) - 1);
    __atomic_store_n(&g_fatal[0], s[0], __ATOMIC_RELEASE);
}

static void smp_setup(void);

/* Memory kept for the next title (native/kernel/mm.c): push-buffer mode's
 * images over it go back into guest RAM first. */
static void keep(uint32_t pa, uint32_t len)
{
    if (hle_pb_active()) {
        hle_pbd_keep(pa, len);
    }
}

static bool keep_wait(uint64_t timeout_ns)
{
    return !hle_pb_active() || hle_pbd_keep_wait(timeout_ns);
}

void nc_hle_boot(NcKernel *k, NcVm *vm, NcCpu *cpu, const NcCpuContext *boot)
{
    g_cpu = cpu;
    g_fs_base = k->kpcr_va;
    smp_setup();
    nc_hle_host_init(k, vm, cpu);
    nc_hle_dirty_init(vm);

    hle_guestmem_init(vm->phys, vm->ram_size, 0);
    hle_init();
    k->hle_keep = keep;
    k->hle_keep_wait = keep_wait;
    {   /* the last title's kept memory, restored by the host: the GPU reads it
         * from memory, its cache lines cleaned first */
        uint32_t pa[8], len[8];
        const unsigned n = nc_mm_kept(pa, len, 8);
        for (unsigned i = 0; i < n; i++) {
            hle_host_mem_set_dirty(pa[i], len[i]);
        }
    }
    if (!hle_active()) {
        const char *mode = getenv("HAKUX_HLE_MODE");
        nc_log("hakuX-native-hle", "HLE GPU off (HAKUX_HLE_MODE %s)", mode ? mode : "unset");
        return;
    }
    g_active = true;

    /* The registers the diagnostics read (hle_host_cpu) start as the boot
     * thread's. */
    HleCpu *env = &nc_hle_regs;
    memset(env, 0, sizeof(*env));
    env->regs[R_ESP] = nc_context_gpr(boot, NC_ESP);
    env->eip = nc_context_eip(boot);
    env->eflags = 0x202;
    env->fs_base = g_fs_base;
    hle_title_start();
    nc_hle_host_flush_discard(); /* nothing translated yet */
    if (hle_pb_active()) {
        nc_log("hakuX-native-hle", "title %08x in push-buffer mode", g_hle.title.xbe.title_id);
    } else {
        nc_log("hakuX-native-hle", "title %08x: no push-buffer mode, nothing is drawn",
               g_hle.title.xbe.title_id);
    }
    if (g_hle_unavailable[0]) {
        /* the Vulkan driver lacks what the HLE GPU needs (vkdev.c says what) */
        nc_hle_set_fatal(NC_FATAL_DRIVER, g_hle_unavailable);
        nc_log("hakuX-native-hle", "can't run the title: %s", g_fatal);
    }
}

const char *nc_hle_fatal(void)
{
    return __atomic_load_n(&g_fatal[0], __ATOMIC_ACQUIRE) ? g_fatal : NULL;
}

int nc_hle_fatal_kind(void)
{
    return __atomic_load_n(&g_fatal[0], __ATOMIC_ACQUIRE) ? g_fatal_kind : NC_FATAL_NONE;
}

bool nc_hle_pb_active(void)
{
    return hle_pb_active();
}

/* ------------------------------------------------------- the PC samplers */

/* The HLE's samplers (the stall monitor, the perf capture) read env->eip,
 * which here changes only at hooks and kernel calls: a long stretch of guest
 * code read as the last call's address (the DPC return stub, 80010f10). They
 * ask the vCPU thread instead: a SIGPROF there reads the host PC it stopped
 * at, a guest EIP in JIT code (FEX's block RIP table) or the offset in this
 * library in host code (addr2line on the unstripped libhakuxcore.so). */
#define SMP_SIGNAL SIGPROF
#define SMP_OUTSIDE 0x07FFFFFFu /* a host PC outside this library (libc, the driver) */

static pthread_t g_smp_thread;
static bool g_smp_ready;
static bool g_smp_installed;            /* the handler, g_smp_old the one before it */
static struct sigaction g_smp_old;
static uintptr_t g_smp_base;
static volatile uint32_t g_smp_pc;
static uint32_t g_smp_seq;
static pthread_mutex_t g_smp_mu = PTHREAD_MUTEX_INITIALIZER;

static void smp_signal(int sig, siginfo_t *si, void *uc)
{
    const uintptr_t pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
    uint32_t eip;
    uint32_t v;
    if (g_nc_idle_waiting) {
        v = HLE_PC_IDLE;
    } else if (nc_cpu_sample_pc(g_cpu, pc, &eip)) {
        v = eip;
    } else {
        const uintptr_t off = pc - g_smp_base;
        v = HLE_PC_HOST | (g_nc_in_sysop ? HLE_PC_SYSOP : 0) |
            (pc >= g_smp_base && off < ((uintptr_t)SMP_OUTSIDE << 4) ? (uint32_t)(off >> 4) : SMP_OUTSIDE);
    }
    g_smp_pc = v;
    __atomic_add_fetch(&g_smp_seq, 1, __ATOMIC_RELEASE);
}

static bool smp_sample(uint32_t *pc)
{
    if (!g_smp_ready) {
        return false;
    }
    bool ok = false;
    pthread_mutex_lock(&g_smp_mu);
    const uint32_t seq = __atomic_load_n(&g_smp_seq, __ATOMIC_ACQUIRE);
    if (g_smp_ready && pthread_kill(g_smp_thread, SMP_SIGNAL) == 0) {
        /* the handler runs at the vCPU thread's next instruction: microseconds */
        for (int i = 0; i < 200 && !ok; i++) {
            if (__atomic_load_n(&g_smp_seq, __ATOMIC_ACQUIRE) != seq) {
                *pc = g_smp_pc;
                ok = true;
            } else {
                const struct timespec ts = {0, 10000};
                nanosleep(&ts, NULL);
            }
        }
    }
    pthread_mutex_unlock(&g_smp_mu);
    return ok;
}

/* On the vCPU thread, before it runs the title. */
static void smp_setup(void)
{
    Dl_info info;
    if (!dladdr((void *)smp_setup, &info) || !info.dli_fbase) {
        return;
    }
    g_smp_base = (uintptr_t)info.dli_fbase;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = smp_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SMP_SIGNAL, &sa, &g_smp_old) != 0) {
        return;
    }
    g_smp_installed = true;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SMP_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    g_smp_thread = pthread_self();
    g_smp_ready = true;
    g_hle_pc_sampler = smp_sample;
    nc_log("hakuX-native-hle", "pc sampler: SIGPROF on the vCPU thread, core library at %p", info.dli_fbase);
}

void nc_hle_cpu_leave(void)
{
    pthread_mutex_lock(&g_smp_mu);      /* after the sample it may be in */
    g_smp_ready = false;
    pthread_mutex_unlock(&g_smp_mu);
}

/* ------------------------------------------------------------ the probes */

bool nc_hle_hook_query(void *opaque, uint32_t eip)
{
    return g_active && hle_probe_at(eip);
}

void nc_hle_hook(NcKernel *k, NcFrame *f)
{
    HleCpu *env = &nc_hle_regs;
    const uint64_t *r = nc_frame_regs(f);
    for (int i = 0; i < 8; i++) {
        env->regs[i] = (uint32_t)r[i];
    }
    env->eip = (uint32_t)*nc_frame_rip(f);
    env->eflags = nc_frame_if(f) ? 0x202u : 0x002u;
    env->fs_base = g_fs_base;
    if (hle_probe_at(env->eip)) {
        hle_probe_hit(env);
    }
    nc_frame_bypass_hook(f); /* the title's code at eip runs */
}

void nc_hle_after_sysop(NcKernel *k)
{
    nc_hle_watch_rearm();
    nc_hle_host_flush();
}

void nc_hle_native_stop(void)
{
    if (g_cpu) {
        hle_shutdown();
    }
}

void nc_hle_native_reset(void)
{
    if (g_smp_installed) {      /* the one before back, if ours is still the one */
        struct sigaction now;
        if (sigaction(SMP_SIGNAL, NULL, &now) == 0 && now.sa_sigaction == smp_signal) {
            sigaction(SMP_SIGNAL, &g_smp_old, NULL);
        }
        g_smp_installed = false;
    }
    g_hle_pc_sampler = NULL;
    g_smp_ready = false;
    memset(&g_smp_thread, 0, sizeof(g_smp_thread));
    g_cpu = NULL;
    g_fs_base = 0;
    g_active = false;
    g_fatal[0] = 0;
    g_fatal_kind = NC_FATAL_NONE;
    nc_hle_host_reset();
}
