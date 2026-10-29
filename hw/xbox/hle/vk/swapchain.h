/*
 * hakuX HLE GPU — the window's presenter (vk/swapchain.c): the front frame
 * drawn into the app's window through a Vulkan swapchain: an Android window,
 * or on Linux an SDL window (KMSDRM) made with SDL_WINDOW_VULKAN.
 */
#ifndef HW_XBOX_HLE_VK_SWAPCHAIN_H
#define HW_XBOX_HLE_VK_SWAPCHAIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __ANDROID__
struct ANativeWindow;
typedef struct ANativeWindow HleWindow;
#else
struct SDL_Window;
typedef struct SDL_Window HleWindow;
#endif

/* A frame smaller than its place in the window: scaled up bilinearly, or by
 * Snapdragon Game Super Resolution (a larger one is always averaged down). */
enum { HLE_UPSCALE_BILINEAR, HLE_UPSCALE_SGSR };

/* This refresh would put up what win already shows (the same front frame,
 * gamma ramp, window and swapchain, and key: the caller's placement and
 * filters, as at the last present that went through): the caller may skip
 * it, the panel keeping the image. A 30 fps title on a 120 Hz panel drew,
 * submitted and presented each frame four times. */
bool hle_present_unchanged(HleWindow *win, uint64_t key);

/* Each display refresh, on the window's thread, these three in order.
 * begin: an image of win's swapchain (made or remade as the window comes,
 * goes or changes size), waiting for one: the panel's pace. *w, *h: the
 * window's size. false: nothing to draw into (the HLE GPU not up yet, no
 * window): skip this refresh, no frame or end. */
bool hle_present_begin(HleWindow *win, uint32_t *w, uint32_t *h);
/* frame: the front frame, held until a newer one is taken; its size, false
 * when there is none yet. */
bool hle_present_frame(uint32_t *w, uint32_t *h);
/* end: the frame drawn into the window's middle (sx, sy: its half size in
 * clip space, 1 the whole window), the rest black, through the display's
 * gamma ramp, and presented. fxaa: FXAA over each new frame first; average:
 * false samples a frame larger than its place bilinearly too (comparisons).
 * key: what hle_present_unchanged compares next. */
void hle_present_end(float sx, float sy, bool fxaa, int upscale, bool average, uint64_t key);

#endif
