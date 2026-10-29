/*
 * hakuX HLE GPU — guest RAM import (AHB) and upload-arena buffer.
 */
#include "hw/xbox/hle/base.h"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/uma/uma.h"
#include "hw/xbox/hle/vk/guest_ram.h"

HleVkGuestRam g_hram;
HleVkArena g_harena;
HleVkArena g_hland;
bool g_hland_coherent;

#define T (g_hvk.t)

#ifdef __ANDROID__
static bool import(AHardwareBuffer *ahb, bool bda)
{
    VkAndroidHardwareBufferPropertiesANDROID props = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    if (!g_hvk.get_ahb_props ||
        g_hvk.get_ahb_props(g_hvk.dev, ahb, &props) != VK_SUCCESS) {
        return false;
    }
    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext,
        .size = g_hram.size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_hram.buf) != VK_SUCCESS) {
        g_hram.buf = VK_NULL_HANDLE;
        return false;
    }
    /* cached and coherent where the import allows it: the native core's RAM
     * is CPU-cached (uma.c hle_uma_ram_fd). The driver doesn't make the GPU
     * snoop an imported buffer through this type: the barrier cleans the
     * CPU's lines over what the GPU uses in place (mem/barrier.c) */
    int mt = hle_vk_memtype(props.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (mt < 0) {
        mt = hle_vk_memtype(props.memoryTypeBits, 0, 0);
    }
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = g_hram.buf };
    VkImportAndroidHardwareBufferInfoANDROID imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &ded, .buffer = ahb };
    VkMemoryAllocateFlagsInfo fl = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &imp,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = bda ? (const void *)&fl : (const void *)&imp,
        .allocationSize = props.allocationSize, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_hram.mem) != VK_SUCCESS) {
        T.vkDestroyBuffer(g_hvk.dev, g_hram.buf, NULL);
        g_hram.buf = VK_NULL_HANDLE;
        g_hram.mem = VK_NULL_HANDLE;
        return false;
    }
    T.vkBindBufferMemory(g_hvk.dev, g_hram.buf, g_hram.mem, 0);
    if (bda) {
        VkBufferDeviceAddressInfo ai = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = g_hram.buf };
        g_hram.bda = T.vkGetBufferDeviceAddress(g_hvk.dev, &ai);
    }
    hle_log("hakuX-hle-back", "guest RAM imported zero-copy: %llu MiB memtype %d "
            "flags 0x%x (of types 0x%x) bda=%s", (unsigned long long)(g_hram.size >> 20), mt,
            g_hvk.mem.memoryTypes[mt].propertyFlags, props.memoryTypeBits, bda ? "yes" : "no");
    return true;
}
#endif

int hle_vk_guest_ram_init(void)
{
    g_hram.size = hle_ram_size();
#ifdef __ANDROID__
    AHardwareBuffer *ahb = hle_uma_ahb();
    g_hram.zero_copy = ahb && ((g_hvk.caps.bda && import(ahb, true)) || import(ahb, false));
    if (!g_hram.zero_copy) {
        hle_log("hakuX-hle-back", "guest RAM not imported (%s): copy fallback",
                ahb ? "import failed" : "RAM is not an AHB");
    }
#else
    g_hram.zero_copy = false;
    hle_log("hakuX-hle-back", "Linux guest RAM: copy fallback (no external-memory import)");
#endif
    return 0;
}

int hle_vk_arena_init(uint32_t size)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_harena.buf) != VK_SUCCESS) {
        g_harena.buf = VK_NULL_HANDLE;
        return -1;
    }
    VkMemoryRequirements req;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, g_harena.buf, &req);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int mt = hle_vk_memtype(req.memoryTypeBits,
                            hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0) {
        mt = hle_vk_memtype(req.memoryTypeBits, hv, 0);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_harena.mem) != VK_SUCCESS) {
        g_harena.mem = VK_NULL_HANDLE;  /* the buffer is the shutdown's */
        return -1;
    }
    T.vkBindBufferMemory(g_hvk.dev, g_harena.buf, g_harena.mem, 0);
    T.vkMapMemory(g_hvk.dev, g_harena.mem, 0, size, 0, (void **)&g_harena.map);
    hle_arena_init(&g_harena.alloc, size);
    return g_harena.map ? 0 : -1;
}

int hle_vk_land_init(uint32_t size)
{
    if (g_hram.zero_copy) {
        return 0;                       /* readbacks go straight to guest RAM */
    }
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    if (T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &buf) != VK_SUCCESS) {
        return 0;
    }
    VkMemoryRequirements req;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, buf, &req);
    VkMemoryPropertyFlags hc = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    int mt = hle_vk_memtype(req.memoryTypeBits, hc | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    g_hland_coherent = mt >= 0;
    if (mt < 0) {
        mt = hle_vk_memtype(req.memoryTypeBits, hc, 0);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    VkDeviceMemory mem;
    uint8_t *map = NULL;
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &mem) != VK_SUCCESS) {
        T.vkDestroyBuffer(g_hvk.dev, buf, NULL);
        hle_log("hakuX-hle-back", "readback landing: no host-cached memory, the arena serves");
        return 0;
    }
    T.vkBindBufferMemory(g_hvk.dev, buf, mem, 0);
    T.vkMapMemory(g_hvk.dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map);
    if (!map) {
        T.vkFreeMemory(g_hvk.dev, mem, NULL);
        T.vkDestroyBuffer(g_hvk.dev, buf, NULL);
        return 0;
    }
    g_hland.mem = mem;
    g_hland.map = map;
    hle_arena_init(&g_hland.alloc, size);
    g_hland.buf = buf;                  /* last: the front reads it as "ready" */
    hle_log("hakuX-hle-back", "readback landing: %u MiB host-cached (type %d, %scoherent)",
            size >> 20, mt, g_hland_coherent ? "" : "not ");
    return 0;
}

/* (the device idle) the buffer, then its memory, unmapped as it is freed */
static void buf_free(VkBuffer buf, VkDeviceMemory mem)
{
    if (buf) {
        T.vkDestroyBuffer(g_hvk.dev, buf, NULL);
    }
    if (mem) {
        T.vkFreeMemory(g_hvk.dev, mem, NULL);
    }
}

void hle_vk_guest_ram_shutdown(void)
{
    /* the imported RAM's memory holds a reference to the AHB, uma.c's: the
     * free drops it, the guest RAM stays */
    buf_free(g_hram.buf, g_hram.mem);
    buf_free(g_harena.buf, g_harena.mem);
    buf_free(g_hland.buf, g_hland.mem);
    memset(&g_hram, 0, sizeof(g_hram));
    memset(&g_harena, 0, sizeof(g_harena));
    memset(&g_hland, 0, sizeof(g_hland));
    g_hland_coherent = false;
}

