/*
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ── G.711 μ-law codec ──
 *
 * Based on the well-known Sun Microsystems reference implementation.
 * Pure integer / lookup — no dynamic allocation.
 */

#include "g711_codec.h"

/* Maximum magnitude representable in 14-bit dynamic range */
#define CLIP    32635

/* Segment boundary lookup table (14-bit dynamic range) */
static const int16_t exp_lut[8] = {
    0, 132, 396, 924, 1980, 4092, 8316, 16764
};

int16_t linear16_to_ulaw(int16_t pcm_val)
{
    int sign, exponent, mantissa;
    int16_t ulaw;

    /* Convert to 14-bit dynamic range */
    pcm_val = pcm_val >> 2;

    /* Extract sign */
    sign = (pcm_val < 0) ? 1 : 0;
    if (sign) {
        pcm_val = -pcm_val;
    }

    /* Clip to maximum representable magnitude */
    if (pcm_val > CLIP) {
        pcm_val = CLIP;
    }

    /* Find segment (exponent) */
    for (exponent = 7; exponent > 0; exponent--) {
        if (pcm_val >= exp_lut[exponent]) {
            break;
        }
    }

    /* Quantization mantissa */
    mantissa = (pcm_val >> (exponent + 3)) & 0x0F;

    /*
     * Compress: sign(1) + exponent(3) + mantissa(4) = 8 bits
     * Then bit-complement (all bits toggled) per μ-law spec.
     */
    ulaw = ~((int16_t)(sign << 7 | (exponent << 4) | mantissa));
    return ulaw & 0xFF;
}

int16_t ulaw_to_linear16(uint8_t ulaw_val)
{
    int sign, exponent, mantissa;
    int16_t pcm_val;

    /* Bit-complement restores sign+exponent+mantissa */
    ulaw_val = ~ulaw_val;
    sign = (ulaw_val & 0x80) ? -1 : 1;
    exponent = (ulaw_val >> 4) & 0x07;
    mantissa = ulaw_val & 0x0F;

    /* Expand: segment base + (mantissa << (exponent + 3)) */
    pcm_val = exp_lut[exponent] + (mantissa << (exponent + 3));

    /* Convert back to 16-bit range (rescale from 14-bit) */
    return sign * (pcm_val << 2);
}
