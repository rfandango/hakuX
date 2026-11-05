/*
 * Adapted from SWH LADSPA Plugins package, modified for xemu
 *
 * Source:  https://github.com/swh/ladspa/blob/master/svf_1214.xml
 * Author:  Steve Harris, andy@vellocet
 * License: GPLv2
 *
 */

#ifndef SVF_H
#define SVF_H

#include <math.h>
#include "mcpx_round.h"

#define flush_to_zero(x) x

// Constants to match filter types
#define F_LP 1
#define F_HP 2
#define F_BP 3
#define F_BR 4
#define F_AP 5

// Number of filter oversamples
#define F_R 1

/* Structure to hold parameters for SV filter */

typedef struct {
    float f;     // 2.0*sin(PI*fs/(fc*r));
    float q;     // 2.0*cos(pow(q, 0.1)*PI*0.5);
    double h;    // high pass output (1/16 PCM-step precision)
    double b;    // band pass output
    double l;    // low pass output
    double p;    // peaking output (allpass with resonance)
    double n;    // notch output
    double *op;  // pointer to output value
} sv_filter;

/* Store data in SVF struct, takes the sampling frequency, cutoff frequency
   and Q, and fills in the structure passed */
/*
static inline void setup_svf(sv_filter *sv, float fs, float fc, float q, int t) {
    sv->f = 2.0f * sin(M_PI * fc / (float)(fs * F_R));
    sv->q = 2.0f * cos(pow(q, 0.1f) * M_PI * 0.5f);
*/
static inline void setup_svf(sv_filter *sv, float fc, float q, int t) {
    sv->f = fc;
    sv->q = q;
    switch(t) {
    case F_LP:
        sv->op = &(sv->l);
        break;
    case F_HP:
        sv->op = &(sv->h);
        break;
    case F_BP:
        sv->op = &(sv->b);
        break;
    case F_BR:
        sv->op = &(sv->n);
        break;
    default:
        sv->op = &(sv->p);
    }
}

/* Run one sample through the SV filter. Filter is by andy@vellocet */
/* MCPX rounds each state-variable filter operation to 1/16 of a 24-bit
 * sample (halves upward). Preserve the intermediate precision in double.
 * Tested on retail Xbox by raddad772, xemu PR #3105 commit 06ef59e. */
static inline float run_svf(sv_filter *sv, float in)
{
    double high = mcpx_round_filter((double)in - sv->l - sv->q * sv->b);
    double band = mcpx_round_filter(sv->b + sv->f * high);
    double low  = mcpx_round_filter(sv->l + sv->f * band);
    sv->h = high; sv->b = band; sv->l = low;
    sv->n = low + high; sv->p = low - high;
    return (float)*(sv->op);
}

/* Same SVF with peaking output rounded to the hardware 1/16-PCM step.
 * Source: raddad772, xemu PR #3105 commits fbe7463, 06ef59e. */
static inline float run_para_eq(sv_filter *sv, float in, float gain)
{
    double high = mcpx_round_filter((double)in - sv->l - sv->q * sv->b);
    double band = mcpx_round_filter(sv->b + sv->f * high);
    double low  = mcpx_round_filter(sv->l + sv->f * band);
    sv->h = high; sv->b = band; sv->l = low;
    return (float)mcpx_round_filter((double)in + gain * sv->q * band);
}

#endif
