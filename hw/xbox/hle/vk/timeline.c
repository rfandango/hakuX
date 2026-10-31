/*
 * hakuX HLE GPU — timeline semaphore = GPU time; watcher thread publishes
 * completions and drives the GPU-progress interrupt (spec §4.7, §5.12).
 */
#include "hw/xbox/hle/base.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/tex/xg_swizzle.h"
#include "hw/xbox/hle/vk/back_int.h"
#include "hw/xbox/hle/vk/guest_ram.h"
#include "hw/xbox/hle/vk/quirks.h"

#ifdef __ANDROID__
#include "hw/xbox/hle/prop.h"
#endif
#include <time.h>

#define T (g_hvk.t)

static struct {
    VkSemaphore sem;
    uint64_t submitted, completed, timeouts;
    HleThread thread;
    HleEvent ev;
    void (*on_complete)(uint64_t);
    int64_t busy_since;                 /* the first submit after the GPU ran out of our work */
    uint32_t gap;                       /* a gap asked for (hle_tl_gap) */
    int64_t gap_hold_ns;                /* how long a gap leaves the GPU idle */
    uint32_t gaps;                      /* served since gaps_log */
    int64_t gaps_log;
} g_tl;

/* Landings in guest memory, each made once the GPU reaches its value and
 * before any thread sees that value, in the order they were queued (a copy
 * queued behind the landing it reads): a readback's rows (hle_tl_land:
 * without zero-copy guest RAM; dropped unless the back end recorded its
 * copy: the image held nothing newer, guest memory stays), a target's
 * texels swizzled into its memory (hle_tl_land_swz), a copy between guest
 * memory (hle_tl_land_copy, a 2D blit's), or a note (hle_tl_land_note) */
enum { LAND_ROWS, LAND_SWZ, LAND_COPY, LAND_NOTE };
#define LAND_MAX 256
typedef struct LandEnt {
    uint64_t value;
    uint8_t kind;
    bool copied;                        /* ROWS, SWZ: the back end recorded the GPU's copy */
    uint32_t off, pa, len;              /* from off (the landing buffer; COPY: guest RAM) to pa */
    uint32_t pitch, row, rows;          /* ROWS: as the GPU wrote them; COPY: the source's pitch */
    uint32_t w, h, bpp;                 /* SWZ */
    uint32_t dpitch;                    /* COPY */
    HleLandDone done;
    uint32_t token;
} LandEnt;
static struct {
    HleMutex lock;
    LandEnt q[LAND_MAX];
    unsigned n;
    uint64_t landed;                    /* the value land() last went through (completed
                                           follows it: publish) */
} g_land;

static void land_one(const LandEnt *e);

/* One whose value land() went through already lands at once (its own
 * landings, older, are in) */
static bool land_add(const LandEnt *e)
{
    bool ok = true;
    hle_mutex_lock(&g_land.lock);
    if (e->value <= g_land.landed) {
        land_one(e);
    } else if (g_land.n < LAND_MAX) {
        g_land.q[g_land.n] = *e;
        hle_atomic_set(&g_land.n, g_land.n + 1);
    } else {
        ok = false;
    }
    hle_mutex_unlock(&g_land.lock);
    return ok;
}

bool hle_tl_land_room(unsigned n)
{
    return hle_atomic_read(&g_land.n) + n <= LAND_MAX;
}

uint64_t hle_tl_land_oldest(void)
{
    uint64_t v = 0;
    hle_mutex_lock(&g_land.lock);
    for (unsigned i = 0; i < g_land.n; i++) {
        v = !v || g_land.q[i].value < v ? g_land.q[i].value : v;
    }
    hle_mutex_unlock(&g_land.lock);
    return v;
}

bool hle_tl_land(uint64_t value, uint32_t arena_off, uint32_t pa, uint32_t len)
{
    const LandEnt e = { .value = value, .kind = LAND_ROWS, .off = arena_off, .pa = pa, .len = len };
    return land_add(&e);
}

bool hle_tl_land_swz(uint64_t value, uint32_t off, uint32_t pa, uint32_t w, uint32_t h, uint32_t bpp,
                     HleLandDone done, uint32_t token)
{
    const LandEnt e = { .value = value, .kind = LAND_SWZ, .off = off, .pa = pa, .len = w * h * bpp,
                        .w = w, .h = h, .bpp = bpp, .done = done, .token = token };
    return land_add(&e);
}

bool hle_tl_land_copy(uint64_t value, uint32_t spa, uint32_t spitch, uint32_t dpa, uint32_t dpitch,
                      uint32_t row, uint32_t rows, HleLandDone done, uint32_t token)
{
    const LandEnt e = { .value = value, .kind = LAND_COPY, .off = spa, .pa = dpa, .pitch = spitch,
                        .dpitch = dpitch, .row = row, .rows = rows, .done = done, .token = token };
    return land_add(&e);
}

bool hle_tl_land_note(uint64_t value, HleLandDone done, uint32_t token)
{
    const LandEnt e = { .value = value, .kind = LAND_NOTE, .done = done, .token = token };
    return land_add(&e);
}

void hle_tl_land_copied(uint32_t arena_off, uint32_t pitch, uint32_t row, uint32_t rows)
{
    hle_mutex_lock(&g_land.lock);
    for (unsigned i = 0; i < g_land.n; i++) {
        LandEnt *e = &g_land.q[i];
        if (e->off != arena_off || e->copied || (e->kind != LAND_ROWS && e->kind != LAND_SWZ)) {
            continue;
        }
        if (e->kind == LAND_SWZ || (rows && (uint64_t)pitch * (rows - 1) + row <= e->len)) {
            e->pitch = pitch;
            e->row = row;
            e->rows = rows;
            e->copied = true;
        }
    }
    hle_mutex_unlock(&g_land.lock);
}

/* The landing buffer's bytes [off, off + len) as the GPU wrote them */
static const uint8_t *land_src(uint32_t off, uint32_t len)
{
    if (g_hland.buf && !g_hland_coherent) {
        VkDeviceSize atom = MAX(g_hvk.props.limits.nonCoherentAtomSize, 1);
        VkDeviceSize lo = off / atom * atom;
        VkDeviceSize hi = (off + (VkDeviceSize)len + atom - 1) / atom * atom;
        VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                   .memory = g_hland.mem, .offset = lo,
                                   .size = MIN(hi, (VkDeviceSize)g_hland.alloc.size) - lo };
        T.vkInvalidateMappedMemoryRanges(g_hvk.dev, 1, &mr);
    }
    return (g_hland.buf ? g_hland.map : g_harena.map) + off;
}

static void land_one(const LandEnt *e)
{
    bool landed = e->kind == LAND_NOTE;
    if (e->kind == LAND_ROWS || e->kind == LAND_SWZ) {
        uint8_t *dst = e->copied ? hle_pa_ptr(e->pa, e->len) : NULL;
        const uint8_t *src = dst ? land_src(e->off, e->len) : NULL;
        if (dst && e->kind == LAND_ROWS) {
            for (uint32_t y = 0; y < e->rows; y++) {
                memcpy(dst + (size_t)y * e->pitch, src + (size_t)y * e->pitch, e->row);
            }
        } else if (dst) {
            const HleXgRect r = { 0, 0, e->w, e->h };
            hle_xg_swizzle_rect(src, e->w * e->bpp, &r, dst, e->w, e->h, 0, 0, e->bpp);
            hle_mem_mark_landed(e->pa, e->len);
        }
        landed = dst != NULL;
    } else if (e->kind == LAND_COPY) {
        for (uint32_t y = 0; y < e->rows; y++) {
            const uint8_t *s = hle_pa_ptr(e->off + y * e->pitch, e->row);
            uint8_t *d = hle_pa_ptr(e->pa + y * e->dpitch, e->row);
            if (s && d) {
                memmove(d, s, e->row);
                hle_mem_mark_written(e->pa + y * e->dpitch, e->row);
            }
        }
        landed = true;
    }
    if (e->done) {
        e->done(e->token, landed);
    }
}

/* The landings the GPU has finished (v), in their order: whoever publishes v
 * first lands them, so a thread that sees v sees them (a CPU write the
 * readback's barrier held waits for v, so lands after them) */
static void land(uint64_t v)
{
    if (v <= __atomic_load_n(&g_land.landed, __ATOMIC_ACQUIRE)) {
        return;                         /* (a waiter publishing what was published) */
    }
    hle_mutex_lock(&g_land.lock);
    if (v > g_land.landed) {
        unsigned keep = 0;
        for (unsigned i = 0; i < g_land.n; i++) {
            if (g_land.q[i].value > v) {
                g_land.q[keep++] = g_land.q[i];
                continue;
            }
            land_one(&g_land.q[i]);
        }
        g_land.n = keep;
        hle_atomic_set(&g_land.n, keep);
        __atomic_store_n(&g_land.landed, v, __ATOMIC_RELEASE);
    }
    hle_mutex_unlock(&g_land.lock);
}

/* `completed` only moves forward. Waiters publish what they observed too, so
 * after hle_tl_wait(v) returns, hle_tl_completed() >= v on every thread; the
 * watcher alone would publish it a little later (the barrier self-test saw
 * stores ordered by the wait but `completed` still behind). */
static void publish(uint64_t v)
{
    land(v);
    uint64_t cur = hle_atomic_read(&g_tl.completed);
    while (v > cur) {
        uint64_t prev = hle_atomic_cmpxchg(&g_tl.completed, cur, v);
        if (prev == cur) {
            break;
        }
        cur = prev;
    }
}

static void *watch_thread(void *opaque)
{
    uint64_t done = 0;
    for (;;) {
        hle_event_reset(&g_tl.ev);
        if (done >= hle_atomic_read(&g_tl.submitted)) {
            hle_event_wait(&g_tl.ev);
            continue;
        }
        uint64_t want = done + 1;
        VkResult r = hle_tl_sem_wait(g_tl.sem, want, 100 * 1000 * 1000ull);
        if (r == VK_TIMEOUT) {
            if (++g_tl.timeouts % 10 == 0) {
                hle_log("hakuX-hle-back", "GPU has not reached %llu for %llu s",
                        (unsigned long long)want,
                        (unsigned long long)(g_tl.timeouts / 10));
            }
            continue;
        }
        if (r != VK_SUCCESS) {
            hle_vk_lose(r, "vkWaitSemaphores (timeline)");
            return NULL;
        }
        uint64_t v = 0;
        T.vkGetSemaphoreCounterValue(g_hvk.dev, g_tl.sem, &v);
        done = v;
        publish(v);
        if (g_tl.on_complete) {
            g_tl.on_complete(v);
        }
    }
    return NULL;
}

/* Waiting for a timeline value: the driver's host wait, or with no_sem_wait
 * a poll of the counter (the wait's path returned VK_ERROR_DEVICE_LOST on
 * Arm's r34p0 Bifrost driver while the counter query kept working; quirks.c).
 * The poll's half-millisecond steps cost the waiter at most that in latency;
 * the GPU work it waits for is milliseconds. */
VkResult hle_tl_sem_wait(VkSemaphore sem, uint64_t value, uint64_t timeout_ns)
{
    if (likely(!hle_quirk(HLE_QUIRK_NO_SEM_WAIT))) {
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1, .pSemaphores = &sem,
                                   .pValues = &value };
        return T.vkWaitSemaphores(g_hvk.dev, &wi, timeout_ns);
    }
    const int64_t end = hle_now_ns() + (int64_t)timeout_ns;
    struct timespec nap = { 0, 500 * 1000 };
    for (;;) {
        uint64_t v = 0;
        VkResult r = T.vkGetSemaphoreCounterValue(g_hvk.dev, sem, &v);
        if (r != VK_SUCCESS) {
            return r;
        }
        if (v >= value) {
            return VK_SUCCESS;
        }
        if (hle_now_ns() >= end) {
            return VK_TIMEOUT;
        }
        nanosleep(&nap, NULL);
    }
}

int hle_tl_init(void (*on_complete)(uint64_t))
{
    VkSemaphoreTypeCreateInfo ti = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
    VkSemaphoreCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
    if (T.vkCreateSemaphore(g_hvk.dev, &si, NULL, &g_tl.sem) != VK_SUCCESS) {
        return -1;
    }
    g_tl.on_complete = on_complete;
    g_tl.gap_hold_ns = 4000000;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (hle_prop_get("debug.hakux.gaphold", v) > 0 && atoi(v) >= 0) {
        g_tl.gap_hold_ns = (int64_t)atoi(v) * 1000;  /* microseconds (the A/B) */
    }
#endif
    hle_mutex_init(&g_land.lock);
    hle_event_init(&g_tl.ev, false);
    hle_thread_create(&g_tl.thread, "hle.timeline", watch_thread, NULL,
                       HLE_THREAD_DETACHED);
    return 0;
}

VkSemaphore hle_tl_sem(void)
{
    return g_tl.sem;
}

void hle_tl_signaled(uint64_t value)
{
    if (hle_atomic_read(&g_tl.completed) >= hle_atomic_read(&g_tl.submitted)) {
        hle_atomic_set(&g_tl.busy_since, hle_now_ns());     /* idle up to here: a new busy span */
    }
    hle_atomic_set(&g_tl.submitted, value);
    hle_event_set(&g_tl.ev);
}

/* How long the GPU has been on our work without running out of it (0: it's
 * out now). A GPU queue below ours (the app's window: our kgsl contexts are
 * at priority 7, native/app/vkdriver.c) only runs when it does. */
int64_t hle_tl_busy_ns(void)
{
    if (!g_tl.sem || hle_atomic_read(&g_tl.completed) >= hle_atomic_read(&g_tl.submitted)) {
        return 0;
    }
    return hle_now_ns() - hle_atomic_read(&g_tl.busy_since);
}

/* A moment of idle GPU asked for: the app's window has a frame waiting on the
 * GPU queue below ours, and the compositor shows the game's frames behind it
 * (a process's frames in order), so the picture would hold until our work ran
 * out (NativeCoreActivity's OverlayPacer). Served before the next submit. */
void hle_tl_gap(void)
{
    hle_atomic_set(&g_tl.gap, 1);
}

/* The render thread, before a submit: a gap asked for is everything
 * submitted finished, then the GPU left idle gap_hold_ns
 * (debug.hakux.gaphold, microseconds; 4000 by default: with 1000 a window
 * frame still waited, the picture held 0.7 s, PGR2 at 3x on an Adreno 650). */
void hle_tl_gap_serve(void)
{
    if (likely(!hle_atomic_read(&g_tl.gap))) {
        return;
    }
    hle_atomic_set(&g_tl.gap, 0);
    hle_tl_wait(hle_atomic_read(&g_tl.submitted));
    if (g_tl.gap_hold_ns > 0) {
        struct timespec ts = { g_tl.gap_hold_ns / 1000000000, g_tl.gap_hold_ns % 1000000000 };
        nanosleep(&ts, NULL);
    }
    g_tl.gaps++;
    const int64_t now = hle_now_ns();
    if (now - g_tl.gaps_log >= 10000000000ll) {
        hle_log("hakuX-hle-back", "gpu gaps for the app's window: %u since the last (%lld us each)",
                g_tl.gaps, (long long)(g_tl.gap_hold_ns / 1000));
        g_tl.gaps = 0;
        g_tl.gaps_log = now;
    }
}

uint64_t hle_tl_completed(void)
{
    return hle_atomic_read(&g_tl.completed);
}

uint64_t hle_tl_submitted(void)
{
    return hle_atomic_read(&g_tl.submitted);
}

/* Every value reached: completed goes to the end of time (it only moves
 * forward), so the waits above and hle_tl_completed's readers stop waiting;
 * the readbacks queued land (with what they have). */
void hle_tl_lose(void)
{
    publish(UINT64_MAX);
    hle_event_set(&g_tl.ev);
}

void hle_tl_wait(uint64_t value)
{
    if (hle_tl_completed() >= value) {
        return;
    }
    uint32_t pa = hle_hitch_enter(HLE_ACT_GPU);
    while (hle_tl_completed() < value) {
        const VkResult r = hle_tl_sem_wait(g_tl.sem, value, 100 * 1000 * 1000ull);
        if (r != VK_SUCCESS && r != VK_TIMEOUT) {
            hle_vk_lose(r, "vkWaitSemaphores");    /* completed: all (hle_tl_lose) */
        }
        uint64_t v = 0;
        T.vkGetSemaphoreCounterValue(g_hvk.dev, g_tl.sem, &v);
        publish(v);
    }
    hle_hitch_leave(pa);
}

