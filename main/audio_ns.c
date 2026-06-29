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
 * ── Spectral Noise Suppressor ──
 *
 * Implements per-bin minimum-statistics noise estimation with
 * parametric Wiener spectral subtraction.  Uses a self-contained
 * 256-point radix-2 FFT (no external DSP library).
 *
 * Algorithm per frame:
 *   1. Copy 160 PCM samples → 256-point float buffer (zero-padded)
 *   2. Forward FFT
 *   3. For each frequency bin k = 0..128:
 *      a. power[k] = re² + im²
 *      b. Update noise estimate via minimum-statistics tracking
 *      c. Wiener gain: G = P / (P + α·N), floored to G_min
 *      d. Apply gain, mirror to conjugate bin
 *   4. Inverse FFT
 *   5. Copy float → int16
 *
 * Noise tracking uses a fast-attack / slow-release scheme:
 *   - If power < noise_est: noise_est = power        (instant track-down)
 *   - Else: noise_est += 0.001 × (power - noise_est)  (20 s upward drift)
 * This ensures the estimate follows the true noise floor while
 * being robust to short speech bursts.
 */

#include "audio_ns.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "audio_ns";

/* ── Configuration ── */
#define FFT_SIZE          256                       /* Radix-2 FFT length */
#define FFT_HALF          (FFT_SIZE / 2)            /* 128 */
#define UNIQUE_BINS       (FFT_HALF + 1)            /* 129 (0..128 inclusive) */
#define MAX_INPUT_SAMPLES 160                       /* 20 ms at 8 kHz */

/* Spectral subtraction parameters (tune for quality vs aggressiveness) */
#define OVERSUB_ALPHA     4.0f                      /* Over-subtraction factor (aggressive) */
#define GAIN_FLOOR        0.06f                     /* Min per-bin gain (−24 dB) */
#define NOISE_LEAK_UP     0.001f                    /* Noise est. upward drift rate */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ── Precomputed tables (built in audio_ns_init) ── */
static float s_twiddle_cos[FFT_HALF];
static float s_twiddle_sin[FFT_HALF];

/* ── Per-bin state ── */
static float s_noise_pwr[UNIQUE_BINS];

/* ── FFT workspace ── */
static float s_re[FFT_SIZE];
static float s_im[FFT_SIZE];

/* ── 256-point in-place radix-2 DIT FFT ── */
static void fft256(float *re, float *im)
{
    /* Bit-reversal permutation */
    for (int i = 1, j = 0; i < FFT_SIZE; i++) {
        int bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    /* Cooley-Tukey butterflies */
    for (int len = 2; len <= FFT_SIZE; len <<= 1) {
        int half  = len >> 1;
        int step  = FFT_SIZE / len;
        for (int i = 0; i < FFT_SIZE; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = s_twiddle_cos[j * step];
                float wi = s_twiddle_sin[j * step];
                int a = i + j;
                int b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

/* ── Inverse FFT via conjugate method ── */
static void ifft256(float *re, float *im)
{
    for (int i = 0; i < FFT_SIZE; i++)
        im[i] = -im[i];

    fft256(re, im);

    float scale = 1.0f / FFT_SIZE;
    for (int i = 0; i < FFT_SIZE; i++) {
        re[i] *= scale;
        im[i] = -im[i] * scale;
    }
}

/* ── Public API ── */

void audio_ns_init(void)
{
    for (int i = 0; i < FFT_HALF; i++) {
        float angle = -2.0f * (float)M_PI * i / FFT_SIZE;
        s_twiddle_cos[i] = cosf(angle);
        s_twiddle_sin[i] = sinf(angle);
    }

    /* Initialize noise estimates to "infinity" so first frame sets them */
    for (int i = 0; i < UNIQUE_BINS; i++)
        s_noise_pwr[i] = 1e20f;

    ESP_LOGI(TAG, "Spectral noise suppressor ready (FFT=%d, alpha=%.1f, floor=%.2f)",
             FFT_SIZE, OVERSUB_ALPHA, GAIN_FLOOR);
}

void audio_ns_process(int16_t *samples, size_t num_samples)
{
    if (num_samples > MAX_INPUT_SAMPLES)
        num_samples = MAX_INPUT_SAMPLES;

    /* 1. Copy int16 → float, zero-pad to FFT_SIZE */
    size_t i;
    for (i = 0; i < num_samples; i++)
        s_re[i] = (float)samples[i];
    for (; i < (size_t)FFT_SIZE; i++)
        s_re[i] = 0.0f;
    memset(s_im, 0, sizeof(s_im));

    /* 2. Forward FFT */
    fft256(s_re, s_im);

    /* 3. Per-bin spectral subtraction */
    for (int k = 0; k < UNIQUE_BINS; k++) {
        float re_k = s_re[k];
        float im_k = s_im[k];
        float power = re_k * re_k + im_k * im_k;

        /* Minimum-statistics noise tracking */
        float noise = s_noise_pwr[k];
        if (power < noise) {
            noise = power;                         /* Fast track-down */
        } else {
            noise += NOISE_LEAK_UP * (power - noise); /* Slow drift up */
        }
        s_noise_pwr[k] = noise;

        /* Wiener gain: G = P / (P + α·N) */
        noise += 1e-10f;  /* Prevent division by zero */
        float gain = power / (power + OVERSUB_ALPHA * noise);
        if (gain < GAIN_FLOOR)
            gain = GAIN_FLOOR;

        /* Apply gain */
        s_re[k] = re_k * gain;
        s_im[k] = im_k * gain;

        /* Mirror to conjugate bin for real-signal IFFT */
        if (k > 0 && k < FFT_HALF) {
            int m = FFT_SIZE - k;
            s_re[m] = s_re[k];    /* Real part symmetric */
            s_im[m] = -s_im[k];   /* Imag part antisymmetric */
        }
    }

    /* 4. Inverse FFT */
    ifft256(s_re, s_im);

    /* 5. Copy float → int16 */
    for (i = 0; i < num_samples; i++) {
        float v = s_re[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        samples[i] = (int16_t)v;
    }
}
