/*
 * hakuX HLE GPU — Vulkan 1.3's entry points on drivers below it (vk/compat.c).
 */
#ifndef HW_XBOX_HLE_VK_COMPAT_H
#define HW_XBOX_HLE_VK_COMPAT_H

#include "hw/xbox/hle/vk/vkdev.h"

/* t's entries for what g_hvk.caps says the device lacks (vk13, dyn_render,
 * sync2, eds1, eds2 off): the extensions' entry points where the driver has
 * them, else this file's on top of Vulkan 1.0's. Every table loaded from the
 * device goes through it (vkdev.c's, the presenter's), before strip batching
 * wraps it. Nothing changes when the caps are all on. */
void hle_compat_install(struct VolkDeviceTable *t);
/* The device goes (hle_vk_shutdown, idle, every view destroyed): the
 * emulated render passes' and framebuffers' objects destroyed, the table
 * loaded again by the next install. */
void hle_compat_shutdown(void);

#endif
