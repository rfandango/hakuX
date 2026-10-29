/*
 * hakuX HLE GPU — the display images (spec §5.11): the frames the render
 * thread writes at its swaps, and the window's presenter (vk/swapchain.c)
 * shows. Slot states are shared by the render thread (write), the timeline
 * watcher (complete), the DPC on the vCPU thread (present) and the window's
 * thread (acquire/release).
 */
#ifndef HW_XBOX_HLE_VK_DISPLAY_H
#define HW_XBOX_HLE_VK_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* The front image, the previous one until the presenter releases it, the
 * next frame (done, waiting for its vblank) and the one being rendered: with
 * the guest's lead of 2 swaps, three slots made the render thread wait for
 * a free one nearly every frame. */
#define HLE_DISP_SLOTS 4

/* Once, before the presenter may come (hle_back_start). */
void hle_disp_setup(void);
/* Render thread (CONFIG_VULKAN builds only). */
int hle_disp_init(uint32_t w, uint32_t h);
void hle_disp_shutdown(void);
int hle_disp_begin_write(uint32_t swap_id, uint64_t value);
void hle_disp_size(uint32_t *w, uint32_t *h);
/* Watcher thread. */
void hle_disp_on_complete(uint64_t completed);
/* DPC (vCPU): make the slot holding swap_id the front image. */
bool hle_disp_present(uint32_t swap_id);
/* A present before its frame is written applies when it is (else it is
 * lost): push-buffer mode, where the title's own flips can run ahead of the
 * render thread (uncapped). */
void hle_disp_present_late(bool on);
/* The presenter's thread: a slot it acquired (back_int.h hle_disp_acquire)
 * and submitted its last read of: the render thread may write it again (the
 * one queue puts the write after the read). */
void hle_disp_release(int slot);
/* The front frame's pixels, RGBA, top row first (g_free them), for captures
 * without the app's window (native/run/capture.c); NULL when there is none. */
uint8_t *hle_disp_read_front(uint32_t *w, uint32_t *h);

typedef struct HleDispStats {
    uint64_t written, presented, dropped, write_waits;
} HleDispStats;
HleDispStats hle_disp_stats(void);

/* The display's gamma ramp (SetGammaRamp). The Xbox applies it at scanout,
 * so a ramp that changes between swaps shows at once: Forza fades its
 * splash screens in and out with it, each presented once. A consumer that
 * applies the ramp where it draws says so before the first swap and gets
 * frames without it; otherwise the render thread bakes it into each frame. */
void hle_disp_gamma_by_consumer(bool on);
bool hle_disp_gamma_consumer(void);
/* Render thread: the ramp SetGammaRamp set (identity: no change). */
void hle_disp_set_gamma(const uint8_t rgb[3][256], bool identity);
/* Consumer: the ramp, when it changed since *gen (false: unchanged). */
bool hle_disp_gamma(uint8_t rgb[3][256], bool *identity, uint32_t *gen);
/* The ramp's sequence number now (what *gen becomes): a look, no copy. */
uint32_t hle_disp_gamma_seq(void);
/* The front frame's generation and swap id, without taking it (the
 * presenter's look for a new frame); false: none. */
bool hle_disp_front(uint32_t *gen, uint32_t *swap_id);

#endif
