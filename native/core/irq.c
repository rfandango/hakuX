/*
 * hakuX native core — interrupt lines, behind a mask like the PIC's: a line
 * nobody has enabled (no interrupt connected to it) can be asserted by its
 * device but is not pending for the CPU and doesn't wake an idle wait.
 */
#include "native/core/irq.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint32_t g_lines;
static uint32_t g_enabled;
static NcCpu *g_cpu;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Interrupt latency: from a line rising to the guest taking it (its ISR
 * lowers it), the most per statistics period; over 8 ms is logged (a vblank
 * the guest takes late presents its frame late). */
static uint64_t g_up_ns[NC_IRQ_LINES];
static uint64_t g_lat_max_ns[NC_IRQ_LINES];
static uint64_t g_up_cpu_ns[NC_IRQ_LINES];
static clockid_t g_cpu_clock;
static bool g_have_cpu_clock;
#define IRQ_SLOW_NS 8000000ull

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void latency_note(unsigned line, uint64_t lat)
{
    uint64_t m = __atomic_load_n(&g_lat_max_ns[line], __ATOMIC_RELAXED);
    while (lat > m && !__atomic_compare_exchange_n(&g_lat_max_ns[line], &m, lat, false, __ATOMIC_RELAXED,
                                                   __ATOMIC_RELAXED)) {
    }
    if (lat > IRQ_SLOW_NS) {
        static uint64_t window;
        static unsigned logged;
        const uint64_t now = mono_ns();
        if (now - window > 1000000000ull) {
            window = now;
            logged = 0;
        }
        if (logged++ < 10) {
            nc_log("hakuX-native-irq", "line %u taken %.1f ms after it rose", line, lat / 1e6);
        }
    }
}

uint64_t nc_irq_rise_ns(unsigned line)
{
    return line < NC_IRQ_LINES ? __atomic_load_n(&g_up_ns[line], __ATOMIC_ACQUIRE) : 0;
}

uint64_t nc_irq_now_ns(void)
{
    return mono_ns();
}

uint64_t nc_irq_cpu_ns(void)
{
    struct timespec ts;
    if (!__atomic_load_n(&g_have_cpu_clock, __ATOMIC_ACQUIRE) || clock_gettime(g_cpu_clock, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t nc_irq_rise_cpu_ns(unsigned line)
{
    return line < NC_IRQ_LINES ? __atomic_load_n(&g_up_cpu_ns[line], __ATOMIC_ACQUIRE) : 0;
}

/* debug (NC_IRQ_SCHED=1): the guest CPU's thread's time runnable but not
 * running (schedstat's run delay) at each rise, so a late line tells a
 * thread waiting for a host CPU from one blocked or sleeping. */
static int g_cpu_tid;
static uint64_t g_up_delay_ns[NC_IRQ_LINES];

static bool sched_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NC_IRQ_SCHED");
        on = e && atoi(e) != 0;
    }
    return on;
}

uint64_t nc_irq_delay_ns(void)
{
    const int tid = __atomic_load_n(&g_cpu_tid, __ATOMIC_ACQUIRE);
    if (!tid || !sched_on()) {
        return 0;
    }
    char path[64], buf[96];
    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unsigned long long run, delay;
    if (n <= 0) {
        return 0;
    }
    buf[n] = 0;
    return sscanf(buf, "%llu %llu", &run, &delay) == 2 ? delay : 0;
}

uint64_t nc_irq_rise_delay_ns(unsigned line)
{
    return line < NC_IRQ_LINES ? __atomic_load_n(&g_up_delay_ns[line], __ATOMIC_ACQUIRE) : 0;
}

uint64_t nc_irq_latency_max(unsigned line, bool reset)
{
    if (line >= NC_IRQ_LINES) {
        return 0;
    }
    return reset ? __atomic_exchange_n(&g_lat_max_ns[line], 0, __ATOMIC_RELAXED)
                 : __atomic_load_n(&g_lat_max_ns[line], __ATOMIC_RELAXED);
}

static pthread_cond_t g_pause_cv;

static void init_cv(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &attr);
    pthread_cond_init(&g_pause_cv, &attr);
    pthread_condattr_destroy(&attr);
}

void nc_irq_attach(NcCpu *cpu)
{
    pthread_once(&g_once, init_cv);
    if (pthread_getcpuclockid(pthread_self(), &g_cpu_clock) == 0) {  /* the guest CPU's thread */
        __atomic_store_n(&g_have_cpu_clock, true, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_cpu_tid, (int)gettid(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_cpu, cpu, __ATOMIC_RELEASE);
    if (nc_irq_lines()) {
        nc_cpu_request_interrupt(cpu);
    }
}

volatile int g_nc_idle_waiting;

/* The guest's CPU woken from its idle wait (nc_irq_idle_wait, the only one
 * waiting on g_cv) for a line or work just set; not woken, nothing to do.
 * The set, a full fence, the flag read here, against the wait's flag set, a
 * full fence and its checks: one of the two sees the other's store. A
 * broadcast a rise was a mutex and a futex wake each time. */
static void wake_idle(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_nc_idle_waiting, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&g_mu);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
}

void nc_irq_set(unsigned line, bool level)
{
    if (line >= NC_IRQ_LINES) {
        return;
    }
    /* NC_TRACE_IRQ=<line>[:<n>]: its level changes, timestamped (the first
     * n, 2000 by default). */
    static int trace_line = -2, trace_left = 2000;
    if (trace_line == -2) {
        const char *t = getenv("NC_TRACE_IRQ");
        trace_line = t ? atoi(t) : -1;
        const char *n = t ? strchr(t, ':') : NULL;
        if (n) {
            trace_left = atoi(n + 1);
        }
    }
    if ((int)line == trace_line && trace_left > 0 &&
        (bool)(__atomic_load_n(&g_lines, __ATOMIC_ACQUIRE) & (1u << line)) != level) {
        trace_left--;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        nc_log("hakuX-native-irq", "%ld.%06ld line %u %s", (long)ts.tv_sec, ts.tv_nsec / 1000, line, level ? "up" : "down");
    }
    pthread_once(&g_once, init_cv);
    if (!level) {
        if (__atomic_fetch_and(&g_lines, ~(1u << line), __ATOMIC_RELEASE) & (1u << line)) {
            const uint64_t up = __atomic_exchange_n(&g_up_ns[line], 0, __ATOMIC_ACQ_REL);
            if (up) {
                latency_note(line, mono_ns() - up);
            }
        }
        return;
    }
    if (__atomic_load_n(&g_lines, __ATOMIC_ACQUIRE) & (1u << line)) {
        return; /* already asserted */
    }
    /* The rise time before the line: the guest can take the line, and lower
     * it, as soon as it is set. Stored after, it outlived that lowering and
     * the next rise was timed from it: "delivered 20.0 ms after it rose"
     * for vblanks taken at once (PGR2's movies, many a second). */
    /* the guest thread's CPU time at the rise, for the slow-delivery log
     * (intr.c): a syscall a rise, so only with NC_IRQ_SCHED, as the delay */
    __atomic_store_n(&g_up_cpu_ns[line], sched_on() ? nc_irq_cpu_ns() : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_up_delay_ns[line], nc_irq_delay_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_up_ns[line], mono_ns(), __ATOMIC_RELEASE);
    if (__atomic_fetch_or(&g_lines, 1u << line, __ATOMIC_ACQ_REL) & (1u << line)) {
        return; /* another device thread asserted it meanwhile */
    }
    if (!(__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE) & (1u << line))) {
        return; /* masked */
    }
    NcCpu *cpu = __atomic_load_n(&g_cpu, __ATOMIC_ACQUIRE);
    if (cpu) {
        nc_cpu_request_interrupt(cpu);
    }
    wake_idle();
}

uint32_t nc_irq_lines(void)
{
    return __atomic_load_n(&g_lines, __ATOMIC_ACQUIRE) & __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
}

void nc_irq_enable(unsigned line, bool on)
{
    if (line >= NC_IRQ_LINES) {
        return;
    }
    pthread_once(&g_once, init_cv);
    if (!on) {
        __atomic_fetch_and(&g_enabled, ~(1u << line), __ATOMIC_RELEASE);
        return;
    }
    __atomic_fetch_or(&g_enabled, 1u << line, __ATOMIC_ACQ_REL);
    if (nc_irq_lines() & (1u << line)) {
        NcCpu *cpu = __atomic_load_n(&g_cpu, __ATOMIC_ACQUIRE);
        if (cpu) {
            nc_cpu_request_interrupt(cpu);
        }
        wake_idle();
    }
}

static uint32_t g_work;

void nc_irq_post_work(void)
{
    pthread_once(&g_once, init_cv);
    __atomic_store_n(&g_work, 1, __ATOMIC_RELEASE);
    NcCpu *cpu = __atomic_load_n(&g_cpu, __ATOMIC_ACQUIRE);
    if (cpu) {
        nc_cpu_request_interrupt(cpu);
    }
    wake_idle();
}

/* The emulation paused (the activity paused: the device suspends, another
 * app in front): the guest's CPU stops at its next kernel entry
 * (nc_kernel_pause_point), the devices that run on their own hold off. */
static bool g_paused;
/* g_pause_cv (above, on CLOCK_MONOTONIC: init_cv): nc_pause_quiesce's deadline is that clock's */

/* The device threads that wait in nc_pause_wait (savestates: all of them
 * there before the state is taken). */
#define PAUSE_THREADS 16
static struct {
    pthread_t th;
    const char *name;
    bool parked;
} g_pt[PAUSE_THREADS];
static unsigned g_npt;
static bool g_cpu_work;

static int pause_slot(void)
{
    const pthread_t me = pthread_self();
    for (unsigned i = 0; i < g_npt; i++) {
        if (pthread_equal(g_pt[i].th, me)) {
            return (int)i;
        }
    }
    return -1;
}

void nc_pause_register(const char *name)
{
    pthread_once(&g_once, init_cv);
    pthread_mutex_lock(&g_mu);
    if (pause_slot() < 0 && g_npt < PAUSE_THREADS) {
        g_pt[g_npt].th = pthread_self();
        g_pt[g_npt].name = name;
        g_pt[g_npt++].parked = false;
    }
    pthread_mutex_unlock(&g_mu);
}

void nc_pause_idle(bool on)
{
    pthread_once(&g_once, init_cv);
    pthread_mutex_lock(&g_mu);
    const int slot = pause_slot();
    if (slot >= 0) {
        g_pt[slot].parked = on;
        pthread_cond_broadcast(&g_pause_cv);
    }
    pthread_mutex_unlock(&g_mu);
}

bool nc_pause_quiesce(uint64_t timeout_ns, const char **who)
{
    pthread_once(&g_once, init_cv);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t until = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec + timeout_ns;
    const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
    pthread_mutex_lock(&g_mu);
    for (;;) {
        int missing = -1;
        for (unsigned i = 0; i < g_npt && missing < 0; i++) {
            missing = g_pt[i].parked ? -1 : (int)i;
        }
        if (missing < 0) {
            pthread_mutex_unlock(&g_mu);
            return true;
        }
        if (pthread_cond_timedwait(&g_pause_cv, &g_mu, &ts) != 0) {
            *who = g_pt[missing].name;
            pthread_mutex_unlock(&g_mu);
            return false;
        }
    }
}

void nc_pause_kick(void)
{
    pthread_once(&g_once, init_cv);
    pthread_mutex_lock(&g_mu);
    g_cpu_work = true;
    pthread_cond_broadcast(&g_pause_cv);
    pthread_mutex_unlock(&g_mu);
}

bool nc_pause_cpu_work(void)
{
    pthread_mutex_lock(&g_mu);
    const bool w = g_cpu_work;
    g_cpu_work = false;
    pthread_mutex_unlock(&g_mu);
    return w;
}

void nc_irq_state(uint32_t *enabled, uint32_t *lines)
{
    *enabled = __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
    *lines = __atomic_load_n(&g_lines, __ATOMIC_ACQUIRE);
}

void nc_irq_state_set(uint32_t enabled, uint32_t lines)
{
    __atomic_store_n(&g_enabled, enabled, __ATOMIC_RELEASE);
    __atomic_store_n(&g_lines, lines, __ATOMIC_RELEASE);
}

void nc_pause_set(bool on)
{
    pthread_once(&g_once, init_cv);
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g_paused, on, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_pause_cv);
    pthread_mutex_unlock(&g_mu);
    if (on) {
        nc_irq_post_work();     /* the guest's CPU into the kernel, an idle wait woken */
    }
}

bool nc_paused(void)
{
    return __atomic_load_n(&g_paused, __ATOMIC_ACQUIRE);
}

void nc_pause_wait(void)
{
    pthread_once(&g_once, init_cv);
    pthread_mutex_lock(&g_mu);
    const int slot = pause_slot();
    if (slot >= 0) {
        g_pt[slot].parked = true;
        pthread_cond_broadcast(&g_pause_cv);     /* nc_pause_quiesce */
    }
    while (__atomic_load_n(&g_paused, __ATOMIC_ACQUIRE) && (slot >= 0 || !g_cpu_work)) {
        pthread_cond_wait(&g_pause_cv, &g_mu);
    }
    if (slot >= 0) {
        g_pt[slot].parked = false;
    }
    pthread_mutex_unlock(&g_mu);
}

bool nc_irq_take_work(void)
{
    /* a load first: every kernel entry asks */
    return __atomic_load_n(&g_work, __ATOMIC_ACQUIRE) && __atomic_exchange_n(&g_work, 0, __ATOMIC_ACQ_REL);
}

uint32_t nc_irq_idle_wait(uint64_t deadline_ns, uint64_t max_ns)
{
    pthread_once(&g_once, init_cv);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    uint64_t until = now_ns + max_ns;
    if (deadline_ns && deadline_ns < until) {
        until = deadline_ns;
    }
    const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
    __atomic_store_n(&g_nc_idle_waiting, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);    /* against wake_idle's */
    pthread_mutex_lock(&g_mu);
    while (!nc_irq_lines() && !__atomic_load_n(&g_work, __ATOMIC_ACQUIRE)) {
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    __atomic_store_n(&g_nc_idle_waiting, 0, __ATOMIC_RELAXED);
    return nc_irq_lines();
}

void nc_irq_reset(void)
{
    pthread_once(&g_once, init_cv);
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g_lines, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_enabled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cpu, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cpu_tid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_have_cpu_clock, false, __ATOMIC_RELEASE);
    memset(g_up_ns, 0, sizeof(g_up_ns));
    memset(g_lat_max_ns, 0, sizeof(g_lat_max_ns));
    memset(g_up_cpu_ns, 0, sizeof(g_up_cpu_ns));
    memset(g_up_delay_ns, 0, sizeof(g_up_delay_ns));
    __atomic_store_n(&g_work, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_paused, false, __ATOMIC_RELEASE);
    memset(g_pt, 0, sizeof(g_pt));      /* the stopped devices' threads */
    g_npt = 0;
    g_cpu_work = false;
    pthread_mutex_unlock(&g_mu);
}
