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
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── G.711 constants ── */
#define G711_SAMPLE_RATE        8000    /* 8 kHz telephony quality */
#define G711_FRAME_DURATION_MS  20      /* 20 ms per frame */
#define G711_FRAME_SIZE         (G711_SAMPLE_RATE * G711_FRAME_DURATION_MS / 1000)  /* 160 samples */

/* ── Audio frame ── */
typedef struct {
    uint8_t  *data;           /* G.711 μ-law encoded audio data */
    size_t    len;            /* Number of bytes (typically G711_FRAME_SIZE=160) */
    uint64_t  timestamp_us;   /* Capture timestamp in microseconds */
} audio_frame_t;

/* ── Audio subscriber types ── */
typedef enum {
    AUDIO_SUB_RTSP,    /* RTSP / streaming consumer */
    AUDIO_SUB_AVI_MUX, /* AVI muxer consumer */
} audio_sub_type_t;

/* ── Audio log event constants ── */
#define LOG_EVENT_AUDIO_STREAM_STARTED  "audio_stream_started"
#define LOG_EVENT_AUDIO_STREAM_STOPPED  "audio_stream_stopped"
#define LOG_EVENT_AUDIO_FRAME_DROP      "audio_frame_drop"

#ifdef __cplusplus
}
#endif
