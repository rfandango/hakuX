/*
 * hakuX native core — MCPX 8-tap Lagrange voice resampler (resample.c).
 * Preserves the subset of the libsamplerate callback API used by hakuX,
 * plus per-output 16.16 pitch steps for the hardware-style 32-sample ramp.
 * Source: raddad772 Xbox measurements, xemu PR #3105 (38e5733).
 */
#ifndef NATIVE_DEV_APU_VP_RESAMPLE_H
#define NATIVE_DEV_APU_VP_RESAMPLE_H

#include <stddef.h>

typedef struct SRC_STATE SRC_STATE;
typedef long (*src_callback_t)(void *cb_data, float **data);

enum {
    SRC_SINC_FASTEST = 0,
};

SRC_STATE *src_callback_new(src_callback_t cb, int converter_type, int channels, int *error, void *cb_data);
long src_callback_read(SRC_STATE *state, double ratio, long frames, float *data);
/* Per-output 16.16 input-advance steps, preserving the existing callback
 * and guest buffer consumption. Reference: xemu PR #3105 38e5733. */
long src_callback_read_steps(SRC_STATE *state, const uint32_t *steps,
                             long frames, float *data);
int src_reset(SRC_STATE *state);
const char *src_strerror(int error);
void src_float_to_short_array(const float *in, short *out, int len);

#endif
