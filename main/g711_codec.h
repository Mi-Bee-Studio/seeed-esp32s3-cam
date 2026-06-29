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
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode one 16-bit linear PCM sample to 8-bit G.711 μ-law.
 * @param pcm_sample Signed 16-bit PCM sample
 * @return 8-bit μ-law encoded byte (zero-extended to int16_t)
 */
int16_t linear16_to_ulaw(int16_t pcm_sample);

/**
 * @brief Decode one 8-bit G.711 μ-law byte to 16-bit linear PCM.
 * @param ulaw_byte 8-bit μ-law encoded byte
 * @return Signed 16-bit PCM sample
 */
int16_t ulaw_to_linear16(uint8_t ulaw_byte);

#ifdef __cplusplus
}
#endif
