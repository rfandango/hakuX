/*
 * hakuX HLE GPU — push-buffer mode's Kelvin (class 0x97) register shadow:
 * the registers as the NV2A holds them, and the draw state they make
 * (kelvin.c).
 */
#ifndef HW_XBOX_HLE_PB_KELVIN_H
#define HW_XBOX_HLE_PB_KELVIN_H

#include "hw/xbox/hle/front/cmd.h"

/* A Kelvin method the decoder ran. */
void hle_kelvin_method(uint32_t m, uint32_t v);
/* The core stops (M2; the decoder joined): every register as never written,
 * as at power-on (the next title's draws take the state's defaults). */
void hle_kelvin_reset(void);
/* The draw state the registers make: render states, texture-stage states,
 * the combiner block (over st's defaults where never written). */
void hle_kelvin_state(HleCmdState *st);
/* changes when hle_kelvin_state's may: a draw's transforms, lights and
 * texture images leave it (the state is built for draws that changed it
 * only) */
uint32_t hle_kelvin_state_gen(void);
/* The XDK format codes of the NV2A's surface formats (SET_SURFACE_FORMAT's
 * colour field; depth: Z24S8 or Z16, float, swizzled), 0 for none. */
uint32_t hle_kelvin_color_xfmt(uint32_t cf, bool swz);
uint32_t hle_kelvin_depth_xfmt(bool z24, bool zfloat, bool swz);

#endif
