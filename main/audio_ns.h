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
 * Per-bin minimum-statistics noise estimation with Wiener-style
 * spectral subtraction.  Self-contained 256-point radix-2 FFT —
 * no external DSP library dependency.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize the noise suppressor (twiddle tables, state).
 *        Call once before audio_ns_process().
 */
void audio_ns_init(void);

/**
 * @brief Suppress stationary noise in a frame of PCM samples (in-place).
 *
 * @param samples     Input/output buffer of 16-bit signed PCM samples.
 * @param num_samples Number of valid samples (typically 160 for 20 ms @ 8 kHz).
 *                    Zero-padded internally to 256 for FFT processing.
 */
void audio_ns_process(int16_t *samples, size_t num_samples);
