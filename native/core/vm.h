/*
 * hakuX native core — guest memory.
 *
 * The guest's 4 GiB address space is a host reservation; guest VA x lives at
 * base + x, and the JIT reaches it through its guest memory base register.
 * Guest physical RAM is one memfd: mapped whole at 0x80000000 (the Xbox's
 * contiguous window) and page by page wherever the kernel maps it, so every
 * physical page has one set of bytes whatever address reaches it.
 */
#ifndef NATIVE_CORE_VM_H
#define NATIVE_CORE_VM_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_RAM_RETAIL    (64u << 20)
#define NC_CONTIG_WINDOW 0x80000000u
#define NC_GPU_WINDOW    0xF0000000u /* NV2A BAR1: another view of RAM */

typedef struct NcVm {
    uint8_t *base;         /* host address of guest VA 0 */
    uint8_t *reservation;  /* base minus the low guard */
    size_t reservation_len;
    int ram_fd;            /* guest physical RAM */
    uint32_t ram_size;
    uint8_t *ram;          /* host view of all of physical RAM (== base + 0x80000000) */
    uint8_t *page_used;    /* physical page allocator: one byte per page */
    uint32_t *pte;         /* per guest page: physical address | NC_PTE_VALID, or 0 */
    uint8_t *phys;         /* host view of physical RAM outside the guest space (never protected) */
    uint32_t *rmap;        /* per physical page: the user VA mapping it (besides the window), or 0 */
    uint8_t *wp;           /* per physical page: dirty tracking's (NC_WP_*); 0 not tracked */
    uint8_t *pin;          /* per physical page: NC_PIN_KNOWN | lock count (nc_vm_pin) */
} NcVm;

/* NcVm.wp: protected in every view, unwritten since; written since (a guest
 * store's fault opened only the view it came through: the others may still
 * be read-only, and a store through one opens that too, hle/dirty.c). */
#define NC_WP_CLEAN   1u
#define NC_WP_WRITTEN 2u

/* A page whose physical address went out (MmGetPhysicalAddress, a title's
 * own page tables) stays where it is until freed; locks count below it. */
#define NC_PIN_KNOWN 0x80u

#define NC_PTE_VALID 1u

bool nc_vm_init(NcVm *vm, uint32_t ram_size);
/* Everything nc_vm_init made unmapped, closed and freed (the reservation and
 * every guest mapping in it, the RAM view and its fd, the tables), and the
 * hooks set on guest memory cleared: no thread may touch the memory after. */
void nc_vm_destroy(NcVm *vm);

/* Map guest VA [va, va+len) onto physical [pa, pa+len). Page-aligned. */
bool nc_vm_map(NcVm *vm, uint32_t va, uint32_t pa, uint32_t len);

/* Write-protects (ro) or unprotects every guest mapping of physical page pa. */
void nc_vm_protect_page(NcVm *vm, uint32_t pa, bool ro);
/* The same for n pages from pa: one mprotect per window, and one per run of
 * pages a guest VA range maps in order. */
void nc_vm_protect_pages(NcVm *vm, uint32_t pa, uint32_t n, bool ro);
/* n pages from pa without any guest access (on: every guest view PROT_NONE,
 * whatever write tracking does meanwhile), or back to what write tracking
 * wants (off). The host's view (vm->phys) keeps its access. */
void nc_vm_noaccess(NcVm *vm, uint32_t pa, uint32_t n, bool on);
bool nc_vm_noaccess_has(NcVm *vm, uint32_t pa);

/* Before host code writes guest memory through a system call (read/pread
 * into a guest buffer: a protected page fails the call instead of faulting):
 * the range becomes writable as a guest write would make it. */
extern void (*nc_vm_write_hook)(uint32_t va, uint32_t len);
static inline void nc_vm_will_write(uint32_t va, uint32_t len)
{
    if (nc_vm_write_hook && len) {
        nc_vm_write_hook(va, len);
    }
}

/* A device write to physical [pa, pa + len) that another thread makes
 * later through the RAM view (an overlapped file read): fence, on the guest's
 * CPU before, the value the GPU must reach first (0: none); wait, on the
 * writing thread, for it; wrote, on the guest's CPU after, the write
 * reported (the tracking's dirty pages). Unset: none of it. */
typedef struct NcVmDma {
    uint64_t (*fence)(uint32_t pa, uint32_t len);
    void (*wait)(uint64_t value);
    void (*wrote)(uint32_t pa, uint32_t len);
} NcVmDma;
extern NcVmDma nc_vm_dma;

/* Back guest VA [va, va+len) with nothing again (accesses fault). */
bool nc_vm_unmap(NcVm *vm, uint32_t va, uint32_t len);

/* Physical address behind a guest VA, or UINT32_MAX if unmapped. */
static inline uint32_t nc_vm_phys(const NcVm *vm, uint32_t va)
{
    uint32_t e = vm->pte[va / NC_PAGE];
    return e & NC_PTE_VALID ? (e & ~(NC_PAGE - 1)) | (va & (NC_PAGE - 1)) : UINT32_MAX;
}

/* Allocate len bytes of contiguous physical pages inside [lo, hi), lowest
 * first. Returns the physical address or UINT32_MAX. */
/* The top of RAM virtual memory leaves until nothing else is free: the
 * GPU's instance memory (MmClaimGpuInstanceMemory) is taken from there. */
#define NC_TOP_RESERVE 0x100000u

uint32_t nc_vm_alloc_phys(NcVm *vm, uint32_t len, uint32_t lo, uint32_t hi);

/* Same, with an alignment (power of two, at least a page) and optionally the
 * highest fitting address first. */
uint32_t nc_vm_alloc_phys_ex(NcVm *vm, uint32_t len, uint32_t lo, uint32_t hi, uint32_t align,
                             bool top_down);

/* The kernel's own blocks of more than a page: the highest window that holds
 * them, below NC_TOP_RESERVE if they can. */
uint32_t nc_vm_alloc_phys_high(NcVm *vm, uint32_t len);

void nc_vm_free_phys(NcVm *vm, uint32_t pa, uint32_t len);

/* Pins the page at pa: known (its address went out) or one more lock
 * (lock > 0) or one less (lock < 0). A pinned page is never relocated. */
void nc_vm_pin(NcVm *vm, uint32_t pa, bool known, int lock);

/* Contiguous physical pages when no free run holds them, as the Xbox
 * kernel's MmAllocateContiguousMemory relocates busy pages: the highest
 * window in [lo, hi) whose used pages can all move (virtual pages mapped
 * at one VA, not pinned) gets them moved to free pages elsewhere and is
 * returned. *moved: pages relocated. UINT32_MAX if no window can be freed. */
uint32_t nc_vm_alloc_phys_compact(NcVm *vm, uint32_t len, uint32_t lo, uint32_t hi, uint32_t align, uint32_t *moved);

/* Contiguous physical pages as the Xbox kernel places them: the highest
 * window in [lo, hi) whose pages are free or can move, the ones that can
 * moved to the highest free pages outside it. *moved: pages relocated.
 * UINT32_MAX if no such window, or nowhere to move its pages. */
uint32_t nc_vm_alloc_phys_top(NcVm *vm, uint32_t len, uint32_t lo, uint32_t hi, uint32_t align, uint32_t *moved);

/* Mark physical [pa, pa+len) as in use (fixed layouts such as the kernel's). */
bool nc_vm_claim_phys(NcVm *vm, uint32_t pa, uint32_t len);

/* Allocate physical pages and map them at va. */
bool nc_vm_alloc_at(NcVm *vm, uint32_t va, uint32_t len, uint32_t *pa_out);
/* Physical RAM not in use (bytes). */
uint32_t nc_vm_free_bytes(const NcVm *vm);
/* The free physical runs of 64 KiB or more (pa+size), their count and the
 * largest, as text (out-of-memory reports). */
void nc_vm_free_runs(const NcVm *vm, char *buf, size_t len);

static inline void *nc_vm_host(const NcVm *vm, uint32_t va)
{
    return vm->base + va;
}

static inline uint32_t nc_vm_rd32(const NcVm *vm, uint32_t va)
{
    return nc_rd32(vm->base + va);
}

static inline void nc_vm_wr32(NcVm *vm, uint32_t va, uint32_t v)
{
    nc_wr32(vm->base + va, v);
}

/* Guest VA of a host pointer inside the reservation, or UINT64_MAX. */
static inline uint64_t nc_vm_guest_of(const NcVm *vm, const void *host)
{
    uintptr_t h = (uintptr_t)host, b = (uintptr_t)vm->base;
    return h >= b && h - b < (1ull << 32) ? (uint64_t)(h - b) : UINT64_MAX;
}

/* Guest VA of a host pointer in a guard around the window, or UINT64_MAX:
 * translated code added a displacement to a register past the 32-bit wrap
 * (0xFFFFFFC4 + 0x60), and the guest's access is to the wrapped address. */
static inline uint64_t nc_vm_guest_of_wrapped(const NcVm *vm, const void *host)
{
    uintptr_t h = (uintptr_t)host, r = (uintptr_t)vm->reservation, b = (uintptr_t)vm->base;
    if (h < r || h - r >= vm->reservation_len || nc_vm_guest_of(vm, host) != UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint32_t)(h - b);
}

#ifdef __cplusplus
}
#endif

#endif
