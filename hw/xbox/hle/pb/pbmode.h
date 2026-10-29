/*
 * hakuX HLE GPU — push-buffer mode: the title's own D3D runs as guest code,
 * and the HLE takes what it sends the GPU, the NV2A push buffer, from the
 * host's register model (host.h hle_host_gpu_reg*). A decode thread runs
 * the DMA pusher (pusher.h) from DMA_GET to DMA_PUT on every kick and
 * reports GET as it consumes; the Kelvin methods are drawn (pbdraw.h), and
 * what synchronizes D3D with the GPU takes effect (object binding, Kelvin's
 * semaphore release, the flip counters, traps, reports). It starts with the
 * title (core/title.c).
 */
#ifndef HW_XBOX_HLE_PB_PBMODE_H
#define HW_XBOX_HLE_PB_PBMODE_H

#include <stdbool.h>
#include <stdint.h>

/* Starts push-buffer mode for the title (the decode thread; the host's GPU
 * model hands its push buffer to the HLE from now on). */
void hle_pb_start(void);
void hle_pb_stop(void);
/* The core stops (M2): after hle_pb_stop (hle_title_stop) and the threads
 * that read its counts (the hitch monitor, the diagnostics' timer: hle_pb_diag),
 * push-buffer mode as before its first start, pbdraw's, kelvin's and pbres's
 * state with it (hle_pbd_reset, hle_kelvin_reset, hle_res_reset); the display
 * mode forgotten. Not while it runs (logged). */
void hle_pb_reset(void);
bool hle_pb_active(void);
/* The host: the guest wrote channel 0's DMA_PUT (any thread). */
void hle_pb_kick(void);
/* The guest wrote DMA_PUT (its vCPU thread): a kick, and the thread known to
 * the stutter / hang monitor (hitch.h) */
void hle_pb_put(void);
/* The guest is writing DMA_PUT = put (its vCPU thread), before the register
 * holds it: the report scout reads what it pushed (mem/barrier.h
 * hle_barrier_report_pushed) before the decoder can. */
void hle_pb_pushed(uint32_t put);
/* The guest wrote DMA_GET, the channel's or CACHE1's (a reset, a channel
 * set-up): the scout syncs again. */
void hle_pb_get(void);
/* The guest wrote PCRTC_START: the display shows the memory at pa (any
 * thread; the decode thread presents it). */
void hle_pb_display_start(uint32_t pa);
/* AvSetDisplayMode (any thread): its frame buffer's pitch and format, the
 * mode's refresh (50 or 60 Hz); the decoder's display takes its size from
 * them (hle_pb_display_mode_get: 0s while none was set; its return value
 * counts the modes set). */
void hle_pb_display_mode(uint32_t pitch, uint32_t xfmt, unsigned hz);
uint32_t hle_pb_display_mode_get(uint32_t *pitch, uint32_t *xfmt, unsigned *hz);
/* The vblank thread, each vblank: when the title hasn't flipped for a while,
 * the decode thread looks whether the CPU changed the frame on screen
 * (hle_pbd_scanout). */
void hle_pb_vblank(void);
/* The guest wrote PVIDEO's BUFFER (show: the overlay the registers
 * describe) or STOP (hidden); any thread, the decode thread applies it. */
void hle_pb_overlay_changed(bool show);
/* The guest wrote PFIFO CACHE1's DMA_SUBROUTINE (any thread): the decoder
 * takes it before its next branch or method. */
void hle_pb_subroutine_written(uint32_t v);
/* The guest programmed the DAC's 256 colour entries (PRMDIO palette, R / G
 * / B planes): the display's gamma ramp (any thread; the decode thread
 * applies it). */
void hle_pb_gamma(const uint8_t rgb[3][256]);
/* PGRAPH waits in FLIP_STALL for a flip (any thread). */
bool hle_pb_flip_waiting(void);
/* Once a second: kicks, words, the method histogram's top, errors. */
void hle_pb_diag(void);
/* debug (the decode thread): the decoder's GET and the guest's PUT */
void hle_pb_debug_pos(uint32_t *get, uint32_t *put);

#endif
