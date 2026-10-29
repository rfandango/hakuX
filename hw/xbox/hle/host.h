/*
 * hakuX HLE GPU — what the host core gives the HLE: the guest CPU's
 * registers at a probe, guest memory, write tracking of guest RAM, the
 * JIT, the GPU device and configuration. The native core implements it
 * (native/hle/host_native.c). Nothing else outside hw/xbox/hle is included by
 * the HLE code.
 */
#ifndef HW_XBOX_HLE_HOST_H
#define HW_XBOX_HLE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- the guest CPU ------------------------------------------------------- */

/* x86 general registers, in their encoding order. */
enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
#define HLE_EFLAGS_IF 0x00000200u

/* The guest's registers as a probe sees them (core/probe.h), filled from
 * the guest CPU. Segments are flat; FS is the KPCR. */
typedef struct HleCpu {
    uint32_t regs[8];
    uint32_t eip;
    uint32_t eflags;            /* IF at least */
    uint32_t fs_base;
    bool halted;                /* diagnostics: the guest CPU idles */
} HleCpu;

/* The registers of the last probe, or the boot thread's (diagnostics: the
 * stall monitor, the PC log), or NULL before boot. */
HleCpu *hle_host_cpu(void);
/* Makes the guest CPU leave its JIT code at the next block. */
void hle_host_kick(void);
/* The JIT's code for guest code is stale (probes changed). */
void hle_host_code_changed(void);
/* How many times hle_host_code_changed took effect (diagnostics). */
unsigned hle_host_code_flushes(void);

/* ---- guest memory -------------------------------------------------------- */

/* Guest memory by VA, never faulting: 0, or -1 if a page is unmapped
 * (nothing copied past it). A write counts as a guest write. */
int hle_va_rw(uint32_t va, void *buf, size_t len, bool write);
/* Host stores by guest VA are guest writes for write tracking: call before
 * writing [va, va + len). */
void hle_va_store(uint32_t va, uint32_t len);

/* ---- write tracking of guest RAM (physical addresses) -------------------- */

/* Written since the last test_and_clear (the page not tracked: written). */
bool hle_host_mem_dirty(uint32_t pa);
/* Whether [pa, pa + len) was written since the last call, and from now on
 * tracked again (the guest's next write to each page is seen:
 * hle_mem_first_write in mem/barrier.c). */
bool hle_host_mem_test_and_clear(uint32_t pa, uint32_t len);
/* [pa, pa + len) written by a device or the host: dirty. */
void hle_host_mem_set_dirty(uint32_t pa, uint32_t len);
/* [pa, pa + len)'s pages without guest access (on), reads included: the
 * guest's next access faults to hle_mem_stale_access (mem/barrier.c); off:
 * back to what write tracking wants. The host's own view keeps its access. */
void hle_host_mem_noaccess(uint32_t pa, uint32_t len, bool on);

/* ---- the GPU device: the display timing ----------------------------------- */

uint64_t hle_host_vblank_count(void);
int64_t hle_host_vblank_period_ns(void);

/* ---- the GPU's registers (push-buffer mode) ------------------------------- */

/* The GPU's register at offset off of its 16 MiB window (PRAMIN, its
 * instance memory, included) as the guest last wrote it, and a write as the
 * GPU makes one (the value only). Any thread. */
uint32_t hle_host_gpu_reg(uint32_t off);
void hle_host_gpu_reg_set(uint32_t off, uint32_t v);
/* The bits of mask set to bits', atomically (fields the guest's writes
 * change too: PGRAPH_SURFACE's flip counters). */
void hle_host_gpu_reg_mask(uint32_t off, uint32_t mask, uint32_t bits);
/* The GPU's interrupt status registers changed (PGRAPH_INTR): the host
 * re-evaluates its interrupt line. */
void hle_host_gpu_irq_update(void);
/* debug: the GPU event trace (push-buffer mode): the host records the
 * guest's interrupt handshake, the HLE its traps and flips; a dump logs the
 * last events (hakuX-native-trace). Any thread. */
enum {
    HLE_GTR_VBLANK = 1,     /* vblank raised (value: count, | 1 << 31 an early one) */
    HLE_GTR_PCRTC_CLR,      /* the guest cleared PCRTC_INTR (value written) */
    HLE_GTR_PMC_EN,         /* the guest wrote PMC_INTR_EN */
    HLE_GTR_INTR_CLR,       /* the guest cleared PGRAPH_INTR (value written) */
    HLE_GTR_FIFO,           /* the guest wrote PGRAPH_FIFO */
    HLE_GTR_INCREMENT,      /* the flip read counter moved (value: new read) */
    HLE_GTR_START,          /* the guest wrote PCRTC_START */
    HLE_GTR_TRAP,           /* the decoder raised a trap (value: argument) */
    HLE_GTR_FLIP_INC,       /* FLIP_INCREMENT_WRITE (value: new write) */
    HLE_GTR_FLIP_STALL,     /* FLIP_STALL waits (value: read << 4 | write) */
    HLE_GTR_RESUME,         /* the decoder goes on (value: 1 trap, 2 flip) */
    HLE_GTR_DPC_QUEUE,      /* the kernel queued a DPC (value: its routine) */
    HLE_GTR_DPC_RUN,        /* ... dispatched one (value: its routine) */
    HLE_GTR_DPC_REFUSED,    /* ... refused to (value: the IRQL, | 0x100 a DPC running) */
    HLE_GTR_ISR_RET,        /* an ISR returned (value: the IRQL restored) */
    HLE_GTR_IRQL,           /* a thread set the IRQL (value: new | old << 8 | thread << 16 |
                               1 KfRaiseIrql, 2 KfLowerIrql, 3 KeRaiseIrqlToDpcLevel << 24) */
};
void hle_host_gpu_trace(unsigned kind, uint32_t value);
/* The trace's position now (for a dump around it later). */
uint32_t hle_host_gpu_trace_mark(void);
/* The events from 150 before mark (a hle_host_gpu_trace_mark) to 6000 after,
 * or to now. */
void hle_host_gpu_trace_dump(const char *why, uint32_t mark);
/* Push-buffer mode: channel 0's DMA_PUT writes kick the HLE (hle_pb_kick),
 * and the FIFO's status follows the GET it reports. */
void hle_host_pb_mode(bool on);
/* The GPU device is lost (vk/vkdev.c hle_vk_lose): nothing more is drawn,
 * so the game can't go on; why says what failed. Any thread, once. */
void hle_host_gpu_lost(const char *why);

/* ---- configuration ------------------------------------------------------- */

/* Where the HLE keeps its files (shader caches, dumps); ends with '/'. */
const char *hle_host_base_path(void);
/* The posture the front end asks for ("replace"), when HAKUX_HLE_MODE
 * doesn't name one; NULL: off. */
const char *hle_host_hle_mode(void);
/* The render scale, 1-4 times the title's resolution. */
int hle_host_render_scale(void);
/* vkGetInstanceProcAddr of the Vulkan driver to use (a custom driver's or
 * the system's), as a void pointer; NULL if none. */
void *hle_host_vk_loader(void);

/* Disc reads so far: operations, bytes and the time spent in them
 * (diagnostics: the hitch monitor); false if the host doesn't count them. */
bool hle_host_disc_stats(uint64_t *ops, uint64_t *bytes, uint64_t *ns);

#endif
