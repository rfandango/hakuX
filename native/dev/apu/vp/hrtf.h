/*
 * HRTF Filter
 *
 * Copyright (c) 2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
/* hakuX native core: the same filter (31-tap HRIR convolution per channel,
 * an interaural delay with linear interpolation, hardware 32-sample
 * parameter ramps), laid out for NEON. The delay line holds its samples twice, so
 * any delay's window is contiguous; the coefficients are stored reversed
 * and padded to 32 with a zero tap, so the convolution is a dot product of
 * two contiguous arrays. Its sums are associated differently from the
 * scalar loop's (rounding-level differences: tests/hle/test_apu_hrtf.c). */
#ifndef HW_XBOX_MCPX_HRTF_H
#define HW_XBOX_MCPX_HRTF_H

#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "native/dev/apu/apu_regs.h"
#define HRTF_SAMPLES_PER_FRAME  NUM_SAMPLES_PER_FRAME
#define HRTF_NUM_TAPS           31
#define HRTF_TAPS_PADDED        32
#define HRTF_MAX_DELAY_SAMPLES  42
#define HRTF_BUFLEN             (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)
typedef struct {
    int buf_pos;
    struct {
        /* the last HRTF_BUFLEN samples, each at i and i + HRTF_BUFLEN; the
         * padded tap reads one past the second copy */
        float buf[2 * HRTF_BUFLEN + 2];
        /* tap k at HRTF_NUM_TAPS - 1 - k; the last (padding) is 0 */
        float hrir_coeff_cur[HRTF_TAPS_PADDED];
        float hrir_coeff_tar[HRTF_TAPS_PADDED];
    } ch[2];
    float itd_cur;
    float itd_tar;
    bool target_initialised; /* first frame uses exact entry (no HRTF fade) */
} HrtfFilter;
static inline void hrtf_filter_init(HrtfFilter *f)
{
    memset(f, 0, sizeof(*f));
}

static inline void
hrtf_filter_set_target_params(HrtfFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS],
                              float itd)
{
    f->itd_tar =
        fmaxf(-HRTF_MAX_DELAY_SAMPLES, fminf(itd, HRTF_MAX_DELAY_SAMPLES));

    for (int ch = 0; ch < 2; ch++) {
        const float *coeff = hrir_coeff[ch];
        /* Preserve raw decoded HRIR gain: retail Xbox hardware does not
         * normalize taps by their absolute sum. In particular, a single
         * 0x40 coefficient must retain 1/16 gain, not become 1. Keep the
         * reversed tap order and zero padding required by the NEON path.
         * Source: raddad772, xemu-project/xemu#3105, commit 03ae86d:
         * https://github.com/xemu-project/xemu/pull/3105
         */
        float *tar = f->ch[ch].hrir_coeff_tar;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            tar[HRTF_NUM_TAPS - 1 - k] = coeff[k];
        }
        tar[HRTF_NUM_TAPS] = 0.0f;
    }
    /* On a newly started voice, the Xbox uses the programmed taps on its
     * FIRST frame; only later target changes use the 32-sample transition.
     * Source: raddad772 real Xbox tests, xemu PR #3105, 21db778. */
    if (!f->target_initialised) {
        f->target_initialised = true;
        f->itd_cur = f->itd_tar;
        for (int ch = 0; ch < 2; ch++) {
            memcpy(f->ch[ch].hrir_coeff_cur, f->ch[ch].hrir_coeff_tar,
                   sizeof(f->ch[ch].hrir_coeff_cur));
        }
    }
}
/* Each target transition spans one 32-sample VP frame. A new target
 * starts from the previous frame's final values, rather than asymptotically
 * approaching it via the old 0.01 exponential smoothing coefficient.
 * Source: raddad772 retail MCPX tests, xemu PR #3105 21db778.
 */
/* The parameters at their targets: a step leaves them as they are. */
static inline bool hrtf_filter_settled(const HrtfFilter *f)
{
    if (f->itd_cur != f->itd_tar) {
        return false;
    }
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < HRTF_TAPS_PADDED; k++) {
            if (f->ch[ch].hrir_coeff_cur[k] != f->ch[ch].hrir_coeff_tar[k]) {
                return false;
            }
        }
    }
    return true;
}
/* sum of c[m] * w[m] over the padded taps. With tar, each c[m] is first
 * incremented by step[m] (the frame-linear parameter ramp) and stored;
 * with dfrac, each w[m] is first interpolated toward w[m - 1] (the
 * sample one further back). */
static inline float hrtf_dot(float *c, const float *step, const float *w, float dfrac)
{
    const bool interp = dfrac > 0.0f;
    const float w0 = 1 - dfrac;
#if defined(__aarch64__)
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
    for (int m = 0; m < HRTF_TAPS_PADDED; m += 8) {
        float32x4_t c0 = vld1q_f32(c + m), c1 = vld1q_f32(c + m + 4);
        if (step) {
            c0 = vaddq_f32(c0, vld1q_f32(step + m));
            c1 = vaddq_f32(c1, vld1q_f32(step + m + 4));
            vst1q_f32(c + m, c0);
            vst1q_f32(c + m + 4, c1);
        }
        float32x4_t s0 = vld1q_f32(w + m), s1 = vld1q_f32(w + m + 4);
        if (interp) {
            s0 = vfmaq_n_f32(vmulq_n_f32(vld1q_f32(w + m - 1), dfrac), s0, w0);
            s1 = vfmaq_n_f32(vmulq_n_f32(w + m + 3), dfrac, s1, w0);
        }
        a0 = vfmaq_f32(a0, c0, s0);
        a1 = vfmaq_f32(a1, c1, s1);
    }
    return vaddvq_f32(vaddq_f32(a0, a1));
#else
    float acc = 0.0f;
    for (int m = 0; m < HRTF_TAPS_PADDED; m++) {
        if (step) {
            c[m] += step[m];
        }
        float s = w[m];
        if (interp) {
            s = s * w0 + w[m - 1] * dfrac;
        }
        acc += c[m] * s;
    }
    return acc;
#endif
}
static inline void hrtf_filter_process(HrtfFilter *f,
                                       float in[HRTF_SAMPLES_PER_FRAME][2],
                                       float out[HRTF_SAMPLES_PER_FRAME][2])
{
    const bool settled = hrtf_filter_settled(f);
    const float itd_step = (f->itd_tar - f->itd_cur) /
                           (float)HRTF_SAMPLES_PER_FRAME;
    float coeff_step[2][HRTF_TAPS_PADDED];
    if (!settled) {
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < HRTF_TAPS_PADDED; k++) {
                coeff_step[ch][k] =
                    (f->ch[ch].hrir_coeff_tar[k] - f->ch[ch].hrir_coeff_cur[k]) /
                    (float)HRTF_SAMPLES_PER_FRAME;
            }
        }
    }
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        if (!settled) {
            f->itd_cur += itd_step;
        }
        const int pos = f->buf_pos;
        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;

            // Push new sample
            buf[pos] = in[n][ch];
            buf[pos + HRTF_BUFLEN] = in[n][ch];

            // Interaural time difference (channel delay)
            float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) {
                d = 0.0f;
            }
            int di = d;
            float dfrac = d - di;
            // HRIR Convolution: tap k reads the sample di + k back,
            // buf[pos + HRTF_BUFLEN - di - k]; reversed, from tap 30 up
            const float *w = &buf[pos + HRTF_BUFLEN - di - (HRTF_NUM_TAPS - 1)];
            out[n][ch] = hrtf_dot(f->ch[ch].hrir_coeff_cur,
                                  settled ? NULL : coeff_step[ch], w, dfrac);
        }

        f->buf_pos = (pos + 1) % HRTF_BUFLEN;
    }
    /* Snap after the final sample to avoid float error accumulating between
     * frames and to keep the settled fast path active thereafter. */
    if (!settled) {
        f->itd_cur = f->itd_tar;
        for (int ch = 0; ch < 2; ch++) {
            memcpy(f->ch[ch].hrir_coeff_cur, f->ch[ch].hrir_coeff_tar,
                   sizeof(f->ch[ch].hrir_coeff_cur));
        }
    }
}
#endif
