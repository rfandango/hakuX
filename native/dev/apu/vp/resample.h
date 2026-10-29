/*
 * hakuX native core — the voices' resampler (resample.c, linear with NEON):
 * libsamplerate's callback API, the subset the VP uses.
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
int src_reset(SRC_STATE *state);
SRC_STATE *src_delete(SRC_STATE *state);    /* NULL */
const char *src_strerror(int error);
void src_float_to_short_array(const float *in, short *out, int len);

#endif
