/*
 * hakuX native core — guest-write tracking for the HLE layer.
 *
 * The HLE layer asks whether the guest CPU wrote guest RAM since it last
 * looked (host.h, hle_host_mem_*), and needs to hear about the first
 * write to a page it cleaned (hle_mem_first_write: the first-write barrier may
 * wait for the GPU before the write happens). A clean page is write-protected
 * in every guest mapping, so the guest's first store faults: the handler
 * calls hle_mem_first_write, marks the page dirty, unprotects it and the store
 * runs again. Invariant: protected <=> clean.
 *
 * Host code writes through the RAM view (vm->phys), which is never protected;
 * host stores by guest VA report themselves (hle_va_store).
 */
#include "native/hle/hle_native.h"

#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/mem/barrier.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static NcVm *g_vm;

void nc_hle_dirty_reset(void)
{
    g_vm = NULL;                /* nc_vm_destroy cleared the hooks */
}

void nc_hle_dirty_init(NcVm *vm)
{
    g_vm = vm; /* every page starts dirty (unprotected) */
    nc_vm_write_hook = hle_va_store;
    /* overlapped file reads: the reader waits for the GPU, not the guest */
    nc_vm_dma = (NcVmDma){hle_mem_write_fence, hle_mem_wait_fence, hle_host_mem_set_dirty};
}

bool hle_host_mem_dirty(uint32_t pa)
{
    if (!g_vm || pa >= g_vm->ram_size) {
        return true;
    }
    return __atomic_load_n(&g_vm->wp[pa / NC_PAGE], __ATOMIC_ACQUIRE) != NC_WP_CLEAN;
}

static uint64_t ram_end(uint32_t pa, uint32_t len)
{
    const uint64_t end = (uint64_t)pa + len;
    return end < g_vm->ram_size ? end : g_vm->ram_size;
}

/* Pages of [pa, pa + len) whose protection is not `ro` get it, a run of
 * them at a time; true if there were any. */
static bool protect_runs(uint32_t pa, uint32_t len, bool ro)
{
    bool any = false;
    const uint64_t end = ram_end(pa, len);
    uint32_t run_pa = 0, run_n = 0;
    for (uint64_t a = pa & ~(uint64_t)(NC_PAGE - 1); a < end; a += NC_PAGE) {
        if (g_vm->wp[a / NC_PAGE] == ro) {
            nc_vm_protect_pages(g_vm, run_pa, run_n, ro);
            run_n = 0;
            continue;
        }
        any = true;
        if (!run_n) {
            run_pa = (uint32_t)a;
        }
        run_n++;
    }
    nc_vm_protect_pages(g_vm, run_pa, run_n, ro);
    return any;
}

bool hle_host_mem_test_and_clear(uint32_t pa, uint32_t len)
{
    return g_vm && protect_runs(pa, len, true);
}

/* A host write: every view of the page writable (the host writes through
 * the guest's), untracked until protected again. */
static void make_dirty(uint32_t pa)
{
    if (g_vm->wp[pa / NC_PAGE]) {
        nc_vm_protect_page(g_vm, pa & ~(NC_PAGE - 1), false);
    }
}

/* A guest store's fault: only the view it came through (at) made writable,
 * then the page marked written (NC_WP_WRITTEN; writable first, as
 * nc_vm_protect_pages clears its flag after). Making a view of the shared
 * RAM writable is the dear part of a fault: all three views 13.5 us a fault
 * on the Odin 3, this one 6.5 (DOA2 takes ~10k a second). A store through
 * another view faults once more and opens that one. */
static void open_view(uint32_t pa, void *at)
{
    void *page = (void *)((uintptr_t)at & ~(uintptr_t)(NC_PAGE - 1));
    if (mprotect(page, NC_PAGE, PROT_READ | PROT_WRITE)) {
        nc_vm_protect_page(g_vm, pa & ~(NC_PAGE - 1), false);   /* every view, as before */
        return;
    }
    __atomic_store_n(&g_vm->wp[pa / NC_PAGE], NC_WP_WRITTEN, __ATOMIC_RELEASE);
}

/* debug: NC_DEVWATCH="pa:len" (hex) logs each device write into that guest
 * RAM that reports itself dirty (the APU's DMA, a device model writing guest
 * memory through the RAM view, which nothing protects), with the host code
 * that wrote it as offsets in this library (llvm-addr2line). */
static int g_watch_on = -1;
static uint32_t g_watch_lo, g_watch_hi;

/* (the callers' return addresses are taken only when it is) */
static bool dev_watch_on(void)
{
    if (g_watch_on < 0) {
        const char *s = getenv("NC_DEVWATCH");
        g_watch_on = s && sscanf(s, "%x:%x", &g_watch_lo, &g_watch_hi) == 2 && g_watch_hi;
        g_watch_hi += g_watch_lo;
    }
    return g_watch_on;
}

static void dev_watch(uint32_t start, uint32_t length, void *ra0, void *ra1)
{
    static unsigned logged;
    const uint32_t lo = g_watch_lo, hi = g_watch_hi;
    if (start >= hi || start + length <= lo || logged >= 200) {
        return;
    }
    logged++;
    Dl_info di;
    const uintptr_t base = dladdr((void *)dev_watch, &di) ? (uintptr_t)di.dli_fbase : 0;
    nc_log("hakuX-native-watch", "device write to pa %08x, %u bytes, from +%lx +%lx",
           start, length, (unsigned long)((uintptr_t)ra0 - base),
           (unsigned long)((uintptr_t)ra1 - base));
}

void hle_host_mem_set_dirty(uint32_t pa, uint32_t len)
{
    if (dev_watch_on()) {
        dev_watch(pa, len, __builtin_return_address(0), __builtin_return_address(1));
    }
    if (!g_vm) {
        return;
    }
    hle_mem_host_wrote(pa, len);        /* its cache lines, before the GPU reads them */
    protect_runs(pa, len, false);
}

void hle_host_mem_noaccess(uint32_t pa, uint32_t len, bool on)
{
    if (g_vm && len) {
        const uint32_t lo = pa & ~(NC_PAGE - 1), hi = (pa + len + NC_PAGE - 1) & ~(NC_PAGE - 1);
        nc_vm_noaccess(g_vm, lo, (hi - lo) / NC_PAGE, on);
    }
}

/* A guest store to a clean page: the first-write barrier, then the write. */
static void first_write(uint32_t pa, uint32_t len)
{
    hle_mem_first_write(pa, len);
    make_dirty(pa);
}

static uint32_t g_wlo, g_whi, g_wdelay; /* NC_WATCHPA="pa:len[:seconds]" */

static bool watch_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("NC_WATCHPA");
        on = s && sscanf(s, "%x:%x:%u", &g_wlo, &g_whi, &g_wdelay) >= 2 && g_whi;
        g_whi += g_wlo;
        if (on) {
            nc_log("hakuX-native-watch", "stores to pa %08x..%08x, from %u s", g_wlo, g_whi, g_wdelay);
        }
    }
    return on;
}

/* Armed once g_wdelay seconds have passed (the clock read every 4096 calls). */
static bool watch_armed(void)
{
    static bool armed;
    static uint64_t t0;
    static unsigned calls;
    if (armed) {
        return true;
    }
    if (calls++ & 4095) {
        return false;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!t0) {
        t0 = now;
    }
    if (now - t0 >= (uint64_t)g_wdelay * 1000000000ull) {
        armed = true;
        nc_log("hakuX-native-watch", "armed");
    }
    return armed;
}

bool nc_hle_watch_pa(uint32_t pa)
{
    return watch_on() && pa >= g_wlo && pa < g_whi;
}

void nc_hle_watch_store(uint32_t va, uint32_t pa, uint32_t eip, uint32_t len)
{
    static unsigned logged;
    if (logged++ < 300) {
        if (len) {
            nc_log("hakuX-native-watch", "host store to %08x (pa %08x), %u bytes", va, pa, len);
        } else {
            nc_log("hakuX-native-watch", "guest store to %08x (pa %08x) at eip %08x", va, pa, eip);
        }
    }
}

void nc_hle_watch_rearm(void)
{
    if (!g_vm || !watch_on() || !watch_armed()) {
        return;
    }
    /* Protected again when written, or when a new mapping of the page came
     * up writable (a page protected before it was mapped). */
    static uint32_t armed[16];
    unsigned i = 0;
    for (uint32_t a = g_wlo & ~(NC_PAGE - 1); a < g_whi && a < g_vm->ram_size && i < 16; a += NC_PAGE, i++) {
        const uint32_t va = g_vm->rmap[a / NC_PAGE];
        if (g_vm->wp[a / NC_PAGE] != NC_WP_CLEAN || armed[i] != va) {
            nc_vm_protect_page(g_vm, a, true);
            armed[i] = va;
        }
    }
}

void hle_va_store(uint32_t va, uint32_t len)
{
    if (!g_vm) {
        return;
    }
    {
        const uint32_t spa = nc_vm_phys(g_vm, va);
        if (spa != UINT32_MAX && watch_on() && spa < g_whi && spa + len > g_wlo) {
            nc_hle_watch_store(va, spa, 0, len);
        }
    }
    for (uint32_t a = va & ~(NC_PAGE - 1); a < va + len; a += NC_PAGE) {
        const uint32_t pa = nc_vm_phys(g_vm, a);
        if (pa == UINT32_MAX || pa >= g_vm->ram_size) {
            continue;
        }
        if (nc_vm_noaccess_has(g_vm, pa)) {
            /* a page without access (barrier.c: a frame buffer whose image is
             * newer, a report page): what a guest access's fault does, as a
             * host write into it fails instead (EFAULT). Halo 2 streams
             * bitmaps from its map cache into an old frame buffer's memory;
             * the reads failed and its scope reticle sampled the frame
             * buffer's leftover bytes. */
            hle_mem_stale_access(pa);
        }
        if (g_vm->wp[pa / NC_PAGE] == NC_WP_CLEAN) {
            first_write(pa, len);
        } else if (g_vm->wp[pa / NC_PAGE] == NC_WP_WRITTEN) {
            make_dirty(pa);             /* written already: its other views opened for the host */
        }
    }
}

static struct {
    uint32_t page, n;
} g_dtop[16];
static uint32_t g_dfaults;

unsigned nc_hle_dirty_top(uint32_t *pages, uint32_t *counts, unsigned max)
{
    unsigned n = 0;
    for (; n < max; n++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_dtop[k].n && (best < 0 || g_dtop[k].n > g_dtop[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        pages[n] = g_dtop[best].page;
        counts[n] = g_dtop[best].n * 64;
        g_dtop[best].n = 0;
    }
    memset(g_dtop, 0, sizeof(g_dtop));
    return n;
}

static void dirty_sample(uint32_t pa)
{
    if (++g_dfaults & 63) {
        return;
    }
    unsigned k = 0;
    while (k < 16 && g_dtop[k].n && g_dtop[k].page != pa >> 12) {
        k++;
    }
    if (k < 16) {
        g_dtop[k].page = pa >> 12;
        g_dtop[k].n++;
    }
}

bool nc_hle_dirty_fault(const siginfo_t *info)
{
    if (!g_vm) {
        return false;
    }
    const uint64_t guest = nc_vm_guest_of(g_vm, info->si_addr);
    if (guest == UINT64_MAX) {
        return false;
    }
    const uint32_t pa = nc_vm_phys(g_vm, (uint32_t)guest);
    if (pa == UINT32_MAX || pa >= g_vm->ram_size) {
        return false;
    }
    if (nc_vm_noaccess_has(g_vm, pa)) {
        /* a read or write of memory an image is newer than (the frame on
         * screen): the image goes in first, then the access runs again */
        hle_mem_stale_access(pa);
        return true;
    }
    const uint8_t wp = __atomic_load_n(&g_vm->wp[pa / NC_PAGE], __ATOMIC_ACQUIRE);
    if (!wp) {
        /* protected, but not by write tracking: someone else's (the JIT's
         * code pages), or tracking lost the page (a crash follows) */
        static unsigned s_logged;
        if (s_logged++ < 8) {
            nc_log("hakuX-native-watch", "write fault at guest %08x (pa %08x), a page tracking "
                   "doesn't hold protected", (uint32_t)guest, pa);
        }
        return false;
    }
    if (wp == NC_WP_WRITTEN) {
        open_view(pa, info->si_addr);   /* written through another view already */
        return true;
    }
    dirty_sample(pa);
    static int all_views = -1;          /* NC_WP_ALL_VIEWS: every view opened, as before (A/B) */
    if (all_views < 0) {
        all_views = getenv("NC_WP_ALL_VIEWS") != NULL;
    }
    if (all_views) {
        first_write(pa, 4);
        return true;
    }
    hle_mem_first_write(pa, 4);
    open_view(pa, info->si_addr);
    return true;
}
