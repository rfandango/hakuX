/*
 * hakuX HLE GPU — first-write barrier and write recorder.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/mem/pageuse.h"

#include <sched.h>

static HlePageUse g_pu;
static uint64_t g_ram;
static uint64_t (*g_completed)(void);
static void (*g_wait)(uint64_t);
static void (*g_flush)(uint64_t);
static uint32_t *g_contended;       /* bit per page: a first write there waited */
static HleBarrierStats g_st;
static uint32_t g_wpages[8];        /* debug: the pages whose first write waited, a ring */
static unsigned g_nwpages;
static bool g_watch;                /* hle_mem_written() cleared HLE bits */
static uint64_t g_stale;            /* hle_barrier_set_stale's range: pa << 32 | len (atomic) */
static void (*g_stale_fill)(uint32_t pa, uint32_t len);
/* per page, the bytes copied for draws a later draw may read again
 * (hle_barrier_arm_copied): first | end << 16, 0 = none (atomic) */
static uint32_t *g_cspan;
static void (*g_catch_up)(void);
static uint8_t *g_hot;              /* per page: hot beside guards (hle_mem_stale_neighbour; stale lock) */
static uint16_t *g_nbr_n;           /* ... per page: accesses served since g_nbr_t (ms; stale lock) */
static uint32_t *g_nbr_t;

/* The GPU reads and writes guest RAM in place (the imported RAM buffer,
 * vk/guest_ram.c) without snooping the CPU's caches: the cached, coherent
 * memory type holds for the driver's own allocations, not for the imported
 * AHB. A store still in a CPU cache line is invisible to it: the movie
 * Oddworld draws on the CPU into its back buffer under the title menu,
 * uploaded in place at each draw, came out with 64-byte black dashes (lines
 * the CPU still held) in push-buffer mode. Each page counts the writes
 * reported to it (first-write faults, host writes through the RAM view);
 * arming a range for the GPU cleans the lines of its pages written since
 * their last clean, sampled before the arm protects them (a store after
 * that faults and counts). */
static uint32_t *g_wseq;            /* per page: writes reported */
static uint32_t *g_cseq;            /* per page: g_wseq + 1 at its last clean */
static uint32_t g_line = 64;        /* data cache line bytes (CTR_EL0.DminLine) */

static uint32_t g_wepoch;          /* writes reported anywhere (atomic): hle_mem_write_epoch */
static uint32_t *g_bseq;            /* per WBLOCK pages: their writes reported (hle_mem_wseq_blocks) */
#define WBLOCK_SHIFT 6

static void wseq_bump(uint32_t pa, uint32_t len)
{
    if (!g_wseq || !len || (uint64_t)pa + len > g_ram) {
        return;
    }
    for (uint32_t p = pa >> 12, last = (pa + len - 1) >> 12; p <= last; p++) {
        __atomic_fetch_add(&g_wseq[p], 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_bseq[p >> WBLOCK_SHIFT], 1, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&g_wepoch, 1, __ATOMIC_RELEASE);
}

uint32_t hle_mem_write_epoch(void)
{
    return __atomic_load_n(&g_wepoch, __ATOMIC_ACQUIRE);
}

uint64_t hle_mem_wseq_blocks(uint32_t pa, uint32_t len)
{
    uint64_t s = 0;
    if (!g_bseq || !len || (uint64_t)pa + len > g_ram) {
        return 0;
    }
    for (uint32_t b = pa >> (12 + WBLOCK_SHIFT), last = (pa + len - 1) >> (12 + WBLOCK_SHIFT); b <= last; b++) {
        s += __atomic_load_n(&g_bseq[b], __ATOMIC_ACQUIRE);
    }
    return s;
}

/* [pa, pa + len)'s cache lines to memory (inval: and out of the cache) */
static void cache_lines(uint32_t pa, uint32_t len, bool inval)
{
#if defined(__aarch64__)
    const uint8_t *p = hle_pa_ptr(pa, len);
    if (!p) {
        return;
    }
    const uintptr_t end = (uintptr_t)p + len;
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)(g_line - 1); a < end; a += g_line) {
        if (inval) {
            __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
        } else {
            __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
        }
    }
    __asm__ volatile("dsb sy" : : : "memory");
#else
    (void)pa;
    (void)len;
    (void)inval;
#endif
}

/* The pages of [pa, pa + len) written since their last clean, as the span
 * [*p0, *p1] (now counted clean); false if none */
static bool stale_pages(uint32_t pa, uint32_t len, uint32_t *p0, uint32_t *p1)
{
    uint32_t lo = UINT32_MAX, hi = 0;
    if (!g_wseq) {
        return false;
    }
    for (uint32_t p = pa >> 12, last = (pa + len - 1) >> 12; p <= last; p++) {
        const uint32_t w = __atomic_load_n(&g_wseq[p], __ATOMIC_RELAXED) + 1;
        if (g_cseq[p] != w) {
            g_cseq[p] = w;
            lo = MIN(lo, p);
            hi = MAX(hi, p);
        }
    }
    *p0 = lo;
    *p1 = hi;
    return lo <= hi;
}

void hle_barrier_init(uint64_t ram_size)
{
    g_ram = ram_size;
    hle_pageuse_init(&g_pu, ram_size);
    g_contended = g_new0(uint32_t, (ram_size >> 12) / 32 + 1);
    g_cspan = g_new0(uint32_t, ram_size >> 12);
    g_hot = g_new0(uint8_t, ram_size >> 12);
    if (!getenv("HAKUX_HLE_NO_CLEAN")) {     /* debug: no cache cleaning (A/B) */
        g_wseq = g_new0(uint32_t, ram_size >> 12);
        g_cseq = g_new0(uint32_t, ram_size >> 12);
        g_bseq = g_new0(uint32_t, (ram_size >> (12 + WBLOCK_SHIFT)) + 1);
    }
    g_nbr_n = g_new0(uint16_t, ram_size >> 12);
    g_nbr_t = g_new0(uint32_t, ram_size >> 12);
#if defined(__aarch64__)
    {
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        g_line = 4u << ((ctr >> 16) & 0xF);
    }
#endif
}

void hle_barrier_set_waiter(uint64_t (*completed)(void), void (*wait)(uint64_t))
{
    g_completed = completed;
    g_wait = wait;
}

void hle_barrier_set_flush(void (*flush)(uint64_t value))
{
    __atomic_store_n(&g_flush, flush, __ATOMIC_RELEASE);
}

void hle_barrier_set_catch_up(void (*catch_up)(void))
{
    __atomic_store_n(&g_catch_up, catch_up, __ATOMIC_RELEASE);
}

void hle_barrier_arm_copied(uint32_t pa, uint32_t len)
{
    if (!g_cspan || !__atomic_load_n(&g_catch_up, __ATOMIC_ACQUIRE) || !len ||
        (uint64_t)pa + len > g_ram) {
        return;
    }
    const uint64_t end = (uint64_t)pa + len;
    for (uint64_t a = pa; a < end; a = (a | 4095) + 1) {
        const uint32_t lo = (uint32_t)(a & 4095);
        const uint32_t hi = end - (a & ~4095ull) < 4096 ? (uint32_t)(end & 4095) : 4096;
        uint32_t *s = &g_cspan[a >> 12];
        uint32_t old = __atomic_load_n(s, __ATOMIC_RELAXED), v;
        do {
            v = old ? MIN(old & 0xFFFF, lo) | MAX(old >> 16, hi) << 16 : lo | hi << 16;
        } while (!__atomic_compare_exchange_n(s, &old, v, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
    /* protected after the spans are set: a store from now on faults and
     * finds its span */
    uint64_t lo = pa & ~4095u;
    hle_host_mem_test_and_clear(hle_ram_addr_base() + lo, ((end + 4095) & ~4095ull) - lo);
}

bool hle_barrier_contended(uint32_t pa, uint32_t len)
{
    if (!g_contended || !len || (uint64_t)pa + len > g_ram) {
        return true;
    }
    for (uint32_t p = pa >> 12, last = (uint32_t)(((uint64_t)pa + len - 1) >> 12); p <= last; p++) {
        if (__atomic_load_n(&g_contended[p >> 5], __ATOMIC_RELAXED) & (1u << (p & 31))) {
            return true;
        }
    }
    return false;
}

bool hle_mem_written(uint32_t pa, uint32_t len)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return true;
    }
    g_watch = true;
    uint64_t lo = pa & ~4095u;
    uint64_t hi = ((uint64_t)pa + len + 4095) & ~4095ull;
    return hle_host_mem_test_and_clear(hle_ram_addr_base() + lo, hi - lo);
}

void hle_mem_mark_written(uint32_t pa, uint32_t len)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    hle_host_mem_set_dirty(hle_ram_addr_base() + pa, len);
}

void hle_mem_mark_landed(uint32_t pa, uint32_t len)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    hle_host_mem_landed(hle_ram_addr_base() + pa, len);
}

uint64_t hle_mem_wseq_sum(uint32_t pa, uint32_t len, uint32_t *first, uint32_t *last)
{
    uint64_t s = 0;
    *first = *last = 0;
    if (!g_wseq || !len || (uint64_t)pa + len > g_ram) {
        return 0;
    }
    const uint32_t p0 = pa >> 12, p1 = (pa + len - 1) >> 12;
    for (uint32_t p = p0; p <= p1; p++) {
        const uint32_t w = __atomic_load_n(&g_wseq[p], __ATOMIC_ACQUIRE);
        s += w;
        *first = p == p0 ? w : *first;
        *last = w;
    }
    return s;
}

void hle_barrier_arm(uint32_t pa, uint32_t len, uint64_t value)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    uint32_t p0, p1;
    const bool stale = stale_pages(pa, len, &p0, &p1);     /* before the protection */
    hle_pageuse_mark(&g_pu, pa, len, value);
    uint64_t lo = pa & ~4095u;
    uint64_t hi = ((uint64_t)pa + len + 4095) & ~4095ull;
    hle_host_mem_test_and_clear(hle_ram_addr_base() + lo, hi - lo);
    if (stale) {
        cache_lines(p0 << 12, (p1 - p0 + 1) << 12, false);
        g_st.cleaned += p1 - p0 + 1;
    }
}

/* g_stale's range and its pages' access change together (the decode thread
 * at each swap, the guest's thread at its access) */
static int g_stale_lock;

static void stale_lock(void)
{
    while (__atomic_exchange_n(&g_stale_lock, 1, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
}

static void stale_unlock(void)
{
    __atomic_store_n(&g_stale_lock, 0, __ATOMIC_RELEASE);
}

/* The ranges without guest access: frames that were on screen, kept so until
 * the guest touches them. A change of protection over a frame buffer is
 * ~90 us (the RP5: Forza, 1.2 MB a range); given back at each swap, the
 * frames' were ~0.4 ms of every swap. Kept, a double-buffered title's two
 * frame buffers are protected after its first two swaps and no swap changes
 * any; a touch of one no longer on screen just gives its access back (no
 * copy: a back buffer's memory as before, Bink movies drawn into it on the
 * CPU), so a title that does that pays a fault and one change a frame.
 * hle_barrier_guard's ranges share the table, each with its fill. */
#define STALE_RANGES 4
#define GUARD_RANGES 128
static struct {
    uint32_t pa, len;
    bool guard;                                 /* a target's (hle_barrier_guard); else a frame's */
    bool open;                                  /* guard: its pages given back, though armed again
                                                   while the guest waited (till its next arming) */
    uint32_t gen;                               /* guard: g_guard_gen at its last arming */
    uint64_t v;                                 /* guard: its image's landing (0: none asked) */
} g_na[STALE_RANGES + GUARD_RANGES];
static unsigned g_nna;
static uint32_t g_guard_gen;                    /* (stale lock) */
static uint32_t g_guard_drops = 1;              /* guards dropped or opened (atomic) */
static uint64_t (*g_guard_fill)(uint32_t pa, uint32_t len, bool read);
/* The guest's accesses beside a guard's bytes on its edge pages
 * (hle_mem_stale_neighbour), each a fault the host serves (a few us): a
 * page where they come often (NBR_HOT in NBR_MS, ~1% of a core) is hot,
 * and no guard closes it as an edge page till its memory is freed or the
 * mode starts again (its targets' bytes there unguarded: a dynamic vertex
 * buffer beside a target would have cost a fault a store). */
#define NBR_HOT 256
#define NBR_MS 100
static uint64_t g_nbr_served, g_nbr_hot;

static bool rpt_page_shut(uint32_t pa);

static int na_find(uint32_t pa, uint32_t len)
{
    for (unsigned i = 0; i < g_nna; i++) {
        if (len ? g_na[i].pa == pa && g_na[i].len == len : pa - g_na[i].pa < g_na[i].len) {
            return (int)i;
        }
    }
    return -1;
}

/* The frame's (guard false) or the guard's (true) range [pa, pa + len) */
static int na_find_kind(uint32_t pa, uint32_t len, bool guard)
{
    for (unsigned i = 0; i < g_nna; i++) {
        if (g_na[i].pa == pa && g_na[i].len == len && g_na[i].guard == guard) {
            return (int)i;
        }
    }
    return -1;
}

static bool na_closed(unsigned i)
{
    return !g_na[i].guard || !g_na[i].open;
}

static uint32_t na_page_lo(unsigned i)
{
    return g_na[i].pa >> 12;
}

static uint32_t na_page_hi(unsigned i)
{
    return (g_na[i].pa + g_na[i].len - 1) >> 12;
}

/* The closed range whose bytes hold pa, else a closed guard with bytes on its
 * page: a target rarely starts and ends on a page, and the guest's access
 * to its own data on a target's edge page that the decoder couldn't do for
 * it (dirty.c) needs the target's image there first */
static int na_find_closed(uint32_t pa)
{
    for (unsigned i = 0; i < g_nna; i++) {
        if (na_closed(i) && pa - g_na[i].pa < g_na[i].len) {
            return (int)i;
        }
    }
    for (unsigned i = 0; i < g_nna; i++) {
        if (na_closed(i) && g_na[i].guard && pa >> 12 >= na_page_lo(i) && pa >> 12 <= na_page_hi(i)) {
            return (int)i;
        }
    }
    return -1;
}

/* Whether a closed range other than `except` has bytes on page p */
static bool na_page_covered(uint32_t p, int except)
{
    for (unsigned i = 0; i < g_nna; i++) {
        if ((int)i != except && na_closed(i) && p >= na_page_lo(i) && p <= na_page_hi(i)) {
            return true;
        }
    }
    return false;
}

/* [pa, pa + len)'s pages given their guest access back, but for those a closed
 * range other than `except` or a shut report page still needs closed (small
 * targets side by side share pages); rearm: protected for write tracking
 * first, so the guest's next store to them is seen (a guard's memory, newer
 * than its image from then on) */
static void na_open_pages(uint32_t pa, uint32_t len, int except, bool rearm)
{
    const uint32_t p1 = (pa + len - 1) >> 12;
    uint32_t run = 0, n = 0;
    for (uint32_t p = pa >> 12; p <= p1 + 1; p++) {
        if (p <= p1 && !na_page_covered(p, except) && !rpt_page_shut(p << 12)) {
            run = n ? run : p;
            n++;
            continue;
        }
        if (n) {
            if (rearm) {
                hle_host_mem_rearm(run << 12, n << 12);
            }
            hle_host_mem_noaccess(run << 12, n << 12, false);
            n = 0;
        }
    }
}

/* A guard's pages closed to the guest, but for a hot edge page (g_hot; one
 * wholly its own always) */
static void na_close(uint32_t pa, uint32_t len)
{
    const uint32_t p0 = pa >> 12, p1 = (pa + len - 1) >> 12;
    const bool e0 = pa & 4095, e1 = (pa + len) & 4095;
    uint32_t run = 0, n = 0;
    for (uint32_t p = p0; p <= p1 + 1; p++) {
        if (p <= p1 && !(g_hot[p] && ((p == p0 && e0) || (p == p1 && e1)))) {
            run = n ? run : p;
            n++;
            continue;
        }
        if (n) {
            hle_host_mem_noaccess(run << 12, n << 12, true);
            n = 0;
        }
    }
}

static void na_drop(unsigned i)
{
    const uint32_t pa = g_na[i].pa, len = g_na[i].len;
    const bool guard = g_na[i].guard, closed = na_closed(i);
    if (guard) {
        __atomic_add_fetch(&g_guard_drops, 1, __ATOMIC_RELEASE);
    }
    g_na[i] = g_na[--g_nna];
    if (closed) {
        na_open_pages(pa, len, -1, guard);
    }
}

/* The first of the table's frames (guard false) or guards, and how many */
static unsigned na_count(bool guards, unsigned *first)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_nna; i++) {
        if (g_na[i].guard == guards && !n++) {
            *first = i;
        }
    }
    return n;
}

void hle_barrier_set_stale(uint32_t pa, uint32_t len, void (*fill)(uint32_t pa, uint32_t len))
{
    if (fill) {
        __atomic_store_n(&g_stale_fill, fill, __ATOMIC_RELEASE);
    }
    const uint64_t nst = len && fill ? (uint64_t)pa << 32 | len : 0;
    stale_lock();
    if (!fill) {
        while (g_nna) {                     /* the mode stops: every range's access back */
            na_drop(g_nna - 1);
        }
        if (g_hot) {
            memset(g_hot, 0, g_ram >> 12);  /* (and a savestate's memory is all new) */
        }
    } else if (nst && na_find_kind(pa, len, false) < 0) {
        unsigned first = 0;
        if (na_count(false, &first) == STALE_RANGES) {
            na_drop(first);                 /* the oldest, if no other room */
        }
        hle_host_mem_noaccess(pa, len, true);
        g_na[g_nna].pa = pa;
        g_na[g_nna].len = len;
        g_na[g_nna].guard = false;
        g_na[g_nna].open = false;
        g_na[g_nna++].v = 0;
    }
    __atomic_store_n(&g_stale, nst, __ATOMIC_RELEASE);
    stale_unlock();
}

void hle_barrier_set_guard_fill(uint64_t (*fill)(uint32_t pa, uint32_t len, bool read))
{
    __atomic_store_n(&g_guard_fill, fill, __ATOMIC_RELEASE);
}

void hle_barrier_guard(uint32_t pa, uint32_t len)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    stale_lock();
    const int i = na_find_kind(pa, len, true);
    if (i >= 0) {
        g_na[i].gen = ++g_guard_gen;        /* drawn again: a landing asked for is older */
        g_na[i].v = 0;
        if (g_na[i].open) {
            g_na[i].open = false;
            na_close(pa, len);
        }
    } else {
        unsigned first = 0;
        if (na_count(true, &first) == GUARD_RANGES) {
            na_drop(first);
        }
        na_close(pa, len);
        g_na[g_nna].pa = pa;
        g_na[g_nna].len = len;
        g_na[g_nna].guard = true;
        g_na[g_nna].open = false;
        g_na[g_nna].gen = ++g_guard_gen;
        g_na[g_nna++].v = 0;
    }
    stale_unlock();
}

void hle_barrier_guard_landing(uint32_t pa, uint32_t len, uint64_t v)
{
    stale_lock();
    const int i = na_find_kind(pa, len, true);
    if (i >= 0) {
        g_na[i].v = v;
    }
    stale_unlock();
}

int hle_barrier_guard_state(uint32_t pa, uint32_t len, uint64_t *v)
{
    stale_lock();
    const int i = na_find_kind(pa, len, true);
    const int s = i < 0 ? HLE_GUARD_NONE : g_na[i].open ? HLE_GUARD_OPEN : g_na[i].v ? HLE_GUARD_LANDING
                                                                                   : HLE_GUARD_ARMED;
    *v = i < 0 ? 0 : g_na[i].v;
    stale_unlock();
    return s;
}

void hle_barrier_unguard(uint32_t pa, uint32_t len)
{
    stale_lock();
    const int i = na_find_kind(pa, len, true);
    if (i >= 0) {
        na_drop((unsigned)i);
    }
    stale_unlock();
}

void hle_barrier_unguard_range(uint32_t pa, uint32_t len)
{
    stale_lock();
    for (unsigned i = g_nna; i-- > 0;) {
        if (g_na[i].guard && g_na[i].pa < pa + len && pa < g_na[i].pa + g_na[i].len) {
            na_drop(i);
        }
    }
    stale_unlock();
}

uint32_t hle_barrier_guard_drops(void)
{
    return __atomic_load_n(&g_guard_drops, __ATOMIC_ACQUIRE);
}

int hle_mem_stale_neighbour(uint32_t lo, uint32_t hi)
{
    bool guarded = false, other = false;
    stale_lock();
    for (unsigned i = 0; i < g_nna && !other; i++) {
        if (!na_closed(i) || lo >> 12 < na_page_lo(i) || lo >> 12 > na_page_hi(i)) {
            continue;
        }
        if (!g_na[i].guard || (g_na[i].pa < hi && lo < g_na[i].pa + g_na[i].len)) {
            other = true;                   /* a frame's page, or the target's own bytes */
        } else {
            guarded = true;
        }
    }
    int r = 0;
    if (guarded && !other && !rpt_page_shut(lo)) {
        const uint32_t p = lo >> 12, now = (uint32_t)(hle_now_ns() / 1000000);
        if (now - g_nbr_t[p] > NBR_MS) {
            g_nbr_t[p] = now;
            g_nbr_n[p] = 0;
        }
        r = 1;
        g_nbr_served++;
        if (++g_nbr_n[p] > NBR_HOT) {
            g_hot[p] = 1;                   /* given back: the access runs again */
            g_nbr_hot++;
            hle_host_mem_noaccess(p << 12, 4096, false);
            r = 2;
        }
    }
    stale_unlock();
    return r;
}

/* [pa, pa + len) freed: its guards go, its pages hot no more */
void hle_barrier_freed(uint32_t pa, uint32_t len)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    hle_barrier_unguard_range(pa, len);
    stale_lock();
    for (uint32_t p = pa >> 12, last = (pa + len - 1) >> 12; p <= last; p++) {
        g_hot[p] = 0;
        g_nbr_n[p] = 0;
    }
    stale_unlock();
}

void hle_barrier_guard_stats(uint64_t *neighbours, uint64_t *hot)
{
    *neighbours = g_nbr_served;
    *hot = g_nbr_hot;
}

/* ---- visibility-test reports (barrier.h hle_barrier_report_pushed) ----
 * Blinx reads its occlusion tests a frame after it ends them and takes a
 * result D3D calls incomplete for "hidden": with ours written once the GPU
 * ran the test, a decal on the ground was drawn in some frames and not in
 * others, about 40 % of its reads finding the result missing. The pages
 * (D3D's report pages, 256 tests each) and the reports pushed into them are
 * kept here: the scout on the guest's thread adds, the decoder marks them
 * reached and the back end written, the guest's access to a page waits.
 * Not while the decoder waits on the guest short of a report: Burnout 3
 * traps ~4 times a frame, and its reads waited out the 30 ms on a decoder
 * held at a trap whose handler was to run on the waiting thread (34 fps for
 * 60; ~0.7 s a second waited). */
#define RPT_PAGES   8
#define RPT_PENDING 256
#define RPT_WAIT_NS 30000000ll
static struct {
    uint32_t page[RPT_PAGES];           /* pa >> 12 */
    bool shut[RPT_PAGES];               /* without guest access */
    unsigned npages;
    struct {
        uint32_t pa, frame;
        uint32_t seq;                   /* 0: free */
        bool decoded;                   /* the decoder reached it */
    } e[RPT_PENDING];
    uint32_t seq;
    uint64_t waits, ns, timeouts, held, lost;
} g_rpt;
static int g_rpt_lock;
static uint32_t (*g_rpt_scout)(void);
static bool (*g_rpt_held)(void);
static HleSem g_rpt_sem;                /* posted at a write while the guest waits */
static int g_rpt_waiting;               /* atomic */

static void rpt_lock(void)
{
    while (__atomic_exchange_n(&g_rpt_lock, 1, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
}

static void rpt_unlock(void)
{
    __atomic_store_n(&g_rpt_lock, 0, __ATOMIC_RELEASE);
}

static int rpt_page(uint32_t pa)
{
    for (unsigned i = 0; i < g_rpt.npages; i++) {
        if (g_rpt.page[i] == pa >> 12) {
            return (int)i;
        }
    }
    return -1;
}

void hle_barrier_set_report_scout(uint32_t (*scout)(void), bool (*held)(void))
{
    static bool sem_made;
    if (!sem_made) {
        hle_sem_init(&g_rpt_sem, 0);
        sem_made = true;
    }
    rpt_lock();
    __atomic_store_n(&g_rpt_held, held, __ATOMIC_RELEASE);
    __atomic_store_n(&g_rpt_scout, scout, __ATOMIC_RELEASE);
    if (!scout) {                       /* the mode stops: every page's access back */
        for (unsigned i = 0; i < g_rpt.npages; i++) {
            if (g_rpt.shut[i]) {
                hle_host_mem_noaccess(g_rpt.page[i] << 12, 4096, false);
            }
        }
        memset(&g_rpt, 0, sizeof(g_rpt));
    }
    rpt_unlock();
}

void hle_barrier_report_pushed(uint32_t pa, uint32_t frame)
{
    rpt_lock();
    int pg = rpt_page(pa);
    if (pg < 0 && g_rpt.npages < RPT_PAGES) {
        pg = (int)g_rpt.npages++;
        g_rpt.page[pg] = pa >> 12;
        g_rpt.shut[pg] = false;
    }
    unsigned k = 0;
    while (k < RPT_PENDING && g_rpt.e[k].seq) {
        k++;
    }
    if (pg < 0 || k == RPT_PENDING) {
        g_rpt.lost++;                   /* not gated: read as before */
        rpt_unlock();
        return;
    }
    g_rpt.e[k].pa = pa;
    g_rpt.e[k].frame = frame;
    g_rpt.e[k].decoded = false;
    g_rpt.e[k].seq = ++g_rpt.seq ? g_rpt.seq : ++g_rpt.seq;
    if (!g_rpt.shut[pg]) {
        g_rpt.shut[pg] = true;
        hle_host_mem_noaccess(pa & ~4095u, 4096, true);
    }
    rpt_unlock();
}

void hle_barrier_report_decoded(uint32_t pa)
{
    rpt_lock();
    int best = -1;                      /* the oldest of its pushes not reached */
    for (unsigned k = 0; k < RPT_PENDING; k++) {
        if (g_rpt.e[k].seq && !g_rpt.e[k].decoded && g_rpt.e[k].pa == pa &&
            (best < 0 || (int32_t)(g_rpt.e[k].seq - g_rpt.e[best].seq) < 0)) {
            best = (int)k;
        }
    }
    if (best >= 0) {
        g_rpt.e[best].decoded = true;
    }
    rpt_unlock();
}

void hle_barrier_report_written(uint32_t pa)
{
    rpt_lock();
    int best = -1;                      /* the oldest of its pushes */
    for (unsigned k = 0; k < RPT_PENDING; k++) {
        if (g_rpt.e[k].seq && g_rpt.e[k].pa == pa &&
            (best < 0 || (int32_t)(g_rpt.e[k].seq - g_rpt.e[best].seq) < 0)) {
            best = (int)k;
        }
    }
    if (best >= 0) {
        g_rpt.e[best].seq = 0;
    }
    rpt_unlock();
    if (best >= 0 && __atomic_load_n(&g_rpt_waiting, __ATOMIC_ACQUIRE)) {
        hle_sem_post(&g_rpt_sem);
    }
}

/* Whether page pg has a report of a frame before `frame` not yet written;
 * *ahead: one of them the decoder hasn't reached. */
static bool rpt_late(unsigned pg, uint32_t frame, bool *ahead)
{
    bool late = false;
    *ahead = false;
    for (unsigned k = 0; k < RPT_PENDING; k++) {
        if (g_rpt.e[k].seq && g_rpt.e[k].pa >> 12 == g_rpt.page[pg] &&
            (int32_t)(g_rpt.e[k].frame - frame) < 0) {
            late = true;
            *ahead |= !g_rpt.e[k].decoded;
        }
    }
    return late;
}

/* The guest's access to a report page (its thread): true if it was one. */
static bool rpt_access(uint32_t pa)
{
    uint32_t (*scout)(void) = __atomic_load_n(&g_rpt_scout, __ATOMIC_ACQUIRE);
    if (!scout) {
        return false;
    }
    rpt_lock();
    int pg = rpt_page(pa);
    rpt_unlock();
    if (pg < 0) {
        return false;
    }
    /* what the guest pushed since its last PUT counted too: the frame it is
     * in now */
    const uint32_t frame = scout();
    bool ahead;
    rpt_lock();
    bool late = rpt_late((unsigned)pg, frame, &ahead);
    rpt_unlock();
    if (late) {
        const int64_t t0 = hle_now_ns();
        hle_host_guest_hold(true);              /* the guest's time stands still meanwhile */
        while (hle_sem_trywait(&g_rpt_sem)) {
            /* posts left from an earlier wait */
        }
        /* not the catch-up: that waits for all the guest pushed (Blinx ~5.7
         * ms a read), these reports come before it, and the decoder submits
         * a test soon after it (pbdraw.c) */
        __atomic_store_n(&g_rpt_waiting, 1, __ATOMIC_RELEASE);
        bool (*held)(void) = __atomic_load_n(&g_rpt_held, __ATOMIC_ACQUIRE);
        for (;;) {
            rpt_lock();
            late = rpt_late((unsigned)pg, frame, &ahead);
            /* the decoder short of a report and waiting on this thread */
            const bool stuck = late && ahead && held && held();
            if (late && (stuck || hle_now_ns() - t0 > RPT_WAIT_NS)) {
                /* the back end won't write them now (stuck), or at all (a
                 * test the decoder dropped): not waited for again */
                for (unsigned k = 0; k < RPT_PENDING; k++) {
                    if (g_rpt.e[k].seq && g_rpt.e[k].pa >> 12 == g_rpt.page[pg] &&
                        (int32_t)(g_rpt.e[k].frame - frame) < 0) {
                        g_rpt.e[k].seq = 0;
                    }
                }
                if (stuck) {
                    g_rpt.held++;
                } else if (g_rpt.timeouts++ < 8) {
                    hle_log("hakuX-hle-pb", "report gate: page %08x's reports not written in 30 ms",
                            g_rpt.page[pg] << 12);
                }
                late = false;
            }
            rpt_unlock();
            if (!late) {
                break;
            }
            hle_sem_timedwait(&g_rpt_sem, 2);   /* a write, or 2 ms */
        }
        __atomic_store_n(&g_rpt_waiting, 0, __ATOMIC_RELEASE);
        hle_host_guest_hold(false);
        g_rpt.waits++;
        g_rpt.ns += (uint64_t)(hle_now_ns() - t0);
    }
    /* (the stale lock first, never inside the report one) */
    stale_lock();
    const bool covered = na_page_covered(pa >> 12, -1);
    stale_unlock();
    rpt_lock();
    if (g_rpt.shut[pg]) {
        g_rpt.shut[pg] = false;
        if (!covered) {                 /* a guard or frame on the page keeps it closed */
            hle_host_mem_noaccess(g_rpt.page[pg] << 12, 4096, false);
        }
    }
    rpt_unlock();
    return true;
}

static bool rpt_page_shut(uint32_t pa)
{
    rpt_lock();
    const int pg = rpt_page(pa);
    const bool shut = pg >= 0 && g_rpt.shut[pg];
    rpt_unlock();
    return shut;
}

void hle_barrier_report_stats(uint64_t *waits, uint64_t *ns, uint64_t *timeouts, uint64_t *held)
{
    *waits = g_rpt.waits;
    *ns = g_rpt.ns;
    *timeouts = g_rpt.timeouts;
    *held = g_rpt.held;
}

void hle_mem_stale_access(uint32_t pa)
{
    hle_mem_stale_access_rw(pa, true);
}

void hle_mem_stale_access_rw(uint32_t pa, bool write)
{
    if (rpt_access(pa)) {
        stale_lock();
        const bool covered = na_page_covered(pa >> 12, -1);
        stale_unlock();
        if (!covered) {
            return;
        }
    }
    stale_lock();
    const int i = na_find_closed(pa);
    if (i < 0) {
        /* a page left without access by a range dropped meanwhile */
        if (!rpt_page_shut(pa)) {
            hle_host_mem_noaccess(pa & ~4095u, 4096, false);
        }
        stale_unlock();
        return;
    }
    const uint32_t lo = g_na[i].pa, len = g_na[i].len;
    if (g_na[i].guard) {
        /* A target's memory, older than its image: its landing (the one
         * asked for already, or one asked for now: read, the guest reads
         * it) is waited for, its value alone, then the memory is the
         * guest's. Armed again meanwhile (drawn into again), it is given
         * back anyway, older than the image, till its next arming: the guest
         * reads what the GPU had drawn when it asked. */
        const uint32_t gen = g_na[i].gen;
        uint64_t v = g_na[i].v;
        stale_unlock();
        hle_host_guest_hold(true);              /* the fill and the wait: the guest's time stands still */
        uint64_t (*fill)(uint32_t, uint32_t, bool) = __atomic_load_n(&g_guard_fill, __ATOMIC_ACQUIRE);
        if (!v && fill) {
            v = fill(lo, len, !write);
        }
        if (v && g_completed && v > g_completed()) {
            void (*flush)(uint64_t) = __atomic_load_n(&g_flush, __ATOMIC_ACQUIRE);
            if (flush) {
                flush(v);
            }
            g_wait(v);
        }
        hle_host_guest_hold(false);
        stale_lock();
        const int j = na_find_kind(lo, len, true);
        if (j >= 0 && g_na[j].gen == gen) {
            na_drop((unsigned)j);
        } else if (j >= 0 && !g_na[j].open) {
            g_na[j].open = true;
            __atomic_add_fetch(&g_guard_drops, 1, __ATOMIC_RELEASE);
            na_open_pages(lo, len, j, true);
        }
        stale_unlock();
        return;
    }
    na_drop((unsigned)i);
    const uint64_t st = __atomic_load_n(&g_stale, __ATOMIC_ACQUIRE);
    const bool shown = st == ((uint64_t)lo << 32 | len);
    if (!shown) {
        stale_unlock();                     /* not the frame on screen: its access back only */
        return;
    }
    __atomic_store_n(&g_stale, 0, __ATOMIC_RELEASE);
    stale_unlock();
    /* the image into the memory, then the wait for the copy (the guest's
     * thread is the only one that touches guest memory through its views),
     * the guest's time standing still */
    hle_host_guest_hold(true);
    __atomic_load_n(&g_stale_fill, __ATOMIC_ACQUIRE)(lo, len);
    const uint64_t v = hle_pageuse_get(&g_pu, lo, len);
    if (g_completed && v > g_completed()) {
        void (*flush)(uint64_t) = __atomic_load_n(&g_flush, __ATOMIC_ACQUIRE);
        if (flush) {
            flush(v);
        }
        g_wait(v);
    }
    hle_host_guest_hold(false);
}

void hle_barrier_arm_landing(uint32_t pa, uint32_t len, uint64_t value)
{
    hle_barrier_arm(pa, len, value);
    if (g_wseq && len && (uint64_t)pa + len <= g_ram) {
        cache_lines(pa, len, true);
    }
}

void hle_barrier_arm_keep(uint32_t pa, uint32_t len, uint64_t value, bool gpu)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    /* the pages wholly inside: a page it shares holds a neighbour's bytes,
     * whose stores mustn't wait (a guard covers the range's own there) */
    const uint32_t lo = (uint32_t)(((uint64_t)pa + 4095) & ~4095ull), hi = (pa + len) & ~4095u;
    if (hi > lo) {
        hle_pageuse_mark(&g_pu, lo, hi - lo, value);
        hle_host_mem_rearm(lo, hi - lo);
    }
    if (gpu && g_wseq) {
        cache_lines(pa, len, true);
    }
}

void hle_barrier_arm_src(uint32_t pa, uint32_t len, uint64_t value)
{
    if (!len || (uint64_t)pa + len > g_ram) {
        return;
    }
    const uint32_t lo = pa & ~4095u, hi = (uint32_t)(((uint64_t)pa + len + 4095) & ~4095ull);
    hle_pageuse_mark(&g_pu, lo, hi - lo, value);
    hle_host_mem_rearm(lo, hi - lo);
}

void hle_mem_cache_inval(uint32_t pa, uint32_t len)
{
    if (g_wseq && len && (uint64_t)pa + len <= g_ram) {
        cache_lines(pa, len, true);
    }
}

void hle_mem_host_wrote(uint32_t pa, uint32_t len)
{
    wseq_bump(pa, len);
}

uint64_t hle_mem_write_fence(uint32_t pa, uint32_t len)
{
    if (!g_completed || (!g_pu.max && !g_watch) || !len || (uint64_t)pa + len > g_ram) {
        return 0;
    }
    const uint64_t v = hle_pageuse_get(&g_pu, pa, len);
    if (!v || v <= g_completed()) {
        return 0;
    }
    void (*flush)(uint64_t) = __atomic_load_n(&g_flush, __ATOMIC_ACQUIRE);
    if (flush) {
        flush(v);
    }
    return v;
}

void hle_mem_wait_fence(uint64_t value)
{
    if (value && g_wait) {
        hle_host_guest_hold(true);
        g_wait(value);
        hle_host_guest_hold(false);
    }
}

void hle_mem_first_write(uint64_t ram_addr, unsigned size)
{
    uint64_t base = hle_ram_addr_base();
    if (ram_addr < base || ram_addr - base >= g_ram) {
        return;
    }
    uint32_t pa = (uint32_t)(ram_addr - base);
    wseq_bump(pa, MAX(size, 1u));
    /* A store into bytes copied for a draw that a later draw, not yet
     * decoded, may read again: those draws come first, as the console's GPU,
     * close behind the pushes, would have read them. The page is dirty from
     * here on; the next copy arms it again. */
    const uint32_t span = g_cspan ? __atomic_exchange_n(&g_cspan[pa >> 12], 0, __ATOMIC_ACQ_REL) : 0;
    if (span && (pa & 4095) >= (span & 0xFFFF) && (pa & 4095) < (span >> 16)) {
        void (*catch_up)(void) = __atomic_load_n(&g_catch_up, __ATOMIC_ACQUIRE);
        if (catch_up) {
            catch_up();
        }
    }
    if (!g_pu.max && !g_watch) {
        if (span) {
            return;                     /* armed for the catch-up only */
        }
        /* Nothing was ever armed, so no HLE bit should be clean. A trap here
         * means the dirty client is not maintained (Step 1(c)). */
        if (g_st.unarmed++ == 0) {
            hle_log("hakuX-hle-mem", "barrier: UNARMED trap at pa=%08x", pa);
        }
        return;
    }
    g_st.traps++;
    uint64_t v = hle_pageuse_get(&g_pu, pa, size);
    g_st.armed += v != 0;
    if (g_completed && v > g_completed()) {
        int64_t t0 = hle_now_ns();
        g_st.waits++;
        g_wpages[g_nwpages++ % 8] = pa >> 12;
        hle_host_guest_hold(true);              /* the guest's time stands still meanwhile */
        void (*flush)(uint64_t) = __atomic_load_n(&g_flush, __ATOMIC_ACQUIRE);
        if (flush) {
            flush(v);
        }
        g_wait(v);
        hle_host_guest_hold(false);
        g_st.wait_ns += hle_now_ns() - t0;
        if (g_contended) {
            __atomic_fetch_or(&g_contended[(pa >> 12) >> 5], 1u << ((pa >> 12) & 31),
                              __ATOMIC_RELAXED);
        }
        /* counted again: what landed meanwhile took the count in, and the
         * store comes after it */
        wseq_bump(pa, MAX(size, 1u));
    }
}

unsigned hle_barrier_wait_pages(uint32_t *pages, unsigned max)
{
    unsigned n = 0;
    for (; n < max && n < 8 && n < g_nwpages; n++) {
        pages[n] = g_wpages[(g_nwpages - 1 - n) % 8];
    }
    return n;
}

HleBarrierStats hle_barrier_stats(void)
{
    return g_st;
}
