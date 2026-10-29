/*
 * hakuX HLE GPU — the display images (display.h): device-local RGBA8 images
 * the render thread writes at its swaps (top row first) and the window's
 * presenter (vk/swapchain.c) samples.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/vk/display.h"
#include "hw/xbox/hle/vk/back_int.h"

#define T (g_hvk.t)

enum { SLOT_FREE = 0, SLOT_PENDING, SLOT_READY, SLOT_FRONT };

typedef struct HleDispSlot {
    VkImage img;
    VkImageView view;
    VkDeviceMemory mem;
    int state;
    bool held;
    uint32_t swap_id;
    uint64_t value;
    int64_t front_ns;           /* when it became the front image */
} HleDispSlot;

static struct {
    HleMutex lock;
    HleCond cond;
    HleMutex use;               /* hle_disp_use_begin/end */
    bool inited;
    uint32_t w, h, gen;
    HleDispSlot s[HLE_DISP_SLOTS];
    HleDispStats st;
    bool present_late;          /* hle_disp_present_late */
    uint32_t want;              /* ... the latest swap presented before it was written */
    uint32_t last_shown;        /* hle_disp_acquire: the swap of the last new frame taken */
    unsigned no_slot_logs;      /* hle_disp_begin_write's */
} g_disp;

static void slot_free_vk(HleDispSlot *s)
{
    if (s->view) {
        T.vkDestroyImageView(g_hvk.dev, s->view, NULL);
    }
    if (s->img) {
        T.vkDestroyImage(g_hvk.dev, s->img, NULL);
    }
    if (s->mem) {
        T.vkFreeMemory(g_hvk.dev, s->mem, NULL);
    }
    memset(s, 0, sizeof(*s));
}

static bool slot_create(HleDispSlot *s, uint32_t w, uint32_t h)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &s->img) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetImageMemoryRequirements(g_hvk.dev, s->img, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0) {
        mt = hle_vk_memtype(mr.memoryTypeBits, 0, 0);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &s->mem) != VK_SUCCESS ||
        T.vkBindImageMemory(g_hvk.dev, s->img, s->mem, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    return T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->view) == VK_SUCCESS;
}

void hle_disp_setup(void)
{
    if (!g_disp.inited) {
        hle_mutex_init(&g_disp.lock);
        hle_cond_init(&g_disp.cond);
        hle_mutex_init(&g_disp.use);
        hle_smp_wmb();
        hle_atomic_set(&g_disp.inited, true);
    }
}

int hle_disp_init(uint32_t w, uint32_t h)
{
    hle_disp_setup();
    if (g_disp.w == w && g_disp.h == h && g_disp.s[0].img) {
        return 0;
    }
    /* the presenter's reads of the old images submitted (use), then done */
    hle_mutex_lock(&g_disp.use);
    hle_vk_queue_lock();
    const VkResult r = T.vkDeviceWaitIdle(g_hvk.dev);
    hle_vk_queue_unlock();
    hle_vk_check(r, "vkDeviceWaitIdle (display)");
    hle_mutex_lock(&g_disp.lock);
    for (int i = 0; i < HLE_DISP_SLOTS; i++) {
        slot_free_vk(&g_disp.s[i]);
    }
    g_disp.w = w;
    g_disp.h = h;
    g_disp.gen++;
    int ok = 0;
    for (int i = 0; i < HLE_DISP_SLOTS; i++) {
        if (!slot_create(&g_disp.s[i], w, h)) {
            hle_log("hakuX-hle-back", "display slot %d %ux%u failed", i, w, h);
            ok = -1;
        }
    }
    if (ok) {
        /* No partial display: with no slot image, begin_write returns -1 at
         * once and SWAP skips the blit (logged), instead of looping. */
        for (int i = 0; i < HLE_DISP_SLOTS; i++) {
            slot_free_vk(&g_disp.s[i]);
        }
        hle_log("hakuX-hle-back", "display disabled: slot creation failed");
    }
    hle_mutex_unlock(&g_disp.lock);
    hle_mutex_unlock(&g_disp.use);
    hle_log("hakuX-hle-back", "display %ux%u gen %u (%d slots)", w, h, g_disp.gen, HLE_DISP_SLOTS);
    return ok;
}

void hle_disp_use_begin(void)
{
    hle_mutex_lock(&g_disp.use);
}

uint32_t hle_disp_gen(void)
{
    return g_disp.gen;
}

void hle_disp_use_end(void)
{
    hle_mutex_unlock(&g_disp.use);
}

void hle_disp_size(uint32_t *w, uint32_t *h)
{
    *w = g_disp.w;
    *h = g_disp.h;
}

VkImage hle_disp_image(int slot)
{
    return g_disp.s[slot].img;
}

/* Slot i (holding swap_id) becomes the front image; the front one before
 * and older frames not shown are let go. Under the lock. */
static void to_front(int i, uint32_t swap_id)
{
    for (int j = 0; j < HLE_DISP_SLOTS; j++) {
        HleDispSlot *o = &g_disp.s[j];
        if (j != i && (o->state == SLOT_FRONT ||
                       (o->state != SLOT_FREE && o->swap_id < swap_id))) {
            if (o->state != SLOT_FRONT) {
                g_disp.st.dropped++;
            }
            o->state = SLOT_FREE;
        }
    }
    g_disp.s[i].state = SLOT_FRONT;
    g_disp.s[i].front_ns = hle_now_ns();
    g_disp.st.presented++;
    hle_hitch_frame(swap_id, HLE_FR_FRONT);
}

int hle_disp_begin_write(uint32_t swap_id, uint64_t value)
{
    hle_mutex_lock(&g_disp.lock);
    int pick = -1;
    bool any = false;
    for (int i = 0; i < HLE_DISP_SLOTS; i++) {
        any |= g_disp.s[i].img != VK_NULL_HANDLE;
    }
    if (!any) {                       /* display disabled (init failed) */
        hle_mutex_unlock(&g_disp.lock);
        return -1;
    }
    /* bounded: ~300 ms, then give up on this frame rather than hang the
     * render thread (all slots held or in flight). The bounds are time: the
     * condition is signalled at every UI release (60 Hz) and present, and
     * counting wakeups dropped the next frame due on screen after two UI
     * frames, which the vblank then could not present (frames skipped,
     * runs of them under load). */
    int64_t t0 = g_get_monotonic_time();
    for (;;) {
        /* a free slot: one the presenter let go of is written after its
         * reads (the one queue orders them: the blit's barrier) */
        for (int i = 0; i < HLE_DISP_SLOTS && pick < 0; i++) {
            const HleDispSlot *s = &g_disp.s[i];
            if (s->img && s->state == SLOT_FREE && !s->held) {
                pick = i;
            }
        }
        int64_t waited = g_get_monotonic_time() - t0;
        if (pick >= 0 || waited >= 300000) {
            break;
        }
        if (waited >= 100000) {
            /* nothing presented for 100 ms (guest paused?): drop the oldest
             * unpresented frame instead of stalling the render thread */
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < HLE_DISP_SLOTS; i++) {
                HleDispSlot *s = &g_disp.s[i];
                if ((s->state == SLOT_PENDING || s->state == SLOT_READY) && !s->held &&
                    s->swap_id < oldest) {
                    oldest = s->swap_id;
                    pick = i;
                }
            }
            if (pick >= 0) {
                g_disp.st.dropped++;
                break;
            }
        }
        g_disp.st.write_waits++;
        uint32_t pa = hle_hitch_enter(HLE_ACT_DISPLAY);
        hle_cond_timedwait(&g_disp.cond, &g_disp.lock, 50);
        hle_hitch_leave(pa);
    }
    if (pick >= 0) {
        HleDispSlot *s = &g_disp.s[pick];
        s->state = SLOT_PENDING;          /* reserved: nobody else picks it */
        s->swap_id = swap_id;
        hle_hitch_frame(swap_id, HLE_FR_WRITE);
        s->value = value;
        g_disp.st.written++;
        if (g_disp.present_late && swap_id <= g_disp.want) {
            to_front(pick, swap_id);      /* presented already */
        }
    } else if (g_disp.no_slot_logs++ < 8) {
        hle_log("hakuX-hle-back", "display: no free slot for swap %u (frame skipped)",
                swap_id);
    }
    hle_mutex_unlock(&g_disp.lock);
    return pick;
}

void hle_disp_on_complete(uint64_t completed)
{
    if (!g_disp.inited) {
        return;
    }
    hle_mutex_lock(&g_disp.lock);
    for (int i = 0; i < HLE_DISP_SLOTS; i++) {
        if (g_disp.s[i].state == SLOT_PENDING && g_disp.s[i].value <= completed) {
            g_disp.s[i].state = SLOT_READY;
            hle_hitch_frame(g_disp.s[i].swap_id, HLE_FR_READY);
        }
    }
    hle_mutex_unlock(&g_disp.lock);
}

void hle_disp_present_late(bool on)
{
    hle_mutex_lock(&g_disp.lock);
    g_disp.present_late = on;
    g_disp.want = 0;
    hle_mutex_unlock(&g_disp.lock);
}

bool hle_disp_present(uint32_t swap_id)
{
    bool found = false;
    if (!g_disp.inited) {
        return false;
    }
    hle_mutex_lock(&g_disp.lock);
    for (int i = 0; i < HLE_DISP_SLOTS; i++) {
        HleDispSlot *s = &g_disp.s[i];
        if (s->state != SLOT_FREE && s->swap_id == swap_id) {
            to_front(i, swap_id);
            found = true;
            break;
        }
    }
    if (!found && g_disp.present_late && swap_id > g_disp.want) {
        g_disp.want = swap_id;            /* applies when the frame is written */
    }
    hle_cond_broadcast(&g_disp.cond);
    hle_mutex_unlock(&g_disp.lock);
    return found;
}

/* HAKUX_DISP_STATS=1: every 10 s, the new frames the consumer took (the
 * window's presenter, or the libretro core), their age from front image to
 * taken (avg, max) and the gaps between them (avg, max, and over 25 ms).
 * The same code for every front end, to compare their display paths. */
static struct {
    int on;                     /* -1: HAKUX_DISP_STATS not read yet */
    int64_t t0, last, age_sum, age_max, gap_max;
    uint64_t n, late;
} g_dst = { .on = -1 };

static void disp_stat(int64_t front_ns)
{
    if (g_dst.on < 0) {
        const char *e = getenv("HAKUX_DISP_STATS");
        g_dst.on = e && atoi(e) != 0;
    }
    if (!g_dst.on) {
        return;
    }
    const int64_t now = hle_now_ns(), age = now - front_ns;
    if (!g_dst.t0) {
        g_dst.t0 = g_dst.last = now;
        return;
    }
    const int64_t gap = now - g_dst.last;
    g_dst.last = now;
    g_dst.n++;
    g_dst.age_sum += age;
    g_dst.age_max = MAX(g_dst.age_max, age);
    g_dst.gap_max = MAX(g_dst.gap_max, gap);
    g_dst.late += gap > 25000000;
    if (now - g_dst.t0 >= 10000000000ll) {
        const double n = (double)g_dst.n, span = (double)(now - g_dst.t0);
        hle_log("hakuX-hle-disp", "stats: %.1f new frames/s, age front to taken %.2f ms (max %.2f), "
                "gap %.2f ms (max %.2f, %llu over 25 ms)", n * 1e9 / span,
                (double)g_dst.age_sum / n / 1e6, (double)g_dst.age_max / 1e6, span / n / 1e6,
                (double)g_dst.gap_max / 1e6, (unsigned long long)g_dst.late);
        g_dst.t0 = now;
        g_dst.n = g_dst.late = 0;
        g_dst.age_sum = g_dst.age_max = g_dst.gap_max = 0;
    }
}

bool hle_disp_acquire(HleDispFrame *f)
{
    bool found = false;
    uint32_t shown = 0;
    int64_t front_ns = 0;
    if (!hle_atomic_load_acquire(&g_disp.inited)) {
        return false;
    }
    hle_mutex_lock(&g_disp.lock);
    for (int i = 0; i < HLE_DISP_SLOTS; i++) {
        HleDispSlot *s = &g_disp.s[i];
        if (s->state == SLOT_FRONT && s->img) {
            if (s->swap_id != g_disp.last_shown) {
                g_disp.last_shown = s->swap_id;
                shown = g_disp.last_shown;
                front_ns = s->front_ns;
            }
            s->held = true;
            *f = (HleDispFrame){ i, g_disp.gen, s->swap_id, g_disp.w, g_disp.h, s->value, s->img, s->view };
            found = true;
            break;
        }
    }
    hle_mutex_unlock(&g_disp.lock);
    if (shown) {
        hle_hitch_frame(shown, HLE_FR_SHOWN);   /* a new frame on screen */
        disp_stat(front_ns);
    }
    return found;
}

uint8_t *hle_disp_read_front(uint32_t *w, uint32_t *h)
{
    HleDispFrame f;
    if (!hle_vk_ready() || !hle_disp_acquire(&f)) {
        return NULL;
    }
    const VkDeviceSize size = (VkDeviceSize)f.w * f.h * 4;
    uint8_t *out = NULL;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    hle_disp_use_begin();
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkMemoryRequirements mr = { 0 };
    int mt = -1;
    if (T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &buf) == VK_SUCCESS) {
        T.vkGetBufferMemoryRequirements(g_hvk.dev, buf, &mr);
        mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                    .queueFamilyIndex = g_hvk.qfam };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (mt >= 0 && T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &mem) == VK_SUCCESS &&
        T.vkBindBufferMemory(g_hvk.dev, buf, mem, 0) == VK_SUCCESS &&
        T.vkCreateCommandPool(g_hvk.dev, &pci, NULL, &pool) == VK_SUCCESS &&
        T.vkCreateFence(g_hvk.dev, &fci, NULL, &fence) == VK_SUCCESS) {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1 };
        T.vkAllocateCommandBuffers(g_hvk.dev, &ai, &cb);
    }
    if (cb) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        T.vkBeginCommandBuffer(cb, &bi);
        hle_img_barrier(cb, f.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                .imageExtent = { f.w, f.h, 1 } };
        T.vkCmdCopyImageToBuffer(cb, f.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
        hle_img_barrier(cb, f.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        T.vkEndCommandBuffer(cb);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                            .pCommandBuffers = &cb };
        hle_vk_queue_lock();
        VkResult r2 = T.vkQueueSubmit(g_hvk.queue, 1, &si, fence);
        hle_vk_queue_unlock();
        void *p = NULL;
        if (hle_vk_check(r2, "vkQueueSubmit (capture)") && T.vkWaitForFences(g_hvk.dev, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS &&
            T.vkMapMemory(g_hvk.dev, mem, 0, size, 0, &p) == VK_SUCCESS) {
            out = g_malloc(size);
            memcpy(out, p, size);
            T.vkUnmapMemory(g_hvk.dev, mem);
            *w = f.w;
            *h = f.h;
        }
    }
    hle_disp_use_end();
    if (fence) {
        T.vkDestroyFence(g_hvk.dev, fence, NULL);
    }
    if (pool) {
        T.vkDestroyCommandPool(g_hvk.dev, pool, NULL);
    }
    if (buf) {
        T.vkDestroyBuffer(g_hvk.dev, buf, NULL);
    }
    if (mem) {
        T.vkFreeMemory(g_hvk.dev, mem, NULL);
    }
    hle_disp_release(f.slot);
    return out;
}

void hle_disp_release(int slot)
{
    hle_mutex_lock(&g_disp.lock);
    g_disp.s[slot].held = false;
    hle_cond_broadcast(&g_disp.cond);
    hle_mutex_unlock(&g_disp.lock);
}

HleDispStats hle_disp_stats(void)
{
    return g_disp.st;
}

/* The ramp, written by the render thread alone and read by the consumer: a
 * sequence count, odd while a ramp is being written. */
static struct {
    uint8_t rgb[3][256];
    bool identity;
    uint32_t seq;
    bool by_consumer;
} g_gamma = { .identity = true };

void hle_disp_gamma_by_consumer(bool on)
{
    hle_atomic_set(&g_gamma.by_consumer, on);
}

bool hle_disp_gamma_consumer(void)
{
    return hle_atomic_read(&g_gamma.by_consumer);
}

void hle_disp_set_gamma(const uint8_t rgb[3][256], bool identity)
{
    if (identity && g_gamma.identity) {
        return;
    }
    hle_atomic_set(&g_gamma.seq, g_gamma.seq + 1);
    hle_smp_wmb();
    memcpy(g_gamma.rgb, rgb, sizeof(g_gamma.rgb));
    g_gamma.identity = identity;
    hle_smp_wmb();
    hle_atomic_set(&g_gamma.seq, g_gamma.seq + 1);
}

uint32_t hle_disp_gamma_seq(void)
{
    return hle_atomic_load_acquire(&g_gamma.seq);
}

bool hle_disp_front(uint32_t *gen, uint32_t *swap_id)
{
    bool found = false;
    if (!hle_atomic_load_acquire(&g_disp.inited)) {
        return false;
    }
    hle_mutex_lock(&g_disp.lock);
    for (int i = 0; i < HLE_DISP_SLOTS && !found; i++) {
        const HleDispSlot *s = &g_disp.s[i];
        if (s->state == SLOT_FRONT && s->img) {
            *gen = g_disp.gen;
            *swap_id = s->swap_id;
            found = true;
        }
    }
    hle_mutex_unlock(&g_disp.lock);
    return found;
}

bool hle_disp_gamma(uint8_t rgb[3][256], bool *identity, uint32_t *gen)
{
    const uint32_t s0 = hle_atomic_load_acquire(&g_gamma.seq);
    if ((s0 & 1) || s0 == *gen) {
        return false;                   /* being written, or unchanged */
    }
    memcpy(rgb, g_gamma.rgb, sizeof(g_gamma.rgb));
    *identity = g_gamma.identity;
    hle_smp_rmb();
    if (hle_atomic_read(&g_gamma.seq) != s0) {
        return false;                   /* rewritten meanwhile: next time */
    }
    *gen = s0;
    return true;
}

/* The core stops (display.h): the slots' images destroyed and the display as
 * before its first hle_disp_init, which then makes them at any size. Kept on
 * purpose: the locks (hle_disp_setup makes them once, and the presenter's
 * thread may still call in), the generation and the ramp's sequence (they go
 * on: the window's presenter and the libretro core's readback keep the ones
 * they saw, and a value seen before would pass for the next images or ramp),
 * and hle_disp_gamma_by_consumer (the consumer's, set once before the core
 * starts). */
void hle_disp_shutdown(void)
{
    static const uint8_t k_none[3][256];
    if (hle_atomic_load_acquire(&g_disp.inited)) {
        /* as hle_disp_init: the presenter's reads of the images submitted
         * (use), then done; a frame it holds is let go (gen) */
        hle_mutex_lock(&g_disp.use);
        if (!hle_vk_lost()) {
            hle_vk_queue_lock();
            const VkResult r = T.vkDeviceWaitIdle(g_hvk.dev);
            hle_vk_queue_unlock();
            hle_vk_check(r, "vkDeviceWaitIdle (display)");
        }
        hle_mutex_lock(&g_disp.lock);
        bool any = false;
        for (int i = 0; i < HLE_DISP_SLOTS; i++) {
            any |= g_disp.s[i].img != VK_NULL_HANDLE;
            slot_free_vk(&g_disp.s[i]);
        }
        if (any) {
            g_disp.gen++;
        }
        g_disp.w = g_disp.h = 0;
        memset(&g_disp.st, 0, sizeof(g_disp.st));
        g_disp.present_late = false;
        g_disp.want = 0;
        g_disp.last_shown = 0;
        g_disp.no_slot_logs = 0;
        memset(&g_dst, 0, sizeof(g_dst));       /* the presenter takes frames inside use */
        g_dst.on = -1;
        hle_mutex_unlock(&g_disp.lock);
        hle_mutex_unlock(&g_disp.use);
    }
    /* no ramp, as a change the consumer sees (the render thread, its writer,
     * is gone); the bytes are not read with identity */
    hle_disp_set_gamma(k_none, true);
    memset(g_gamma.rgb, 0, sizeof(g_gamma.rgb));
}
