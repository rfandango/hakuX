/*
 * hakuX native core, app front end (libhakuxcore.so): SDL3 window and input,
 * the core on its own thread, and the HLE GPU's frames presented (through
 * its Vulkan swapchain).
 * Independent of xemu: it talks to native/core and the HLE GPU's display only.
 */
#ifndef NATIVE_APP_APP_H
#define NATIVE_APP_APP_H

#include "native/core/nc.h"
#include "native/dev/usb.h"
#include "hw/xbox/hle/vk/swapchain.h"

struct SDL_Gamepad;

/* present.c — on the window's thread. */
bool nc_present_init(void);
/* Once per display refresh: the newest frame (or the last one) letterboxed
 * into win, waiting for the panel; false when nothing could be presented
 * (the HLE GPU not up yet, no window). */
bool nc_present_frame(HleWindow *win);
/* The picture's shape on screen: the frame's own (auto: 4:3 for 640x480),
 * 4:3, 16:9 (a widescreen title's anamorphic 640x480), or the whole window. */
enum { NC_ASPECT_AUTO, NC_ASPECT_4_3, NC_ASPECT_16_9, NC_ASPECT_STRETCH };
void nc_present_set_aspect(int mode);
/* The console is set to widescreen: auto shows a 4:3-sized frame as 16:9
 * (a widescreen title renders anamorphic into it). */
void nc_present_set_widescreen(bool on);
/* FXAA over each frame before it's scaled to the window. */
void nc_present_set_fxaa(bool on);
/* A frame smaller than its place in the window: scaled up bilinearly, or by
 * Snapdragon Game Super Resolution (a larger one is always averaged down). */
enum { NC_UPSCALE_BILINEAR, NC_UPSCALE_SGSR };   /* swapchain.h HLE_UPSCALE_*'s */
void nc_present_set_upscale(int mode);
/* Frames the title presented per second over the last half second, their
 * average interval, and the last one's (ms). */
void nc_present_stats(float *fps, float *avg_ms, float *last_ms);
/* A title frame has been on screen (any thread). */
bool nc_present_shown(void);

/* gamepad.c — a modern gamepad's state added to s as the Duke's: face and
 * shoulder buttons fully pressed, the triggers and sticks the furthest of
 * the two. */
void nc_read_gamepad(struct SDL_Gamepad *p, NcPadState *s);

/* vklayer.c — debug.hakux.vkvalidation=1: the Khronos validation layer in
 * front of the custom driver, its vkGetInstanceProcAddr returned in place of
 * the driver's (drv_gipa as it is otherwise, or without the layer). */
void *nc_vk_layers(void *drv_gipa);
/* vkdriver.c — "<driver dir>|<library>|<hook library dir>": the custom
 * Vulkan driver (Turnip) loaded through adrenotools and handed to the HLE
 * GPU (the system driver when it fails, or the build has no adrenotools). */
void nc_vk_driver_load(const char *spec);
/* The kgsl priority the loaded driver's draw contexts got (1 the highest,
 * 15 the lowest), 0 when none was set (no custom driver, not Turnip, or the
 * system driver after a failed load: the kernel's default, 8). */
int nc_vk_driver_kgsl_priority(void);

/* vsync.c — the panel's vsyncs to the emulated display's vblank clock. */
void nc_vsync_start(void);

#endif
