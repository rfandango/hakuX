/*
 * Helper FP conversions
 *
 * Copyright (c) 2020-2025 Matt Borgerson
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


#ifndef FLOATCONV_H
#define FLOATCONV_H

#include <stdint.h>
#include <math.h>

static inline float int8_to_float(int8_t x)
{
    return x / 128.0f;
}

/* hakuX: these scale by powers of two, so the float forms give the double
 * division's result bit for bit (the voices' per-sample decode did each in
 * double). */
static inline float uint8_to_float(uint8_t value)
{
    return (float)((int)value - 0x80) * (1.0f / 0x80);
}

static inline float int16_to_float(int16_t value)
{
    return (float)value * (1.0f / 0x8000);
}

static inline float s6p9_to_float(int16_t value)
{
    return value / 512.0f;
}

static inline float int32_to_float(int32_t value)
{
    return (float)value * (1.0f / 0x80000000u);   /* rounding commutes with the power of two */
}

static inline float int24_to_float(int32_t value)
{
    return int32_to_float((uint32_t)value << 8);
}

static inline uint32_t float_to_24b(float value)
{
    double scaled_value = value * (8.0 * 0x100000);
    int int24;
    if (scaled_value >= (1.0 * 0x7fffff)) {
        int24 = 0x7fffff;
    } else if (scaled_value <= (-8.0 * 0x100000)) {
        int24 = -1 - 0x7fffff;
    } else {
        /* MCPX rounds 24-bit mixbin writes with halves UP (towards +inf),
         * not the host FPU's ties-to-even lrint behaviour.
         * Source: raddad772 Xbox APU tests, xemu PR #3105 45af4ac. */
        int24 = (int)floor(scaled_value + 0.5);
    }
    return int24 & 0xffffff;
}

#endif
