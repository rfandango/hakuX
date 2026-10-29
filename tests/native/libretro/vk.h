/*
 * hakuX libretro core — the harness's Vulkan frontend (vk.c).
 */
#ifndef TESTS_NATIVE_LIBRETRO_VK_H
#define TESTS_NATIVE_LIBRETRO_VK_H

#define VK_NO_PROTOTYPES
#include "native/libretro/libretro.h"
#include "native/libretro/libretro_vulkan.h"

#include <stdbool.h>

/* libvulkan loaded (once); its vkGetInstanceProcAddr. */
bool hv_load(void);
PFN_vkGetInstanceProcAddr hv_gipa(void);
/* A new instance, and the device the core makes on it through its
 * negotiation interface (create_device2): before the core's context_reset. */
bool hv_create(const struct retro_hw_render_context_negotiation_interface_vulkan *neg);
/* The render interface for GET_HW_RENDER_INTERFACE; NULL without a device. */
const struct retro_hw_render_interface_vulkan *hv_interface(void);
/* A frame handed over (video_cb with RETRO_HW_FRAME_BUFFER_VALID), w x h:
 * the core's command buffers submitted, the image read back now and then. */
void hv_frame(unsigned w, unsigned h);
/* The device and instance destroyed: after the core's context_destroy. */
void hv_destroy(void);

#endif
