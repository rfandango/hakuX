/*
 * hakuX native core — memory manager.
 *
 * Virtual memory (NtAllocateVirtualMemory and friends) in the title's
 * 0x00010000-0x7FFE0000 range: reservations at 64 KiB granularity, commits
 * backed page by page with physical RAM (contiguous runs where possible).
 * Contiguous and system memory come from physical RAM top-down and are
 * reached through the 0x80000000 window, so VA = 0x80000000 | PA there.
 */
#include "native/kernel/kernel.h"

#include <stdio.h>
#include <unistd.h>

#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u
#define MEM_FREE     0x10000u
#define MEM_PRIVATE  0x20000u
#define MEM_TOP_DOWN 0x100000u
#define MEM_NOZERO   0x800000u
#define MEM_IMAGE    0x1000000u

#define PAGE_NOACCESS  0x01u
#define PAGE_READWRITE 0x04u

#define USER_LO   0x00010000u
#define USER_HI   0x7FFE0000u
#define VA_GRAIN  0x10000u
#define MAX_REGIONS 4096
#define MAX_CONTIG  4096

typedef struct Region {
    uint32_t base, size, protect;
    bool image;
} Region;

static Region g_regions[MAX_REGIONS];
static unsigned g_nregions;

typedef struct Contig {
    uint32_t va, size;
    uint32_t ra;    /* the caller's return address (who holds the memory) */
    uint32_t slack; /* free pages above it it took (contig_alloc), freed with it */
    uint32_t protect; /* its pages' protection, cache bits included (MmQueryAddressProtect);
                         0: PAGE_READWRITE (a state from before it was kept) */
} Contig;

static Contig g_contig[MAX_CONTIG];
static unsigned g_ncontig;
static uint32_t g_gpu_instance_end;

void nc_mm_usage(NcKernel *k, uint32_t *contig_bytes, uint32_t *virtual_bytes, unsigned *contig_blocks)
{
    uint32_t c = 0, v = 0;
    for (unsigned i = 0; i < g_ncontig; i++) {
        c += g_contig[i].size;
    }
    for (unsigned i = 0; i < g_nregions; i++) {
        const Region *r = &g_regions[i];
        for (uint32_t p = r->base; p < r->base + r->size; p += NC_PAGE) {
            v += nc_vm_phys(k->vm, p) != UINT32_MAX ? NC_PAGE : 0;
        }
    }
    *contig_bytes = c;
    *virtual_bytes = v;
    *contig_blocks = g_ncontig;
}

/* The return address of the kernel call being served (at [esp] in the trampoline). */
static uint32_t caller_of(NcKernel *k, NcFrame *f)
{
    return f ? nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)) : 0;
}

/* Who holds the RAM, once, at the first allocation that finds none: the
 * contiguous blocks summed by caller and the regions with the most committed. */
static void log_holders(NcKernel *k, const char *why)
{
    static bool s_done;
    if (s_done) {
        return;
    }
    s_done = true;
    enum { N = 64 };
    uint32_t ra[N], bytes[N], blocks[N];
    unsigned n = 0;
    for (unsigned i = 0; i < g_ncontig; i++) {
        unsigned j = 0;
        while (j < n && ra[j] != g_contig[i].ra) {
            j++;
        }
        if (j == n) {
            if (n == N) {
                continue;
            }
            ra[n] = g_contig[i].ra, bytes[n] = blocks[n] = 0;
            n++;
        }
        bytes[j] += g_contig[i].size;
        blocks[j]++;
    }
    char line[640];
    int m = snprintf(line, sizeof(line), "%s: contiguous by caller:", why);
    for (unsigned t = 0; t < 12 && t < n; t++) {
        unsigned best = t;
        for (unsigned j = t + 1; j < n; j++) {
            best = bytes[j] > bytes[best] ? j : best;
        }
        uint32_t x;
        x = ra[t], ra[t] = ra[best], ra[best] = x;
        x = bytes[t], bytes[t] = bytes[best], bytes[best] = x;
        x = blocks[t], blocks[t] = blocks[best], blocks[best] = x;
        m += snprintf(line + m, sizeof(line) - (size_t)m, " %08x %uK/%u", ra[t], bytes[t] >> 10, blocks[t]);
    }
    nc_log("hakuX-native-mm", "%s", line);
    m = snprintf(line, sizeof(line), "%s: regions by committed (of %u):", why, g_nregions);
    bool used[MAX_REGIONS] = {false};
    for (unsigned t = 0; t < 12; t++) {
        int best = -1;
        uint32_t best_c = 0;
        for (unsigned i = 0; i < g_nregions; i++) {
            if (used[i]) {
                continue;
            }
            uint32_t c = 0;
            for (uint32_t va = g_regions[i].base; va < g_regions[i].base + g_regions[i].size; va += NC_PAGE) {
                c += nc_vm_phys(k->vm, va) != UINT32_MAX ? NC_PAGE : 0;
            }
            if (best < 0 || c > best_c) {
                best = (int)i, best_c = c;
            }
        }
        if (best < 0 || !best_c) {
            break;
        }
        used[best] = true;
        m += snprintf(line + m, sizeof(line) - (size_t)m, " %08x+%x:%uK%s", g_regions[best].base,
                      g_regions[best].size, best_c >> 10, g_regions[best].image ? "(image)" : "");
    }
    nc_log("hakuX-native-mm", "%s", line);
}

static Region *region_at(uint32_t va)
{
    for (unsigned i = 0; i < g_nregions; i++) {
        if (va >= g_regions[i].base && va - g_regions[i].base < g_regions[i].size) {
            return &g_regions[i];
        }
    }
    return NULL;
}

static bool range_free(uint32_t base, uint32_t size)
{
    for (unsigned i = 0; i < g_nregions; i++) {
        const Region *r = &g_regions[i];
        if (base < r->base + r->size && r->base < base + size) {
            return false;
        }
    }
    return true;
}

static Region *add_region(uint32_t base, uint32_t size, uint32_t protect, bool image)
{
    if (g_nregions == MAX_REGIONS) {
        return NULL;
    }
    g_regions[g_nregions] = (Region){base, size, protect, image};
    return &g_regions[g_nregions++];
}

void nc_mm_note_image(uint32_t base, uint32_t size)
{
    add_region(base, nc_align_up(size, NC_PAGE), PAGE_READWRITE, true);
}

static uint32_t find_free(uint32_t size, bool top_down)
{
    size = nc_align_up(size, VA_GRAIN);
    if (top_down) {
        for (uint32_t va = (USER_HI - size) & ~(VA_GRAIN - 1); va >= USER_LO; va -= VA_GRAIN) {
            if (range_free(va, size)) {
                return va;
            }
        }
    } else {
        for (uint32_t va = USER_LO; va + size <= USER_HI; va += VA_GRAIN) {
            if (range_free(va, size)) {
                return va;
            }
        }
    }
    return 0;
}

static bool commit(NcKernel *k, uint32_t va, uint32_t size, bool zero)
{
    for (uint32_t p = va; p < va + size;) {
        if (nc_vm_phys(k->vm, p) != UINT32_MAX) {
            p += NC_PAGE;
            continue;
        }
        uint32_t run = 0;
        while (p + run < va + size && nc_vm_phys(k->vm, p + run) == UINT32_MAX) {
            run += NC_PAGE;
        }
        /* Largest contiguous physical piece we can get for this run. */
        uint32_t piece = run, pa = UINT32_MAX;
        while (piece >= NC_PAGE && (pa = nc_vm_alloc_phys(k->vm, piece, 0, k->vm->ram_size)) == UINT32_MAX) {
            piece = piece > NC_PAGE ? nc_align_up(piece / 2, NC_PAGE) : 0;
        }
        if (pa == UINT32_MAX || !nc_vm_map(k->vm, p, pa, piece)) {
            return false;
        }
        if (zero) {
            memset(k->vm->ram + pa, 0, piece);
        }
        k->vm_commits += piece / NC_PAGE;
        p += piece;
    }
    return true;
}

/* One unmap (a syscall) per run of committed pages, then their physical
 * pages: Riddick decommits ~20k pages in a second or two while it loads, and
 * a syscall a page made 80-180 ms frames. */
static void decommit(NcKernel *k, uint32_t va, uint32_t size)
{
    uint32_t pas[256];
    for (uint32_t p = va; p < va + size;) {
        unsigned n = 0;
        while (n < sizeof(pas) / sizeof(pas[0]) && p + n * NC_PAGE < va + size &&
               (pas[n] = nc_vm_phys(k->vm, p + n * NC_PAGE)) != UINT32_MAX) {
            n++;
        }
        if (!n) {
            p += NC_PAGE;
            continue;
        }
        nc_vm_unmap(k->vm, p, n * NC_PAGE);
        for (unsigned i = 0; i < n; i++) {
            nc_vm_free_phys(k->vm, pas[i] & ~(NC_PAGE - 1), NC_PAGE);
        }
        k->vm_decommits += n;
        p += n * NC_PAGE;
    }
}

/* NtAllocateVirtualMemory(BaseAddress, ZeroBits, RegionSize, AllocationType, Protect) */
static uint64_t NtAllocateVirtualMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t base = nc_vm_rd32(k->vm, a[0]);
    uint32_t size = nc_vm_rd32(k->vm, a[2]);
    const uint32_t type = a[3];
    if (!size || !(type & (MEM_COMMIT | MEM_RESERVE))) {
        return NC_STATUS_INVALID_PARAMETER;
    }

    /* A commit at no address reserves the range too (VirtualAlloc(NULL, n, MEM_COMMIT)). */
    if (type & MEM_RESERVE || !base) {
        if (base) {
            uint32_t end = nc_align_up(base + size, NC_PAGE);
            base &= ~(VA_GRAIN - 1);
            size = end - base;
            if (base < USER_LO || end > USER_HI || !range_free(base, size)) {
                return 0xC0000018u; /* STATUS_CONFLICTING_ADDRESSES */
            }
        } else {
            size = nc_align_up(size, NC_PAGE);
            base = find_free(size, type & MEM_TOP_DOWN);
            if (!base) {
                return NC_STATUS_NO_MEMORY;
            }
        }
        if (!add_region(base, size, a[4], false)) {
            return NC_STATUS_NO_MEMORY;
        }
    } else {
        uint32_t end = nc_align_up(base + size, NC_PAGE);
        base &= ~(NC_PAGE - 1);
        size = end - base;
        Region *r = region_at(base);
        if (!r || end > r->base + r->size) {
            nc_log("hakuX-native-mm", "commit %08x+%x: not inside a reservation (%s %08x+%x)", base, size,
                   r ? "region" : "no region", r ? r->base : 0, r ? r->size : 0);
            return 0xC000002Du; /* STATUS_CONFLICTING_ADDRESSES-ish: not reserved */
        }
    }
    if ((type & MEM_COMMIT) && !commit(k, base, size, !(type & MEM_NOZERO))) {
        nc_log("hakuX-native-mm", "commit %08x+%x: out of physical memory", base, size);
        log_holders(k, "out of memory");
        return NC_STATUS_NO_MEMORY;
    }
    nc_vm_wr32(k->vm, a[0], base);
    nc_vm_wr32(k->vm, a[2], size);
    /* Only the big ones: a title's heap commits and decommits pages by the
     * thousand a second while it loads (the counts are in the stats line). */
    if ((type & MEM_COMMIT) && size >= 0x40000) {
        char frames[96];
        nc_kernel_frame_chain(k, f, frames, sizeof(frames)); /* who grows the heap by this much */
        nc_log("hakuX-native-mm", "allocate %08x+%x type %x protect %x | frames%s", base, size, type,
               a[4], frames);
    }
    return NC_STATUS_SUCCESS;
}

/* NtFreeVirtualMemory(BaseAddress, RegionSize, FreeType) */
static uint64_t NtFreeVirtualMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t base = nc_vm_rd32(k->vm, a[0]);
    uint32_t size = nc_vm_rd32(k->vm, a[1]);
    Region *r = region_at(base);
    if (!r || r->image) {
        return 0xC00000A0u; /* STATUS_MEMORY_NOT_ALLOCATED */
    }
    if (a[2] & MEM_RELEASE) {
        /* Size 0 frees the whole reservation, from its base only; a size frees
         * those pages of one reservation, trimming or splitting it (Blood Wake
         * keeps the first page of a 1.9 MiB one and releases the rest). */
        const uint32_t r_end = r->base + r->size;
        const uint32_t end = size ? nc_align_up(base + size, NC_PAGE) : r_end;
        base &= ~(NC_PAGE - 1);
        if (!size && base != r->base) {
            return 0xC000009Fu; /* STATUS_FREE_VM_NOT_AT_BASE */
        }
        if (end > r_end) {
            return 0xC000001Au; /* STATUS_UNABLE_TO_FREE_VM */
        }
        if (base > r->base && end < r_end && !add_region(end, r_end - end, r->protect, false)) {
            return NC_STATUS_NO_MEMORY;
        }
        size = end - base;
        decommit(k, base, size);
        if (base > r->base) {
            r->size = base - r->base;
        } else if (end < r_end) {
            r->base = end;
            r->size = r_end - end;
        } else {
            *r = g_regions[--g_nregions];
        }
    } else if (a[2] & MEM_DECOMMIT) {
        uint32_t end = size ? nc_align_up(base + size, NC_PAGE) : r->base + r->size;
        base &= ~(NC_PAGE - 1);
        size = end - base;
        decommit(k, base, size);
    } else {
        return NC_STATUS_INVALID_PARAMETER;
    }
    nc_vm_wr32(k->vm, a[0], base);
    nc_vm_wr32(k->vm, a[1], size);
    return NC_STATUS_SUCCESS;
}

/* Titles patch their own code between two NtProtectVirtualMemory calls (Rainbow
 * Six 3: Black Arrow points the CRT's malloc, realloc and free at Unreal's
 * allocator, protect, memcpy, protect, on pages whose protection stays
 * PAGE_READWRITE). Nothing else tells the translator, so a function translated
 * before the write kept running the old code: Black Arrow's malloc still took
 * blocks from the process heap while free handed them to Unreal's, and a mission
 * load crashed in its allocator. So each protect of image pages remembers a
 * checksum of them, and a later protect of the same range that finds other
 * bytes drops their translations. JSRF re-protects unchanged pages every frame:
 * those keep theirs. */
static void protect_code_check(NcKernel *k, uint32_t base, uint32_t size)
{
    enum { N = 32, MAX_SPAN = 0x100000 };
    static struct { uint32_t base, size, sum; } s_seen[N];
    static unsigned s_next;
    if (size > MAX_SPAN) {
        return;
    }
    const uint8_t *p = nc_vm_host(k->vm, base);
    uint32_t sum = 2166136261u; /* FNV-1a */
    for (uint32_t i = 0; i < size; i++) {
        sum = (sum ^ p[i]) * 16777619u;
    }
    for (unsigned i = 0; i < N; i++) {
        if (s_seen[i].size && s_seen[i].base == base && s_seen[i].size == size) {
            if (s_seen[i].sum != sum) {
                nc_log("hakuX-native-mm", "protect %08x+%x: the title rewrote its code there: translated again", base,
                       size);
                nc_cpu_invalidate(k->cpu, base, size);
                s_seen[i].sum = sum;
            }
            return;
        }
    }
    s_seen[s_next] = (typeof(s_seen[0])){base, size, sum};
    s_next = (s_next + 1) % N;
}

/* NtProtectVirtualMemory(BaseAddress, RegionSize, NewProtect, OldProtect). Guest
 * memory is flat here (no access checks), so the protection is recorded for
 * NtQueryVirtualMemory and the old one returned; the range rounds out to pages
 * as the kernel's does (JSRF protects its own pages). */
static uint64_t NtProtectVirtualMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t base = nc_vm_rd32(k->vm, a[0]);
    const uint32_t end = nc_align_up(base + nc_vm_rd32(k->vm, a[1]), NC_PAGE);
    base &= ~(NC_PAGE - 1);
    const uint32_t size = end - base;
    if (!size) {
        return NC_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t p = base; p < end; p += NC_PAGE) {
        if (nc_vm_phys(k->vm, p) == UINT32_MAX) {
            return 0xC000002Du; /* STATUS_NOT_COMMITTED */
        }
    }
    Region *r = region_at(base);
    const uint32_t old = r ? r->protect : PAGE_READWRITE;
    if (r && base == r->base && size == r->size) {
        r->protect = a[2];
    }
    if (r && r->image) {
        protect_code_check(k, base, size);
    }
    nc_vm_wr32(k->vm, a[0], base);
    nc_vm_wr32(k->vm, a[1], size);
    nc_vm_wr32(k->vm, a[3], old);
    static unsigned s_logged; /* JSRF flips a region's protection every frame */
    if (s_logged < 16) {
        nc_log("hakuX-native-mm", "protect %08x+%x %x (was %x)%s", base, size, a[2], old,
               ++s_logged == 16 ? " (no more protect logs)" : "");
    }
    return NC_STATUS_SUCCESS;
}

/* NtQueryVirtualMemory(BaseAddress, MemoryInformation) -> MEMORY_BASIC_INFORMATION
 * { BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type } */
static uint64_t NtQueryVirtualMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t va = a[0] & ~(NC_PAGE - 1);
    const uint32_t mbi = a[1];
    Region *r = region_at(va);
    uint32_t info[7] = {va, 0, 0, 0, MEM_FREE, PAGE_NOACCESS, 0};
    if (!r) {
        uint32_t end = USER_HI;
        for (unsigned i = 0; i < g_nregions; i++) {
            if (g_regions[i].base > va && g_regions[i].base < end) {
                end = g_regions[i].base;
            }
        }
        info[3] = end > va ? end - va : NC_PAGE;
    } else {
        const bool committed = nc_vm_phys(k->vm, va) != UINT32_MAX;
        uint32_t end = va;
        while (end < r->base + r->size && (nc_vm_phys(k->vm, end) != UINT32_MAX) == committed) {
            end += NC_PAGE;
        }
        info[1] = r->base;
        info[2] = r->protect;
        info[3] = end - va;
        info[4] = committed ? MEM_COMMIT : MEM_RESERVE;
        info[5] = committed ? r->protect : 0;
        info[6] = r->image ? MEM_IMAGE : MEM_PRIVATE;
    }
    memcpy(nc_vm_host(k->vm, mbi), info, sizeof(info));
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------- contiguous and system */

/* Out-of-memory reports: what holds the small used runs (256 KiB or less)
 * between free runs, which keep the free memory from being one run. */
static void log_splitters(NcKernel *k)
{
    const NcVm *vm = k->vm;
    const uint32_t pages = vm->ram_size / NC_PAGE;
    char line[700];
    int n = snprintf(line, sizeof(line), "mm:   between free runs:");
    for (uint32_t p = 1; p < pages && n < (int)sizeof(line) - 90;) {
        if (!vm->page_used[p] || vm->page_used[p - 1]) {
            p++;
            continue;
        }
        uint32_t q = p;
        while (q < pages && vm->page_used[q]) {
            q++;
        }
        if (q < pages && q - p <= 64) {
            const uint32_t pa = p * NC_PAGE;
            char what[48] = "kernel";
            for (unsigned i = 0; i < g_ncontig; i++) {
                const uint32_t cpa = g_contig[i].va & ~NC_CONTIG_WINDOW;
                if (pa >= cpa && pa - cpa < g_contig[i].size) {
                    snprintf(what, sizeof(what), "contiguous %08x+%uK", g_contig[i].va, g_contig[i].size >> 10);
                    break;
                }
            }
            if (!strcmp(what, "kernel") && vm->rmap[p]) {
                const Region *r = region_at(vm->rmap[p]);
                /* pinned (MmLockUnlockBufferPages): compaction can't move it;
                 * write-protected for dirty tracking: it waits for the GPU */
                snprintf(what, sizeof(what), "va %08x%s%s%s", vm->rmap[p],
                         r && r->image ? " image" : r ? "" : " (no region)", vm->pin[p] ? " pinned" : "",
                         vm->wp[p] ? " tracked" : "");
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %x+%uK=%s", pa, (q - p) * (NC_PAGE >> 10), what);
        }
        p = q;
    }
    nc_log("hakuX-native", "%s", line);
}

/* What a used page is to compaction (vm.c movable): kind, and the contiguous
 * block it belongs to (kind 'C'). */
static char page_kind(NcVm *vm, uint32_t p, unsigned *block)
{
    if (!vm->page_used[p]) {
        return '.';
    }
    const uint32_t pa = p * NC_PAGE;
    for (unsigned i = 0; i < g_ncontig; i++) {
        const uint32_t cpa = g_contig[i].va & ~NC_CONTIG_WINDOW;
        if (pa >= cpa && pa - cpa < g_contig[i].size + g_contig[i].slack) {
            *block = i;
            return 'C';
        }
    }
    const uint32_t va = vm->rmap[p];
    if (!va || vm->pte[va / NC_PAGE] != (pa | NC_PTE_VALID)) {
        return 'K';                     /* no user mapping: the kernel's, or the GPU's instance memory */
    }
    if (vm->pin[p]) {
        return 'P';
    }
    return nc_vm_noaccess_has(vm, pa) ? 'N' : 'v';
}

/* Out-of-memory reports: all of RAM in runs of one kind, low to high, the
 * ones that can't move named (contiguous blocks with their holder, the
 * kernel's, pinned, without guest access 'N'): what stands in every window
 * of the size asked for. */
static void log_phys_map(NcKernel *k)
{
    NcVm *vm = k->vm;
    const uint32_t pages = vm->ram_size / NC_PAGE;
    char line[760];
    int n = snprintf(line, sizeof(line), "mm:   map:");
    unsigned lines = 0;
    for (uint32_t p = 0; p < pages && lines < 16;) {
        /* a contiguous run: the blocks of one holder */
        unsigned block = 0, b2 = 0, blocks = 1;
        const char kind = page_kind(vm, p, &block);
        uint32_t q = p + 1;
        for (unsigned prev = block; q < pages && page_kind(vm, q, &b2) == kind &&
                                    (kind != 'C' || g_contig[b2].ra == g_contig[block].ra); q++) {
            blocks += kind == 'C' && b2 != prev;
            prev = b2;
        }
        char what[64];
        if (kind == 'C' && blocks > 1) {
            snprintf(what, sizeof(what), "C%08x.. (%u blocks) by %08x", g_contig[block].va, blocks, g_contig[block].ra);
        } else if (kind == 'C') {
            snprintf(what, sizeof(what), "C%08x by %08x", g_contig[block].va, g_contig[block].ra);
        } else if (kind == 'P' || kind == 'N') {
            const Region *r = region_at(vm->rmap[p]);
            snprintf(what, sizeof(what), "%c va %08x%s", kind, vm->rmap[p], r && r->image ? " image" : "");
        } else {
            snprintf(what, sizeof(what), "%c", kind);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %x+%uK=%s", p * NC_PAGE, (q - p) * (NC_PAGE >> 10), what);
        if (n > (int)sizeof(line) - 80) {
            nc_log("hakuX-native", "%s", line);
            n = snprintf(line, sizeof(line), "mm:   map (%u):", ++lines);
        }
        p = q;
    }
    nc_log("hakuX-native", "%s", line);
}

static uint32_t contig_alloc(NcKernel *k, uint32_t size, uint32_t lo, uint32_t hi, uint32_t align,
                             uint32_t protect, uint32_t ra)
{
    size = nc_align_up(size ? size : 1, NC_PAGE);
    /* The top of RAM, virtual pages moved out of the way (vm.c), then the
     * window needing the fewest moves; neither when there are fewer free
     * pages than asked for. */
    uint32_t pa = UINT32_MAX, moved = 0;
    static unsigned s_failed;           /* failures since the last success (reports below) */
    if (nc_vm_free_bytes(k->vm) >= size) {
        pa = nc_vm_alloc_phys_top(k->vm, size, lo, hi, align, &moved);
    }
    if (pa == UINT32_MAX && g_ncontig < MAX_CONTIG && nc_vm_free_bytes(k->vm) >= size) {
        pa = nc_vm_alloc_phys_compact(k->vm, size, lo, hi, align, &moved);
    }
    if (pa != UINT32_MAX && moved) {
        static unsigned s_moves;
        const unsigned n = ++s_moves;
        if (n <= 8 || !(n & (n - 1))) {
            nc_log("hakuX-native", "mm: contiguous allocation of %u KiB at %08x: %u pages relocated (#%u)",
                   size >> 10, pa, moved, n);
        }
    }
    if (pa == UINT32_MAX || g_ncontig == MAX_CONTIG) {
        /* A failure must be as quick as the kernel's: titles size their heap
         * by asking for less and less (The Warriors: 46.5 MiB down to 38.5
         * in 4 KiB steps after its 18.5 MiB block, 2033 failures), and each
         * report scanned RAM twice and logged three lines: 35 s of its boot.
         * The first 4 failures since a success, then every power of two, are
         * reported: a run's own failure later (Amped 2's 1.4 MiB at a level's
         * load, after ~2500 sizing its heap at boot) is reported whole. */
        const unsigned n = ++s_failed;
        if (n <= 4 || !(n & (n - 1))) {
            char runs[400];
            nc_vm_free_runs(k->vm, runs, sizeof(runs));
            nc_log("hakuX-native", "mm: contiguous allocation of %u KiB failed (#%u; free %u KiB:%s)", size >> 10, n,
                   nc_vm_free_bytes(k->vm) >> 10, runs);
            log_splitters(k);
            log_phys_map(k);
            log_holders(k, "contiguous allocation failed");
        }
        return 0;
    }
    s_failed = 0;
    memset(k->vm->ram + pa, 0, size);
    /* An aligned block below another leaves the pages its alignment skipped
     * free between them, and virtual memory takes the highest free pages:
     * the block takes them too. On the console the blocks pack: what runs
     * past one lands in the next. Project Gotham Racing's first boot
     * decompresses a 640x512 loading bitmap into its 640x480 back buffer,
     * 80 KiB past the end; in push-buffer mode D3D's frame buffers (16 KiB
     * aligned) sat 8 KiB below its push buffer, the title's heap took those
     * pages, and the front end died on a null buffer at car select. */
    uint32_t slack = 0;
    const uint32_t top = pa + size;
    while (align > NC_PAGE && slack + NC_PAGE < align && top + slack < k->vm->ram_size &&
           nc_vm_claim_phys(k->vm, top + slack, NC_PAGE)) {
        slack += NC_PAGE;
    }
    const uint32_t va = NC_CONTIG_WINDOW | pa;
    g_contig[g_ncontig++] = (Contig){va, size, ra, slack, protect};
    return va;
}

static Contig *contig_at(uint32_t va)
{
    for (unsigned i = 0; i < g_ncontig; i++) {
        if (g_contig[i].va == va) {
            return &g_contig[i];
        }
    }
    return NULL;
}

/* The block whose pages hold va. */
static Contig *contig_holding(uint32_t va)
{
    for (unsigned i = 0; i < g_ncontig; i++) {
        if (va - g_contig[i].va < g_contig[i].size) {
            return &g_contig[i];
        }
    }
    return NULL;
}

static void contig_free(NcKernel *k, uint32_t va, uint32_t ra)
{
    Contig *c = contig_at(va);
    if (c) {
        nc_vm_free_phys(k->vm, va & ~NC_CONTIG_WINDOW, c->size + c->slack);
        *c = g_contig[--g_ncontig];
    } else if (va) {
        static unsigned s_logged; /* a free that matches no block keeps its memory */
        if (s_logged++ < 16) {
            nc_log("hakuX-native-mm", "contiguous free of %08x from %08x: no such block", va, ra);
        }
    }
}

uint32_t nc_mm_contig_free_callers(NcKernel *k, uint32_t lo, uint32_t hi)
{
    uint32_t bytes = 0;
    /* from the last: a block freed takes the last one's place, already seen */
    for (unsigned i = g_ncontig; i-- > 0;) {
        const Contig c = g_contig[i];
        if (c.ra - lo < hi - lo) {
            bytes += c.size;
            contig_free(k, c.va, 0);
        }
    }
    return bytes;
}

uint32_t nc_mm_contig_alloc(NcKernel *k, uint32_t size)
{
    return contig_alloc(k, size, 0, 0, NC_PAGE, PAGE_READWRITE, 0);
}

/* MmAllocateContiguousMemory(NumberOfBytes): read-write (the kernel's Ex
 * with PAGE_READWRITE) */
static uint64_t MmAllocateContiguousMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return contig_alloc(k, a[0], 0, 0, NC_PAGE, PAGE_READWRITE, caller_of(k, f));
}

/* MmAllocateContiguousMemoryEx(NumberOfBytes, LowestAcceptableAddress,
 *   HighestAcceptableAddress, Alignment, ProtectionType). The bounds are page
 * frames, as the kernel's: the lowest address's page rounded up, the highest
 * address's page the last one the block may take (all of it, past the
 * address too). Fatal Frame II asks for 0x184E400 bytes in exactly
 * 0x00D0B000..0x025593FF, its texture pool, and uses that memory whether it
 * gets it or not: bounded by the address, the block's last page didn't fit,
 * the call failed, and the title's disc loads went to pages the heap had
 * (a heap block's header overwritten: a crash at boot; or a load's request:
 * a hang before the title screen). */
static uint64_t MmAllocateContiguousMemoryEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t hi = a[2] >= k->vm->ram_size - 1 ? 0 : (a[2] | (NC_PAGE - 1)) + 1;
    return contig_alloc(k, a[0], a[1], hi, a[3] ? a[3] : NC_PAGE, a[4], caller_of(k, f));
}

/* MmFreeContiguousMemory(BaseAddress) */
static uint64_t MmFreeContiguousMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    contig_free(k, a[0], caller_of(k, f));
    return 0;
}

/* MmAllocateSystemMemory(NumberOfBytes, Protect) */
static uint64_t MmAllocateSystemMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return contig_alloc(k, a[0], 0, 0, NC_PAGE, a[1], caller_of(k, f));
}

/* MmFreeSystemMemory(BaseAddress, NumberOfBytes) -> pages freed */
static uint64_t MmFreeSystemMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    Contig *c = contig_at(a[0]);
    uint32_t pages = c ? c->size / NC_PAGE : 0;
    contig_free(k, a[0], caller_of(k, f));
    return pages;
}

/* MmQueryAllocationSize(BaseAddress) */
static uint64_t MmQueryAllocationSize(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    Contig *c = contig_at(a[0]);
    if (c) {
        return c->size;
    }
    Region *r = region_at(a[0]);
    return r ? r->size : 0;
}

/* MmGetPhysicalAddress(BaseAddress) */
static uint64_t MmGetPhysicalAddress(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t pa = nc_vm_phys(k->vm, a[0]);
    if (pa == UINT32_MAX) {
        return 0;
    }
    nc_vm_pin(k->vm, pa, true, 0); /* the title may keep it: the page stays put */
    return pa;
}

/* MmIsAddressValid(VirtualAddress) */
static uint64_t MmIsAddressValid(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_vm_phys(k->vm, a[0]) != UINT32_MAX;
}

/* MmLockUnlockBufferPages(BaseAddress, NumberOfBytes, UnlockPages): locked
 * pages (DMA) are never relocated. */
static uint64_t MmLockUnlockBufferPages(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (!a[1]) {
        return 0;
    }
    const uint64_t end = (uint64_t)a[0] + a[1];
    for (uint64_t va = a[0] & ~(NC_PAGE - 1); va < end; va += NC_PAGE) {
        const uint32_t pa = nc_vm_phys(k->vm, (uint32_t)va);
        if (pa != UINT32_MAX) {
            nc_vm_pin(k->vm, pa, false, a[2] & 0xFF ? -1 : 1);
        }
    }
    return 0;
}

/* MmLockUnlockPhysicalPage(PhysicalAddress, UnlockPage) */
static uint64_t MmLockUnlockPhysicalPage(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_vm_pin(k->vm, a[0], false, a[1] & 0xFF ? -1 : 1);
    return 0;
}

/* MmUnmapIoSpace: nothing to do. */
static uint64_t MmNothing(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

/* ----------------------------------------- memory kept for the next title */

/* MmPersistContiguousMemory(BaseAddress, NumberOfBytes, Persist): contiguous
 * memory kept across a quick reboot. D3D's PersistDisplay keeps the frame on
 * screen for the next title (AvSetSavedDataAddress), which draws its loading
 * screen over it (Splinter Cell Chaos Theory's missions). The core boots
 * afresh for the next XBE: at the reboot the kept ranges go to a file (the
 * HLE GPU's images over them written back first), and the next boot puts
 * them back where they were, allocated, before the title loads. */
#define MAX_KEPT   8
#define KEPT_MAGIC 0x5045454Bu /* "KEEP" */

typedef struct KeptRange {
    uint32_t va, len;
} KeptRange;

static KeptRange g_kept[MAX_KEPT];
static unsigned g_nkept;

static uint64_t MmPersistContiguousMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t va = a[0], len = a[1];
    const bool keep = (a[2] & 0xFF) != 0;
    unsigned i = 0;
    while (i < g_nkept && g_kept[i].va != va) {
        i++;
    }
    if (keep && i == g_nkept && g_nkept < MAX_KEPT) {
        g_nkept++;
    }
    if (keep && i < g_nkept) {
        g_kept[i].va = va;
        g_kept[i].len = len;
    } else if (!keep && i < g_nkept) {
        g_kept[i] = g_kept[--g_nkept];
    }
    nc_log("hakuX-native-mm", "MmPersistContiguousMemory %08x+%u: %s", va, len, keep ? "kept" : "released");
    return 0;
}

void nc_mm_keep_drop(uint32_t va)
{
    for (unsigned i = 0; i < g_nkept; i++) {
        if (g_kept[i].va == va) {
            g_kept[i] = g_kept[--g_nkept];
            return;
        }
    }
}

static void kept_file(char *out, size_t n)
{
    snprintf(out, n, "%s/../kept.bin", nc_io_hdd_root());
}

static uint32_t kept_pa(NcKernel *k, uint32_t va)
{
    return (va & NC_CONTIG_WINDOW) ? va & ~NC_CONTIG_WINDOW : nc_vm_phys(k->vm, va);
}

void nc_mm_keep_save(NcKernel *k, uint32_t saved)
{
    char path[512];
    kept_file(path, sizeof(path));
    unlink(path);
    if (!g_nkept) {
        return;
    }
    for (unsigned i = 0; i < g_nkept && k->hle_keep; i++) {
        k->hle_keep(kept_pa(k, g_kept[i].va), g_kept[i].len);
    }
    if (k->hle_keep_wait && !k->hle_keep_wait(2000000000ull)) {
        nc_log("hakuX-native-mm", "kept memory: the GPU's copies did not come back; saved as it is");
    }
    FILE *fp = fopen(path, "wb");
    uint32_t hdr[3] = {KEPT_MAGIC, saved, g_nkept}, bytes = 0;
    bool ok = fp && fwrite(hdr, sizeof(hdr), 1, fp) == 1;
    for (unsigned i = 0; ok && i < g_nkept; i++) {
        const uint32_t pa = kept_pa(k, g_kept[i].va), len = g_kept[i].len;
        const uint32_t rec[2] = {g_kept[i].va, len};
        if (pa < k->vm->ram_size && len <= k->vm->ram_size - pa) {
            nc_vm_will_read(NC_CONTIG_WINDOW + pa, len);    /* the window's pages a target closed */
        }
        ok = pa < k->vm->ram_size && len <= k->vm->ram_size - pa && fwrite(rec, sizeof(rec), 1, fp) == 1 &&
             fwrite(k->vm->ram + pa, 1, len, fp) == len;
        bytes += len;
    }
    if (fp) {
        fclose(fp);
    }
    if (!ok) {
        unlink(path);
    }
    nc_log("hakuX-native-mm", "kept memory: %u range(s), %u KiB, saved display %08x%s", g_nkept, bytes >> 10, saved,
           ok ? " for the next title" : ": not written");
}

uint32_t nc_mm_keep_restore(NcKernel *k)
{
    char path[512];
    kept_file(path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    uint32_t hdr[3] = {0};
    if (fread(hdr, sizeof(hdr), 1, fp) != 1 || hdr[0] != KEPT_MAGIC) {
        hdr[1] = hdr[2] = 0;
    }
    for (unsigned i = 0; i < hdr[2] && i < MAX_KEPT; i++) {
        uint32_t rec[2];
        if (fread(rec, sizeof(rec), 1, fp) != 1) {
            break;
        }
        const uint32_t va = rec[0], len = rec[1], pa = va & ~NC_CONTIG_WINDOW;
        const uint32_t size = nc_align_up(len ? len : 1, NC_PAGE);
        const bool fits = (va & NC_CONTIG_WINDOW) && pa < k->vm->ram_size && size <= k->vm->ram_size - pa &&
                          g_ncontig < MAX_CONTIG;
        if (!fits || !nc_vm_claim_phys(k->vm, pa, size) || fread(k->vm->ram + pa, 1, len, fp) != len) {
            nc_log("hakuX-native-mm", "kept memory %08x+%u: not restored", va, len);
            if (fits) {
                fseek(fp, len, SEEK_CUR);
            }
            continue;
        }
        g_contig[g_ncontig++] = (Contig){va, size, 0, 0, PAGE_READWRITE};
        g_kept[g_nkept].va = va;
        g_kept[g_nkept++].len = len;
        nc_log("hakuX-native-mm", "kept memory %08x+%u restored", va, len);
    }
    fclose(fp);
    unlink(path);
    return hdr[1];
}

unsigned nc_mm_kept(uint32_t *pa, uint32_t *len, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_nkept && n < max; i++, n++) {
        pa[n] = g_kept[i].va & ~NC_CONTIG_WINDOW;
        len[n] = g_kept[i].len;
    }
    return n;
}

/* MmQueryAddressProtect(VirtualAddress): the page's protection as it was
 * allocated or set, cache bits included. Ultimate Spider-Man's free() tells
 * its write-combined contiguous blocks (PAGE_READWRITE | PAGE_WRITECOMBINE)
 * by them: told PAGE_READWRITE, it freed one as a heap block, through the
 * dword before it (a fault the title doesn't handle, at boot). */
static uint64_t MmQueryAddressProtect(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (nc_vm_phys(k->vm, a[0]) == UINT32_MAX) {
        return 0;
    }
    const Contig *c = contig_holding(a[0]);
    const Region *r = c ? NULL : region_at(a[0]);
    const uint32_t p = c ? c->protect : r ? r->protect : 0;
    return p ? p : PAGE_READWRITE;
}

/* MmSetAddressProtect(BaseAddress, NumberOfBytes, NewProtect): recorded for
 * MmQueryAddressProtect, for the contiguous block it falls in (memory is flat
 * here: no access checks) */
static uint64_t MmSetAddressProtect(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    Contig *c = contig_holding(a[0]);
    if (c) {
        c->protect = a[2];
    }
    return 0;
}

/* MmClaimGpuInstanceMemory(NumberOfBytes, NumberOfPaddingBytes): the GPU's
 * instance memory sits at the top of RAM below a 64 KiB pad; the result is
 * the end of that block. */
static uint64_t MmClaimGpuInstanceMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t pad = 0x10000;
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], pad);
    }
    if (!g_gpu_instance_end) {
        g_gpu_instance_end = k->vm->ram_size - pad;
        uint32_t size = nc_align_up(a[0], NC_PAGE);
        nc_vm_claim_phys(k->vm, g_gpu_instance_end - size, size + pad);
    }
    return NC_CONTIG_WINDOW | g_gpu_instance_end;
}

/* MmMapIoSpace(PhysicalAddress, NumberOfBytes, ProtectType): device windows
 * keep their addresses; RAM is reached through the contiguous window. */
static uint64_t MmMapIoSpace(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return a[0] < k->vm->ram_size ? (NC_CONTIG_WINDOW | a[0]) : a[0];
}

/* MmCreateKernelStack(NumberOfBytes, DebuggerThread) -> top of the new stack */
static uint64_t MmCreateKernelStack(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t base;
    if (!nc_ps_stack_alloc(k, nc_align_up(a[0], NC_PAGE), &base)) {
        return 0;
    }
    return base + nc_align_up(a[0], NC_PAGE);
}

/* MmDeleteKernelStack(StackBase, StackLimit) */
static uint64_t MmDeleteKernelStack(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    decommit(k, a[1], a[0] - a[1]);
    if (a[0] > a[1]) {
        nc_ps_stack_free(k, a[1], a[0] - a[1]);
    }
    return 0;
}

/* MmQueryStatistics(MemoryStatistics): MM_STATISTICS { Length (0x24), TotalPhysicalPages,
 *   AvailablePages, VirtualMemoryBytesCommitted, VirtualMemoryBytesReserved,
 *   CachePagesCommitted, PoolPagesCommitted, StackPagesCommitted, ImagePagesCommitted } */
static uint64_t MmQueryStatistics(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (nc_vm_rd32(k->vm, a[0]) != 0x24) {
        return NC_STATUS_INVALID_PARAMETER;
    }
    const uint32_t total = k->vm->ram_size / NC_PAGE;
    uint32_t used = 0;
    for (uint32_t p = 0; p < total; p++) {
        used += k->vm->page_used[p] != 0;
    }
    uint32_t committed = 0, reserved = 0, image = 0;
    for (unsigned i = 0; i < g_nregions; i++) {
        reserved += g_regions[i].size;
        for (uint32_t p = g_regions[i].base; p < g_regions[i].base + g_regions[i].size; p += NC_PAGE) {
            if (nc_vm_phys(k->vm, p) != UINT32_MAX) {
                if (g_regions[i].image) {
                    image++;
                } else {
                    committed += NC_PAGE;
                }
            }
        }
    }
    const uint32_t s[9] = {0x24, total, total - used, committed, reserved, 0, k->pool_len / NC_PAGE, 0, image};
    memcpy(nc_vm_host(k->vm, a[0]), s, sizeof(s));
    return NC_STATUS_SUCCESS;
}

void nc_mm_register(NcKernel *k)
{
    nc_kernel_impl(k, "NtAllocateVirtualMemory", NtAllocateVirtualMemory);
    nc_kernel_impl(k, "NtFreeVirtualMemory", NtFreeVirtualMemory);
    nc_kernel_impl(k, "NtQueryVirtualMemory", NtQueryVirtualMemory);
    nc_kernel_impl(k, "NtProtectVirtualMemory", NtProtectVirtualMemory);
    nc_kernel_impl(k, "MmAllocateContiguousMemory", MmAllocateContiguousMemory);
    nc_kernel_impl(k, "MmAllocateContiguousMemoryEx", MmAllocateContiguousMemoryEx);
    nc_kernel_impl(k, "MmFreeContiguousMemory", MmFreeContiguousMemory);
    nc_kernel_impl(k, "MmAllocateSystemMemory", MmAllocateSystemMemory);
    nc_kernel_impl(k, "MmFreeSystemMemory", MmFreeSystemMemory);
    nc_kernel_impl(k, "MmQueryAllocationSize", MmQueryAllocationSize);
    nc_kernel_impl(k, "MmGetPhysicalAddress", MmGetPhysicalAddress);
    nc_kernel_impl(k, "MmIsAddressValid", MmIsAddressValid);
    nc_kernel_impl(k, "MmPersistContiguousMemory", MmPersistContiguousMemory);
    nc_kernel_impl(k, "MmSetAddressProtect", MmSetAddressProtect);
    nc_kernel_impl(k, "MmLockUnlockBufferPages", MmLockUnlockBufferPages);
    nc_kernel_impl(k, "MmLockUnlockPhysicalPage", MmLockUnlockPhysicalPage);
    nc_kernel_impl(k, "MmUnmapIoSpace", MmNothing);
    nc_kernel_impl(k, "MmQueryAddressProtect", MmQueryAddressProtect);
    nc_kernel_impl(k, "MmClaimGpuInstanceMemory", MmClaimGpuInstanceMemory);
    nc_kernel_impl(k, "MmMapIoSpace", MmMapIoSpace);
    nc_kernel_impl(k, "MmCreateKernelStack", MmCreateKernelStack);
    nc_kernel_impl(k, "MmDeleteKernelStack", MmDeleteKernelStack);
    nc_kernel_impl(k, "MmQueryStatistics", MmQueryStatistics);
}

/* ---------------------------------------------------------- savestates */

typedef struct NcMmState {
    uint32_t nregions, ncontig, nkept, gpu_instance_end;
} NcMmState;

#define NC_DEF_SECTION MM
#define NC_DEF_FILE "native/state/mm.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_mm_state_save(NcBuf *b)
{
    const NcMmState st = {g_nregions, g_ncontig, g_nkept, g_gpu_instance_end};
    nc_rec_put(b, &nc_rec_NcMmState, &st);
    nc_recs_put(b, &nc_rec_Region, g_regions, sizeof(g_regions[0]), g_nregions);
    nc_recs_put(b, &nc_rec_Contig, g_contig, sizeof(g_contig[0]), g_ncontig);
    nc_recs_put(b, &nc_rec_KeptRange, g_kept, sizeof(g_kept[0]), g_nkept);
    return true;
}

bool nc_mm_state_load(NcRd *r, unsigned v)
{
    NcMmState st = {0};
    nc_rec_get(r, &nc_rec_NcMmState, &st, v);
    if (st.nregions > MAX_REGIONS || st.ncontig > MAX_CONTIG || st.nkept > MAX_KEPT) {
        return false;
    }
    memset(g_regions, 0, sizeof(g_regions));
    memset(g_contig, 0, sizeof(g_contig));
    memset(g_kept, 0, sizeof(g_kept));
    nc_recs_get(r, &nc_rec_Region, g_regions, sizeof(g_regions[0]), st.nregions, v);
    nc_recs_get(r, &nc_rec_Contig, g_contig, sizeof(g_contig[0]), st.ncontig, v);
    nc_recs_get(r, &nc_rec_KeptRange, g_kept, sizeof(g_kept[0]), st.nkept, v);
    g_nregions = st.nregions;
    g_ncontig = st.ncontig;
    g_nkept = st.nkept;
    g_gpu_instance_end = st.gpu_instance_end;
    return !r->fail;
}
