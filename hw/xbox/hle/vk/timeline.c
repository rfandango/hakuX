/*
 * hakuX HLE GPU — timeline semaphore = GPU time; watcher thread publishes
 * completions and drives the GPU-progress interrupt (spec §4.7, §5.12).
 */
#include "hw/xbox/hle/base.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/vk/back_int.h"
#include "hw/xbox/hle/vk/guest_ram.h"

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
    int quit;                   /* hle_tl_stop (atomic) */
    int64_t busy_since;                 /* the first submit after the GPU ran out of our work */
    uint32_t gap;                       /* a gap asked for (hle_tl_gap) */
    int64_t gap_hold_ns;                /* how long a gap leaves the GPU idle */
    uint32_t gaps;                      /* served since gaps_log */
    int64_t gaps_log;
} g_tl;

/* Readbacks waiting to land in guest memory (hle_tl_land): each lands once
 * the back end recorded its copy (copied), else is dropped (the image held
 * nothing newer: guest memory stays) */
static struct {
    HleMutex lock;
    struct {
        uint64_t value;
        uint32_t off, pa, len;
        uint32_t pitch, row, rows;      /* copied: what the GPU wrote, rows row bytes apart */
        bool copied;
    } q[32];
    unsigned n;
} g_land;

bool hle_tl_land(uint64_t value, uint32_t arena_off, uint32_t pa, uint32_t len)
{
    bool ok = false;
    hle_mutex_lock(&g_land.lock);
    if (g_land.n < ARRAY_SIZE(g_land.q)) {
        g_land.q[g_land.n].value = value;
        g_land.q[g_land.n].off = arena_off;
        g_land.q[g_land.n].pa = pa;
        g_land.q[g_land.n].len = len;
        g_land.q[g_land.n].copied = false;
        hle_atomic_set(&g_land.n, g_land.n + 1);
        ok = true;
    }
    hle_mutex_unlock(&g_land.lock);
    return ok;
}

void hle_tl_land_copied(uint32_t arena_off, uint32_t pitch, uint32_t row, uint32_t rows)
{
    hle_mutex_lock(&g_land.lock);
    for (unsigned i = 0; i < g_land.n; i++) {
        if (g_land.q[i].off == arena_off && rows &&
            (uint64_t)pitch * (rows - 1) + row <= g_land.q[i].len) {
            g_land.q[i].pitch = pitch;
            g_land.q[i].row = row;
            g_land.q[i].rows = rows;
            g_land.q[i].copied = true;
        }
    }
    hle_mutex_unlock(&g_land.lock);
}

/* The readbacks the GPU has finished (v) into guest memory: whoever publishes
 * v first lands them, so a thread that sees v sees their rows (a CPU write
 * the readback's barrier held waits for v, so lands after them) */
static void land(uint64_t v)
{
    if (!hle_atomic_read(&g_land.n)) {
        return;
    }
    hle_mutex_lock(&g_land.lock);
    for (unsigned i = 0; i < g_land.n;) {
        if (g_land.q[i].value > v) {
            i++;
            continue;
        }
        uint8_t *dst = g_land.q[i].copied ? hle_pa_ptr(g_land.q[i].pa, g_land.q[i].len) : NULL;
        const uint8_t *src = (g_hland.buf ? g_hland.map : g_harena.map) + g_land.q[i].off;
        if (dst && g_hland.buf && !g_hland_coherent) {
            VkDeviceSize atom = MAX(g_hvk.props.limits.nonCoherentAtomSize, 1);
            VkDeviceSize lo = g_land.q[i].off / atom * atom;
            VkDeviceSize hi = (g_land.q[i].off + (VkDeviceSize)g_land.q[i].len + atom - 1) / atom * atom;
            VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                       .memory = g_hland.mem, .offset = lo,
                                       .size = MIN(hi, (VkDeviceSize)g_hland.alloc.size) - lo };
            T.vkInvalidateMappedMemoryRanges(g_hvk.dev, 1, &mr);
        }
        for (uint32_t y = 0; dst && y < g_land.q[i].rows; y++) {
            memcpy(dst + (size_t)y * g_land.q[i].pitch, src + (size_t)y * g_land.q[i].pitch,
                   g_land.q[i].row);
        }
        g_land.q[i] = g_land.q[--g_land.n];
        hle_atomic_set(&g_land.n, g_land.n);
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
    while (!__atomic_load_n(&g_tl.quit, __ATOMIC_ACQUIRE)) {
        hle_event_reset(&g_tl.ev);
        if (done >= hle_atomic_read(&g_tl.submitted)) {
            if (!__atomic_load_n(&g_tl.quit, __ATOMIC_ACQUIRE)) {
                hle_event_wait(&g_tl.ev);
            }
            continue;
        }
        uint64_t want = done + 1;
        VkSemaphoreWaitInfo wi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1, .pSemaphores = &g_tl.sem, .pValues = &want };
        VkResult r = T.vkWaitSemaphores(g_hvk.dev, &wi, 100 * 1000 * 1000ull);
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
    g_tl.quit = 0;
    hle_thread_create(&g_tl.thread, "hle.timeline", watch_thread, NULL,
                       HLE_THREAD_JOINABLE);
    return 0;
}

void hle_tl_stop(void)
{
    if (!g_tl.sem) {
        return;
    }
    __atomic_store_n(&g_tl.quit, 1, __ATOMIC_RELEASE);
    hle_event_set(&g_tl.ev);
    hle_thread_join(&g_tl.thread);      /* within its 100 ms wait */
}

void hle_tl_destroy(void)
{
    if (!g_tl.sem) {
        return;
    }
    hle_tl_stop();
    T.vkDestroySemaphore(g_hvk.dev, g_tl.sem, NULL);
    hle_event_destroy(&g_tl.ev);
    hle_mutex_destroy(&g_land.lock);
    memset(&g_tl, 0, sizeof(g_tl));
    memset(&g_land, 0, sizeof(g_land));
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
        VkSemaphoreWaitInfo wi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1, .pSemaphores = &g_tl.sem, .pValues = &value };
        const VkResult r = T.vkWaitSemaphores(g_hvk.dev, &wi, 100 * 1000 * 1000ull);
        if (r != VK_SUCCESS && r != VK_TIMEOUT) {
            hle_vk_lose(r, "vkWaitSemaphores");    /* completed: all (hle_tl_lose) */
        }
        uint64_t v = 0;
        T.vkGetSemaphoreCounterValue(g_hvk.dev, g_tl.sem, &v);
        publish(v);
    }
    hle_hitch_leave(pa);
}

