/*
 * hakuX native core — interrupt lines.
 *
 * Devices set line levels from any host thread (PCI lines are level
 * triggered). An asserted line rings the CPU's doorbell, so the guest leaves
 * at its next block for the kernel to deliver the interrupt, and wakes the
 * scheduler if the guest CPU is idle.
 */
#ifndef NATIVE_CORE_IRQ_H
#define NATIVE_CORE_IRQ_H

#include "native/cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_IRQ_LINES 16

/* The CPU whose doorbell asserted lines ring (set once it exists). */
void nc_irq_attach(NcCpu *cpu);
/* The core stopped: no lines, no CPU, not paused, no threads registered for
 * the pause, as in a new process. */
void nc_irq_reset(void);
void nc_irq_set(unsigned line, bool level);
/* Bit n set: line n asserted. */
uint32_t nc_irq_lines(void);
/* The longest a line stayed up before the guest took it (ns), since the last
 * reset. */
uint64_t nc_irq_latency_max(unsigned line, bool reset);
/* When an asserted line rose (host monotonic ns; 0: not up), and now. */
uint64_t nc_irq_rise_ns(unsigned line);
uint64_t nc_irq_now_ns(void);
/* The guest CPU's host thread: its CPU time when an asserted line rose, and
 * now (ns; 0: unknown). Over a late delivery, how long it ran: about all of
 * it, translated code or a host call kept it from the kernel; little, the
 * host didn't run it. */
uint64_t nc_irq_rise_cpu_ns(unsigned line);
uint64_t nc_irq_cpu_ns(void);
/* debug (NC_IRQ_SCHED=1): the thread's run delay (runnable, waiting for a
 * host CPU; schedstat) when the line rose, and now (ns; 0: off). Over a late
 * delivery it ran little: this much of it waited for a CPU, the rest it
 * slept or blocked. */
uint64_t nc_irq_rise_delay_ns(unsigned line);
uint64_t nc_irq_delay_ns(void);
/* Unmasks (on) or masks a line: KeConnectInterrupt / KeDisconnectInterrupt. */
void nc_irq_enable(unsigned line, bool on);
/* Blocks until a line is asserted, work is posted, or the host monotonic
 * time reaches deadline_ns (0: wait for a line only, up to max_ns). Returns
 * the lines. */
uint32_t nc_irq_idle_wait(uint64_t deadline_ns, uint64_t max_ns);
/* Work for the kernel from a host thread (a disc read done): rings the
 * doorbell and wakes an idle wait as a line does, but delivers no interrupt.
 * nc_irq_take_work says whether any was posted since it last asked. */
void nc_irq_post_work(void);
bool nc_irq_take_work(void);
/* The emulation paused or resumed (the activity's onPause / onResume). The
 * guest's CPU stops at its next kernel entry (nc_kernel_pause_point); a
 * device thread that runs on its own checks nc_paused and waits in
 * nc_pause_wait (until resumed). */
void nc_pause_set(bool on);
bool nc_paused(void);
void nc_pause_wait(void);
/* Savestates: a device thread that waits in nc_pause_wait registers once
 * (its name for the logs); nc_pause_quiesce waits, paused, until every
 * registered thread is there (false after timeout_ns: *who names one that
 * isn't). nc_pause_kick wakes the guest CPU's pause wait for its work
 * (nc_pause_cpu_work) without resuming. */
void nc_pause_register(const char *name);
/* A registered thread idle in a wait of its own (nothing to do until the
 * guest acts): counts as parked meanwhile. */
void nc_pause_idle(bool on);
bool nc_pause_quiesce(uint64_t timeout_ns, const char **who);
void nc_pause_kick(void);
/* The guest CPU's thread, paused: whether nc_pause_kick asked since it last
 * looked. */
bool nc_pause_cpu_work(void);
/* The lines' mask and levels (a savestate's), and setting them back. */
void nc_irq_state(uint32_t *enabled, uint32_t *lines);
void nc_irq_state_set(uint32_t enabled, uint32_t lines);
/* set while the guest CPU's thread waits in nc_irq_idle_wait (the samplers' idle) */
extern volatile int g_nc_idle_waiting;

#ifdef __cplusplus
}
#endif

#endif
