/*
 * hakuX HLE GPU — front-end command producer (vCPU thread only).
 */
#ifndef HW_XBOX_HLE_FRONT_FE_H
#define HW_XBOX_HLE_FRONT_FE_H

#include "hw/xbox/hle/front/ring.h"

int hle_fe_init(void);
/* The core stops (M2: hle_back_shutdown, the render thread joined): the ring
 * freed, the submission values from 0 again as the timeline's (hle_tl_destroy);
 * the next hle_fe_init makes it again. */
void hle_fe_shutdown(void);
HleRing *hle_fe_ring(void);
/* Zeroed payload; waits for ring space (kicking the back end). */
void *hle_fe_begin(uint16_t op, uint32_t len);
/* The same, payload not zeroed (the caller writes all of it). */
void *hle_fe_begin_raw(uint16_t op, uint32_t len);
/* Publishes everything reserved so far and wakes the render thread. */
void hle_fe_commit(void);
/* Draw state st to the back end, which holds *last (the state sent so far;
 * all of st the first time, !*sent): only the 64-byte runs that changed
 * (HLE_OP_STATE_PART). Reserved, not committed (the draw after it is).
 * Returns the runs sent. */
unsigned hle_fe_state(const HleCmdState *st, HleCmdState *last, bool *sent);
/* Next submission value (GPU time, spec §5.12). Monotonic. */
uint64_t hle_fe_next_value(void);
uint64_t hle_fe_last_value(void);

#endif
