/*
 * hakuX HLE GPU — first-write barrier (spec §4.10; P2 Decision 5): the
 * guest's first store to a page the HLE cleaned waits for the GPU work that
 * reads it.
 */
#ifndef HW_XBOX_HLE_MEM_BARRIER_H
#define HW_XBOX_HLE_MEM_BARRIER_H

#include <stdbool.h>
#include <stdint.h>

void hle_barrier_init(uint64_t ram_size);
/* The core stops (M2; the guest's CPU, the decoder and the back end done):
 * the tables freed, the hooks and counts as before hle_barrier_init. The
 * stale ranges and report pages are forgotten, their access given back as
 * push-buffer mode stopped. */
void hle_barrier_free(void);
/* GPU work up to `value` reads [pa, pa+len) in place: the first CPU write
 * waits, and the CPU cache lines of its pages written since they were last
 * cleaned go to memory first (the GPU doesn't snoop the CPU's caches over
 * the imported RAM). */
void hle_barrier_arm(uint32_t pa, uint32_t len, uint64_t value);
/* GPU work up to `value` writes [pa, pa+len) in place (a readback into the
 * imported RAM): as hle_barrier_arm, and the range's CPU cache lines are
 * cleaned and invalidated, so neither a dirty line written back later nor a
 * stale clean one hides what the GPU wrote. */
void hle_barrier_arm_landing(uint32_t pa, uint32_t len, uint64_t value);
/* [pa, pa+len) is older than an image over it, until fill(pa, len) puts the
 * image there (push-buffer mode's frame on screen, which the GPU drew into
 * its own image): the guest's first access to a page of it, a read or a
 * write (the pages have no guest access meanwhile: host.h
 * hle_host_mem_noaccess), calls fill on the guest's thread, once, then waits
 * for what fill armed. One range at a time; len 0 drops it. */
void hle_barrier_set_stale(uint32_t pa, uint32_t len, void (*fill)(uint32_t pa, uint32_t len));
/* The host, at a guest access to a page without access (hle_barrier_set_stale):
 * the fill, then the pages' access back. */
void hle_mem_stale_access(uint32_t pa);
/* Guest RAM the host wrote through the RAM view (device models, HLE copies:
 * host.h hle_host_mem_set_dirty): its pages' cache lines are cleaned before
 * the GPU next reads them in place. */
void hle_mem_host_wrote(uint32_t pa, uint32_t len);
/* Was [pa, pa+len) written by the CPU since the last call (HLE dirty bits,
 * then cleared)? Texture snapshots use it to see writes made without Lock
 * (spec §5.5 "dirty bitmap over the object's range"). True when unknown. */
bool hle_mem_written(uint32_t pa, uint32_t len);
/* Guest RAM written by the emulated GPU (a copy the decode performs): the
 * next hle_mem_written() over it sees the write. */
void hle_mem_mark_written(uint32_t pa, uint32_t len);
/* GPU progress source (back end, Task 13). */
void hle_barrier_set_waiter(uint64_t (*completed)(void), void (*wait)(uint64_t));
/* Before a first write waits for `value`: the producer that may still hold
 * it open closes it (push-buffer mode's decode thread), else the wait could
 * outlast it. NULL: none. */
void hle_barrier_set_flush(void (*flush)(uint64_t value));
/* Whether a first write to a page of [pa, pa + len) had to wait for the GPU
 * (the CPU rewrites memory while the GPU still reads it). */
bool hle_barrier_contended(uint32_t pa, uint32_t len);
/* Push-buffer mode: [pa, pa + len) was just copied for a draw, and a later
 * draw the decoder hasn't reached may read it again. The guest's first
 * store into those bytes calls catch_up (hle_barrier_set_catch_up) on the
 * guest's thread first: it returns once the decoder has read what the guest
 * pushed before the store. */
void hle_barrier_arm_copied(uint32_t pa, uint32_t len);
void hle_barrier_set_catch_up(void (*catch_up)(void));
/* Push-buffer mode's visibility-test reports. The console's GPU writes one
 * soon after the guest pushes its GET_REPORT; ours once the GPU ran the
 * test, often after the title has read it. The scout (the guest's thread,
 * at each PUT it writes) passes each report it finds with the frame it is in
 * (the swaps pushed before it): its page goes without guest access. A guest
 * access to the page then waits until the page's reports of earlier frames
 * are written (the decoder caught up, the GPU done; 30 ms at most) and gives
 * the page back. The decoder passes each report it reaches, the back end
 * each it writes. scout: scans to the guest's PUT, returns the frame it is
 * in (NULL: no gate). held: whether the decoder waits on the guest itself (a
 * trap for its interrupt handler, the FIFO access it turned off, a flip
 * stall): a report it hasn't reached can't come while the guest waits. */
void hle_barrier_report_pushed(uint32_t pa, uint32_t frame);
void hle_barrier_report_decoded(uint32_t pa);
void hle_barrier_report_written(uint32_t pa);
void hle_barrier_set_report_scout(uint32_t (*scout)(void), bool (*held)(void));
/* The gate's counts: accesses that waited, their time, waits given up after
 * 30 ms, and at once (the decoder held by the guest short of the report). */
void hle_barrier_report_stats(uint64_t *waits, uint64_t *ns, uint64_t *timeouts, uint64_t *held);
/* The host, at the guest's first store to a page tracking made clean
 * (host.h hle_host_mem_test_and_clear). */
void hle_mem_first_write(uint64_t ram_addr, unsigned size);
/* A device write to [pa, pa + len) that another thread makes later (an
 * overlapped file read): the GPU value it must wait for, as a first write
 * would (0: none), the producer flushed meanwhile; hle_mem_wait_fence on
 * that thread before the write. Leaves the pages' tracking alone: the
 * writer reports the write (hle_host_mem_set_dirty) once done. */
uint64_t hle_mem_write_fence(uint32_t pa, uint32_t len);
void hle_mem_wait_fence(uint64_t value);


typedef struct HleBarrierStats {
    uint64_t traps, waits, wait_ns;
    uint64_t armed;                 /* traps on pages armed for GPU reads (else: dirty tracking) */
    uint64_t unarmed;               /* traps with nothing armed: always a bug */
    uint64_t cleaned;               /* pages whose cache lines went to memory for the GPU */
} HleBarrierStats;
HleBarrierStats hle_barrier_stats(void);
/* debug: the guest RAM pages (PA >> 12) whose first write waited, the last
 * few (newest first); returns how many */
unsigned hle_barrier_wait_pages(uint32_t *pages, unsigned max);

#endif
