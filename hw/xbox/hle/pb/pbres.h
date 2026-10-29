/*
 * hakuX HLE GPU — push-buffer mode's resources (pbres.c): texels into the
 * upload arena for the back end, render targets' rows the CPU wrote,
 * readbacks landing in guest memory, and the primitive conversions.
 */
#ifndef HW_XBOX_HLE_PB_PBRES_H
#define HW_XBOX_HLE_PB_PBRES_H

#include "hw/xbox/hle/front/cmd.h"

/* Arena space for the next submission; HLE_ARENA_FULL if none. */
uint32_t hle_draw_arena_alloc(uint32_t len, uint8_t **ptr);
/* The same, 16 bytes aligned instead of 256 (a draw's indices). */
uint32_t hle_draw_arena_alloc16(uint32_t len, uint8_t **ptr);
/* The texels of header words h5 (P8: palette pal, npal entries) defined as
 * object id's image (HLE_OP_TEX); id, or 0 if they can't be. *bytes: the
 * span of guest memory they came from. */
uint32_t hle_draw_tex_define(uint32_t id, const uint32_t h5[5], const uint32_t *pal, uint32_t npal,
                             uint32_t *bytes);
/* The video overlay's surface (header words h5, YUY2 / UYVY) as B8G8R8A8 in
 * the arena: its offset, HLE_ARENA_FULL = none. */
uint32_t hle_draw_overlay_upload(const uint32_t h5[5], uint32_t *w, uint32_t *h);
/* NV2A / Xbox D3D primitive types (1 points .. 10 polygon): the Vulkan
 * topology they are drawn with, the index count after conversion, and the
 * converted indices (in NULL: 0..n-1; rebased by -base). */
uint32_t hle_draw_prim_topology(uint32_t pt);
uint32_t hle_draw_prim_count(uint32_t pt, uint32_t n);
void hle_draw_prim_indices(uint32_t pt, const uint16_t *in, uint32_t n, uint32_t base,
                           uint16_t *out);

/* Texels the CPU wrote at pa (linear rows of pitch bytes) into object id's
 * image: rects of source pixels, each to its pts[i] (HLE_OP_UPLOAD). */
void hle_res_upload(uint32_t id, uint32_t pa, uint32_t pitch, uint32_t n,
                    const HleRect *rects, const int32_t (*pts)[2]);
/* A direct readback (HLE_RB_DIRECT) when the GPU can't write guest RAM (not
 * imported: the native core's): hle_res_rb_fb reserves len bytes of arena
 * for the back end's copy (0: none needed, or no room: the readback is
 * skipped and guest memory stays), before the command (a full arena kicks
 * one); hle_res_rb_land, between the command's begin and commit, has the
 * copy land at rb->pa once the GPU reaches value. */
uint32_t hle_res_rb_fb(uint32_t len);
void hle_res_rb_land(HleCmdReadback *rb, uint32_t fb, uint32_t len, uint64_t value);
/* The depth word's byte for each channel (HleCmdZRead.map) of a 32-bit
 * linear colour format read over a D24S8 buffer; false for other formats. */
bool hle_res_zread_map(uint32_t xfmt, uint8_t map[4]);
/* The core stops (M2): the log lines' limits and the texture dump asked
 * for as at the start (hle_pb_reset's). */
void hle_res_reset(void);

#endif
