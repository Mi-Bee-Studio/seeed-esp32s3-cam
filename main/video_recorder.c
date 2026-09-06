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

#include "video_recorder.h"
#include "camera_driver.h"
#include "storage_manager.h"
#include "frame_broadcaster.h"
#include "config_manager.h"
#include "time_sync.h"
#include "status_led.h"
#include "logging.h"
#include "motion_detect.h"
#include "audio_broadcaster.h"
#include "audio_common.h"
#include "ws_server.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>   /* fileno(), fsync() */
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include "sha256.h"

/* Forward declarations */
static void cleanup_orphan_zero_byte_files(void);
static esp_err_t spawn_capture_tasks_locked(void);
static void recorder_full_stop(bool emit_events);

/* ------------------------------------------------------------------ */
/*  AVI binary helpers                                                */
/* ------------------------------------------------------------------ */

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                            ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define AVIIF_KEYFRAME 0x10

/* avih dwFlags: has index */
#define AVIF_HASINDEX 0x10

/* ------------------------------------------------------------------ */
/*  AVI sizes                                                         */
/* ------------------------------------------------------------------ */

#define AVI_RIFF_HDR_SIZE  12   /* "RIFF" + size + "AVI "             */
#define AVI_AVIH_SIZE      64   /* "avih" tag(4) + size(4) + data(56) */
#define AVI_STRH_SIZE      64   /* "strh" tag(4) + size(4) + data(56) */
#define AVI_STRF_SIZE      48   /* "strf" tag(4) + size(4) + data(40) */

/* hdrl LIST = 12(LIST+size+"hdrl") + AVI_AVIH_SIZE + 12(LIST+size+"strl") + AVI_STRH_SIZE + AVI_STRF_SIZE */
#define AVI_HDRL_TOTAL     (12 + AVI_AVIH_SIZE + 12 + AVI_STRH_SIZE + AVI_STRF_SIZE) /* 236 */

#define AVI_FRAME_HDR_SIZE 8    /* "00dc" + 4-byte length              */

#define AVI_WAVEFMT_SIZE    18   /* WAVEFORMATEX: 18 bytes for G.711 μ-law    */
#define AVI_AUDIO_STRL_SIZE 102  /* audio strl: LIST(12) + strh(64) + strf(26) */
#define AVI_HDRL_WITH_AUDIO (AVI_HDRL_TOTAL + AVI_AUDIO_STRL_SIZE)           /* 302 */

/* ------------------------------------------------------------------ */
/*  Private state                                                     */
/* ------------------------------------------------------------------ */

/* Maximum idx1 index entries to prevent unbounded realloc growth */
#define MAX_IDX1_ENTRIES  100000

static const char *TAG = "recorder";

static recorder_state_t   s_state        = RECORDER_IDLE;
/* PIT-020 预览引用计数：MJPEG/RTSP 客户端接入 +1、断开 -1；
 * >0 时采集任务不得退出（否则流死到下次重启） */
static volatile uint8_t   s_preview_refs = 0;
static TaskHandle_t       s_task_handle  = NULL;
static char               s_current_file[128] = {0};
static recorder_segment_cb_t s_segment_cb = NULL;
static SemaphoreHandle_t  s_mutex        = NULL;
static uint32_t           s_stack_hwm    = 0;   /* Stack high-water mark */
static uint32_t          s_frames_dropped = 0;
static int64_t           s_last_drop_log_us = 0; /* throttle drop log */

typedef struct {
    frame_buf_t *fb;       /* refcounted JPEG buffer; NULL = sentinel */
    int64_t capture_us;    /* capture timestamp for AV sync */
} write_msg_t;

static QueueHandle_t s_write_q = NULL;
static TaskHandle_t s_writer_handle = NULL;

/* Resolution → pixel dimensions（家族刻度 framesize_t：10=VGA … 15=UXGA，
 * 契约 v1.3 §5；与 camera_driver.c res_to_framesize 恒等映射同源） */
/**
 * @brief 根据分辨率编号获取对应的像素宽度和高度
 */
static void resolution_dims(uint8_t res, uint16_t *w, uint16_t *h)
{
    switch (res) {
        case 10:  *w = 640;  *h = 480;  break;   /* VGA  */
        case 11:  *w = 800;  *h = 600;  break;   /* SVGA */
        case 12:  *w = 1024; *h = 768;  break;   /* XGA  */
        case 13:  *w = 1280; *h = 720;  break;   /* HD   */
        case 14:  *w = 1280; *h = 1024; break;   /* SXGA */
        case 15:  *w = 1600; *h = 1200; break;   /* UXGA */
        default:  *w = 800;  *h = 600;  break;   /* SVGA（本板默认档） */
    }
}

/* ------------------------------------------------------------------ */
/*  Little-endian binary write helpers                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 向字节缓冲区写入16位小端无符号整数
 */
static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

/**
 * @brief 向字节缓冲区写入32位小端无符号整数
 */
static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ */
/*  idx1 dynamic index                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t fourcc;   /* chunk FOURCC: '00dc' or '01wb' */
    uint32_t offset;   /* offset from start of 'movi' list data */
    uint32_t size;     /* data size (without header and padding) */
} idx1_entry_t;

typedef struct {
    idx1_entry_t *entries;
    int           count;
    int           capacity;
} idx1_t;

/**
 * @brief 初始化AVI帧索引结构体
 */
static void idx1_init(idx1_t *idx)
{
    idx->entries  = NULL;
    idx->count    = 0;
    idx->capacity = 0;
}

/**
 * @brief 向AVI帧索引追加一条记录，容量不足时自动扩展
 */
static esp_err_t idx1_append(idx1_t *idx, uint32_t fourcc, uint32_t offset, uint32_t size)
{
    if (idx->count >= idx->capacity) {
        int new_cap = idx->capacity == 0 ? 512 : idx->capacity * 2;
        if (new_cap > MAX_IDX1_ENTRIES) {
            ESP_LOGE(TAG, "idx1 overflow: entries would exceed %d", MAX_IDX1_ENTRIES);
            return ESP_ERR_NO_MEM;
        }
        /* Allocate idx1 in PSRAM: 100000 entries * 12B = 1.2MB, which far
         * exceeds internal RAM headroom and would fail/fragment the default
         * heap. PSRAM has 8MB; index access is sequential at segment close,
         * so the cache-miss penalty is negligible. */
        idx1_entry_t *new_buf = heap_caps_realloc(idx->entries,
                                                  (size_t)new_cap * sizeof(idx1_entry_t),
                                                  MALLOC_CAP_SPIRAM);
        if (!new_buf) {
            ESP_LOGE(TAG, "idx1 PSRAM realloc failed (%d entries)", new_cap);
            return ESP_ERR_NO_MEM;
        }
        idx->entries  = new_buf;
        idx->capacity = new_cap;
    }
    idx->entries[idx->count].fourcc = fourcc;
    idx->entries[idx->count].offset = offset;
    idx->entries[idx->count].size   = size;
    idx->count++;
    return ESP_OK;
}

/**
 * @brief 释放AVI帧索引占用的内存
 */
static void idx1_free(idx1_t *idx)
{
    free(idx->entries);
    idx->entries  = NULL;
    idx->count    = 0;
    idx->capacity = 0;
}

/* ------------------------------------------------------------------ */
/*  AVI header writing                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 写入AVI文件RIFF头（12字节），文件大小字段占位待关闭时回填
 */
static void write_riff_hdr(uint8_t *buf)
{
    memcpy(buf,      "RIFF", 4);
    put_u32(buf + 4, 0);          /* placeholder — patched at close */
    memcpy(buf + 8,  "AVI ", 4);
}

/**
 * @brief 写入AVI文件头列表（hdrl），包含主头部avih和视频流信息strl
 */
static void write_hdrl(uint8_t *buf, uint16_t w, uint16_t h, uint8_t playback_fps, bool with_audio)
{
    int pos = 0;

    /* LIST "hdrl" */
    memcpy(buf + pos, "LIST", 4);                         pos += 4;
    put_u32(buf + pos, AVI_HDRL_TOTAL - 8 + (with_audio ? AVI_AUDIO_STRL_SIZE : 0)); pos += 4;
    memcpy(buf + pos, "hdrl", 4);                         pos += 4;

    /* avih chunk */
    memcpy(buf + pos, "avih", 4);                         pos += 4;
    put_u32(buf + pos, 56);                               pos += 4;
    put_u32(buf + pos, 1000000 / playback_fps); /* dwMicroSecPerFrame */ pos += 4;
    put_u32(buf + pos, 0);              /* dwMaxBytesPerSec     */ pos += 4;
    put_u32(buf + pos, 0);              /* dwPaddingGranularity */ pos += 4;
    put_u32(buf + pos, AVIF_HASINDEX);  /* dwFlags              */ pos += 4;
    put_u32(buf + pos, 0);              /* dwTotalFrames        */ pos += 4;
    put_u32(buf + pos, 0);              /* dwInitialFrames      */ pos += 4;
    put_u32(buf + pos, with_audio ? 2 : 1);  /* dwStreams    */ pos += 4;
    put_u32(buf + pos, 0x100000);       /* dwSuggestedBufSize   */ pos += 4;
    put_u32(buf + pos, w);              /* dwWidth              */ pos += 4;
    put_u32(buf + pos, h);              /* dwHeight             */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[0]          */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[1]          */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[2]          */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[3]          */ pos += 4;

    /* LIST "strl" — video stream */
    memcpy(buf + pos, "LIST", 4);                         pos += 4;
    put_u32(buf + pos, AVI_STRH_SIZE + AVI_STRF_SIZE + 4); /* strl content size */ pos += 4;
    memcpy(buf + pos, "strl", 4);                         pos += 4;

    /* strh chunk */
    memcpy(buf + pos, "strh", 4);                         pos += 4;
    put_u32(buf + pos, 56);                               pos += 4;
    memcpy(buf + pos, "vids", 4);       /* fccType        */ pos += 4;
    memcpy(buf + pos, "MJPG", 4);       /* fccHandler     */ pos += 4;
    put_u32(buf + pos, 0);              /* dwFlags        */ pos += 4;
    put_u16(buf + pos, 0);              /* wPriority      */ pos += 2;
    put_u16(buf + pos, 0);              /* wLanguage      */ pos += 2;
    put_u32(buf + pos, 0);              /* dwInitialFrames */ pos += 4;
    put_u32(buf + pos, 1);              /* dwScale        */ pos += 4;
    put_u32(buf + pos, playback_fps);   /* dwRate         */ pos += 4;
    put_u32(buf + pos, 0);              /* dwStart        */ pos += 4;
    put_u32(buf + pos, 0);              /* dwLength       */ pos += 4;
    put_u32(buf + pos, 0x100000);       /* dwSuggestedBuf */ pos += 4;
    put_u32(buf + pos, 0xFFFFFFFF);     /* dwQuality      */ pos += 4;
    put_u32(buf + pos, 0);              /* dwSampleSize   */ pos += 4;
    put_u16(buf + pos, 0);              /* rcFrame.left   */ pos += 2;
    put_u16(buf + pos, 0);              /* rcFrame.top    */ pos += 2;
    put_u16(buf + pos, w);              /* rcFrame.right  */ pos += 2;
    put_u16(buf + pos, h);              /* rcFrame.bottom */ pos += 2;

    /* strf chunk — BITMAPINFOHEADER */
    memcpy(buf + pos, "strf", 4);                         pos += 4;
    put_u32(buf + pos, 40);             /* chunk data size */ pos += 4;
    put_u32(buf + pos, 40);             /* biSize         */ pos += 4;
    put_u32(buf + pos, w);              /* biWidth        */ pos += 4;
    put_u32(buf + pos, h);              /* biHeight       */ pos += 4;
    put_u16(buf + pos, 1);              /* biPlanes       */ pos += 2;
    put_u16(buf + pos, 24);             /* biBitCount     */ pos += 2;
    memcpy(buf + pos, "MJPG", 4);       /* biCompression  */ pos += 4;
    put_u32(buf + pos, (uint32_t)w * h * 3); /* biSizeImage */ pos += 4;
    put_u32(buf + pos, 0);              /* biXPelsPerMeter */ pos += 4;
    put_u32(buf + pos, 0);              /* biYPelsPerMeter */ pos += 4;
    put_u32(buf + pos, 0);              /* biClrUsed      */ pos += 4;
    put_u32(buf + pos, 0);              /* biClrImportant */ pos += 4;

    if (with_audio) {
        /* LIST "strl" — audio stream */
        memcpy(buf + pos, "LIST", 4);                         pos += 4;
        put_u32(buf + pos, AVI_STRH_SIZE + 8 + AVI_WAVEFMT_SIZE + 4); pos += 4;
        memcpy(buf + pos, "strl", 4);                         pos += 4;

        /* strh chunk — audio stream header */
        memcpy(buf + pos, "strh", 4);                         pos += 4;
        put_u32(buf + pos, 56);                               pos += 4;
        memcpy(buf + pos, "auds", 4);      /* fccType = audio */ pos += 4;
        put_u32(buf + pos, 0x0007);         /* fccHandler = WAVE_FORMAT_MULAW */ pos += 4;
        put_u32(buf + pos, 0);              /* dwFlags        */ pos += 4;
        put_u16(buf + pos, 0);              /* wPriority      */ pos += 2;
        put_u16(buf + pos, 0);              /* wLanguage      */ pos += 2;
        put_u32(buf + pos, 0);              /* dwInitialFrames */ pos += 4;
        put_u32(buf + pos, 1);              /* dwScale        */ pos += 4;
        put_u32(buf + pos, G711_SAMPLE_RATE); /* dwRate    */ pos += 4;
        put_u32(buf + pos, 0);              /* dwStart        */ pos += 4;
        put_u32(buf + pos, 0);              /* dwLength       */ pos += 4;
        put_u32(buf + pos, G711_FRAME_SIZE); /* dwSuggestedBuf */ pos += 4;
        put_u32(buf + pos, 0xFFFFFFFF);     /* dwQuality      */ pos += 4;
        put_u32(buf + pos, 1);              /* dwSampleSize   */ pos += 4;
        put_u16(buf + pos, 0);              /* rcFrame.left   */ pos += 2;
        put_u16(buf + pos, 0);              /* rcFrame.top    */ pos += 2;
        put_u16(buf + pos, 0);              /* rcFrame.right  */ pos += 2;
        put_u16(buf + pos, 0);              /* rcFrame.bottom */ pos += 2;

        /* strf chunk — WAVEFORMATEX */
        memcpy(buf + pos, "strf", 4);                         pos += 4;
        put_u32(buf + pos, AVI_WAVEFMT_SIZE);                 pos += 4;
        put_u16(buf + pos, 0x0007);         /* wFormatTag = WAVE_FORMAT_MULAW */ pos += 2;
        put_u16(buf + pos, 1);              /* nChannels = mono */ pos += 2;
        put_u32(buf + pos, G711_SAMPLE_RATE); /* nSamplesPerSec */ pos += 4;
        put_u32(buf + pos, G711_SAMPLE_RATE); /* nAvgBytesPerSec */ pos += 4;
        put_u16(buf + pos, 1);              /* nBlockAlign     */ pos += 2;
        put_u16(buf + pos, 8);              /* wBitsPerSample  */ pos += 2;
        put_u16(buf + pos, 0);              /* cbSize          */ pos += 2;
    }
}

/* ------------------------------------------------------------------ */
/*  Directory / filename helpers                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief 递归创建目录路径中所有不存在的子目录
 */
static int mkdirs(const char *path)
{
    char tmp[128];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);      /* ignore EEXIST */
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
    return 0;
}

/**
 * @brief 根据当前时间生成分段录像文件路径（/sdcard/recordings/YYYY-MM/DD/REC_YYYYMMDD_HHMMSS.avi）
 */
static void build_segment_path(char *out, size_t out_len, const char *prefix)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* Directory: /sdcard/recordings/YYYY-MM/DD/ */
    char dir[96];
    snprintf(dir, sizeof(dir), "/sdcard/recordings/%04d-%02d/%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    mkdirs(dir);

    /* Filename: REC_YYYYMMDD_HHMMSS.avi */
    snprintf(out, out_len, "%s/%s_%04d%02d%02d_%02d%02d%02d.avi",
      dir, prefix,
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ------------------------------------------------------------------ */
/*  Segment open / close / frame write                                */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE       *fp;
    idx1_t      idx;
    uint32_t    movi_data_size;   /* bytes written inside 'movi' data area  */
    uint32_t    frame_count;
    int64_t     start_ms;        /* segment start timestamp (esp_timer)     */
    audio_sub_t *audio_sub;       /* audio subscriber (NULL if mux disabled) */
    uint32_t    hdrl_size;        /* actual hdrl bytes written to file       */
    uint8_t    *stdio_buf;        /* 64KB setvbuf buffer (PSRAM) — cuts SD sector-write count */
    int         flush_counter;    /* periodic fflush/fsync counter, reset per segment */
} segment_t;

static segment_t s_seg = {0};

/**
 * @brief 获取当前系统时间戳（毫秒），用于计算分段已录制时长
 */
static int64_t s_seg_elapsed_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/**
 * @brief 打开新的录像分段文件，写入RIFF头、hdrl和movi列表头
 */
static esp_err_t open_segment(uint16_t w, uint16_t h, uint8_t playback_fps, const char *prefix)
{
    bool with_audio = config_get()->audio_record_to_sd;
    size_t hdrl_size = with_audio ? AVI_HDRL_WITH_AUDIO : AVI_HDRL_TOTAL;

    build_segment_path(s_current_file, sizeof(s_current_file), prefix);

    s_seg.fp = fopen(s_current_file, "wb");
    if (!s_seg.fp) {
        ESP_LOGE(TAG, "Failed to open %s", s_current_file);
        return ESP_FAIL;
    }

    /* Use a 64KB full-write buffer in PSRAM. Without this, fwrite defaults to
     * the VFS/FatFS sector size (512B), so a typical 50KB JPEG frame would
     * trigger ~100 SD sector writes. With a 64KB buffer, multiple frames
     * coalesce before hitting the SD bus, cutting write latency and SD GC
     * churn. Buffer must outlive the FILE — freed in close_segment. */
    s_seg.stdio_buf = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
    if (s_seg.stdio_buf) {
        if (setvbuf(s_seg.fp, (char *)s_seg.stdio_buf, _IOFBF, 65536) != 0) {
            ESP_LOGW(TAG, "setvbuf failed, using default stdio buffer");
            free(s_seg.stdio_buf);
            s_seg.stdio_buf = NULL;
        }
    } else {
        ESP_LOGW(TAG, "stdio_buf PSRAM alloc 64KB failed, using default buffer");
    }

    /* Write RIFF header (placeholder size) */
    uint8_t hdr[AVI_RIFF_HDR_SIZE];
    write_riff_hdr(hdr);
    if (fwrite(hdr, 1, AVI_RIFF_HDR_SIZE, s_seg.fp) != AVI_RIFF_HDR_SIZE) {
        ESP_LOGE(TAG, "Failed to write RIFF header to %s", s_current_file);
        goto fail_open;
    }

    /* Write hdrl LIST (with optional audio stream headers) */
    uint8_t hdrl[AVI_HDRL_WITH_AUDIO];
    write_hdrl(hdrl, w, h, playback_fps, with_audio);
    if (fwrite(hdrl, 1, hdrl_size, s_seg.fp) != hdrl_size) {
        ESP_LOGE(TAG, "Failed to write hdrl to %s", s_current_file);
        goto fail_open;
    }

    /* Write movi LIST header — 'LIST' + size(placeholder) + 'movi' = 12 bytes */
    uint8_t movi_hdr[12];
    memcpy(movi_hdr, "LIST", 4);
    put_u32(movi_hdr + 4, 0);   /* patched at close */
    memcpy(movi_hdr + 8, "movi", 4);
    if (fwrite(movi_hdr, 1, 12, s_seg.fp) != 12) {
        ESP_LOGE(TAG, "Failed to write movi header to %s", s_current_file);
        goto fail_open;
    }

    /* Reset counters */
    idx1_init(&s_seg.idx);
    s_seg.movi_data_size = 0;
    s_seg.frame_count    = 0;
    s_seg.start_ms       = s_seg_elapsed_ms();
    s_seg.hdrl_size      = (uint32_t)hdrl_size;
    s_seg.flush_counter = 0;

    /* Subscribe to audio broadcaster if muxing audio */
    s_seg.audio_sub = NULL;
    if (with_audio) {
        s_seg.audio_sub = audio_bcast_subscribe(AUDIO_SUB_AVI_MUX);
        if (!s_seg.audio_sub) {
            ESP_LOGW(TAG, "Failed to subscribe to audio broadcaster");
        }
    }

    ESP_LOGI(TAG, "Started %s", s_current_file);
    LOG_EVENT(LOG_EVENT_REC_STARTED, "file=%s", s_current_file);
    return ESP_OK;

fail_open:
    if (s_seg.audio_sub) {
        audio_bcast_unsubscribe(s_seg.audio_sub);
        s_seg.audio_sub = NULL;
    }
    fclose(s_seg.fp);
    s_seg.fp = NULL;
    if (s_seg.stdio_buf) {
        free(s_seg.stdio_buf);
        s_seg.stdio_buf = NULL;
    }
    remove(s_current_file);
    s_current_file[0] = '\0';
    ESP_LOGE(TAG, "Removing failed segment file");
    return ESP_FAIL;
}

/**
 * @brief 向当前分段写入一帧MJPEG数据，包含帧头和2字节对齐填充
 */
static esp_err_t write_avi_frame(const uint8_t *jpeg, size_t jpeg_len)
{
    uint8_t hdr[AVI_FRAME_HDR_SIZE];
    memcpy(hdr, "00dc", 4);
    put_u32(hdr + 4, (uint32_t)jpeg_len);

    if (fwrite(hdr, 1, AVI_FRAME_HDR_SIZE, s_seg.fp) != AVI_FRAME_HDR_SIZE)
        return ESP_FAIL;
    if (fwrite(jpeg, 1, jpeg_len, s_seg.fp) != jpeg_len)
        return ESP_FAIL;

    /* Pad to 2-byte alignment */
    if (jpeg_len & 1) {
        uint8_t pad = 0;
        fwrite(&pad, 1, 1, s_seg.fp);
    }

    /* Record in index */
    uint32_t chunk_offset = s_seg.movi_data_size;
    idx1_append(&s_seg.idx, FOURCC('0','0','d','c'), chunk_offset, (uint32_t)jpeg_len);

    /* Protect against SIZE_MAX overflow in alignment calculation */
    if (jpeg_len == SIZE_MAX) {
        ESP_LOGE(TAG, "jpeg_len overflow: SIZE_MAX");
        return ESP_FAIL;
    }

    size_t aligned_len = (jpeg_len + 1) & ~1u;
    s_seg.movi_data_size += AVI_FRAME_HDR_SIZE + (uint32_t)aligned_len;
    s_seg.frame_count++;

    return ESP_OK;
}

/**
 * @brief 向当前分段写入一帧G.711音频数据（'01wb'块），含2字节对齐
 */
static esp_err_t write_avi_audio_chunk(const uint8_t *data, size_t len)
{
    uint8_t hdr[AVI_FRAME_HDR_SIZE];
    memcpy(hdr, "01wb", 4);
    put_u32(hdr + 4, (uint32_t)len);

    if (fwrite(hdr, 1, AVI_FRAME_HDR_SIZE, s_seg.fp) != AVI_FRAME_HDR_SIZE)
        return ESP_FAIL;
    if (fwrite(data, 1, len, s_seg.fp) != len)
        return ESP_FAIL;

    /* Pad to 2-byte alignment */
    if (len & 1) {
        uint8_t pad = 0;
        fwrite(&pad, 1, 1, s_seg.fp);
    }

    /* Record in index */
    uint32_t chunk_offset = s_seg.movi_data_size;
    idx1_append(&s_seg.idx, FOURCC('0','1','w','b'), chunk_offset, (uint32_t)len);

    size_t aligned_len = (len + 1) & ~1u;
    s_seg.movi_data_size += AVI_FRAME_HDR_SIZE + (uint32_t)aligned_len;

    return ESP_OK;
}

/**
 * @brief 关闭当前分段文件，写入idx1索引并回填RIFF/movi大小和帧数
 */
static void close_segment(void)
{
    if (!s_seg.fp) {
        /* No file was open — just reset audio subscription */
        if (s_seg.audio_sub) {
            audio_bcast_unsubscribe(s_seg.audio_sub);
            s_seg.audio_sub = NULL;
        }
        idx1_free(&s_seg.idx);
        return;
    }

    /* Unsubscribe from audio broadcaster first */
    if (s_seg.audio_sub) {
        audio_bcast_unsubscribe(s_seg.audio_sub);
        s_seg.audio_sub = NULL;
    }

    /* Write idx1 index */
    uint32_t idx1_data_size = (uint32_t)s_seg.idx.count * 16;  /* 16 bytes per entry */
    uint8_t idx1_hdr[8];
    memcpy(idx1_hdr, "idx1", 4);
    put_u32(idx1_hdr + 4, idx1_data_size);
    fwrite(idx1_hdr, 1, 8, s_seg.fp);

    for (int i = 0; i < s_seg.idx.count; i++) {
        uint8_t entry[16];
        memcpy(entry, &s_seg.idx.entries[i].fourcc, 4);         /* dwChunkId          */
        put_u32(entry + 4,  AVIIF_KEYFRAME);                    /* dwFlags            */
        put_u32(entry + 8,  s_seg.idx.entries[i].offset + 4);   /* dwOffset (from movi)*/
        put_u32(entry + 12, s_seg.idx.entries[i].size);          /* dwSize             */
        fwrite(entry, 1, 16, s_seg.fp);
    }

    /* Patch RIFF size: file_size - 8 */
    long file_size = ftell(s_seg.fp);
    if (file_size > 0) {
        uint8_t riff_size[4];
        put_u32(riff_size, (uint32_t)(file_size - 8));
        fseek(s_seg.fp, 4, SEEK_SET);
        fwrite(riff_size, 1, 4, s_seg.fp);

        /* Patch movi LIST size — use actual hdrl_size written */
        uint8_t movi_size[4];
        put_u32(movi_size, s_seg.movi_data_size + 4); /* +4 for "movi" tag */
        fseek(s_seg.fp, AVI_RIFF_HDR_SIZE + s_seg.hdrl_size + 4, SEEK_SET);
        fwrite(movi_size, 1, 4, s_seg.fp);

        /* Patch avih: dwTotalFrames — offset = RIFF(12) + LIST_hdrl(12) + avih_hdr(8) + 16 */
        fseek(s_seg.fp, AVI_RIFF_HDR_SIZE + 12 + 8 + 16, SEEK_SET);
        uint8_t tf[4];
        put_u32(tf, s_seg.frame_count);
        fwrite(tf, 1, 4, s_seg.fp);

        /* Patch strh: dwLength — offset = strh_data_start + 32 */
        long strh_data_pos = AVI_RIFF_HDR_SIZE + 12 + AVI_AVIH_SIZE + 12 + 4 + 4;
        fseek(s_seg.fp, strh_data_pos + 32, SEEK_SET);
        put_u32(tf, s_seg.frame_count);
        fwrite(tf, 1, 4, s_seg.fp);
    }

    fclose(s_seg.fp);
    s_seg.fp = NULL;

    /* Release the 64KB setvbuf buffer now that the FILE is closed. fclose
     * has already flushed any buffered data to the SD card. */
    if (s_seg.stdio_buf) {
        free(s_seg.stdio_buf);
        s_seg.stdio_buf = NULL;
    }

    /* Delete zero-frame segments to prevent 0B file accumulation */
    if (s_seg.frame_count == 0 && s_current_file[0] != '\0') {
        ESP_LOGW(TAG, "Deleting zero-frame segment: %s", s_current_file);
        if (remove(s_current_file) != 0) {
            ESP_LOGE(TAG, "Failed to delete zero-frame file: %s (errno=%d)", s_current_file, errno);
        }
        /* Also remove SHA256 file if it exists */
        char sha_path[160];
        snprintf(sha_path, sizeof(sha_path), "%s.sha256", s_current_file);
        remove(sha_path);
        idx1_free(&s_seg.idx);
        return;
    }

    /* Compute SHA256 for integrity verification */
    /* DISABLED: re-reading file after recording causes heap corruption crash */
    /* TODO: investigate root cause of heap corruption and re-enable */
    if (false && file_size > 0 && s_current_file[0] != '\0') {
        FILE *f = fopen(s_current_file, "rb");
        if (f) {
            sha256_ctx_t ctx;
            sha256_init(&ctx);
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                sha256_update(&ctx, buf, n);
            }
            unsigned char hash[32];
            sha256_finish(&ctx, hash);
            fclose(f);

            char sha_path[160];
            snprintf(sha_path, sizeof(sha_path), "%s.sha256", s_current_file);
            FILE *sf = fopen(sha_path, "w");
            if (sf) {
                char hex[65];
                for (int i = 0; i < 32; i++) {
                    sprintf(hex + i * 2, "%02x", hash[i]);
                }
                hex[64] = '\n';
                fwrite(hex, 1, 65, sf);
                fclose(sf);
            }
        }
    }

    /* Calculate actual file size from file_size variable */
    float mb = (float)file_size / (1024.0f * 1024.0f);
    ESP_LOGI(TAG, "Segment complete: %s  size=%.1f MB  frames=%lu",
             s_current_file, mb, (unsigned long)s_seg.frame_count);
    LOG_EVENT(LOG_EVENT_SEGMENT_COMPLETE, "file=%s size=%.1fMB frames=%lu",
             s_current_file, mb, (unsigned long)s_seg.frame_count);

    idx1_free(&s_seg.idx);
}

/* ------------------------------------------------------------------ */
/*  SD writer task — drains write queue, performs async SD I/O       */
/* ------------------------------------------------------------------ */

/**
 * @brief Dedicated SD write task running on Core 1 (priority 2).
 * Consumes frame buffers from s_write_q, writes to AVI via
 * write_avi_frame, muxes audio, and releases frame_buf references.
 * The sentinel pattern (.fb == NULL) provides a flush barrier for
 * segment rotation and shutdown.
 */
static void sd_writer_task(void *arg)
{
    esp_task_wdt_add(NULL);
    /* 与 recording_task 的运行态保持一致（含 PREVIEW，PIT-020）：写盘任务必须
     * 与采集任务同生共死——若在预览期先行退场，recorder 的 sentinel 屏障会
     * 对着无人排水的队列阻塞（TWDT 实测复现，2026-09-04）。 */
    while (s_state == RECORDER_RECORDING || s_state == RECORDER_PAUSED ||
           s_state == RECORDER_PREVIEW) {
        esp_task_wdt_reset();
        write_msg_t wmsg;
        if (xQueueReceive(s_write_q, &wmsg, pdMS_TO_TICKS(1000)) != pdPASS)
            continue;
        if (wmsg.fb == NULL) {
            /* Sentinel: ack the barrier, do NOT release NULL */
            xTaskNotifyGive(s_task_handle);
            continue;
        }
        if (s_seg.fp) {
            write_avi_frame(wmsg.fb->data, wmsg.fb->len);
            /* Drain audio and mux — moved here from recording_task */
            if (s_seg.audio_sub) {
                audio_frame_t aframe;
                while (audio_bcast_receive(s_seg.audio_sub, &aframe, 0)) {
                    write_avi_audio_chunk(aframe.data, aframe.len);
                    audio_bcast_release(&aframe);
                }
            }
        }
        frame_buf_release(wmsg.fb);
    }
    /* Drain remaining queue on stop */
    write_msg_t wmsg;
    while (xQueueReceive(s_write_q, &wmsg, 0)) {
        if (wmsg.fb == NULL) {
            if (s_task_handle) xTaskNotifyGive(s_task_handle);
            continue;
        }
        if (s_seg.fp) write_avi_frame(wmsg.fb->data, wmsg.fb->len);
        frame_buf_release(wmsg.fb);
    }
    esp_task_wdt_delete(NULL);
    /* 必须先置空句柄再自删：recorder_stop 的等待循环靠它判断任务已退出，
     * 否则等满 10s 后对已释放的 TCB vTaskDelete → uxListRemoved 崩溃
     * （2026-09-03 两次停录后 LoadProhibited 重启，栈定位此处） */
    s_writer_handle = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Recording task                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 录像任务主循环，持续采集摄像头帧并写入AVI分段文件
 */
static void recording_task(void *arg)
{
    cam_config_t *cfg = config_get();
    uint16_t w, h;
    resolution_dims(cfg->cam_framesize, &w, &h);
    uint8_t fps = cfg->cam_fps > 0 ? cfg->cam_fps : 10;
    /* 家族延时动态模型（契约 §3.2）：timelapse_enabled 总开关 +
     * timelapse_mode 0=静态（固定 interval_s）/ 1=动态（min/max/decay 衰减） */
    uint8_t tl_mode = cfg->timelapse_mode;
    bool timelapse = cfg->timelapse_enabled;
    bool tl_dynamic = timelapse && tl_mode == 1;
    uint8_t playback_fps = timelapse ? 15 : fps;
    const char *prefix = tl_dynamic ? "DTL_" : timelapse ? "TLM_" : "REC_";

    /* 动态延时衰减状态（镜像 ai-thinker timelapse.c 的 s_current_interval_s
     * 逻辑：有运动 = min；每 decay_period_s 无运动 ×decay_factor，封顶 max）。
     * 运动信号 = motion_detector_is_active()（Core 1 异步任务滞回判定，
     * 最多滞后一个分析周期）——最小侵入钩子，无新增任务间耦合。 */
    uint16_t cur_interval_s = cfg->timelapse_min_interval_s ? cfg->timelapse_min_interval_s : 1;
    int64_t last_motion_us = esp_timer_get_time();

    bool segment_open = false;
    uint32_t total_bytes = 0;

    /* Register with task watchdog */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Recording task started: tl_en=%u mode=%u %ux%u @ %u fps, segment=%u s",
             timelapse, tl_mode, w, h, playback_fps, cfg->segment_sec);

    while (s_state == RECORDER_RECORDING || s_state == RECORDER_PAUSED ||
           s_state == RECORDER_PREVIEW) {
        /* Feed task watchdog each iteration */
        esp_task_wdt_reset();

        /* 暂停且无人在看：静默等待恢复；暂停但有人在看：按预览出帧（PIT-020） */
        if (s_state == RECORDER_PAUSED && s_preview_refs == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const bool preview_active = (s_state == RECORDER_PREVIEW) ||
                                    (s_state == RECORDER_PAUSED && s_preview_refs > 0);

        /* Re-read config in case it changed */
        cfg = config_get();
        fps = cfg->cam_fps > 0 ? cfg->cam_fps : 10;
        tl_mode = cfg->timelapse_mode;
        timelapse = cfg->timelapse_enabled;
        tl_dynamic = timelapse && tl_mode == 1;
        playback_fps = timelapse ? 15 : fps;
        prefix = tl_dynamic ? "DTL_" : timelapse ? "TLM_" : "REC_";

        /* 本周期延时摄影拍摄间隔（ms）。预览态不间隔（实时出帧）。 */
        int tl_delay_ms = 0;
        if (timelapse && !preview_active) {
            if (tl_dynamic) {
                if (motion_detector_is_active()) {
                    if (cur_interval_s != cfg->timelapse_min_interval_s) {
                        ESP_LOGI(TAG, "Motion active, tl interval reset to %us",
                                 cfg->timelapse_min_interval_s);
                    }
                    cur_interval_s = cfg->timelapse_min_interval_s;
                    last_motion_us = esp_timer_get_time();
                } else {
                    int64_t elapsed_s = (esp_timer_get_time() - last_motion_us) / 1000000;
                    if (elapsed_s >= (int64_t)cfg->timelapse_decay_period_s) {
                        uint16_t next = (uint16_t)(cur_interval_s * cfg->timelapse_decay_factor);
                        if (next > cfg->timelapse_max_interval_s || next < cur_interval_s) {
                            next = cfg->timelapse_max_interval_s;
                        }
                        if (next != cur_interval_s) {
                            ESP_LOGI(TAG, "No motion for %llds, tl interval decayed %us->%us",
                                     (long long)elapsed_s, cur_interval_s, next);
                        }
                        cur_interval_s = next;
                        last_motion_us = esp_timer_get_time();
                    }
                }
                tl_delay_ms = (cur_interval_s > 0 ? cur_interval_s : 1) * 1000;
            } else {
                tl_delay_ms = (cfg->timelapse_interval_s > 0)
                              ? cfg->timelapse_interval_s * 1000 : 30000;
            }
        }

        bool write_to_sd = cfg->video_record_to_sd && storage_is_available()
                           && !preview_active;
        bool mux_audio = write_to_sd && cfg->audio_record_to_sd;

        /* If write_to_sd turned off mid-segment, close the segment */
        if (s_seg.fp && !write_to_sd) {
            /* Flush pending writes before closing segment */
            write_msg_t sentinel = { .fb = NULL, .capture_us = 0 };
            if (xQueueSend(s_write_q, &sentinel, pdMS_TO_TICKS(5000)) != pdPASS) {
                    ESP_LOGW(TAG, "Sentinel enqueue timeout (writer busy/dead?)");
                }
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
                ESP_LOGW(TAG, "Sentinel barrier timeout during segment close");
            }
            close_segment();
            segment_open = false;
        }
        /* Open first segment — flag 只在真正开段后置位。
         * PIT-020 崩溃教训：预览迭代（write_to_sd=false）不能提前消耗该标志，
         * 否则停录→预览→恢复录像时跳过 open_segment，直接写已 close 的
         * FILE*（fflush 崩溃，2026-09-04 实测复现于"from preview"路径）。 */
        if (!segment_open && write_to_sd) {
            if (open_segment(w, h, playback_fps, prefix) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to open segment — trying cleanup");
                storage_cleanup();
                /* Retry once after cleanup */
                if (open_segment(w, h, playback_fps, prefix) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open segment after cleanup, retrying in 5 s");
                    storage_mark_io_error();
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
            total_bytes  = 0;
            segment_open = true;
        }
        /* Record start of capture cycle */
        int64_t cycle_start_us = esp_timer_get_time();

        /* Capture frame */
        camera_frame_t frame = {0};
        esp_err_t err = camera_capture(&frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Capture failed (0x%x)", err);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Copy frame data to PSRAM and return camera fb immediately.
         * This frees the camera framebuffer for the MJPEG streamer,
         * reducing contention that causes stream disconnects when
         * motion detection holds the fb for 200-500ms.
         *
         * The copy lives in a refcounted frame_buf_t: publish_buf shares
         * references with subscribers + cache (zero extra copies), while
         * this task keeps its own reference for motion detect + SD write. */
        size_t jpeg_data_len = frame.len;
        frame_buf_t *fb = frame_buf_alloc(frame.buf, frame.len);
        const uint8_t *jpeg_data;
        bool holding_camera_fb = false;  /* true if we couldn't alloc and are holding camera fb */

        if (fb) {
            jpeg_data = fb->data;
            camera_return_fb(&frame);
        } else {
            ESP_LOGW(TAG, "PSRAM alloc %zu failed, holding camera fb", frame.len);
            jpeg_data = frame.buf;
            holding_camera_fb = true;
        }

        /* Publish frame to streamer subscribers (RTSP, MJPEG).
         * publish_buf adds references for cache + each subscriber; our own
         * reference (from frame_buf_alloc) is unaffected. */
        if (fb) {
            fbroadcast_publish_buf(fb);
        } else {
            /* Fallback: camera fb held; do a one-shot copy publish so subscribers
             * get something. We can't share the camera fb by reference. */
            fbroadcast_publish(jpeg_data, jpeg_data_len);
        }

        /* Smart frame dropping: skip write if cycle exceeds threshold.
         * Motion detection now uses JPEG_IMAGE_SCALE_4X (~30ms instead of ~500ms),
         * so dynamic timelapse no longer needs a relaxed threshold — unified at 500ms. */
        int64_t drop_threshold_us = 500000;
        int64_t cycle_elapsed_us = esp_timer_get_time() - cycle_start_us;
        if (cycle_elapsed_us > drop_threshold_us && cfg->frame_drop_enabled) {
            if (fb) {
                frame_buf_release(fb);
            } else if (holding_camera_fb) {
                camera_return_fb(&frame);
            }
            s_frames_dropped++;
            /* Throttle warning to max 1/sec */
            if (cycle_start_us - s_last_drop_log_us > 1000000) {
                ESP_LOGW(TAG, "Frame dropped: cycle=%lldms total_dropped=%lu",
                         (long long)(cycle_elapsed_us / 1000),
                         (unsigned long)s_frames_dropped);
                LOG_EVENT(LOG_EVENT_FRAME_DROP, "cycle=%lldms total=%lu",
                         (long long)(cycle_elapsed_us / 1000),
                         (unsigned long)s_frames_dropped);
                s_last_drop_log_us = cycle_start_us;
            }
            {
                int drop_delay_ms = preview_active ? broker_frame_delay() :
                    (tl_delay_ms > 0 ? tl_delay_ms : broker_frame_delay());
                while (drop_delay_ms > 0 && (s_state == RECORDER_RECORDING ||
                                             s_state == RECORDER_PREVIEW)) {
                    int chunk_ms = (drop_delay_ms > 5000) ? 5000 : drop_delay_ms;
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(chunk_ms));
                    drop_delay_ms -= chunk_ms;
                }
            }
            continue;
        }

        /* Write frame to AVI (only if recording to SD).
         * Normal (fb != NULL): enqueue to sd_writer_task for async SD write.
         * Fallback (holding_camera_fb): synchronous inline write. */
        if (write_to_sd) {
            if (fb) {
                /* Normal path: enqueue for async write */
                write_msg_t wmsg = { .fb = fb, .capture_us = 0 };
                __atomic_fetch_add(&fb->refcount, 1, __ATOMIC_ACQ_REL);
                if (xQueueSend(s_write_q, &wmsg, 0) != pdPASS) {
                    /* Queue full -- drop frame */
                    frame_buf_release(fb);
                    s_frames_dropped++;
                    if (cycle_start_us - s_last_drop_log_us > 1000000) {
                        ESP_LOGW(TAG, "SD write dropped: queue full, total_dropped=%lu",
                                 (unsigned long)s_frames_dropped);
                        s_last_drop_log_us = cycle_start_us;
                    }
                    /* Drain and DISCARD audio to maintain A/V alignment */
                    if (mux_audio && s_seg.audio_sub) {
                        audio_frame_t aframe;
                        while (audio_bcast_receive(s_seg.audio_sub, &aframe, 0)) {
                            audio_bcast_release(&aframe);
                        }
                    }
                }
                err = ESP_OK;
            } else {
                /* FALLBACK: holding camera fb -- sync inline write (rare, PSRAM alloc failed) */
                err = write_avi_frame(jpeg_data, jpeg_data_len);
                if (err == ESP_OK && mux_audio && s_seg.audio_sub) {
                    audio_frame_t aframe;
                    while (audio_bcast_receive(s_seg.audio_sub, &aframe, 0)) {
                        write_avi_audio_chunk(aframe.data, aframe.len);
                        audio_bcast_release(&aframe);
                    }
                }
                camera_return_fb(&frame);
                holding_camera_fb = false;
            }
        }
        if (fb) {
            frame_buf_release(fb);
        } else if (holding_camera_fb) {
            /* Reached when write_to_sd was false */
            camera_return_fb(&frame);
        }

        if (write_to_sd && err != ESP_OK) {
            ESP_LOGW(TAG, "SD write failed — closing segment, attempting cleanup");
            /* Flush pending writes before closing segment */
            write_msg_t sentinel = { .fb = NULL, .capture_us = 0 };
            if (xQueueSend(s_write_q, &sentinel, pdMS_TO_TICKS(5000)) != pdPASS) {
                    ESP_LOGW(TAG, "Sentinel enqueue timeout (writer busy/dead?)");
                }
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
                ESP_LOGW(TAG, "Sentinel barrier timeout during error recovery");
            }
            close_segment();  /* Save what we have */
            segment_open = false;
            cleanup_orphan_zero_byte_files();  /* safety net */
            /* Try cleanup to free space (SD may be full) */
            storage_cleanup();

            /* Re-open segment after cleanup — give it one more chance */
            if (open_segment(w, h, playback_fps, prefix) == ESP_OK) {
                segment_open = true;
                total_bytes = 0;
                ESP_LOGI(TAG, "Resumed recording after cleanup");
                continue;
            }

            /* Cleanup didn't help — truly unrecoverable */
            ESP_LOGE(TAG, "SD write failed after cleanup — entering ERROR state");
            storage_mark_io_error();
            s_state = RECORDER_ERROR;
            led_set_status(LED_ERROR);
            break;
        }

        if (write_to_sd) {
            total_bytes += jpeg_data_len;

            /* Sync to SD: every frame in timelapse (sparse), every ~1s in continuous.
             * fflush drains the libc stdio buffer into FatFS; fsync (-> f_sync)
             * additionally flushes FAT directory/FAT-table entries so a power
             * loss mid-recording leaves a playable, correctly-sized AVI rather
             * than a truncated/zero-length file. */
            if (timelapse) {
                fflush(s_seg.fp);
                fsync(fileno(s_seg.fp));
            } else if (++s_seg.flush_counter >= 10) {
                fflush(s_seg.fp);
                fsync(fileno(s_seg.fp));
                s_seg.flush_counter = 0;
            }

            /* Check segment duration */
            int64_t elapsed_ms = s_seg_elapsed_ms() - s_seg.start_ms;
            if (elapsed_ms >= (int64_t)cfg->segment_sec * 1000) {
                /* Close current segment */
                char completed_file[128];
                strncpy(completed_file, s_current_file, sizeof(completed_file) - 1);
                completed_file[sizeof(completed_file) - 1] = '\0';
                uint32_t completed_size = total_bytes;

                /* Flush pending writes before closing segment */
                write_msg_t sentinel = { .fb = NULL, .capture_us = 0 };
                if (xQueueSend(s_write_q, &sentinel, pdMS_TO_TICKS(5000)) != pdPASS) {
                    ESP_LOGW(TAG, "Sentinel enqueue timeout (writer busy/dead?)");
                }
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
                    ESP_LOGW(TAG, "Sentinel barrier timeout during segment rotation");
                }
                close_segment();
                segment_open = false;

                /* Safety net: remove any zero-byte files left by close_segment() failures */
                cleanup_orphan_zero_byte_files();

                /* Notify via callback */
                if (s_segment_cb && completed_size > 0) {
                    s_segment_cb(completed_file, completed_size);
                }

                /* Immediately open next segment */
                if (open_segment(w, h, playback_fps, prefix) == ESP_OK) {
                    segment_open = true;
                    total_bytes  = 0;
                }
            }
        }

        /* Frame rate control — break long sleeps to feed watchdog (TWDT=30s) */
        {
            int delay_ms;
            if (preview_active) {
                /* 预览按正常帧率出帧（延时摄影间隔对实时预览无意义） */
                delay_ms = broker_frame_delay();
            } else if (timelapse) {
                delay_ms = (tl_delay_ms > 0) ? tl_delay_ms : 1000;
            } else {
                delay_ms = broker_frame_delay();
            }
            /* Sleep in 5-second chunks, feeding watchdog each iteration */
            while (delay_ms > 0 && (s_state == RECORDER_RECORDING ||
                                    s_state == RECORDER_PREVIEW)) {
                int chunk_ms = (delay_ms > 5000) ? 5000 : delay_ms;
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(chunk_ms));
                delay_ms -= chunk_ms;
            }
        }
    }

    /* Cleanup: close any open segment and notify */
    if (segment_open) {
        char completed_file[128];
        strncpy(completed_file, s_current_file, sizeof(completed_file) - 1);
        completed_file[sizeof(completed_file) - 1] = '\0';
        uint32_t completed_size = total_bytes;
        /* Flush pending writes before closing segment */
        write_msg_t sentinel = { .fb = NULL, .capture_us = 0 };
        if (xQueueSend(s_write_q, &sentinel, pdMS_TO_TICKS(5000)) != pdPASS) {
                    ESP_LOGW(TAG, "Sentinel enqueue timeout (writer busy/dead?)");
                }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
            ESP_LOGW(TAG, "Sentinel barrier timeout during cleanup");
        }
        close_segment();
        if (s_segment_cb && completed_size > 0) {
            s_segment_cb(completed_file, completed_size);
        }
    }
    s_current_file[0] = '\0';

    /* Unregister from task watchdog */
    esp_task_wdt_delete(NULL);

    ESP_LOGI(TAG, "Recording task exiting");
    s_task_handle = NULL;
    vTaskDelete(NULL);
}


/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化录像模块，创建互斥锁，设置初始状态为IDLE
 */
esp_err_t recorder_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    s_state = RECORDER_IDLE;
    ESP_LOGI(TAG, "Recorder initialized");
    return ESP_OK;
}

/**
 * @brief 开始录像，若已暂停则恢复，否则在核心0创建录像任务
 */
esp_err_t recorder_start(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state == RECORDER_RECORDING) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;   /* already recording */
    }

    if (s_state == RECORDER_PAUSED) {
        s_state = RECORDER_RECORDING;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Recording resumed");
        return ESP_OK;
    }

    if (s_state == RECORDER_PREVIEW) {
        /* PIT-020：预览态下任务已在跑，仅切状态；循环下一轮按 write_to_sd
         * 重新开分段，避免任务重启造成流断流。 */
        s_state = RECORDER_RECORDING;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Recording started (from preview)");
        LOG_EVENT(LOG_EVENT_REC_STARTED, "recording started");
        ws_broadcast("recording_started", "{}");
        return ESP_OK;
    }
    s_state = RECORDER_RECORDING;  /* Set state BEFORE creating task to avoid race */

    esp_err_t err = spawn_capture_tasks_locked();
    xSemaphoreGive(s_mutex);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Recording started");
    LOG_EVENT(LOG_EVENT_REC_STARTED, "recording started");
    ws_broadcast("recording_started", "{}");
    return ESP_OK;
}

/* 创建采集/写盘任务（调用方持有 s_mutex，s_state 已置为目标态）。
 * 动态延时模式一并启动运动检测任务（家族动态模型的运动信号源）。 */
static esp_err_t spawn_capture_tasks_locked(void)
{
/* Initialize motion detector if dynamic timelapse mode（契约 §3.2：
 * enabled + mode=1 时需要 motion_detector_is_active() 供衰减逻辑取信号） */
if (config_get()->timelapse_enabled && config_get()->timelapse_mode == 1) {
    motion_detector_init();
    /* Start async motion detection task on Core 1. Subscribes to the frame
     * broadcaster and runs JPEG decode + scoring off the recorder's hot path.
     * motion_detector_deinit() (in recorder_stop) stops the task. */
    motion_detector_start();
}

    BaseType_t ret = xTaskCreatePinnedToCore(
        recording_task,
        "recorder",
        10240,   /* increased for motion detection JPEG decode */
        NULL,
        3,          /* priority 3 — below audio_capture (5) to prevent audio dropouts during SD writes */
        &s_task_handle,
        0);         /* KEEP Core 0 — moving to Core 1 would make recorder the
               * highest-prio user task there (prio 3 > streamers' prio 2),
               * preempting mjpeg_cli/rtsp_video and hurting stream smoothness */

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recording task");
        s_state = RECORDER_IDLE;
        return ESP_FAIL;
    }

    /* Create write queue and dedicated SD writer task (Core 1, prio 2) */
    s_write_q = xQueueCreate(2, sizeof(write_msg_t));
    if (!s_write_q) {
        ESP_LOGE(TAG, "Failed to create write queue");
        s_state = RECORDER_IDLE;
        return ESP_FAIL;
    }
    BaseType_t wret = xTaskCreatePinnedToCore(
        sd_writer_task,
        "sd_writer",
        8192,
        NULL,
        2,          /* priority 2 — same as streamers, I/O bound */
        &s_writer_handle,
        1);         /* Core 1 — I/O bound, doesn't need camera proximity */
    if (wret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sd_writer task");
        vQueueDelete(s_write_q);
        s_write_q = NULL;
        s_state = RECORDER_IDLE;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* 彻底停下并回收采集/写盘任务（调用方不得持有 s_mutex；状态已置 IDLE）。
 * emit_events：用户停录像时播 recording_stopped；预览自然收尾不播（用户没在录像）。 */
static void recorder_full_stop(bool emit_events)
{
    /* Wait for both tasks to finish — recording_task uses 5s watchdog chunks,
     * sd_writer_task drains remaining queue on exit. Extended to 10s total. */
    int waited = 0;
    while ((s_task_handle != NULL || s_writer_handle != NULL) && waited < 10000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }

/* Cleanup motion detector */
motion_detector_deinit();

    if (s_task_handle != NULL) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    if (s_writer_handle != NULL) {
        vTaskDelete(s_writer_handle);
        s_writer_handle = NULL;
    }
    if (s_write_q) {
        vQueueDelete(s_write_q);
        s_write_q = NULL;
    }

    ESP_LOGI(TAG, "Recording stopped");
    if (emit_events) {
        LOG_EVENT(LOG_EVENT_REC_STOPPED, "recording stopped");
        ws_broadcast("recording_stopped", "{}");
    }
}

/**
 * @brief 停止录像。有流观众时降级为预览（只出帧不写盘），否则彻底停止。
 */
esp_err_t recorder_stop(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state != RECORDER_RECORDING && s_state != RECORDER_PAUSED) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    if (s_preview_refs > 0) {
        /* PIT-020：观众仍在（MJPEG/RTSP）——采集任务降级为 PREVIEW 继续供帧，
         * 循环内 write_to_sd 变 false 会走既有分段收尾路径干净关段。 */
        s_state = RECORDER_PREVIEW;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Recording stopped — capture continues for %u stream viewer(s)",
                 s_preview_refs);
        LOG_EVENT(LOG_EVENT_REC_STOPPED, "recording stopped");
        ws_broadcast("recording_stopped", "{}");
        return ESP_OK;
    }

    s_state = RECORDER_IDLE;   /* task checks this and exits */
    xSemaphoreGive(s_mutex);
    recorder_full_stop(true);
    return ESP_OK;
}

void recorder_preview_acquire(const char *who)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_preview_refs++;
    if (s_state == RECORDER_IDLE) {
        /* 无活跃采集任务：以预览态拉起（只出帧不写盘，PIT-020） */
        s_state = RECORDER_PREVIEW;   /* BEFORE spawn, same anti-race rule */
        if (spawn_capture_tasks_locked() != ESP_OK) {
            s_state = RECORDER_IDLE;  /* spawn 失败路径内部已置 IDLE，此处双保险 */
        } else {
            ESP_LOGI(TAG, "Preview capture started by %s", who);
        }
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Preview acquire by %s (refs=%u)", who, s_preview_refs);
}

void recorder_preview_release(const char *who)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_preview_refs > 0) {
        s_preview_refs--;
    }
    bool last = (s_preview_refs == 0 && s_state == RECORDER_PREVIEW);
    if (last) {
        s_state = RECORDER_IDLE;
    }
    xSemaphoreGive(s_mutex);
    if (last) {
        recorder_full_stop(false);
        ESP_LOGI(TAG, "Preview ended (last viewer %s) — capture task stopped", who);
    } else {
        ESP_LOGI(TAG, "Preview release by %s (refs=%u)", who, s_preview_refs);
    }
}

/**
 * @brief 暂停录像，仅当当前状态为RECORDING时有效
 */
esp_err_t recorder_pause(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state != RECORDER_RECORDING) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_state = RECORDER_PAUSED;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Recording paused");
    return ESP_OK;
}

/**
 * @brief 获取当前录像状态
 */
recorder_state_t recorder_get_state(void)
{
    return s_state;
}

/**
 * @brief 设置分段录像完成时的回调函数
 */
void recorder_set_segment_cb(recorder_segment_cb_t cb)
{
    s_segment_cb = cb;
}

/**
 * @brief 获取当前正在写入的录像文件路径
 */
const char *recorder_get_current_file(void)
{
    return s_current_file;
}

/**
 * @brief 获取录像任务的栈高水位标记（字节）
 */
uint32_t recorder_get_stack_hwm(void)
{
    return s_stack_hwm;
}

/** @brief 获取录像任务启动以来累计丢帧数 */
uint32_t recorder_get_frames_dropped(void)
{
    return s_frames_dropped;
}

/* Recursive helper: scan dir for .avi files with RIFF size=0 and delete them */
/**
 * @brief 递归扫描目录，删除RIFF大小为0的不完整AVI文件
 */
static int cleanup_dir_recursive(const char *dirpath, int depth)
{
    if (depth > 3) return 0;  /* safety: don't recurse too deep */

    DIR *dir = opendir(dirpath);
    if (!dir) return 0;

    int deleted = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[300];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory (YYYY-MM/DD/ structure) */
            deleted += cleanup_dir_recursive(fullpath, depth + 1);
            continue;
        }

        /* Check if it's an .avi file */
        size_t nlen = strlen(entry->d_name);
        if (nlen < 5 || strcmp(entry->d_name + nlen - 4, ".avi") != 0) continue;

        /* Read first 8 bytes: RIFF(4) + size(4) */
        FILE *fp = fopen(fullpath, "rb");
        if (!fp) continue;
        uint8_t hdr[8];
        size_t n = fread(hdr, 1, 8, fp);
        fclose(fp);

        /* Check for incomplete/empty AVI files */
        bool should_delete = false;

        /* File too small to be a valid AVI (min 260 bytes = RIFF + hdrl + movi headers) */
        /* Also catch truly empty files (0 bytes — crash during fopen before any write) */
        if (st.st_size == 0) {
            should_delete = true;
            ESP_LOGI(TAG, "Found empty AVI (0 bytes): %s", fullpath);
        } else if ((size_t)st.st_size < (AVI_RIFF_HDR_SIZE + AVI_HDRL_TOTAL + 12)) {
            should_delete = true;
            ESP_LOGI(TAG, "Found undersized AVI (%ld bytes): %s", (long)st.st_size, fullpath);
        } else {
            /* Check RIFF header and size=0 (incomplete write) */
            if (n == 8 && memcmp(hdr, "RIFF", 4) == 0) {
                uint32_t riff_size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
                if (riff_size == 0) {
                    should_delete = true;
                }
            }
        }

        if (should_delete) {
            remove(fullpath);
            deleted++;
            ESP_LOGI(TAG, "Cleaned incomplete: %s", fullpath);
        }
    }
    closedir(dir);
    return deleted;
}

/**
 * @brief 清理启动时发现的未完整写入的录像文件
 */
void recorder_cleanup_incomplete(void)
{
    int deleted = cleanup_dir_recursive("/sdcard/recordings", 0);
    if (deleted > 0) {
        ESP_LOGI(TAG, "Boot cleanup: removed %d incomplete recordings", deleted);
    }
}

 /** @brief One-shot cleanup of zero-byte .avi files in the recordings dir.
 *  Called after segment rotation as a safety net for close_segment() failures.
 */
static void cleanup_orphan_zero_byte_files(void)
    {
    int deleted = cleanup_dir_recursive("/sdcard/recordings", 0);
    if (deleted > 0) {
        ESP_LOGI(TAG, "Post-rotation cleanup: removed %d orphan zero-byte files", deleted);
    }
}
