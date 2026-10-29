/*
 * hakuX native core — timers and DPCs.
 *
 * KTIMER (nxdk xboxkrnl.h, CC0): Header 0x00 (Type 8 notification / 9
 * synchronization, Inserted 0x03, SignalState 0x04), DueTime 0x10 (interrupt
 * time), TimerListEntry 0x18, Dpc 0x20, Period 0x24 (ms); 0x28 bytes.
 * KDPC: Type 0x00, Inserted 0x02, DpcListEntry 0x04, DeferredRoutine 0x0C,
 * DeferredContext 0x10, SystemArgument1 0x14, SystemArgument2 0x18.
 *
 * Inserted timers are kept on the host with a host-monotonic due time. They
 * are checked on kernel calls and while the scheduler idles; an expired timer
 * is signaled, queues its DPC and, if periodic, is inserted again.
 *
 * DPCs run as guest code on a kernel thread that outranks every guest thread,
 * at DISPATCH_LEVEL: DeferredRoutine(Dpc, DeferredContext, SystemArgument1,
 * SystemArgument2), returning to a stub that runs the next one. They are
 * dispatched when the scheduler runs below DISPATCH_LEVEL, so they don't
 * interrupt guest code between kernel calls yet.
 */
#include "native/kernel/kernel.h"
#include "hw/xbox/hle/host.h"

#define KT_INSERTED 0x03
#define KT_SIGNAL   0x04
#define KT_DUE      0x10
#define KT_LIST     0x18
#define KT_DPC      0x20
#define KT_PERIOD   0x24
#define KTIMER_SIZE 0x28

#define DPC_INSERTED 0x02
#define DPC_ROUTINE  0x0C
#define DPC_CONTEXT  0x10
#define DPC_ARG1     0x14
#define DPC_ARG2     0x18

#define DISPATCH_LEVEL 2
#define DPC_STACK      0x4000u
#define DPC_PRIORITY   1000

#define MAX_TIMERS 512
#define MAX_DPCS   256

typedef struct NcTimerEntry {
    uint32_t timer;
    uint64_t due_ns;
    uint64_t period_ns;
    /* NtSetTimerEx's TimerApcRoutine(TimerContext, low, high), queued to the
     * thread that set it (ETHREAD) each time the timer expires; 0: none */
    uint32_t apc, apc_ctx, apc_thread;
} NcTimerEntry;

static NcTimerEntry g_timers[MAX_TIMERS];
static unsigned g_ntimers;

static uint32_t g_dpcs[MAX_DPCS];
static uint64_t g_dpc_t[MAX_DPCS];      /* when each went in (host ns) */
static unsigned g_dpc_head, g_ndpcs;

/* DPC timing: the one running, and the one before it. A DPC that waits long
 * to run holds up what it does (the HLE GPU's vblank DPC presents frames). */
static struct {
    uint32_t routine, prev_routine;
    uint64_t start_ns, wait_ns, prev_run_ns;
    unsigned logged;
    uint64_t log_window_ns;
    /* dispatches refused since the last one ran: a DPC running, the IRQL
     * (and its value); the running DPC's, copied when it starts */
    unsigned refused_dpc, refused_irql, run_refused_dpc, run_refused_irql;
    uint8_t refused_irql_val, run_refused_irql_val;
} g_dpc_run;
#define DPC_SLOW_NS 8000000ull

static uint8_t *irql(NcKernel *k)
{
    return (uint8_t *)nc_vm_host(k->vm, k->kpcr_va + 0x24);
}

static void next_due(NcKernel *k)
{
    uint64_t due = 0;
    for (unsigned i = 0; i < g_ntimers; i++) {
        if (!due || g_timers[i].due_ns < due) {
            due = g_timers[i].due_ns;
        }
    }
    k->timer_due_ns = due;
    nc_kernel_clock_arm(k);
}

/* ----------------------------------------------------------------- DPCs */

/* Whether DPCs are queued, where the guest-code KfLowerIrql reads it
 * (fs:NC_KPCR_DPC_PENDING): it enters the kernel only then. Kept with the
 * queue, on the guest CPU's thread. */
void nc_dpc_sync(NcKernel *k)
{
    *(uint8_t *)nc_vm_host(k->vm, k->kpcr_va + NC_KPCR_DPC_PENDING) = g_ndpcs != 0;
}

bool nc_dpc_queue(NcKernel *k, uint32_t dpc, uint32_t arg1, uint32_t arg2)
{
    uint8_t *d = nc_vm_host(k->vm, dpc);
    if (d[DPC_INSERTED] || g_ndpcs == MAX_DPCS) {
        return false;
    }
    d[DPC_INSERTED] = 1;
    hle_host_gpu_trace(HLE_GTR_DPC_QUEUE, nc_vm_rd32(k->vm, dpc + DPC_ROUTINE));
    nc_vm_wr32(k->vm, dpc + DPC_ARG1, arg1);
    nc_vm_wr32(k->vm, dpc + DPC_ARG2, arg2);
    g_dpc_t[(g_dpc_head + g_ndpcs) % MAX_DPCS] = nc_ke_now_ns();
    g_dpcs[(g_dpc_head + g_ndpcs++) % MAX_DPCS] = dpc;
    nc_dpc_sync(k);
    if (*irql(k) < DISPATCH_LEVEL) {
        k->resched = true;
    }
    return true;
}

bool nc_dpc_pending(void)
{
    return g_ndpcs != 0;
}

static bool dpc_unqueue(NcKernel *k, uint32_t dpc)
{
    for (unsigned i = 0; i < g_ndpcs; i++) {
        if (g_dpcs[(g_dpc_head + i) % MAX_DPCS] == dpc) {
            for (unsigned j = i; j + 1 < g_ndpcs; j++) {
                g_dpcs[(g_dpc_head + j) % MAX_DPCS] = g_dpcs[(g_dpc_head + j + 1) % MAX_DPCS];
                g_dpc_t[(g_dpc_head + j) % MAX_DPCS] = g_dpc_t[(g_dpc_head + j + 1) % MAX_DPCS];
            }
            g_ndpcs--;
            nc_dpc_sync(k);
            ((uint8_t *)nc_vm_host(k->vm, dpc))[DPC_INSERTED] = 0;
            return true;
        }
    }
    return false;
}

void nc_dpc_dispatch(NcKernel *k)
{
    if (!g_ndpcs) {
        return;
    }
    if (k->in_dpc || *irql(k) >= DISPATCH_LEVEL) {
        hle_host_gpu_trace(HLE_GTR_DPC_REFUSED, *irql(k) | (k->in_dpc ? 0x100u : 0));
        if (k->in_dpc) {
            g_dpc_run.refused_dpc++;
        } else {
            g_dpc_run.refused_irql++;
            g_dpc_run.refused_irql_val = *irql(k);
        }
        return;
    }
    if (!k->dpc_thread) {
        k->dpc_thread = nc_ps_kernel_thread(k, DPC_STACK, DPC_PRIORITY);
        if (!k->dpc_thread) {
            return;
        }
    }
    const uint32_t dpc = g_dpcs[g_dpc_head];
    hle_host_gpu_trace(HLE_GTR_DPC_RUN, nc_vm_rd32(k->vm, dpc + DPC_ROUTINE));
    const uint64_t now = nc_ke_now_ns();
    g_dpc_run.routine = nc_vm_rd32(k->vm, dpc + DPC_ROUTINE);
    g_dpc_run.start_ns = now;
    g_dpc_run.wait_ns = now - g_dpc_t[g_dpc_head];
    g_dpc_run.run_refused_dpc = g_dpc_run.refused_dpc;
    g_dpc_run.run_refused_irql = g_dpc_run.refused_irql;
    g_dpc_run.run_refused_irql_val = g_dpc_run.refused_irql_val;
    g_dpc_run.refused_dpc = g_dpc_run.refused_irql = 0;
    g_dpc_head = (g_dpc_head + 1) % MAX_DPCS;
    g_ndpcs--;
    nc_dpc_sync(k);
    ((uint8_t *)nc_vm_host(k->vm, dpc))[DPC_INSERTED] = 0;

    NcThread *t = k->dpc_thread;
    const uint32_t esp = nc_ps_frame_top(t) - 16 - 20;
    nc_vm_wr32(k->vm, esp, k->dpc_return_va);
    nc_vm_wr32(k->vm, esp + 4, dpc);
    nc_vm_wr32(k->vm, esp + 8, nc_vm_rd32(k->vm, dpc + DPC_CONTEXT));
    nc_vm_wr32(k->vm, esp + 12, nc_vm_rd32(k->vm, dpc + DPC_ARG1));
    nc_vm_wr32(k->vm, esp + 16, nc_vm_rd32(k->vm, dpc + DPC_ARG2));
    nc_context_init(k->cpu, &t->ctx, nc_vm_rd32(k->vm, dpc + DPC_ROUTINE), esp);
    t->fresh = true;
    t->state = NC_T_READY;
    k->in_dpc = true;
    k->dpc_saved_irql = *irql(k);
    *irql(k) = DISPATCH_LEVEL;
}

void nc_dpc_return(NcKernel *k)
{
    const uint64_t now = nc_ke_now_ns(), run = now - g_dpc_run.start_ns, wait = g_dpc_run.wait_ns;
    if (wait > g_nc_sched.dpc_wait_max_ns) {
        g_nc_sched.dpc_wait_max_ns = wait;
    }
    if (run > g_nc_sched.dpc_run_max_ns) {
        g_nc_sched.dpc_run_max_ns = run;
    }
    if (wait > DPC_SLOW_NS || run > DPC_SLOW_NS) {
        g_nc_sched.dpc_slow++;
        if (now - g_dpc_run.log_window_ns > 1000000000ull) {
            g_dpc_run.log_window_ns = now;
            g_dpc_run.logged = 0;
        }
        if (g_dpc_run.logged++ < 20) {
            nc_log("hakuX-native-dpc", "dpc %08x waited %.1f ms, ran %.1f ms (the one before: %08x, %.1f ms; "
                   "refused meanwhile: %u for a DPC running, %u for the IRQL (%u))",
                   g_dpc_run.routine, wait / 1e6, run / 1e6, g_dpc_run.prev_routine, g_dpc_run.prev_run_ns / 1e6,
                   g_dpc_run.run_refused_dpc, g_dpc_run.run_refused_irql, g_dpc_run.run_refused_irql_val);
        }
    }
    g_dpc_run.prev_routine = g_dpc_run.routine;
    g_dpc_run.prev_run_ns = run;
    k->in_dpc = false;
    *irql(k) = k->dpc_saved_irql;
    if (k->dpc_thread) {
        k->dpc_thread->state = NC_T_SUSPENDED;
    }
    k->resched = true;
}

/* KeInsertQueueDpc(Dpc, SystemArgument1, SystemArgument2) -> TRUE if queued now */
static uint64_t KeInsertQueueDpc(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_dpc_queue(k, a[0], a[1], a[2]);
}

/* KeRemoveQueueDpc(Dpc) -> TRUE if it was queued */
static uint64_t KeRemoveQueueDpc(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return dpc_unqueue(k, a[0]);
}

/* KeIsExecutingDpc() */
static uint64_t KeIsExecutingDpc(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return k->in_dpc;
}

/* --------------------------------------------------------------- timers */

static int timer_find(uint32_t timer)
{
    for (unsigned i = 0; i < g_ntimers; i++) {
        if (g_timers[i].timer == timer) {
            return (int)i;
        }
    }
    return -1;
}

static bool timer_cancel(NcKernel *k, uint32_t timer)
{
    int i = timer_find(timer);
    uint8_t *t = nc_vm_host(k->vm, timer);
    t[KT_INSERTED] = 0;
    if (i < 0) {
        return false;
    }
    g_timers[i] = g_timers[--g_ntimers];
    next_due(k);
    return true;
}

static void timer_init(NcKernel *k, uint32_t timer, uint32_t type)
{
    uint8_t *t = nc_vm_host(k->vm, timer);
    timer_cancel(k, timer);
    memset(t, 0, KTIMER_SIZE);
    t[0] = (uint8_t)(8 + (type & 1));
    t[2] = KTIMER_SIZE / 4;
    nc_vm_wr32(k->vm, timer + 0x08, timer + 0x08);
    nc_vm_wr32(k->vm, timer + 0x0C, timer + 0x08);
    nc_vm_wr32(k->vm, timer + KT_LIST, timer + KT_LIST);
    nc_vm_wr32(k->vm, timer + KT_LIST + 4, timer + KT_LIST);
}

/* Inserts the timer: due is an NT time (negative relative, else absolute
 * system time, in 100 ns), period in ms. Returns whether it was inserted. */
static bool timer_set(NcKernel *k, uint32_t timer, int64_t due, uint32_t period_ms, uint32_t dpc)
{
    const bool was = timer_cancel(k, timer);
    uint8_t *t = nc_vm_host(k->vm, timer);
    nc_vm_wr32(k->vm, timer + KT_SIGNAL, 0);
    nc_vm_wr32(k->vm, timer + KT_DPC, dpc);
    nc_vm_wr32(k->vm, timer + KT_PERIOD, period_ms);

    int64_t rel = due < 0 ? -due : due - (int64_t)nc_ke_system_time();
    if (rel < 0) {
        rel = 0;
    }
    const uint64_t at = nc_ke_interrupt_time() + (uint64_t)rel;
    nc_vm_wr32(k->vm, timer + KT_DUE, (uint32_t)at);
    nc_vm_wr32(k->vm, timer + KT_DUE + 4, (uint32_t)(at >> 32));

    if (g_ntimers == MAX_TIMERS) {
        nc_log("hakuX-native", "ke: out of timer slots");
        return was;
    }
    g_timers[g_ntimers] = (NcTimerEntry){.timer = timer,
                                         .due_ns = nc_ke_now_ns() + (uint64_t)rel * 100u,
                                         .period_ns = (uint64_t)period_ms * 1000000u};
    g_ntimers++;
    t[KT_INSERTED] = 1;
    next_due(k);
    return was;
}

uint64_t nc_timer_poll(NcKernel *k, bool *fired)
{
    *fired = false;
    if (!k->timer_due_ns) {
        return 0;
    }
    const uint64_t now = nc_ke_now_ns();
    if (now < k->timer_due_ns) {
        return k->timer_due_ns;
    }
    const uint64_t systime = nc_ke_system_time();
    for (unsigned i = 0; i < g_ntimers;) {
        if (g_timers[i].due_ns > now) {
            i++;
            continue;
        }
        const uint32_t timer = g_timers[i].timer;
        nc_vm_wr32(k->vm, timer + KT_SIGNAL, 1);
        const uint32_t dpc = nc_vm_rd32(k->vm, timer + KT_DPC);
        if (dpc) {
            nc_dpc_queue(k, dpc, (uint32_t)systime, (uint32_t)(systime >> 32));
        }
        NcThread *at = g_timers[i].apc ? nc_ps_thread_of(k, g_timers[i].apc_thread) : NULL;
        if (at) {
            /* delivered when the thread next waits alertably, as NT's user APCs */
            const NcApc apc = {.normal = g_timers[i].apc, .ctx = g_timers[i].apc_ctx,
                               .a1 = (uint32_t)systime, .a2 = (uint32_t)(systime >> 32)};
            nc_apc_queue(k, at, &apc);
        }
        *fired = true;
        if (g_timers[i].period_ns) {
            uint64_t due = g_timers[i].due_ns + g_timers[i].period_ns;
            g_timers[i].due_ns = due > now ? due : now + g_timers[i].period_ns;
            i++;
        } else {
            ((uint8_t *)nc_vm_host(k->vm, timer))[KT_INSERTED] = 0;
            g_timers[i] = g_timers[--g_ntimers];
        }
    }
    next_due(k);
    return k->timer_due_ns;
}

static int64_t rd64(NcKernel *k, uint32_t va)
{
    return (int64_t)((uint64_t)nc_vm_rd32(k->vm, va) | ((uint64_t)nc_vm_rd32(k->vm, va + 4) << 32));
}

/* KeInitializeTimerEx(Timer, Type) */
static uint64_t KeInitializeTimerEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    timer_init(k, a[0], a[1]);
    return 0;
}

/* KeSetTimer(Timer, DueTime (by value), Dpc) -> was inserted */
static uint64_t KeSetTimer(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return timer_set(k, a[0], (int64_t)((uint64_t)a[1] | ((uint64_t)a[2] << 32)), 0, a[3]);
}

/* KeSetTimerEx(Timer, DueTime (by value), Period, Dpc) -> was inserted */
static uint64_t KeSetTimerEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return timer_set(k, a[0], (int64_t)((uint64_t)a[1] | ((uint64_t)a[2] << 32)), a[3], a[4]);
}

/* KeCancelTimer(Timer) -> was inserted */
static uint64_t KeCancelTimer(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return timer_cancel(k, a[0]);
}

/* NtCreateTimer(TimerHandle, ObjectAttributes, TimerType) */
static uint64_t NtCreateTimer(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t timer = nc_ob_create(k, NC_OB_TIMER, KTIMER_SIZE);
    if (!timer) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    timer_init(k, timer, a[2]);
    nc_vm_wr32(k->vm, a[0], nc_ob_handle(k, timer, NC_OB_TIMER));
    return NC_STATUS_SUCCESS;
}

static uint32_t timer_of(NcKernel *k, uint32_t handle)
{
    enum NcObType type;
    uint32_t obj = nc_ob_lookup(k, handle, &type);
    return obj && type == NC_OB_TIMER ? obj : 0;
}

/* NtSetTimerEx(TimerHandle, DueTime*, TimerApcRoutine, ApcMode, TimerContext, ResumeTimer,
 *   Period, PreviousState*) */
static uint64_t NtSetTimerEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t timer = timer_of(k, a[0]);
    if (!timer) {
        return NC_STATUS_INVALID_HANDLE;
    }
    if (a[7]) {
        *(uint8_t *)nc_vm_host(k->vm, a[7]) = nc_vm_rd32(k->vm, timer + KT_SIGNAL) != 0;
    }
    timer_set(k, timer, rd64(k, a[1]), a[6], 0);
    const int i = timer_find(timer);
    if (i >= 0 && a[2] && k->current) {
        g_timers[i].apc = a[2];
        g_timers[i].apc_ctx = a[4];
        g_timers[i].apc_thread = k->current->ethread;
    }
    return NC_STATUS_SUCCESS;
}

/* NtCancelTimer(TimerHandle, CurrentState*) */
static uint64_t NtCancelTimer(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t timer = timer_of(k, a[0]);
    if (!timer) {
        return NC_STATUS_INVALID_HANDLE;
    }
    timer_cancel(k, timer);
    if (a[1]) {
        *(uint8_t *)nc_vm_host(k->vm, a[1]) = nc_vm_rd32(k->vm, timer + KT_SIGNAL) != 0;
    }
    return NC_STATUS_SUCCESS;
}

/* NtQueryTimer(TimerHandle, TimerInformation): TIMER_BASIC_INFORMATION { RemainingTime, TimerState } */
static uint64_t NtQueryTimer(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t timer = timer_of(k, a[0]);
    if (!timer) {
        return NC_STATUS_INVALID_HANDLE;
    }
    int i = timer_find(timer);
    const uint64_t now = nc_ke_now_ns();
    const uint64_t left = i >= 0 && g_timers[i].due_ns > now ? (g_timers[i].due_ns - now) / 100u : 0;
    nc_vm_wr32(k->vm, a[1], (uint32_t)left);
    nc_vm_wr32(k->vm, a[1] + 4, (uint32_t)(left >> 32));
    *(uint8_t *)nc_vm_host(k->vm, a[1] + 8) = nc_vm_rd32(k->vm, timer + KT_SIGNAL) != 0;
    return NC_STATUS_SUCCESS;
}

void nc_timer_register(NcKernel *k)
{
    nc_kernel_impl(k, "KeInitializeTimerEx", KeInitializeTimerEx);
    nc_kernel_impl(k, "KeSetTimer", KeSetTimer);
    nc_kernel_impl(k, "KeSetTimerEx", KeSetTimerEx);
    nc_kernel_impl(k, "KeCancelTimer", KeCancelTimer);
    nc_kernel_impl(k, "NtCreateTimer", NtCreateTimer);
    nc_kernel_impl(k, "NtSetTimerEx", NtSetTimerEx);
    nc_kernel_impl(k, "NtCancelTimer", NtCancelTimer);
    nc_kernel_impl(k, "NtQueryTimer", NtQueryTimer);
    nc_kernel_impl(k, "KeInsertQueueDpc", KeInsertQueueDpc);
    nc_kernel_impl(k, "KeRemoveQueueDpc", KeRemoveQueueDpc);
    nc_kernel_impl(k, "KeIsExecutingDpc", KeIsExecutingDpc);
}

/* ---------------------------------------------------------- savestates */

typedef struct NcDpcEntry {
    uint32_t dpc;
    uint64_t queued_ns;
} NcDpcEntry;

typedef struct NcTimerState {
    uint32_t ntimers, ndpcs;
} NcTimerState;

#define NC_DEF_SECTION TIMR
#define NC_DEF_FILE "native/state/timer.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_timr_state_save(NcBuf *b)
{
    const NcTimerState st = {g_ntimers, g_ndpcs};
    nc_rec_put(b, &nc_rec_NcTimerState, &st);
    nc_recs_put(b, &nc_rec_NcTimerEntry, g_timers, sizeof(g_timers[0]), g_ntimers);
    for (unsigned i = 0; i < g_ndpcs; i++) {
        const NcDpcEntry e = {g_dpcs[(g_dpc_head + i) % MAX_DPCS], g_dpc_t[(g_dpc_head + i) % MAX_DPCS]};
        nc_rec_put(b, &nc_rec_NcDpcEntry, &e);
    }
    return true;
}

bool nc_timr_state_load(NcRd *r, unsigned v)
{
    NcTimerState st = {0};
    nc_rec_get(r, &nc_rec_NcTimerState, &st, v);
    if (st.ntimers > MAX_TIMERS || st.ndpcs > MAX_DPCS) {
        return false;
    }
    memset(g_timers, 0, sizeof(g_timers));
    nc_recs_get(r, &nc_rec_NcTimerEntry, g_timers, sizeof(g_timers[0]), st.ntimers, v);
    g_ntimers = st.ntimers;
    g_dpc_head = 0;
    for (unsigned i = 0; i < st.ndpcs; i++) {
        NcDpcEntry e = {0};
        nc_rec_get(r, &nc_rec_NcDpcEntry, &e, v);
        g_dpcs[i] = e.dpc;
        g_dpc_t[i] = e.queued_ns;
    }
    g_ndpcs = st.ndpcs;
    return !r->fail;
}

void nc_timer_reset(void)
{
    memset(g_timers, 0, sizeof(g_timers));
    g_ntimers = 0;
    memset(g_dpcs, 0, sizeof(g_dpcs));
    g_dpc_head = g_ndpcs = 0;
    memset(&g_dpc_run, 0, sizeof(g_dpc_run));
}
