/*
 * hakuX native core — time, dispatcher objects, waits, IRQL.
 *
 * Dispatcher objects live in guest memory with the Xbox layouts (nxdk
 * xboxkrnl.h, CC0): DISPATCHER_HEADER { Type, Absolute, Size, Inserted,
 * SignalState 0x04, WaitListHead 0x08 }; KMUTANT OwnerThread 0x18;
 * KSEMAPHORE Limit 0x10. RTL_CRITICAL_SECTION is a synchronization KEVENT
 * followed by LockCount 0x10, RecursionCount 0x14 and OwningThread 0x18.
 *
 * Waits don't use the guest's wait blocks: a waiting thread's objects are
 * kept on the host and rechecked whenever something is signaled and when the
 * scheduler runs. A blocked kernel call returns its status later, written
 * into the thread's saved EAX when the wait ends.
 */
#include "native/kernel/kernel.h"
#include "hw/xbox/hle/host.h"

#include <pthread.h>
#include <time.h>

enum {
    OBJ_NOTIFICATION_EVENT = 0,
    OBJ_SYNCHRONIZATION_EVENT = 1,
    OBJ_MUTANT = 2,
    OBJ_SEMAPHORE = 5,
    OBJ_THREAD = 6,
    OBJ_NOTIFICATION_TIMER = 8,
    OBJ_SYNCHRONIZATION_TIMER = 9,
};

#define HDR_SIGNAL_STATE 0x04
#define HDR_WAIT_LIST    0x08
#define MUTANT_OWNER     0x18
#define SEMAPHORE_LIMIT  0x10
#define CS_LOCK_COUNT    0x10
#define CS_RECURSION     0x14
#define CS_OWNER         0x18
#define PCR_IRQL         0x24

#define WAIT_ALL 0
#define WAIT_ANY 1

/* The Xbox's performance counter runs at the ACPI timer's 3.375 MHz. */
#define PERF_FREQUENCY 3375000ull

/* The system time's origin, 2001-01-01 as a FILETIME (100 ns since 1601):
 * the system time is it + g_time_offset + the guest's time since boot. */
#define BOOT_FILETIME 0x01C0739BEB8F8000ull

static uint64_t g_boot_ns;
/* The console's clock at boot past BOOT_FILETIME (the phone's time, moved by
 * the settings: clock_boot), and what NtSetSystemTime moved it since; 100 ns
 * (wraps for negative). A savestate keeps it. */
static uint64_t g_time_offset;
static int64_t g_clock_ms;      /* nc_kernel_set_clock */

/* One clock read per kernel entry. On the RP5 a read traps to the host
 * kernel, which emulates the counter (~160 ns, CLOCK_MONOTONIC and
 * cntvct_el0 alike: a coarse read is 8 ns), and an entry made about a dozen
 * (timers, the slice, DPC and IRQ bookkeeping): a fifth of Crimson Sea's
 * guest thread in push-buffer mode. Within nc_kernel_sysop, on the guest's
 * thread, the first read is kept for the entry; a wait that blocked drops it
 * (nc_ke_update_time follows each), and so does a tick of the coarse clock,
 * which bounds a blocking wait this misses to one tick (4 ms). Other threads
 * read the clock. */
static pthread_t g_entry_thread;
static bool g_entry;
static uint64_t g_entry_ns, g_entry_coarse;

static uint64_t coarse_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The emulation paused (nc_kernel_pause_point): the guest's time stops where
 * the pause began and goes on from there, the time paused left out (g_off,
 * host ns). A seqlock: the guest's CPU writes, any thread reads. */
static uint32_t g_tseq;
static bool g_tpaused;
static uint64_t g_tpause_raw, g_off;

static uint64_t raw_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t guest_of_raw(uint64_t raw)
{
    for (;;) {
        const uint32_t s = __atomic_load_n(&g_tseq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            continue;                   /* the guest's CPU is writing it */
        }
        const bool p = __atomic_load_n(&g_tpaused, __ATOMIC_RELAXED);
        const uint64_t pr = __atomic_load_n(&g_tpause_raw, __ATOMIC_RELAXED);
        const uint64_t off = __atomic_load_n(&g_off, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_tseq, __ATOMIC_RELAXED) == s) {
            return (p ? pr : raw) - off;
        }
    }
}

static void time_write(bool paused)
{
    const uint64_t raw = raw_ns();
    __atomic_fetch_add(&g_tseq, 1, __ATOMIC_ACQ_REL);
    if (paused) {
        __atomic_store_n(&g_tpause_raw, raw, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&g_off, g_off + (raw - g_tpause_raw), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_tpaused, paused, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_tseq, 1, __ATOMIC_RELEASE);
    g_entry_ns = 0;
}

void nc_ke_pause_begin(void)
{
    time_write(true);
}

uint64_t nc_ke_pause_end(void)
{
    const uint64_t was = g_off;
    time_write(false);
    return g_off - was;
}

void nc_ke_state(uint64_t *boot_ns, uint64_t *time_offset, uint64_t *guest_ns)
{
    *boot_ns = g_boot_ns;
    *time_offset = g_time_offset;
    *guest_ns = guest_of_raw(raw_ns());
}

/* A savestate's: the guest's time goes on from guest_ns, now. */
void nc_ke_state_set(uint64_t boot_ns, uint64_t time_offset, uint64_t guest_ns)
{
    g_boot_ns = boot_ns;
    g_time_offset = time_offset;
    const uint64_t raw = raw_ns();
    __atomic_fetch_add(&g_tseq, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_off, raw - guest_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tpause_raw, raw, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tpaused, false, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_tseq, 1, __ATOMIC_RELEASE);
    g_entry_ns = 0;
}

uint64_t nc_ke_host_ns(uint64_t guest_ns)
{
    return guest_ns + __atomic_load_n(&g_off, __ATOMIC_RELAXED);
}

uint64_t nc_ke_now_ns(void)
{
    const bool mine = __atomic_load_n(&g_entry, __ATOMIC_RELAXED) &&
                      pthread_equal(pthread_self(), g_entry_thread);
    if (mine && g_entry_ns && coarse_ns() == g_entry_coarse) {
        return g_entry_ns;
    }
    const uint64_t v = guest_of_raw(raw_ns());
    if (mine) {
        g_entry_ns = v;
        g_entry_coarse = coarse_ns();
    }
    return v;
}

void nc_ke_time_enter(void)
{
    g_entry_thread = pthread_self();
    g_entry_ns = 0;
    __atomic_store_n(&g_entry, true, __ATOMIC_RELAXED);
}

void nc_ke_time_leave(void)
{
    __atomic_store_n(&g_entry, false, __ATOMIC_RELAXED);
}

void nc_ke_time_stale(void)
{
    g_entry_ns = 0;
}

static uint64_t since_boot_100ns(void)
{
    return (nc_ke_now_ns() - g_boot_ns) / 100u;
}

uint64_t nc_ke_system_time(void)
{
    return BOOT_FILETIME + g_time_offset + since_boot_100ns();
}

uint64_t nc_ke_interrupt_time(void)
{
    return since_boot_100ns();
}

static void wr_ksystem_time(NcVm *vm, uint32_t va, uint64_t t)
{
    nc_vm_wr32(vm, va + 8, (uint32_t)(t >> 32)); /* High2Time */
    nc_vm_wr32(vm, va + 0, (uint32_t)t);         /* LowPart */
    nc_vm_wr32(vm, va + 4, (uint32_t)(t >> 32)); /* High1Time */
}

static uint32_t data_va(NcKernel *k, const char *name);

static uint32_t g_refreshes;

uint32_t nc_ke_time_refreshes(void)
{
    return __atomic_load_n(&g_refreshes, __ATOMIC_RELAXED);
}

/* The guest's time as the clock thread last read it (nc_ke_publish_time,
 * about every millisecond, the Xbox's clock interrupt), with the coarse
 * clock then; 0 until it runs. Kernel entries take the tick counts from it
 * (nc_ke_tick_time) and look whether something may be due by it (kernel.c
 * clock_tick), without a clock read of their own. */
static uint64_t g_pub_ns, g_pub_coarse;

void nc_ke_publish_time(uint64_t guest_ns)
{
    __atomic_store_n(&g_pub_coarse, coarse_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_pub_ns, guest_ns, __ATOMIC_RELEASE);
}

/* The published time, or 0 when there is none or the coarse clock says it
 * is stale (more than its tick, 4 ms on the RP5, without a publish: the
 * clock thread held off). */
uint64_t nc_ke_published_ns(void)
{
    const uint64_t pub = __atomic_load_n(&g_pub_ns, __ATOMIC_ACQUIRE);
    if (!pub || coarse_ns() - __atomic_load_n(&g_pub_coarse, __ATOMIC_RELAXED) > 4000000ull) {
        return 0;
    }
    return pub;
}

/* KeTickCount, KeInterruptTime and KeSystemTime at t (100 ns since boot):
 * only ever forward, whichever clock they came from. */
static uint64_t g_times_t;

static void write_times(NcKernel *k, uint64_t t)
{
    static uint32_t tick, interrupt, system;
    if (!tick) {
        tick = data_va(k, "KeTickCount");
        interrupt = data_va(k, "KeInterruptTime");
        system = data_va(k, "KeSystemTime");
    }
    if (t < g_times_t) {
        t = g_times_t;
    }
    g_times_t = t;
    __atomic_store_n(&g_refreshes, g_refreshes + 1, __ATOMIC_RELAXED);  /* the guest's thread */
    nc_vm_wr32(k->vm, tick, (uint32_t)(t / 10000u));
    wr_ksystem_time(k->vm, interrupt, t);
    wr_ksystem_time(k->vm, system, BOOT_FILETIME + g_time_offset + t);
}

void nc_ke_update_time(NcKernel *k)
{
    nc_ke_time_stale();                 /* after a wait: now is later */
    write_times(k, since_boot_100ns());
}

/* A kernel entry's: the times from the published clock when it moved, as
 * nc_ke_update_time without its clock read (~160 ns on the RP5, one each
 * entry). */
void nc_ke_tick_time(NcKernel *k)
{
    const uint64_t pub = nc_ke_published_ns();
    if (pub < g_boot_ns + 1) {
        nc_ke_update_time(k);           /* none yet */
        return;
    }
    const uint64_t t = (pub - g_boot_ns) / 100u;
    if (t > g_times_t) {
        write_times(k, t);
    }
}

static uint32_t data_va(NcKernel *k, const char *name)
{
    for (unsigned o = 1; o < NC_KORDINAL_MAX; o++) {
        const NcKExport *e = nc_kernel_export(o);
        if (e && e->abi == NC_KABI_DATA && strcmp(e->name, name) == 0) {
            return k->data_va[o];
        }
    }
    return 0;
}

/* ---------------------------------------------------------- dispatcher */

static int32_t signal_state(NcKernel *k, uint32_t obj)
{
    return (int32_t)nc_vm_rd32(k->vm, obj + HDR_SIGNAL_STATE);
}

static void set_signal_state(NcKernel *k, uint32_t obj, int32_t v)
{
    nc_vm_wr32(k->vm, obj + HDR_SIGNAL_STATE, (uint32_t)v);
}

static bool is_signaled(NcKernel *k, uint32_t obj, const NcThread *t)
{
    const uint8_t type = *(const uint8_t *)nc_vm_host(k->vm, obj);
    if (type == OBJ_MUTANT) {
        return signal_state(k, obj) > 0 || nc_vm_rd32(k->vm, obj + MUTANT_OWNER) == t->ethread;
    }
    return signal_state(k, obj) > 0;
}

static void acquire(NcKernel *k, uint32_t obj, const NcThread *t)
{
    const uint8_t type = *(const uint8_t *)nc_vm_host(k->vm, obj);
    switch (type) {
    case OBJ_SYNCHRONIZATION_EVENT:
    case OBJ_SYNCHRONIZATION_TIMER:
        set_signal_state(k, obj, 0);
        break;
    case OBJ_SEMAPHORE:
        set_signal_state(k, obj, signal_state(k, obj) - 1);
        break;
    case OBJ_MUTANT:
        set_signal_state(k, obj, signal_state(k, obj) - 1);
        nc_vm_wr32(k->vm, obj + MUTANT_OWNER, t->ethread);
        break;
    default:
        break; /* notification objects and threads stay signaled */
    }
}

/* Index of the object that satisfies the wait (wait-any), 0 for a satisfied
 * wait-all, or -1. */
static int satisfied(NcKernel *k, const NcThread *t, const NcWait *w)
{
    if (w->wait_all) {
        for (unsigned i = 0; i < w->count; i++) {
            if (!is_signaled(k, w->objects[i], t)) {
                return -1;
            }
        }
        return w->count ? 0 : -1;
    }
    for (unsigned i = 0; i < w->count; i++) {
        if (is_signaled(k, w->objects[i], t)) {
            return (int)i;
        }
    }
    return -1;
}

static void take(NcKernel *k, const NcThread *t, const NcWait *w, int idx)
{
    if (w->wait_all) {
        for (unsigned i = 0; i < w->count; i++) {
            acquire(k, w->objects[i], t);
        }
    } else {
        acquire(k, w->objects[idx], t);
    }
    if (w->cs) {
        nc_vm_wr32(k->vm, w->cs + CS_OWNER, t->ethread);
        nc_vm_wr32(k->vm, w->cs + CS_RECURSION, 1);
    }
}

uint64_t nc_ke_check_waits(NcKernel *k)
{
    const uint64_t now = nc_ke_now_ns();
    uint64_t earliest = 0;
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        NcThread *t = &k->threads[i];
        if (t->state != NC_T_WAITING) {
            continue;
        }
        int idx = satisfied(k, t, &t->wait);
        if (idx >= 0) {
            take(k, t, &t->wait, idx);
            nc_context_set_gpr(&t->ctx, NC_EAX, (uint32_t)idx);
            t->state = NC_T_READY;
        } else if (t->wait.deadline_ns && now >= t->wait.deadline_ns) {
            /* A delay ends successfully; a wait times out. */
            nc_context_set_gpr(&t->ctx, NC_EAX, t->wait.count ? NC_STATUS_TIMEOUT : NC_STATUS_SUCCESS);
            t->state = NC_T_READY;
        } else if (t->wait.deadline_ns && (!earliest || t->wait.deadline_ns < earliest)) {
            earliest = t->wait.deadline_ns;
        }
    }
    k->wait_due_ns = earliest;
    return earliest;
}

/* After something was signaled: wake waiters, and let a higher-priority
 * thread that became ready run first. */
static void signaled(NcKernel *k)
{
    nc_ke_check_waits(k);
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        if (k->threads[i].state == NC_T_READY && k->current && k->threads[i].priority > k->current->priority) {
            k->resched = true;
            return;
        }
    }
}

/* A poll (a zero timeout) found nothing signaled. One poll is NT's: the
 * thread keeps the CPU. A thread polling back to back (Bink's scheduler in
 * Halo, Panzer Dragoon Orta's loader: a million and a half polls a second,
 * each a trip through the kernel here) gives its turn to a ready thread of
 * its priority or above, as if it had slept, and the thread it waits for
 * runs sooner. With nothing else to run, after a millisecond it waits for
 * the next interrupt on the host, 20 us at a time. */
static void poll_spin(NcKernel *k)
{
    const NcThread *cur = k->current;
    static const NcThread *s_last;
    static uint64_t s_last_ns, s_spin_since;
    static unsigned s_polls;
    const uint64_t now = nc_ke_now_ns();
    if (s_last != cur || now - s_last_ns > 20000) {
        s_polls = 0;
        s_spin_since = now;
    }
    s_last = cur;
    if (++s_polls >= 64) {
        for (unsigned i = 0; i < k->threads_hi; i++) {
            const NcThread *t = &k->threads[i];
            if (t != cur && t->state == NC_T_READY && !t->suspend && t->priority >= cur->priority) {
                nc_ps_yield(k);     /* behind the ready thread */
                break;
            }
        }
        if (!k->resched && now - s_spin_since > 1000000) {
            nc_irq_idle_wait(0, 20000);
            nc_ke_update_time(k);
        }
    }
    s_last_ns = nc_ke_now_ns();
}

/* Waits on objects with an NT timeout (PLARGE_INTEGER in 100 ns: NULL forever,
 * negative relative, positive absolute system time, zero a poll). An
 * alertable wait runs the thread's queued APCs instead and returns
 * STATUS_USER_APC (apc.c). */
static uint32_t wait(NcKernel *k, const uint32_t *objs, unsigned count, bool wait_all, uint32_t timeout_va,
                     uint32_t cs, bool alertable)
{
    NcThread *t = k->current;
    if (alertable && nc_apc_pending(t) && k->frame) {
        const uint32_t esp = nc_frame_gpr(k->frame, NC_ESP);
        if (nc_apc_begin_frame(k, k->frame, t, esp + k->call_pop, nc_vm_rd32(k->vm, esp))) {
            k->redirected = true;
            return 0;
        }
    }
    NcWait w = {.count = count, .wait_all = wait_all, .cs = cs, .alertable = alertable};
    memcpy(w.objects, objs, count * sizeof(uint32_t));

    int idx = satisfied(k, t, &w);
    if (idx >= 0) {
        take(k, t, &w, idx);
        return (uint32_t)idx;
    }
    const int64_t to = timeout_va ? (int64_t)((uint64_t)nc_vm_rd32(k->vm, timeout_va) |
                                              ((uint64_t)nc_vm_rd32(k->vm, timeout_va + 4) << 32))
                                  : 0;
    if (timeout_va && to == INT64_MIN) {
        /* XAPI's SleepEx(INFINITE) delays by the most negative interval:
         * forever, until alerted. */
        if (!count) {
            t->wait = w;
            t->state = NC_T_WAITING;
            k->resched = true;
            return NC_STATUS_SUCCESS;
        }
    } else if (timeout_va) {
        int64_t rel_100ns = to < 0 ? -to : to - (int64_t)nc_ke_system_time();
        if (rel_100ns <= 0) {
            if (count && t && t->state == NC_T_RUNNING && !k->redirected) {
                poll_spin(k);
            }
            return count ? NC_STATUS_TIMEOUT : NC_STATUS_SUCCESS;
        }
        w.deadline_ns = nc_ke_now_ns() + (uint64_t)rel_100ns * 100u;
        if (!k->wait_due_ns || w.deadline_ns < k->wait_due_ns) {
            k->wait_due_ns = w.deadline_ns;
            nc_kernel_clock_arm(k);
        }
    } else if (!count) {
        return NC_STATUS_SUCCESS;
    }
    t->wait = w;
    t->state = NC_T_WAITING;
    k->resched = true;
    return NC_STATUS_TIMEOUT; /* replaced when the wait ends */
}

/* The running thread waits for work the host does for its kernel call (a
 * synchronous read on the reader thread, io.c): no object, no timeout, not
 * alertable. The call's status comes with nc_ke_wake. */
void nc_ke_block(NcKernel *k)
{
    NcThread *t = k->current;
    memset(&t->wait, 0, sizeof(t->wait));
    t->state = NC_T_WAITING;
    k->resched = true;
}

/* Ends t's nc_ke_block: status in its EAX, ready, ahead of the running
 * thread if its priority is higher. */
void nc_ke_wake(NcKernel *k, NcThread *t, uint32_t status)
{
    nc_context_set_gpr(&t->ctx, NC_EAX, status);
    t->state = NC_T_READY;
    if (k->current && t->priority > k->current->priority) {
        k->resched = true;
    }
}

static uint32_t handle_object(NcKernel *k, uint32_t handle)
{
    /* NtCurrentThread (-2): a thread is signalled only once it ends, so a wait
     * on itself is an (alertable) sleep; a Kakuto Chojin thread sleeps so,
     * and failing at once it spun and starved the main thread at boot */
    if (handle == 0xFFFFFFFEu) {
        return k->current ? k->current->ethread : 0;
    }
    enum NcObType type;
    const uint32_t obj = nc_ob_lookup(k, handle, &type);
    /* A file is waited on through its Event (FILE_OBJECT +0x38): Bink's reads
     * without an event of their own complete there, and its scheduler polls
     * the file handles (Halo's attract movies spun on a header read as one). */
    return obj && type == NC_OB_FILE ? obj + NC_FO_EVENT : obj;
}

/* ------------------------------------------------------------- events */

static void init_header(NcKernel *k, uint32_t obj, uint8_t type, uint8_t size_dwords, int32_t state)
{
    uint8_t *h = nc_vm_host(k->vm, obj);
    h[0] = type;
    h[1] = 0;
    h[2] = size_dwords;
    h[3] = 0;
    set_signal_state(k, obj, state);
    nc_vm_wr32(k->vm, obj + HDR_WAIT_LIST, obj + HDR_WAIT_LIST);
    nc_vm_wr32(k->vm, obj + HDR_WAIT_LIST + 4, obj + HDR_WAIT_LIST);
}

static uint32_t set_event(NcKernel *k, uint32_t ev)
{
    int32_t old = signal_state(k, ev);
    set_signal_state(k, ev, 1);
    signaled(k);
    return (uint32_t)old;
}

uint32_t nc_ke_set_event(NcKernel *k, uint32_t event)
{
    return set_event(k, event);
}

void nc_ke_clear_event(NcKernel *k, uint32_t event)
{
    set_signal_state(k, event, 0);
}

void nc_ke_init_event(NcKernel *k, uint32_t event, unsigned type, bool signaled)
{
    init_header(k, event, (uint8_t)type, 4, signaled ? 1 : 0);
}

/* KeInitializeEvent(Event, Type, State) */
static uint64_t KeInitializeEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    init_header(k, a[0], (uint8_t)a[1], 4, (int32_t)(a[2] & 0xff));
    return 0;
}

/* KeSetEvent(Event, Increment, Wait) -> previous state */
static uint64_t KeSetEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return set_event(k, a[0]);
}

/* KeResetEvent(Event) -> previous state */
static uint64_t KeResetEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    int32_t old = signal_state(k, a[0]);
    set_signal_state(k, a[0], 0);
    return (uint32_t)old;
}

/* KePulseEvent(Event, Increment, Wait) -> previous state */
static uint64_t KePulseEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t old = set_event(k, a[0]);
    set_signal_state(k, a[0], 0);
    return old;
}

/* NtCreateEvent(EventHandle, ObjectAttributes, EventType, InitialState) */
static uint64_t NtCreateEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t ev = nc_ob_create(k, NC_OB_EVENT, 0x10);
    if (!ev) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    init_header(k, ev, (uint8_t)a[2], 4, (int32_t)(a[3] & 0xff));
    nc_vm_wr32(k->vm, a[0], nc_ob_handle(k, ev, NC_OB_EVENT));
    return NC_STATUS_SUCCESS;
}

/* NtSetEvent(EventHandle, PreviousState) */
static uint64_t NtSetEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t ev = handle_object(k, a[0]);
    if (!ev) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t old = set_event(k, ev);
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], old);
    }
    return NC_STATUS_SUCCESS;
}

/* NtClearEvent(EventHandle) */
static uint64_t NtClearEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t ev = handle_object(k, a[0]);
    if (!ev) {
        return NC_STATUS_INVALID_HANDLE;
    }
    set_signal_state(k, ev, 0);
    return NC_STATUS_SUCCESS;
}

/* NtPulseEvent(EventHandle, PreviousState) */
static uint64_t NtPulseEvent(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t ev = handle_object(k, a[0]);
    if (!ev) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t old = set_event(k, ev);
    set_signal_state(k, ev, 0);
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], old);
    }
    return NC_STATUS_SUCCESS;
}

/* --------------------------------------------- semaphores and mutants */

/* KeInitializeSemaphore(Semaphore, Count, Limit) */
static uint64_t KeInitializeSemaphore(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    init_header(k, a[0], OBJ_SEMAPHORE, 5, (int32_t)a[1]);
    nc_vm_wr32(k->vm, a[0] + SEMAPHORE_LIMIT, a[2]);
    return 0;
}

static uint32_t release_semaphore(NcKernel *k, uint32_t sem, int32_t count, uint32_t *status)
{
    int32_t old = signal_state(k, sem);
    int32_t limit = (int32_t)nc_vm_rd32(k->vm, sem + SEMAPHORE_LIMIT);
    if (count <= 0 || old + count > limit) {
        *status = 0xC0000047u; /* STATUS_SEMAPHORE_LIMIT_EXCEEDED */
        return (uint32_t)old;
    }
    set_signal_state(k, sem, old + count);
    signaled(k);
    *status = NC_STATUS_SUCCESS;
    return (uint32_t)old;
}

/* KeReleaseSemaphore(Semaphore, Increment, Adjustment, Wait) -> previous count.
 * Past its limit the Xbox kernel raises STATUS_SEMAPHORE_LIMIT_EXCEEDED (an
 * unhandled one ends the title); here it isn't released, and told once. */
static uint64_t KeReleaseSemaphore(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    static bool told;
    uint32_t status;
    const uint32_t old = release_semaphore(k, a[0], (int32_t)a[2], &status);
    if (status != NC_STATUS_SUCCESS && !told) {
        told = true;
        nc_log("hakuX-native", "ke: KeReleaseSemaphore %08x by %d past its limit %d (count %d): not released",
               a[0], (int32_t)a[2], (int32_t)nc_vm_rd32(k->vm, a[0] + SEMAPHORE_LIMIT), (int32_t)old);
    }
    return old;
}

/* NtCreateSemaphore(SemaphoreHandle, ObjectAttributes, InitialCount, MaximumCount) */
static uint64_t NtCreateSemaphore(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t sem = nc_ob_create(k, NC_OB_SEMAPHORE, 0x14);
    if (!sem) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    init_header(k, sem, OBJ_SEMAPHORE, 5, (int32_t)a[2]);
    nc_vm_wr32(k->vm, sem + SEMAPHORE_LIMIT, a[3]);
    nc_vm_wr32(k->vm, a[0], nc_ob_handle(k, sem, NC_OB_SEMAPHORE));
    return NC_STATUS_SUCCESS;
}

/* NtReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount) */
static uint64_t NtReleaseSemaphore(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t sem = handle_object(k, a[0]);
    if (!sem) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t status;
    uint32_t old = release_semaphore(k, sem, (int32_t)a[1], &status);
    if (a[2] && status == NC_STATUS_SUCCESS) {
        nc_vm_wr32(k->vm, a[2], old);
    }
    return status;
}

static void init_mutant(NcKernel *k, uint32_t m, bool owned)
{
    init_header(k, m, OBJ_MUTANT, 8, owned ? 0 : 1);
    nc_vm_wr32(k->vm, m + MUTANT_OWNER, owned && k->current ? k->current->ethread : 0);
}

static uint32_t release_mutant(NcKernel *k, uint32_t m)
{
    int32_t old = signal_state(k, m);
    set_signal_state(k, m, old + 1);
    if (old + 1 > 0) {
        nc_vm_wr32(k->vm, m + MUTANT_OWNER, 0);
        signaled(k);
    }
    return (uint32_t)old;
}

/* KeInitializeMutant(Mutant, InitialOwner) */
static uint64_t KeInitializeMutant(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    init_mutant(k, a[0], a[1] & 0xff);
    return 0;
}

/* KeReleaseMutant(Mutant, Increment, Abandoned, Wait) -> previous state */
static uint64_t KeReleaseMutant(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return release_mutant(k, a[0]);
}

/* NtCreateMutant(MutantHandle, ObjectAttributes, InitialOwner) */
static uint64_t NtCreateMutant(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t m = nc_ob_create(k, NC_OB_MUTANT, 0x20);
    if (!m) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    init_mutant(k, m, a[2] & 0xff);
    nc_vm_wr32(k->vm, a[0], nc_ob_handle(k, m, NC_OB_MUTANT));
    return NC_STATUS_SUCCESS;
}

/* NtReleaseMutant(MutantHandle, PreviousCount) */
static uint64_t NtReleaseMutant(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t m = handle_object(k, a[0]);
    if (!m) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t old = release_mutant(k, m);
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], old);
    }
    return NC_STATUS_SUCCESS;
}

/* --------------------------------------------------------------- waits */

/* KeWaitForSingleObject(Object, WaitReason, WaitMode, Alertable, Timeout) */
static uint64_t KeWaitForSingleObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return wait(k, &a[0], 1, false, a[4], 0, a[3] & 0xff);
}

/* KeWaitForMultipleObjects(Count, Object[], WaitType, WaitReason, WaitMode, Alertable,
 *   Timeout, WaitBlockArray) */
static uint64_t KeWaitForMultipleObjects(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t objs[NC_MAX_WAIT_OBJECTS];
    unsigned n = a[0] < NC_MAX_WAIT_OBJECTS ? a[0] : NC_MAX_WAIT_OBJECTS;
    for (unsigned i = 0; i < n; i++) {
        objs[i] = nc_vm_rd32(k->vm, a[1] + 4 * i);
    }
    return wait(k, objs, n, a[2] == WAIT_ALL, a[6], 0, a[5] & 0xff);
}

/* NtWaitForSingleObject(Handle, Alertable, Timeout) */
static uint64_t NtWaitForSingleObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t obj = handle_object(k, a[0]);
    return obj ? wait(k, &obj, 1, false, a[2], 0, a[1] & 0xff) : NC_STATUS_INVALID_HANDLE;
}

/* NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, Timeout) */
static uint64_t NtWaitForSingleObjectEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t obj = handle_object(k, a[0]);
    return obj ? wait(k, &obj, 1, false, a[3], 0, a[2] & 0xff) : NC_STATUS_INVALID_HANDLE;
}

/* NtSignalAndWaitForSingleObjectEx(SignalHandle, WaitHandle, WaitMode, Alertable, Timeout):
 * the first object signalled (an event set, a semaphore released by one, a
 * mutant released), then a wait on the second, in the one call (no thread
 * runs between them). A semaphore at its limit is not waited on, as on NT. */
static uint64_t NtSignalAndWaitForSingleObjectEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    enum NcObType type;
    const uint32_t sig = nc_ob_lookup(k, a[0], &type);
    const uint32_t obj = handle_object(k, a[1]);
    if (!sig || !obj) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t status = NC_STATUS_SUCCESS;
    switch (type) {
    case NC_OB_EVENT:
        set_event(k, sig);
        break;
    case NC_OB_SEMAPHORE:
        release_semaphore(k, sig, 1, &status);
        break;
    case NC_OB_MUTANT:
        release_mutant(k, sig);
        break;
    default:
        status = 0xC0000024u;   /* STATUS_OBJECT_TYPE_MISMATCH */
        break;
    }
    return status == NC_STATUS_SUCCESS ? wait(k, &obj, 1, false, a[4], 0, a[3] & 0xff) : status;
}

/* NtWaitForMultipleObjectsEx(Count, Handles, WaitType, WaitMode, Alertable, Timeout) */
static uint64_t NtWaitForMultipleObjectsEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t objs[NC_MAX_WAIT_OBJECTS];
    unsigned n = a[0] < NC_MAX_WAIT_OBJECTS ? a[0] : NC_MAX_WAIT_OBJECTS;
    for (unsigned i = 0; i < n; i++) {
        objs[i] = handle_object(k, nc_vm_rd32(k->vm, a[1] + 4 * i));
        if (!objs[i]) {
            return NC_STATUS_INVALID_HANDLE;
        }
    }
    return wait(k, objs, n, a[2] == WAIT_ALL, a[5], 0, a[4] & 0xff);
}

/* KeDelayExecutionThread(WaitMode, Alertable, Interval) */
static uint64_t KeDelayExecutionThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t st = wait(k, NULL, 0, false, a[2], 0, a[1] & 0xff);
    NcThread *cur = k->current;
    if (cur && cur->state == NC_T_RUNNING && !k->redirected) {
        /* A delay that has nothing to wait for (Sleep(0), or one already
         * elapsed) gives the rest of the slice to a ready thread of the same
         * or higher priority, as NT does. */
        for (unsigned i = 0; i < k->threads_hi; i++) {
            const NcThread *t = &k->threads[i];
            if (t != cur && t->state == NC_T_READY && !t->suspend && t->priority >= cur->priority) {
                nc_ps_yield(k);     /* behind the ready thread */
                break;
            }
        }
        /* A thread spinning on them with nothing else to run: polling (D3D
         * waits on the GPU with Sleep(0)) gets them back at once for its
         * first millisecond; a spin that goes on (Fable's frame limiter
         * spends what is left of the frame, two million calls a second;
         * D3D waiting on a busy GPU) waits for the next interrupt on the
         * host, 20 us at a time. Sleeping from the first call made every
         * short GPU wait 100 us or more. */
        static const NcThread *s_last;
        static uint64_t s_last_ns, s_spin_since;
        const uint64_t now = nc_ke_now_ns();
        const bool spinning = !k->resched && s_last == cur && now - s_last_ns < 20000;
        if (!spinning) {
            s_spin_since = now;
        } else if (now - s_spin_since > 1000000) {
            nc_irq_idle_wait(0, 20000);
            nc_ke_update_time(k);
        } else {
#if defined(__aarch64__)
            /* Polling: a pause of a few ISBs (a fraction of a microsecond)
             * before the guest looks again, as a spin-wait hint, instead of
             * re-entering the kernel back to back. */
            for (int i = 0; i < 8; i++) {
                __asm__ __volatile__("isb" ::: "memory");
            }
#endif
        }
        s_last = cur;
        s_last_ns = nc_ke_now_ns();
    }
    return st;
}

/* NtYieldExecution() */
static uint64_t NtYieldExecution(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_ps_yield(k);         /* behind the ready threads of its priority */
    return NC_STATUS_SUCCESS;
}

/* ----------------------------------------------------- critical sections */

/* RtlInitializeCriticalSection(CriticalSection) */
static uint64_t RtlInitializeCriticalSection(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    init_header(k, a[0], OBJ_SYNCHRONIZATION_EVENT, 4, 0);
    nc_vm_wr32(k->vm, a[0] + CS_LOCK_COUNT, 0xFFFFFFFFu);
    nc_vm_wr32(k->vm, a[0] + CS_RECURSION, 0);
    nc_vm_wr32(k->vm, a[0] + CS_OWNER, 0);
    return 0;
}

/* RtlEnterCriticalSection(CriticalSection) */
static uint64_t RtlEnterCriticalSection(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t cs = a[0];
    const uint32_t me = k->current->ethread;
    int32_t lock = (int32_t)nc_vm_rd32(k->vm, cs + CS_LOCK_COUNT) + 1;
    nc_vm_wr32(k->vm, cs + CS_LOCK_COUNT, (uint32_t)lock);
    if (lock == 0) {
        nc_vm_wr32(k->vm, cs + CS_OWNER, me);
        nc_vm_wr32(k->vm, cs + CS_RECURSION, 1);
    } else if (nc_vm_rd32(k->vm, cs + CS_OWNER) == me) {
        nc_vm_wr32(k->vm, cs + CS_RECURSION, nc_vm_rd32(k->vm, cs + CS_RECURSION) + 1);
    } else {
        wait(k, &cs, 1, false, 0, cs, false); /* the event is at the start of the section */
    }
    return 0;
}

/* RtlLeaveCriticalSection(CriticalSection) */
static uint64_t RtlLeaveCriticalSection(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t cs = a[0];
    uint32_t rec = nc_vm_rd32(k->vm, cs + CS_RECURSION) - 1;
    nc_vm_wr32(k->vm, cs + CS_RECURSION, rec);
    int32_t lock = (int32_t)nc_vm_rd32(k->vm, cs + CS_LOCK_COUNT) - 1;
    nc_vm_wr32(k->vm, cs + CS_LOCK_COUNT, (uint32_t)lock);
    if (rec) {
        return 0;
    }
    nc_vm_wr32(k->vm, cs + CS_OWNER, 0);
    if (lock >= 0) {
        set_event(k, cs); /* a waiter takes it over */
    }
    return 0;
}

/* RtlTryEnterCriticalSection(CriticalSection) -> BOOLEAN */
static uint64_t RtlTryEnterCriticalSection(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t cs = a[0];
    const uint32_t me = k->current->ethread;
    if ((int32_t)nc_vm_rd32(k->vm, cs + CS_LOCK_COUNT) == -1) {
        nc_vm_wr32(k->vm, cs + CS_LOCK_COUNT, 0);
        nc_vm_wr32(k->vm, cs + CS_OWNER, me);
        nc_vm_wr32(k->vm, cs + CS_RECURSION, 1);
        return 1;
    }
    if (nc_vm_rd32(k->vm, cs + CS_OWNER) == me) {
        nc_vm_wr32(k->vm, cs + CS_LOCK_COUNT, nc_vm_rd32(k->vm, cs + CS_LOCK_COUNT) + 1);
        nc_vm_wr32(k->vm, cs + CS_RECURSION, nc_vm_rd32(k->vm, cs + CS_RECURSION) + 1);
        return 1;
    }
    return 0;
}

/* KeEnterCriticalRegion() / KeLeaveCriticalRegion(): kernel APCs are not delivered yet. */
static uint64_t KeCriticalRegion(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

/* ---------------------------------------------------------------- IRQL */

static uint8_t *irql(NcKernel *k)
{
    return (uint8_t *)nc_vm_host(k->vm, k->kpcr_va + PCR_IRQL);
}

/* debug: the IRQL changes in the GPU event trace (push-buffer mode) */
static void irql_trace(NcKernel *k, unsigned op, uint8_t old, uint8_t now)
{
    const uint32_t t = k->current ? (uint32_t)(k->current - k->threads) : 0xFF;
    hle_host_gpu_trace(HLE_GTR_IRQL, now | (uint32_t)old << 8 | (t & 0xFF) << 16 | op << 24);
}

/* KfRaiseIrql(NewIrql) -> old */
static uint64_t KfRaiseIrql(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t old = *irql(k);
    *irql(k) = (uint8_t)a[0];
    irql_trace(k, 1, old, (uint8_t)a[0]);
    return old;
}

/* KfLowerIrql(NewIrql) */
static uint64_t KfLowerIrql(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    /* A KIRQL in CL: the rest of ECX is whatever the caller left there
     * (Halo's push-buffer wait: its vblank DPC never ran, the flip stalled). */
    const uint8_t now = (uint8_t)a[0];
    irql_trace(k, 2, *irql(k), now);
    *irql(k) = now;
    if (now < 2 && nc_dpc_pending()) {
        k->resched = true; /* DPCs queued while the IRQL was raised run now */
    }
    return 0;
}

/* KeRaiseIrqlToDpcLevel() -> old */
static uint64_t KeRaiseIrqlToDpcLevel(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t old = *irql(k);
    irql_trace(k, 3, old, 2);
    *irql(k) = 2;
    return old;
}

/* KeRaiseIrqlToSynchLevel() -> old */
static uint64_t KeRaiseIrqlToSynchLevel(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t old = *irql(k);
    *irql(k) = 28;
    return old;
}

/* KeGetCurrentIrql() */
static uint64_t KeGetCurrentIrql(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return *irql(k);
}

/* KeGetCurrentThread(): the KPCR's current thread (guest code, but for NC_NO_FAST) */
static uint64_t KeGetCurrentThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_vm_rd32(k->vm, k->kpcr_va + 0x28);
}

/* ---------------------------------------------------------- interlocked */

/* The Interlocked family (fastcall: ECX, EDX, then the stack) runs as guest
 * code (kernel.c's install_fast_paths); these are the same in the kernel,
 * for NC_NO_FAST. One guest CPU, and the kernel runs it: nothing to lock. */
static uint64_t InterlockedIncrement(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t v = nc_vm_rd32(k->vm, a[0]) + 1;
    nc_vm_wr32(k->vm, a[0], v);
    return v;
}

static uint64_t InterlockedDecrement(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t v = nc_vm_rd32(k->vm, a[0]) - 1;
    nc_vm_wr32(k->vm, a[0], v);
    return v;
}

/* InterlockedExchange(Target, Value) -> the old value */
static uint64_t InterlockedExchange(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t old = nc_vm_rd32(k->vm, a[0]);
    nc_vm_wr32(k->vm, a[0], a[1]);
    return old;
}

/* InterlockedExchangeAdd(Addend, Value) -> the old value */
static uint64_t InterlockedExchangeAdd(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t old = nc_vm_rd32(k->vm, a[0]);
    nc_vm_wr32(k->vm, a[0], old + a[1]);
    return old;
}

/* InterlockedCompareExchange(Destination, ExChange, Comparand) -> the old value */
static uint64_t InterlockedCompareExchange(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t old = nc_vm_rd32(k->vm, a[0]);
    if (old == a[2]) {
        nc_vm_wr32(k->vm, a[0], a[1]);
    }
    return old;
}

/* ---------------------------------------------------------------- time */

/* KeQuerySystemTime(CurrentTime) */
static uint64_t KeQuerySystemTime(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint64_t t = nc_ke_system_time();
    nc_vm_wr32(k->vm, a[0], (uint32_t)t);
    nc_vm_wr32(k->vm, a[0] + 4, (uint32_t)(t >> 32));
    return 0;
}

/* NtSetSystemTime(SystemTime, PreviousTime) */
static uint64_t NtSetSystemTime(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint64_t old = nc_ke_system_time();
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], (uint32_t)old);
        nc_vm_wr32(k->vm, a[1] + 4, (uint32_t)(old >> 32));
    }
    if (a[0]) {
        const uint64_t t = (uint64_t)nc_vm_rd32(k->vm, a[0]) | ((uint64_t)nc_vm_rd32(k->vm, a[0] + 4) << 32);
        if ((int64_t)t < 0) {
            return NC_STATUS_INVALID_PARAMETER;
        }
        g_time_offset += t - old;
        nc_ke_update_time(k);
    }
    return NC_STATUS_SUCCESS;
}

void nc_kernel_set_clock(int64_t offset_ms)
{
    g_clock_ms = offset_ms;
}

/* The console's clock at boot: the phone's time moved by the settings'
 * offset, never before BOOT_FILETIME. */
static void clock_boot(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* Unix time is 11644473600 s past the FILETIME's 1601 */
    const int64_t t = ((int64_t)ts.tv_sec + 11644473600ll) * 10000000ll + ts.tv_nsec / 100 + g_clock_ms * 10000;
    g_time_offset = t > (int64_t)BOOT_FILETIME ? (uint64_t)t - BOOT_FILETIME : 0;
    const time_t s = (time_t)((BOOT_FILETIME + g_time_offset) / 10000000u) - (time_t)11644473600ll;
    struct tm tm;
    gmtime_r(&s, &tm);
    nc_log("hakuX-native", "ke: console clock %04d-%02d-%02d %02d:%02d:%02d UTC, %s", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
           g_clock_ms ? "as set in the settings" : "the phone's time");
}

/* KeQueryInterruptTime() -> 100 ns since boot */
static uint64_t KeQueryInterruptTime(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return since_boot_100ns();
}

/* KeQueryPerformanceCounter() */
static uint64_t KeQueryPerformanceCounter(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint64_t ns = nc_ke_now_ns() - g_boot_ns;
    return (uint64_t)((unsigned __int128)ns * PERF_FREQUENCY / 1000000000u);
}

/* KeQueryPerformanceFrequency() */
static uint64_t KeQueryPerformanceFrequency(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return PERF_FREQUENCY;
}

/* ------------------------------------------------------ thread priority */

/* The thread a KTHREAD is, or NULL: one that has exited (its object lives on
 * while handles to it do) takes priority changes to nowhere, as on NT. They
 * went to the calling thread instead: Panzer Dragoon Orta raises a finished
 * worker to 15 as it shuts the group down, its main thread got that, and at
 * the next stage it polled a worker of priority 9 that never ran again. */
static NcThread *thread_arg(NcKernel *k, uint32_t kthread)
{
    return nc_ps_thread_of(k, kthread);
}

#define BASE_PRIORITY 8

/* KeSetBasePriorityThread(Thread, Increment) -> old increment. An increment of
 * +-16 saturates to the top or bottom of the dynamic range. */
static uint64_t KeSetBasePriorityThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = thread_arg(k, a[0]);
    if (!t) {
        return 0;
    }
    int old = t->priority - BASE_PRIORITY;
    int inc = (int32_t)a[1];
    int p = inc >= 16 ? 15 : inc <= -16 ? 1 : BASE_PRIORITY + inc;
    t->priority = p < 1 ? 1 : p > 31 ? 31 : p;
    k->resched = true;
    return (uint32_t)old;
}

/* KeQueryBasePriorityThread(Thread) -> increment */
static uint64_t KeQueryBasePriorityThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NcThread *t = thread_arg(k, a[0]);
    return t ? (uint32_t)(t->priority - BASE_PRIORITY) : 0;
}

/* KeSetPriorityThread(Thread, Priority) -> old priority */
static uint64_t KeSetPriorityThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = thread_arg(k, a[0]);
    if (!t) {
        return 0;
    }
    int old = t->priority;
    t->priority = (int32_t)a[1];
    k->resched = true;
    return (uint32_t)old;
}

/* KeSetDisableBoostThread(Thread, Disable) -> old; KeBoostPriorityThread: no boosts here. */
static uint64_t KeNoBoost(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

void nc_ke_register(NcKernel *k)
{
    g_boot_ns = nc_ke_now_ns();
    clock_boot();
    nc_ke_update_time(k);

    nc_kernel_impl(k, "KeInitializeEvent", KeInitializeEvent);
    nc_kernel_impl(k, "KeSetEvent", KeSetEvent);
    nc_kernel_impl(k, "KeResetEvent", KeResetEvent);
    nc_kernel_impl(k, "KePulseEvent", KePulseEvent);
    nc_kernel_impl(k, "NtCreateEvent", NtCreateEvent);
    nc_kernel_impl(k, "NtSetEvent", NtSetEvent);
    nc_kernel_impl(k, "NtClearEvent", NtClearEvent);
    nc_kernel_impl(k, "NtPulseEvent", NtPulseEvent);

    nc_kernel_impl(k, "KeInitializeSemaphore", KeInitializeSemaphore);
    nc_kernel_impl(k, "KeReleaseSemaphore", KeReleaseSemaphore);
    nc_kernel_impl(k, "NtCreateSemaphore", NtCreateSemaphore);
    nc_kernel_impl(k, "NtReleaseSemaphore", NtReleaseSemaphore);
    nc_kernel_impl(k, "KeInitializeMutant", KeInitializeMutant);
    nc_kernel_impl(k, "KeReleaseMutant", KeReleaseMutant);
    nc_kernel_impl(k, "NtCreateMutant", NtCreateMutant);
    nc_kernel_impl(k, "NtReleaseMutant", NtReleaseMutant);

    nc_kernel_impl(k, "KeWaitForSingleObject", KeWaitForSingleObject);
    nc_kernel_impl(k, "KeWaitForMultipleObjects", KeWaitForMultipleObjects);
    nc_kernel_impl(k, "NtWaitForSingleObject", NtWaitForSingleObject);
    nc_kernel_impl(k, "NtWaitForSingleObjectEx", NtWaitForSingleObjectEx);
    nc_kernel_impl(k, "NtSignalAndWaitForSingleObjectEx", NtSignalAndWaitForSingleObjectEx);
    nc_kernel_impl(k, "NtWaitForMultipleObjectsEx", NtWaitForMultipleObjectsEx);
    nc_kernel_impl(k, "KeDelayExecutionThread", KeDelayExecutionThread);
    nc_kernel_impl(k, "NtYieldExecution", NtYieldExecution);

    nc_kernel_impl(k, "RtlInitializeCriticalSection", RtlInitializeCriticalSection);
    nc_kernel_impl(k, "RtlEnterCriticalSection", RtlEnterCriticalSection);
    nc_kernel_impl(k, "RtlLeaveCriticalSection", RtlLeaveCriticalSection);
    nc_kernel_impl(k, "RtlTryEnterCriticalSection", RtlTryEnterCriticalSection);
    nc_kernel_impl(k, "KeEnterCriticalRegion", KeCriticalRegion);
    nc_kernel_impl(k, "KeLeaveCriticalRegion", KeCriticalRegion);

    nc_kernel_impl(k, "KfRaiseIrql", KfRaiseIrql);
    nc_kernel_impl(k, "KfLowerIrql", KfLowerIrql);
    nc_kernel_impl(k, "KeRaiseIrqlToDpcLevel", KeRaiseIrqlToDpcLevel);
    nc_kernel_impl(k, "KeRaiseIrqlToSynchLevel", KeRaiseIrqlToSynchLevel);
    nc_kernel_impl(k, "KeGetCurrentIrql", KeGetCurrentIrql);
    nc_kernel_impl(k, "KeGetCurrentThread", KeGetCurrentThread);
    nc_kernel_impl(k, "InterlockedIncrement", InterlockedIncrement);
    nc_kernel_impl(k, "InterlockedDecrement", InterlockedDecrement);
    nc_kernel_impl(k, "InterlockedExchange", InterlockedExchange);
    nc_kernel_impl(k, "InterlockedExchangeAdd", InterlockedExchangeAdd);
    nc_kernel_impl(k, "InterlockedCompareExchange", InterlockedCompareExchange);

    nc_kernel_impl(k, "KeQuerySystemTime", KeQuerySystemTime);
    nc_kernel_impl(k, "NtSetSystemTime", NtSetSystemTime);
    nc_kernel_impl(k, "KeQueryInterruptTime", KeQueryInterruptTime);
    nc_kernel_impl(k, "KeQueryPerformanceCounter", KeQueryPerformanceCounter);
    nc_kernel_impl(k, "KeQueryPerformanceFrequency", KeQueryPerformanceFrequency);

    nc_kernel_impl(k, "KeSetBasePriorityThread", KeSetBasePriorityThread);
    nc_kernel_impl(k, "KeQueryBasePriorityThread", KeQueryBasePriorityThread);
    nc_kernel_impl(k, "KeSetPriorityThread", KeSetPriorityThread);
    nc_kernel_impl(k, "KeSetDisableBoostThread", KeNoBoost);
    nc_kernel_impl(k, "KeBoostPriorityThread", KeNoBoost);
}

void nc_ke_reset(void)
{
    g_boot_ns = 0;
    g_time_offset = 0;          /* clock_boot sets it again (g_clock_ms, the front end's, stays) */
    g_entry = false;
    g_entry_ns = g_entry_coarse = 0;
    memset(&g_entry_thread, 0, sizeof(g_entry_thread));
    g_tseq = 0;
    g_tpaused = false;
    g_tpause_raw = g_off = 0;
    g_refreshes = 0;
    g_pub_ns = g_pub_coarse = 0;
    g_times_t = 0;              /* else the next boot's tick counts stay at this one's last */
}
