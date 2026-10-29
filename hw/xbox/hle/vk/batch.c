/*
 * hakuX HLE GPU — strip batching.
 *
 * A title draws its meshes as many short triangle strips with nothing
 * changing between them (Blinx: ~500k strip draws a second, state every ~20
 * draws), and the GPU pays ~1.7 us per draw whatever it renders. A run of
 * strip (or fan) draws that records nothing in between becomes one indexed
 * draw: each strip's vertices as 32-bit indices in a per-frame-slot buffer,
 * the strips separated by primitive restart, so the GPU draws exactly the
 * same triangles. vertexOffset is the run's first vertex, so gl_VertexIndex
 * keeps the value each draw gave it.
 *
 * The held draws are recorded before anything else: every recording command
 * in the device table goes through a wrapper (batch_wrap.h) that records them
 * first. A run of one is recorded as the plain draw it was.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/batch.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/vk/vkdev.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif

void hle_draw_ibuf_lost(void);

/* Indices per frame slot (4 MiB each, one slot per command buffer in
 * flight): a run that would not fit is recorded as it stands and a new one
 * starts. */
#define SLOT_INDICES (1u << 20)
#define SLOTS        HLE_FRAMES
#define RESTART      0xFFFFFFFFu

static struct VolkDeviceTable R;    /* the real entry points */

static struct {
    bool on;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *map;
    uint32_t slot_base, pos;        /* the current slot's first index, the next free */
    /* the run held */
    VkCommandBuffer cb;             /* NULL: none */
    VkPrimitiveTopology prim;
    uint32_t first, count;          /* its first draw, as it came */
    uint32_t base;                  /* vertexOffset of the merged draw */
    uint32_t istart, n;             /* its indices (absolute index, count); n = 0: not written */
    uint32_t draws;
    uint64_t batches, merged;
} g_b;

/* Records the run held on cb: the plain draw for a run of one. */
static void flush(void)
{
    VkCommandBuffer cb = g_b.cb;
    g_b.cb = VK_NULL_HANDLE;
    if (g_b.draws == 1) {
        R.vkCmdDraw(cb, g_b.count, 1, g_b.first, 0);
        return;
    }
    R.vkCmdBindIndexBuffer(cb, g_b.buf, (VkDeviceSize)g_b.istart * 4, VK_INDEX_TYPE_UINT32);
    R.vkCmdSetPrimitiveRestartEnable(cb, VK_TRUE);
    R.vkCmdDrawIndexed(cb, g_b.n, 1, 0, (int32_t)g_b.base, 0);
    R.vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    hle_draw_ibuf_lost();               /* the draw path's index buffer is no longer bound */
    g_b.batches++;
    g_b.merged += g_b.draws;
}

/* Anything recorded on cb comes after the held run. */
static inline void batch_before(VkCommandBuffer cb)
{
    if (g_b.cb == cb && cb) {
        flush();
    }
}

#include "hw/xbox/hle/vk/batch_wrap.h"

void hle_batch_install(struct VolkDeviceTable *t)
{
    R = *t;
    wrap_table(t);
}

/* Writes a strip's indices (relative to the run's base) at the slot's head. */
static bool put(uint32_t first, uint32_t count, bool restart)
{
    uint32_t need = count + (restart ? 1 : 0);
    if (g_b.pos + need > SLOT_INDICES || first < g_b.base) {
        return false;
    }
    uint32_t *p = g_b.map + g_b.slot_base + g_b.pos;
    if (restart) {
        *p++ = RESTART;
    }
    for (uint32_t i = 0; i < count; i++) {
        p[i] = first - g_b.base + i;
    }
    g_b.pos += need;
    g_b.n += need;
    return true;
}

bool hle_batch_draw(VkCommandBuffer cb, VkPrimitiveTopology prim, uint32_t first, uint32_t count)
{
    if (!g_b.on || !count) {
        return false;
    }
    if (g_b.cb == cb && g_b.prim == prim) {
        if (g_b.draws == 1) {           /* a second strip: the first goes in as indices too */
            g_b.istart = g_b.slot_base + g_b.pos;
            g_b.n = 0;
            if (!put(g_b.first, g_b.count, false)) {
                flush();
                goto start;
            }
        }
        uint32_t pos = g_b.pos, n = g_b.n;
        if (put(first, count, true)) {
            g_b.draws++;
            return true;
        }
        g_b.pos = pos;
        g_b.n = n;
        flush();
    } else if (g_b.cb) {
        flush();
    }
start:
    g_b.cb = cb;
    g_b.prim = prim;
    g_b.first = first;
    g_b.count = count;
    g_b.base = first;
    g_b.draws = 1;
    g_b.n = 0;
    return true;
}

void hle_batch_slot(unsigned slot)
{
    g_b.slot_base = (slot % SLOTS) * SLOT_INDICES;
    g_b.pos = 0;
}

void hle_batch_flush(VkCommandBuffer cb)
{
    batch_before(cb);
}

void hle_batch_stats(uint64_t *batches, uint64_t *merged)
{
    *batches = g_b.batches;
    *merged = g_b.merged;
}

static bool batch_wanted(void)
{
    const char *e = getenv("HAKUX_HLE_BATCH");
    if (e) {
        return atoi(e) != 0;
    }
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (hle_prop_get("debug.hakux.batch", v) > 0 && v[0]) {
        return atoi(v) != 0;
    }
#endif
    return true;
}

void hle_batch_init(void)
{
    if (g_b.buf || !batch_wanted()) {
        hle_log("hakuX-hle-back", "strip batching %s", g_b.buf ? "on" : "off");
        return;
    }
    if (!g_hvk.caps.eds2) {             /* restart is the pipelines' then, off (vk/draw.c) */
        hle_log("hakuX-hle-back", "strip batching off: no dynamic primitive restart");
        return;
    }
    const VkDeviceSize size = (VkDeviceSize)SLOTS * SLOT_INDICES * 4;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (R.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_b.buf) != VK_SUCCESS) {
        g_b.buf = VK_NULL_HANDLE;
        return;
    }
    VkMemoryRequirements req;
    R.vkGetBufferMemoryRequirements(g_hvk.dev, g_b.buf, &req);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int mt = hle_vk_memtype(req.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0) {
        mt = hle_vk_memtype(req.memoryTypeBits, hv, 0);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || R.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_b.mem) != VK_SUCCESS) {
        R.vkDestroyBuffer(g_hvk.dev, g_b.buf, NULL);
        g_b.buf = VK_NULL_HANDLE;
        g_b.mem = VK_NULL_HANDLE;
        hle_log("hakuX-hle-back", "strip batching off: no index memory");
        return;
    }
    R.vkBindBufferMemory(g_hvk.dev, g_b.buf, g_b.mem, 0);
    R.vkMapMemory(g_hvk.dev, g_b.mem, 0, size, 0, (void **)&g_b.map);
    g_b.on = g_b.map != NULL;
    hle_log("hakuX-hle-back", "strip batching %s (%u KiB of indices per frame slot)", g_b.on ? "on" : "off",
            SLOT_INDICES * 4 / 1024);
}

/* The recordings ended (no run held), the device idle: the index buffer
 * gone (its memory unmapped as it is freed), and the real entry points
 * with it: hle_batch_install takes the next device's table. */
void hle_batch_shutdown(void)
{
    if (g_b.buf) {
        R.vkDestroyBuffer(g_hvk.dev, g_b.buf, NULL);
    }
    if (g_b.mem) {
        R.vkFreeMemory(g_hvk.dev, g_b.mem, NULL);
    }
    memset(&g_b, 0, sizeof(g_b));
    memset(&R, 0, sizeof(R));
}

