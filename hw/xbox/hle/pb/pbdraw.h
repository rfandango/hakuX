/*
 * hakuX HLE GPU — push-buffer mode's drawing: Kelvin (class 0x97) methods
 * into the back end's commands (front/cmd.h), on the decode thread. So
 * far: the render targets (SET_SURFACE_*), CLEAR_SURFACE, the frame
 * presented at each FLIP_INCREMENT_WRITE (the colour target just
 * finished), and draws with the stages' textures: from the vertex arrays
 * (indexed and DRAW_ARRAYS), inline arrays and immediate vertices, through
 * the vertex program or the fixed-function stage (shader/ffhw.h).
 */
#ifndef HW_XBOX_HLE_PB_PBDRAW_H
#define HW_XBOX_HLE_PB_PBDRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The back end, at push-buffer mode's start; false if it can't run. */
bool hle_pbd_start(void);
void hle_pbd_stop(void);
/* A Kelvin method (after the decoder's own handling). */
void hle_pbd_method(uint32_t m, uint32_t v);
/* n data words of one packet (method m, then +4 each unless noninc). */
void hle_pbd_run(uint32_t m, bool noninc, const uint32_t *w, uint32_t n);
/* Decode thread: the guest pointed the display at pa (PCRTC_START). A
 * title that flips that way itself, with no FLIP_INCREMENT_WRITE since the
 * last time (Riddick), gets the render target there presented; flip: D3D's
 * flip (hle_pbd_flip_due when the start was written), presented by it. */
void hle_pbd_display(uint32_t pa, bool flip);
/* The guest writes PCRTC_START (its thread): a swap waits for its flip and
 * the guest has been flipping (within a second), so the start is D3D's
 * flip, whose read counter increment comes next and presents the frame. */
bool hle_pbd_flip_due(void);
/* Decode thread: PVIDEO's registers changed: the overlay they describe
 * (buffer 0: YUY2 / UYVY at BASE + OFFSET, SIZE_IN onto POINT_OUT /
 * SIZE_OUT) goes over each presented frame, or none when stopped. */
void hle_pbd_overlay(bool show);
/* debug: the skipped draws by reason, " reason=n ..." */
void hle_pbd_skip_reasons(char *buf, size_t len);
/* debug: overlay updates sent to the back end */
uint64_t hle_pbd_overlays(void);
/* PCRTC_START writes that named the object shown, unchanged (not presented) */
uint64_t hle_pbd_display_repeats(void);
/* program draws that reused the last draw's constant block */
uint64_t hle_pbd_const_reuses(void);
/* debug: vertex ranges read in place and copied; submissions closed for a
 * barrier's waiter; DRAW_ARRAYS drawn without indices (one run in order) */
void hle_pbd_vb_counts(uint64_t *in_place, uint64_t *copied, uint64_t *bnd_kicks, uint64_t *unindexed);
/* debug: small targets copied back to guest memory (writeback_small) */
uint64_t hle_pbd_writebacks(void);
/* Decode thread, between methods: a guest store waits for `value` (the
 * first-write barrier); the open submission, if it is that one, is closed
 * and sent at once. */
void hle_pbd_boundary(uint64_t value);
/* Decode thread: the display's gamma ramp (R / G / B planes), at once. */
void hle_pbd_gamma(const uint8_t rgb[3][256]);
/* The host: D3D's vblank handler flipped (the flip read counter moved), the
 * display starting at start (PCRTC_START): the oldest frame swapped and not
 * yet shown goes on screen. At the target being drawn (hle_pbd_trap), the
 * swap that comes is one flipped (hle_pbd_scanout). Any thread. */
void hle_pb_flipped(uint32_t start);
/* Decode thread, at a software trap (its handler may flip): the bound
 * target, if drawn since the last swap, is a frame whose flip can come
 * before its FLIP_INCREMENT_WRITE. */
void hle_pbd_trap(void);
/* Counters for the decoder's log line (swaps: FLIP_INCREMENT_WRITE's and
 * PCRTC_START's presents; rt_uploads: render targets the CPU wrote). */
void hle_pbd_counts(uint64_t *clears, uint64_t *swaps, unsigned *objects,
                    uint64_t *rt_uploads);
/* draws: all of them; ff: of those, fixed function */
/* A 2D blit (surfaces at sbase / dbase, pitches sp / dp, bpp bytes a pixel;
 * w x h from (ix, iy) to (ox, oy)), decode thread: true if it was a copy
 * of render targets' images (a linear one's into another, or into memory
 * with none: the GPU's, at their scale, the memory left alone); false: the
 * caller copies the memory (hle_pbd_copy_begin, _done), then the images
 * follow (hle_pbd_blit). */
bool hle_pbd_blit_images(uint32_t sbase, uint32_t sp, uint32_t dbase, uint32_t dp, uint32_t bpp,
                         uint32_t ix, uint32_t iy, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h);
/* A copy of guest memory by the 2D engine (a blit, M2MF: rows of row bytes
 * from slo into dlo, sp / dp apart, which may be negative) (decode thread):
 * hle_pbd_copy_begin, 0 if the caller makes it now, else the value it was
 * queued at on the timeline (vk/timeline.c hle_tl_land_copy: behind the
 * landings of the targets over the source whose images are newer, and the
 * copies queued over either range before it; dbase: a blit's destination
 * surface, 0 for M2MF); then hle_pbd_copy_done: a queued one's destination
 * closed to the guest till it lands, the targets over it with their memory
 * newer than their images. */
uint64_t hle_pbd_copy_begin(uint32_t slo, uint32_t sp, uint32_t dlo, uint32_t dp, uint32_t row,
                            uint32_t rows, uint32_t dbase);
void hle_pbd_copy_done(uint32_t dlo, uint32_t dp, uint32_t row, uint32_t rows, uint64_t dep);
/* Counters: targets' images landed, copies queued, decoder waits for them */
void hle_pbd_coh_counts(uint64_t *lands, uint64_t *copies, uint64_t *waits);
/* After a blit's copy in memory (dep: queued, hle_pbd_copy_begin): the
 * linear render target at dbase takes the rows into its image. */
void hle_pbd_blit(uint32_t dbase, uint32_t dp, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h, uint64_t dep);
/* Decode thread: a fence (a semaphore release, a report, the push buffer
 * consumed): the images of targets the guest reads (learned) landed for it. */
void hle_pbd_fence(void);
/* Decode thread: GET_REPORT for the visibility test that SET_ZPASS_PIXEL_
 * COUNT_ENABLE opened: the back end writes its report at pa (time stamp,
 * pixels, status 0) once the GPU ran its draws. False when nothing can
 * count it yet (no device): the caller writes the report itself. */
bool hle_pbd_vis_report(uint32_t pa);
/* Decode thread: the push buffer is consumed up to PUT (or stalled): reports
 * still unsubmitted go to the GPU now, kept memory goes back (hle_pbd_keep). */
void hle_pbd_idle(void);
/* Decode thread, each pass: the frame on screen into its memory when the
 * guest's first write there asked (front_stale). */
void hle_pbd_front_serve(void);
/* Decode thread, after a swap: true if the decoder stalls for the frame
 * pace until the GPU finishes an earlier swap (pbdraw.c g_pace); the GPU
 * kicks it then (hle_pb_kick), and hle_pbd_pace_reached says it has. */
bool hle_pbd_pace(void);
bool hle_pbd_pace_reached(void);
/* Any thread: nothing presented for a while (the title isn't flipping), so
 * the display's memory is worth a look (hle_pbd_scanout). */
bool hle_pbd_scan_due(void);
/* Decode thread: the frame on screen, if the CPU wrote its memory or the
 * GPU drew into it since and the title doesn't flip, presented again (the
 * NV2A scans the memory out: a loading prompt or screen drawn into the front
 * buffer shows); first, a swap its flip came before, if no flip followed. */
void hle_pbd_scanout(void);
/* Decode thread: the frame buffer AvSetDisplayMode gave, when the title has
 * no GPU device (it draws its frames with the CPU: nxdk homebrew such as
 * Insignia's Setup Assistant), shown as the memory holds it; the scanout
 * shows it again as the CPU writes it. A title with a device keeps its own
 * flips. */
void hle_pbd_cpu_display(uint32_t pa);
/* Any thread: guest memory [pa, pa + len) is kept for the next title (a quick
 * reboot): the decode thread writes the images over it back into it when it
 * is next idle. hle_pbd_keep_wait: until those copies are in memory (false:
 * not served within timeout_ns). */
void hle_pbd_keep(uint32_t pa, uint32_t len);
bool hle_pbd_keep_wait(uint64_t timeout_ns);
/* debug: what the decode thread waits for in a draw ("arena", "ring"), or NULL */
const char *hle_pbd_phase(void);
void hle_pbd_draw_counts(uint64_t *draws, uint64_t *ff, uint64_t *skipped, uint64_t *tex_defs);
/* Visibility tests ended, and the submit boundaries they needed of their own */
void hle_pbd_vis_counts(uint64_t *tests, uint64_t *kicks);
/* debug (HAKUX_HLE_PB_NOVIS=1): no occlusion queries; visibility tests
 * report every pixel passed */
bool hle_pbd_novis(void);

#endif
