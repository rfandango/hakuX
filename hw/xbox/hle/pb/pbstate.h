/*
 * hakuX HLE GPU — push-buffer mode in a savestate (native core): the
 * decoder's state as fixed-width fields (pbstate.c stores them through
 * native/state/pbd.def), gathered from and given back to the modules that
 * own it, and the park that stops the decoder where a state can be taken.
 */
#ifndef HW_XBOX_HLE_PB_PBSTATE_H
#define HW_XBOX_HLE_PB_PBSTATE_H

#include <stdbool.h>
#include <stdint.h>

/* pbmode.c: the pusher, the subchannels, the waits, the 2D objects */
typedef struct HlePbState {
    uint32_t dma_base, dma_limit, get;
    bool sub_active;
    uint32_t sub_return;
    uint32_t method, left, count;
    uint8_t subch;
    bool noninc;
    int32_t error;
    uint32_t error_get, error_word;
    bool synced;
    uint32_t inst, last_get, last_sub, sub_mail;
    bool sub_req;
    uint64_t sub_writes;
    uint32_t obj_inst[8];
    uint8_t obj_class[8];
    uint32_t sem_inst, sem_offset, report_inst;
    bool flip_wait, trap_wait;
    uint32_t sub_r[8][256];
    uint32_t s2d[256];
    uint32_t disp_pa;
    bool disp_req, disp_flip, overlay_req, overlay_show;
    uint8_t gamma[3][256];
    bool gamma_req, gamma_set;
} HlePbState;

/* pbdraw.c: the Kelvin methods as written, the transform unit, the draw
 * being gathered (its vertices, inline words and indices follow) */
typedef struct HlePbdState {
    uint32_t r[0x800];
    uint32_t pmem[136][4];
    uint32_t pload, cload;
    uint32_t consts[192][4];
    float cur[16][4];
    bool in_draw;
    uint32_t prim;
    uint32_t nimm, ninl, nidx;
    bool vis_open;
    uint64_t clears, swaps, draws;
    uint32_t disp_w, disp_h;            /* the device's display (v2; 0: none yet) */
    bool disp_moded;                    /* ... the video mode's size */
} HlePbdState;

/* pb/kelvin.c: its register shadow (the draw state reads it) */
typedef struct HleKvState {
    uint32_t r[0x800];
    uint32_t w[0x800 / 32];
} HleKvState;

void hle_pb_state_get(HlePbState *s);
void hle_pb_state_set(const HlePbState *s);
/* the arrays: immediate vertices (nimm x 16 x 4 floats), inline words,
 * indices */
void hle_pbd_state_get(HlePbdState *s, const float **imm, const uint32_t **inl, const uint32_t **idx);
/* false: the counts don't fit this build's buffers */
bool hle_pbd_state_set(const HlePbdState *s, const float *imm, const uint32_t *inl, const uint32_t *idx);
void hle_kelvin_state_get(HleKvState *s);
void hle_kelvin_state_set(const HleKvState *s);

/* The decoder stops between passes (true once it has): at_flip, only
 * stalled at a flip (FLIP_STALL: the frame before it complete); else
 * wherever it is. Parked, the render targets the GPU wrote are in guest
 * memory and the GPU is idle. hle_pb_unpark lets it go on. */
bool hle_pb_park(bool at_flip, uint64_t timeout_ns);
void hle_pb_unpark(void);
/* pbdraw.c, on the decode thread as it parks: everything gathered goes to
 * the GPU, the colour targets it wrote back to guest memory, and the GPU
 * finishes. */
void hle_pbd_park(void);
/* pbdraw.c, after a load: targets made in the next frames take their
 * pixels from guest memory (what the save wrote back). */
void hle_pbd_restored(void);

#endif
