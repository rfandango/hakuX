/*
 * hakuX native core — interrupt objects and interrupt delivery.
 *
 * KINTERRUPT (nxdk xboxkrnl.h, CC0): ServiceRoutine 0x00, ServiceContext 0x04,
 * BusInterruptLevel 0x08, Irql 0x0C, Connected 0x10, ShareVector 0x11,
 * Mode 0x12. Vectors are 0x30 + bus level; a level's IRQL is 26 - level.
 *
 * An asserted line (core/irq.c) is delivered when its IRQL is above the
 * current one and the running code hasn't disabled interrupts: the ISR,
 * ServiceRoutine(Interrupt, ServiceContext), runs as guest code on a kernel
 * thread that outranks the DPC thread, at the interrupt's IRQL, then returns
 * to a stub. The interrupted thread is resumed first if it was at
 * DISPATCH_LEVEL or above; otherwise queued DPCs run, then the scheduler
 * picks as usual. Lines are level triggered: one still asserted after its
 * ISR is delivered again.
 */
#include "native/kernel/kernel.h"
#include "hw/xbox/hle/host.h"
#include "native/core/irq.h"

#include <stdio.h>
#include <string.h>

#define KI_ROUTINE   0x00
#define KI_CONTEXT   0x04
#define KI_LEVEL     0x08
#define KI_IRQL      0x0C
#define KI_CONNECTED 0x10

#define PCR_IRQL      0x24
#define ISR_STACK     0x4000u
#define ISR_PRIORITY  2000

static uint8_t *irql(NcKernel *k)
{
    return (uint8_t *)nc_vm_host(k->vm, k->kpcr_va + PCR_IRQL);
}

/* HalGetInterruptVector(BusInterruptLevel, Irql) -> vector */
static uint64_t HalGetInterruptVector(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (a[1]) {
        *(uint8_t *)nc_vm_host(k->vm, a[1]) = (uint8_t)(26 - a[0]);
    }
    return 0x30 + a[0];
}

/* KeInitializeInterrupt(Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, InterruptMode,
 * ShareVector) */
static uint64_t KeInitializeInterrupt(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_vm_wr32(k->vm, a[0] + KI_ROUTINE, a[1]);
    nc_vm_wr32(k->vm, a[0] + KI_CONTEXT, a[2]);
    nc_vm_wr32(k->vm, a[0] + KI_LEVEL, a[3] - 0x30);
    nc_vm_wr32(k->vm, a[0] + KI_IRQL, a[4] & 0xff);
    uint8_t *i = nc_vm_host(k->vm, a[0]);
    i[KI_CONNECTED] = 0;
    i[0x11] = (uint8_t)a[6];
    i[0x12] = (uint8_t)a[5];
    return 0;
}

/* KeConnectInterrupt(Interrupt) -> BOOLEAN */
static uint64_t KeConnectInterrupt(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t *i = nc_vm_host(k->vm, a[0]);
    const uint32_t level = nc_vm_rd32(k->vm, a[0] + KI_LEVEL);
    if (i[KI_CONNECTED] || level >= NC_IRQ_LINES || k->irq_obj[level]) {
        return 0;
    }
    i[KI_CONNECTED] = 1;
    k->irq_obj[level] = a[0];
    nc_irq_enable(level, true);
    nc_log("hakuX-native", "ke: interrupt connected: level %u, irql %u, routine %08x", level,
           nc_vm_rd32(k->vm, a[0] + KI_IRQL), nc_vm_rd32(k->vm, a[0] + KI_ROUTINE));
    return 1;
}

/* KeDisconnectInterrupt(Interrupt) -> BOOLEAN */
static uint64_t KeDisconnectInterrupt(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t *i = nc_vm_host(k->vm, a[0]);
    const uint32_t level = nc_vm_rd32(k->vm, a[0] + KI_LEVEL);
    if (!i[KI_CONNECTED]) {
        return 0;
    }
    i[KI_CONNECTED] = 0;
    if (level < NC_IRQ_LINES && k->irq_obj[level] == a[0]) {
        k->irq_obj[level] = 0;
        nc_irq_enable(level, false);
    }
    return 1;
}

/* KeSynchronizeExecution(Interrupt, SynchronizeRoutine, SynchronizeContext) -> the routine's
 * BOOLEAN, run at the interrupt's IRQL so its ISR can't come in between. */
static uint64_t KeSynchronizeExecution(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint8_t level = (uint8_t)nc_vm_rd32(k->vm, a[0] + KI_IRQL);
    nc_kernel_callout_at_irql(k, f, a[1], &a[2], 1, level > *irql(k) ? level : *irql(k));
    return 0;
}

/* Why an asserted line waited (diagnostics): the times the kernel couldn't
 * deliver it since it rose, the first reason, and the IRQL and thread then.
 * One delivered over 8 ms after it rose is logged with them; with none
 * refused, the guest's CPU never came to the kernel meanwhile (its host
 * thread didn't run, or translated code ran on without an exit). */
enum { HOLD_ISR = 1, HOLD_IF, HOLD_IRQL };
static const char *const k_hold[] = {"-", "an ISR running", "interrupts off (IF 0)", "IRQL at or above it"};
static struct {
    uint64_t rise;
    unsigned refusals;
    uint8_t why, irql, isr_line;
    uint32_t thread;
} g_hold[NC_IRQ_LINES];
static uint8_t g_isr_line = 0xFF;

uint8_t nc_intr_isr_line(void)
{
    return g_isr_line;
}

void nc_intr_isr_line_set(uint8_t line)
{
    g_isr_line = line;
}

static void hold_note(NcKernel *k, uint32_t lines, uint8_t why)
{
    for (unsigned l = 0; l < NC_IRQ_LINES; l++) {
        if (!(lines & (1u << l))) {
            continue;
        }
        const uint64_t rise = nc_irq_rise_ns(l);
        if (g_hold[l].rise != rise) {
            memset(&g_hold[l], 0, sizeof(g_hold[l]));
            g_hold[l].rise = rise;
        }
        if (!g_hold[l].refusals++) {
            g_hold[l].why = why;
            g_hold[l].irql = *irql(k);
            g_hold[l].isr_line = g_isr_line;
            g_hold[l].thread = k->current ? k->current->id : 0;
        }
    }
}

static void hold_report(unsigned l)
{
    const uint64_t rise = nc_irq_rise_ns(l);
    const uint64_t lat = rise ? nc_irq_now_ns() - rise : 0;
    if (lat > 8000000ull) {
        static uint64_t window;
        static unsigned logged;
        const uint64_t now = nc_irq_now_ns();
        if (now - window > 1000000000ull) {
            window = now;
            logged = 0;
        }
        if (logged++ < 10) {
            const bool same = g_hold[l].rise == rise && g_hold[l].refusals;
            const uint64_t c0 = nc_irq_rise_cpu_ns(l), c1 = c0 ? nc_irq_cpu_ns() : 0;
            const uint64_t d0 = nc_irq_rise_delay_ns(l), d1 = d0 ? nc_irq_delay_ns() : 0;
            char dl[48] = "";
            if (d1 > d0) {
                snprintf(dl, sizeof(dl), ", waited for a host CPU %.1f ms", (d1 - d0) / 1e6);
            }
            nc_log("hakuX-native-irq", "line %u delivered %.1f ms after it rose: %u refusals, first %s "
                   "(IRQL %u, ISR line %d, thread t%u); the guest CPU's thread ran %.1f ms of it%s",
                   l, lat / 1e6, same ? g_hold[l].refusals : 0,
                   same ? k_hold[g_hold[l].why] : "none: no kernel entry meanwhile", same ? g_hold[l].irql : 0,
                   same && g_hold[l].isr_line != 0xFF ? (int)g_hold[l].isr_line : -1,
                   same ? g_hold[l].thread : 0, c1 > c0 ? (c1 - c0) / 1e6 : 0.0, dl);
        }
    }
    memset(&g_hold[l], 0, sizeof(g_hold[l]));
}

bool nc_intr_dispatch(NcKernel *k, bool cpu_enabled)
{
    const uint32_t lines = nc_irq_lines();
    if (!lines) {
        return false;
    }
    if (k->in_isr || !cpu_enabled) {
        hold_note(k, lines, k->in_isr ? HOLD_ISR : HOLD_IF);
        return false;
    }
    /* The highest IRQL first: the lowest bus level. */
    for (unsigned level = 0; level < NC_IRQ_LINES; level++) {
        if (!(lines & (1u << level))) {
            continue;
        }
        const uint32_t obj = k->irq_obj[level];
        if (!obj) {
            continue; /* nothing connected: the line stays asserted, unserved */
        }
        const uint8_t level_irql = (uint8_t)nc_vm_rd32(k->vm, obj + KI_IRQL);
        if (level_irql <= *irql(k)) {
            hold_note(k, 1u << level, HOLD_IRQL);
            continue;
        }
        hold_report(level);
        g_isr_line = (uint8_t)level;
        if (!k->isr_thread) {
            k->isr_thread = nc_ps_kernel_thread(k, ISR_STACK, ISR_PRIORITY);
            if (!k->isr_thread) {
                return false;
            }
        }
        NcThread *t = k->isr_thread;
        const uint32_t esp = nc_ps_frame_top(t) - 16 - 12;
        nc_vm_wr32(k->vm, esp, k->isr_return_va);
        nc_vm_wr32(k->vm, esp + 4, obj);
        nc_vm_wr32(k->vm, esp + 8, nc_vm_rd32(k->vm, obj + KI_CONTEXT));
        nc_context_init(k->cpu, &t->ctx, nc_vm_rd32(k->vm, obj + KI_ROUTINE), esp);
        t->fresh = true; /* it may still be the current thread, back from its last ISR */
        t->state = NC_T_READY;

        k->in_isr = true;
        k->isr_saved_irql = *irql(k);
        k->isr_interrupted = k->current && k->current != t ? k->current : NULL;
        *irql(k) = level_irql;
        k->interrupts++;
        g_nc_sched.isr[level]++;
        k->force_switch = true;
        k->resched = true;
        return true;
    }
    return false;
}

void nc_intr_return(NcKernel *k)
{
    k->in_isr = false;
    g_isr_line = 0xFF;
    *irql(k) = k->isr_saved_irql;
    hle_host_gpu_trace(HLE_GTR_ISR_RET, k->isr_saved_irql);
    if (k->isr_thread) {
        k->isr_thread->state = NC_T_SUSPENDED;
    }
    /* Code interrupted at DISPATCH_LEVEL or above continues before anything else. */
    if (k->isr_saved_irql >= 2 && k->isr_interrupted && k->isr_interrupted->state == NC_T_READY) {
        k->resume_first = k->isr_interrupted;
    }
    k->isr_interrupted = NULL;
    k->resched = true;
}

void nc_intr_register(NcKernel *k)
{
    nc_kernel_impl(k, "HalGetInterruptVector", HalGetInterruptVector);
    nc_kernel_impl(k, "KeInitializeInterrupt", KeInitializeInterrupt);
    nc_kernel_impl(k, "KeConnectInterrupt", KeConnectInterrupt);
    nc_kernel_impl(k, "KeDisconnectInterrupt", KeDisconnectInterrupt);
    nc_kernel_impl(k, "KeSynchronizeExecution", KeSynchronizeExecution);
}

void nc_intr_reset(void)
{
    memset(g_hold, 0, sizeof(g_hold));
    g_isr_line = 0xFF;
}
