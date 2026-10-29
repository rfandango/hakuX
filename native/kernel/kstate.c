/*
 * hakuX native core — the kernel in a savestate: NcKernel's own state, the
 * time and interrupt state of the modules around it (KERN), and the threads
 * (THRD). Thread pointers are stored as slots, CPU contexts by their fields
 * (NcCpuState), never as the JIT's layout.
 *
 * A state is taken where the guest CPU pauses (nc_kernel_pause_point):
 * entering the kernel, the running thread's live frame is at the
 * instruction that entered it, nothing of the call done, so on load the
 * call runs again from the start; idle, every thread is switched out and the
 * scheduler picks again on load.
 */
#include "native/kernel/kernel.h"
#include "native/core/irq.h"

#include <stdlib.h>

typedef struct NcKState {
    uint64_t boot_ns, time_offset, guest_ns;
    uint64_t guest_cycles;
    bool x87_single;
    uint32_t av_saved;
    uint8_t isr_line;
    uint32_t irq_enabled, irq_lines;
} NcKState;

typedef struct NcThreadRefs {
    int32_t current, slice_thread, dpc_thread, isr_thread, isr_interrupted, resume_first;
    uint8_t point;
} NcThreadRefs;

#define NC_DEF_SECTION KERN
#define NC_DEF_FILE "native/state/kernel.def"
#include "native/state/def.h"
#undef NC_DEF_SECTION
#undef NC_DEF_FILE
#define NC_DEF_SECTION THRD
#define NC_DEF_FILE "native/state/thread.def"
#include "native/state/def.h"
#include "native/state/sections.h"

/* the capture's: where the guest CPU paused, and the running thread's live
 * frame (entry) */
static uint8_t g_point;
static NcCpuContext g_live;
static int g_live_slot = -1;

void nc_kstate_capture_frame(NcKernel *k, const NcFrame *f, bool idle)
{
    g_point = idle ? NC_STATE_AT_IDLE : NC_STATE_AT_ENTRY;
    g_live_slot = -1;
    if (!idle && k->current) {
        nc_frame_save(f, &g_live);
        g_live_slot = (int)(k->current - k->threads);
    }
}

/* ------------------------------------------------------------------ KERN */

bool nc_kern_state_save(NcBuf *b)
{
    NcKernel *k = nc_state_kernel();
    nc_rec_put(b, &nc_rec_NcKernel, k);
    NcKState s = {0};
    nc_ke_state(&s.boot_ns, &s.time_offset, &s.guest_ns);
    /* RDTSC as the pause began: the time paused so far isn't the guest's */
    s.guest_cycles = nc_cpu_guest_cycles(k->cpu) - (nc_cpu_host_cycles() - nc_kernel_pause_cycles());
    s.x87_single = nc_cpu_x87_single(k->cpu);
    s.av_saved = nc_kernel_av_saved();
    s.isr_line = nc_intr_isr_line();
    nc_irq_state(&s.irq_enabled, &s.irq_lines);
    nc_rec_put(b, &nc_rec_NcKState, &s);
    return true;
}

bool nc_kern_state_load(NcRd *r, unsigned v)
{
    NcKernel *k = nc_state_kernel();
    const uint32_t base = k->xbe_base, size = k->xbe_size, kprocess = k->kprocess;
    nc_rec_get(r, &nc_rec_NcKernel, k, v);
    if (v < 2) {
        memset(k->handles_more, 0, sizeof(k->handles_more));    /* its table ended at NC_HANDLES_V1 */
    }
    if (k->xbe_base != base || k->xbe_size != size || k->kprocess != kprocess) {
        nc_log("hakuX-native-state", "the state's image is at %08x+%x (process %08x), this boot's at %08x+%x (%08x)",
               k->xbe_base, k->xbe_size, k->kprocess, base, size, kprocess);
        return false;
    }
    NcKState s = {0};
    s.isr_line = 0xFF;
    nc_rec_get(r, &nc_rec_NcKState, &s, v);
    if (r->fail) {
        return false;
    }
    k->nstack_free = 0;                 /* the boot's slots given back, not the state's */
    nc_ke_state_set(s.boot_ns, s.time_offset, s.guest_ns);
    nc_cpu_guest_cycles_set(k->cpu, s.guest_cycles);
    if (s.x87_single) {
        nc_cpu_x87_single_restore(k->cpu);
    }
    nc_kernel_av_saved_set(s.av_saved);
    nc_intr_isr_line_set(s.isr_line);
    nc_irq_state_set(s.irq_enabled, s.irq_lines);
    nc_log("hakuX-native-state", "kernel: guest time %.3f s, %llu kernel calls", s.guest_ns / 1e9,
           (unsigned long long)k->calls);
    return true;
}

/* ------------------------------------------------------------------ THRD */

static int32_t slot_of(const NcKernel *k, const NcThread *t)
{
    return t ? (int32_t)(t - k->threads) : -1;
}

static NcThread *thread_at(NcKernel *k, int32_t slot)
{
    return slot >= 0 && slot < NC_MAX_THREADS && k->threads[slot].state != NC_T_FREE ? &k->threads[slot] : NULL;
}

bool nc_thrd_state_save(NcBuf *b)
{
    NcKernel *k = nc_state_kernel();
    const NcThreadRefs refs = {slot_of(k, k->current),     slot_of(k, k->slice_thread),
                               slot_of(k, k->dpc_thread),  slot_of(k, k->isr_thread),
                               slot_of(k, k->isr_interrupted), slot_of(k, k->resume_first),
                               g_point};
    nc_rec_put(b, &nc_rec_NcThreadRefs, &refs);
    uint32_t n = 0;
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        n += k->threads[i].state != NC_T_FREE;
    }
    nc_buf_u32(b, n);
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        const NcThread *t = &k->threads[i];
        if (t->state == NC_T_FREE) {
            continue;
        }
        nc_buf_u32(b, i);
        nc_rec_put(b, &nc_rec_NcThread, t);
        NcCpuState cs;
        nc_context_export((int)i == g_live_slot ? &g_live : &t->ctx, &cs);
        nc_rec_put(b, &nc_rec_NcCpuState, &cs);
        nc_recs_put(b, &nc_rec_NcApc, t->apcs, sizeof(NcApc), t->apcs ? t->napcs : 0);
    }
    return true;
}

bool nc_thrd_state_load(NcRd *r, unsigned v)
{
    NcKernel *k = nc_state_kernel();
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        free(k->threads[i].apcs);
    }
    memset(k->threads, 0, sizeof(k->threads));
    NcThreadRefs refs = {-1, -1, -1, -1, -1, -1, NC_STATE_AT_ENTRY};
    nc_rec_get(r, &nc_rec_NcThreadRefs, &refs, v);
    const uint32_t n = nc_rd_u32(r);
    if (n > NC_MAX_THREADS) {
        return false;
    }
    for (uint32_t j = 0; j < n; j++) {
        const uint32_t i = nc_rd_u32(r);
        if (i >= NC_MAX_THREADS || k->threads[i].state != NC_T_FREE) {
            return false;
        }
        NcThread *t = &k->threads[i];
        nc_rec_get(r, &nc_rec_NcThread, t, v);
        NcCpuState cs = {0};
        nc_rec_get(r, &nc_rec_NcCpuState, &cs, v);
        nc_context_import(&t->ctx, &cs);
        t->apcs = NULL;
        t->apc_cap = 0;
        if (t->napcs) {
            t->apcs = calloc(t->napcs, sizeof(NcApc));
            if (!t->apcs) {
                return false;
            }
            t->apc_cap = t->napcs;
            nc_recs_get(r, &nc_rec_NcApc, t->apcs, sizeof(NcApc), t->napcs, v);
        }
        if (r->fail || t->state == NC_T_FREE) {
            return false;
        }
    }
    k->current = thread_at(k, refs.current);
    k->slice_thread = thread_at(k, refs.slice_thread);
    k->dpc_thread = thread_at(k, refs.dpc_thread);
    k->isr_thread = thread_at(k, refs.isr_thread);
    k->isr_interrupted = thread_at(k, refs.isr_interrupted);
    k->resume_first = thread_at(k, refs.resume_first);
    g_point = refs.point;
    nc_log("hakuX-native-state", "threads: %u, current %d (%s)", n, refs.current,
           g_point == NC_STATE_AT_IDLE ? "idle: the scheduler picks first" : "at its kernel entry");
    return true;
}

const NcCpuContext *nc_kstate_start_context(NcKernel *k)
{
    nc_dpc_sync(k);                     /* a state from before the flag had none */
    nc_ps_old_stacks(k);                /* every section loaded: the memory map is the state's */
    NcThread *t = k->current;
    if (g_point == NC_STATE_AT_IDLE || !t || t->state != NC_T_RUNNING) {
        /* Every thread was switched out: the guest CPU starts in the
         * kernel, before its first instruction (an interrupt request), and
         * the scheduler picks the thread. The frame it starts with is the
         * current thread's own saved context, which the scheduler saves
         * back unchanged. */
        if (!t) {
            for (unsigned i = 0; i < NC_MAX_THREADS && !t; i++) {
                t = k->threads[i].state != NC_T_FREE ? &k->threads[i] : NULL;
            }
        }
        k->resched = true;
        nc_cpu_request_interrupt(k->cpu);
    }
    return t ? &t->ctx : NULL;
}

void nc_kstate_reset(void)
{
    g_point = 0;
    memset(&g_live, 0, sizeof(g_live));
    g_live_slot = -1;
}
