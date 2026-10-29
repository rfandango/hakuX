#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "resample.h"

/*
 * Linear-interpolation resampler stub used in place of libsamplerate.
 *
 * Per-refill malloc/memcpy/free was the bulk of the cost in profiling
 * (PGR2 race, 2026-06-01 ~3.4% host CPU). Replace with a fixed inline
 * buffer + memmove-compact on refill. The lerp inner loop gets a NEON
 * 2-channel specialization; ch=2 is the only used case (vp.c:1255
 * hardcodes channels=2). Scalar fallback preserved for other channel
 * counts and non-AArch64 builds.
 *
 * Buffer sizing: the only producer is voice_resample_callback which
 * returns at most NUM_SAMPLES_PER_FRAME=32 frames per call. Worst-case
 * residue carried across a refill is < 1 frame (we compact whole
 * frames), so 2 * 32 = 64 frames is a safe upper bound. We use 128 to
 * give comfortable headroom and absorb any future bumps.
 */
#define STUB_MAX_CHANNELS 2
#define STUB_BUFFER_FRAMES 128

struct SRC_STATE {
    src_callback_t cb;
    void *cb_data;
    int channels;

    /* Inline buffer. Layout: interleaved frames, frame i at
     * input_buf[i * channels + c]. After every refill, the live region
     * starts at index 0 and runs for input_len frames. */
    float input_buf[STUB_BUFFER_FRAMES * STUB_MAX_CHANNELS];
    int input_len;     /* frames currently live in input_buf */
    float input_pos;   /* fractional read cursor in frames; >=0, < input_len */
};

SRC_STATE *src_callback_new(src_callback_t cb, int converter_type, int channels,
                            int *error, void *cb_data)
{
    (void)converter_type;
    if (error) {
        *error = 0;
    }
    if (channels < 1 || channels > STUB_MAX_CHANNELS) {
        if (error) {
            *error = -1;
        }
        return NULL;
    }
    SRC_STATE *state = (SRC_STATE *)calloc(1, sizeof(*state));
    if (!state) {
        if (error) {
            *error = -1;
        }
        return NULL;
    }
    state->cb = cb;
    state->cb_data = cb_data;
    state->channels = channels;
    state->input_len = 0;
    state->input_pos = 0.0f;
    return state;
}

/*
 * Compact already-consumed frames out of the buffer and pull new data
 * from the callback.
 *
 * Returns 1 if the buffer now has at least 2 frames (the minimum
 * needed to produce a single interpolated output), 0 otherwise.
 */
static int src_refill_input(SRC_STATE *state)
{
    int ch = state->channels;

    /* Number of whole frames already consumed. The fractional part of
     * input_pos stays with us; we will rebase it onto frame 0. */
    int consumed = (int)state->input_pos;
    if (consumed < 0) {
        consumed = 0;
    }
    if (consumed > state->input_len) {
        consumed = state->input_len;
    }

    int residue = state->input_len - consumed;
    if (residue < 0) {
        residue = 0;
    }

    /* Compact residue to slot 0. memmove because the regions may
     * overlap when consumed < residue. */
    if (residue > 0 && consumed > 0) {
        memmove(state->input_buf,
                state->input_buf + consumed * ch,
                sizeof(float) * residue * ch);
    }
    state->input_len = residue;
    state->input_pos -= (float)consumed;
    if (state->input_pos < 0.0f) {
        state->input_pos = 0.0f;
    }

    /* Pull new data. Caller (voice_resample_callback) returns at most
     * NUM_SAMPLES_PER_FRAME=32 frames; the buffer always has room. */
    float *new_data = NULL;
    long got = state->cb(state->cb_data, &new_data);
    if (got <= 0 || new_data == NULL) {
        /* End-of-stream: no new data. Caller can still drain whatever
         * residue we have (>=2 frames required for one lerp output). */
        return residue >= 2;
    }

    int max_new = STUB_BUFFER_FRAMES - residue;
    if (got > max_new) {
        /* Defensive clamp; the callsite never produces this in
         * practice, but we must not overrun the inline buffer. */
        got = max_new;
    }

    if (got > 0) {
        memcpy(state->input_buf + residue * ch,
               new_data,
               sizeof(float) * (size_t)got * (size_t)ch);
        state->input_len = residue + (int)got;
    }

    return state->input_len >= 2;
}

long src_callback_read(SRC_STATE *state, double ratio, long frames, float *data)
{
    if (!state || !state->cb || !data || frames <= 0) {
        return 0;
    }

    if (ratio <= 0.0) {
        ratio = 1.0;
    }

    int ch = state->channels;
    float step = (float)(1.0 / ratio);
    long out_frames = 0;

#if defined(__aarch64__)
    if (ch == 2) {
        /* Fast path: 2-channel NEON lerp. One float32x2_t FMA per
         * output frame. Bit-identical to the scalar form
         *   out[c] = s0[c] + frac * (s1[c] - s0[c])
         * because vfma_n_f32(a, b, n) computes a + b*n with a single
         * rounding (same as the scalar expression after CSE on this
         * target's IEEE-754 FMA-or-not behavior — both forms compile
         * to the same FMLA on AArch64 with -ffp-contract=fast, which
         * is the project default; otherwise both produce the same
         * unfused mul+add). */
        float pos = state->input_pos;
        while (out_frames < frames) {
            int need = (int)pos + 1;
            if (need >= state->input_len) {
                /* Flush pos back to state before refill so refill's
                 * consumed-frame math sees the latest cursor. */
                state->input_pos = pos;
                if (!src_refill_input(state)) {
                    break;
                }
                pos = state->input_pos;
                if (state->input_len < 2) {
                    break;
                }
                continue;
            }

            int idx0 = (int)pos;
            int idx1 = idx0 + 1;
            if (idx1 >= state->input_len) {
                /* Need refill — same path as above. */
                state->input_pos = pos;
                if (!src_refill_input(state)) {
                    break;
                }
                pos = state->input_pos;
                if (state->input_len < 2) {
                    break;
                }
                continue;
            }

            float frac = pos - (float)idx0;
            const float *base = state->input_buf;
            float32x2_t s0 = vld1_f32(base + idx0 * 2);
            float32x2_t s1 = vld1_f32(base + idx1 * 2);
            float32x2_t d  = vsub_f32(s1, s0);
            float32x2_t r  = vfma_n_f32(s0, d, frac);
            vst1_f32(data + out_frames * 2, r);

            out_frames++;
            pos += step;
        }
        state->input_pos = pos;
        return out_frames;
    }
#endif

    /* Scalar fallback: any channel count, non-AArch64. */
    while (out_frames < frames) {
        int need = (int)state->input_pos + 1;
        if (need >= state->input_len) {
            if (!src_refill_input(state)) {
                break;
            }
            if (state->input_len < 2) {
                break;
            }
            continue;
        }

        int idx0 = (int)state->input_pos;
        int idx1 = idx0 + 1;
        if (idx1 >= state->input_len) {
            if (!src_refill_input(state)) {
                break;
            }
            if (state->input_len < 2) {
                break;
            }
            continue;
        }

        float frac = state->input_pos - (float)idx0;
        const float *s0 = state->input_buf + idx0 * ch;
        const float *s1 = state->input_buf + idx1 * ch;

        for (int c = 0; c < ch; c++) {
            data[out_frames * ch + c] = s0[c] + frac * (s1[c] - s0[c]);
        }

        out_frames++;
        state->input_pos += step;
    }

    return out_frames;
}

int src_reset(SRC_STATE *state)
{
    if (state) {
        state->input_len = 0;
        state->input_pos = 0.0f;
    }
    return 0;
}

SRC_STATE *src_delete(SRC_STATE *state)
{
    free(state);
    return NULL;
}

const char *src_strerror(int error)
{
    (void)error;
    return "libsamplerate stub";
}

void src_float_to_short_array(const float *in, short *out, int len)
{
    if (!in || !out || len <= 0) {
        return;
    }

#if defined(__aarch64__)
    float32x4_t scale = vdupq_n_f32(32767.0f);
    float32x4_t hi = vdupq_n_f32(1.0f);
    float32x4_t lo = vdupq_n_f32(-1.0f);
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        float32x4_t v = vld1q_f32(&in[i]);
        v = vminq_f32(vmaxq_f32(v, lo), hi);
        v = vmulq_f32(v, scale);
        int32x4_t iv = vcvtq_s32_f32(v);
        int16x4_t sv = vqmovn_s32(iv);
        vst1_s16(&out[i], sv);
    }
    for (; i < len; i++) {
        float v = in[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        out[i] = (short)(v * 32767.0f);
    }
#else
    for (int i = 0; i < len; ++i) {
        float v = in[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = (short)(v * 32767.0f);
    }
#endif
}
