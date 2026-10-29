/*
 * hakuX HLE GPU — guest RAM as one Vulkan buffer (spec §5.2) and the UMA
 * upload-arena buffer (§5.3). Include only under CONFIG_VULKAN.
 */
#ifndef HW_XBOX_HLE_VK_GUEST_RAM_H
#define HW_XBOX_HLE_VK_GUEST_RAM_H

#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/front/arena.h"

typedef struct HleVkGuestRam {
    VkBuffer buf;               /* guest_ram_buf: offset = guest PA */
    VkDeviceMemory mem;
    VkDeviceAddress bda;        /* 0 when BDA import was refused */
    uint64_t size;
    bool zero_copy;             /* false = copy fallback (§5.2) */
} HleVkGuestRam;

typedef struct HleVkArena {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    HleArena alloc;
} HleVkArena;

extern HleVkGuestRam g_hram;
extern HleVkArena g_harena;

/* Readbacks that land in guest memory without zero-copy RAM (hle_tl_land)
 * go through host-cached memory the CPU reads fast; the arena's is
 * write-combined (a 640x480 frame took ~25 ms to read). g_hland.buf NULL:
 * none (no cached type, or zero-copy RAM): the arena serves. Not coherent:
 * invalidated before the CPU reads it. */
extern HleVkArena g_hland;
extern bool g_hland_coherent;

int hle_vk_guest_ram_init(void);
int hle_vk_arena_init(uint32_t size);
int hle_vk_land_init(uint32_t size);
void hle_vk_guest_ram_shutdown(void);

#endif
