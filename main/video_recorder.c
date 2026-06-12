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
#include "motion_detector.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include "sha256.h"

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

/* ------------------------------------------------------------------ */
/*  Private state                                                     */
/* ------------------------------------------------------------------ */

/* Maximum idx1 index entries to prevent unbounded realloc growth */
#define MAX_IDX1_ENTRIES  100000

static const char *TAG = "recorder";

static recorder_state_t   s_state        = RECORDER_IDLE;
static TaskHandle_t       s_task_handle  = NULL;
static char               s_current_file[128] = {0};
static recorder_segment_cb_t s_segment_cb = NULL;
static SemaphoreHandle_t  s_mutex        = NULL;
static uint32_t           s_stack_hwm    = 0;   /* Stack high-water mark */
static uint32_t          s_frames_dropped = 0;
static int64_t           s_last_drop_log_us = 0; /* throttle drop log */

/* Resolution → pixel dimensions */
/**
 * @brief 根据分辨率编号获取对应的像素宽度和高度
 */
static void resolution_dims(uint8_t res, uint16_t *w, uint16_t *h)
{
    switch (res) {
        case 0:  *w = 640;  *h = 480;  break;   /* VGA  */
        case 1:  *w = 800;  *h = 600;  break;   /* SVGA */
        case 2:  *w = 1024; *h = 768;  break;   /* XGA  */
        default: *w = 800;  *h = 600;  break;
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
    uint32_t offset;   /* offset from start of 'movi' list data */
    uint32_t size;     /* JPEG data size                         */
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
static esp_err_t idx1_append(idx1_t *idx, uint32_t offset, uint32_t size)
{
    if (idx->count >= idx->capacity) {
        int new_cap = idx->capacity == 0 ? 512 : idx->capacity * 2;
        if (new_cap > MAX_IDX1_ENTRIES) {
            ESP_LOGE(TAG, "idx1 overflow: entries would exceed %d", MAX_IDX1_ENTRIES);
            return ESP_ERR_NO_MEM;
        }
        idx1_entry_t *new_buf = realloc(idx->entries, (size_t)new_cap * sizeof(idx1_entry_t));
        if (!new_buf) {
            ESP_LOGE(TAG, "idx1 realloc failed");
            return ESP_ERR_NO_MEM;
        }
        idx->entries  = new_buf;
        idx->capacity = new_cap;
    }
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
static void write_hdrl(uint8_t *buf, uint16_t w, uint16_t h, uint8_t playback_fps)
{
    int pos = 0;

    /* LIST "hdrl" */
    memcpy(buf + pos, "LIST", 4);                         pos += 4;
    put_u32(buf + pos, AVI_HDRL_TOTAL - 8);              pos += 4;
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
    put_u32(buf + pos, 1);              /* dwStreams            */ pos += 4;
    put_u32(buf + pos, 0x100000);       /* dwSuggestedBufSize   */ pos += 4;
    put_u32(buf + pos, w);              /* dwWidth              */ pos += 4;
    put_u32(buf + pos, h);              /* dwHeight             */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[0]          */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[1]          */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[2]          */ pos += 4;
    put_u32(buf + pos, 0);              /* reserved[3]          */ pos += 4;

    /* LIST "strl" */
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
    put_u32(buf + pos, playback_fps); /* dwRate */ pos += 4;
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
    build_segment_path(s_current_file, sizeof(s_current_file), prefix);

    s_seg.fp = fopen(s_current_file, "wb");
    if (!s_seg.fp) {
        ESP_LOGE(TAG, "Failed to open %s", s_current_file);
        return ESP_FAIL;
    }

    /* Write RIFF header (placeholder size) */
    uint8_t hdr[AVI_RIFF_HDR_SIZE];
    write_riff_hdr(hdr);
    if (fwrite(hdr, 1, AVI_RIFF_HDR_SIZE, s_seg.fp) != AVI_RIFF_HDR_SIZE) {
        ESP_LOGE(TAG, "Failed to write RIFF header to %s", s_current_file);
        goto fail_open;
    }

    /* Write hdrl LIST */
    uint8_t hdrl[AVI_HDRL_TOTAL];
    write_hdrl(hdrl, w, h, playback_fps);
    if (fwrite(hdrl, 1, AVI_HDRL_TOTAL, s_seg.fp) != AVI_HDRL_TOTAL) {
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

    ESP_LOGI(TAG, "Started %s", s_current_file);
    LOG_EVENT(LOG_EVENT_REC_STARTED, "file=%s", s_current_file);
    return ESP_OK;

fail_open:
    fclose(s_seg.fp);
    s_seg.fp = NULL;
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
    idx1_append(&s_seg.idx, chunk_offset, (uint32_t)jpeg_len);

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
 * @brief 关闭当前分段文件，写入idx1索引并回填RIFF/movi大小和帧数
 */
static void close_segment(void)
{
    if (!s_seg.fp) return;

    /* Write idx1 index */
    uint32_t idx1_data_size = (uint32_t)s_seg.idx.count * 16;  /* 16 bytes per entry */
    uint8_t idx1_hdr[8];
    memcpy(idx1_hdr, "idx1", 4);
    put_u32(idx1_hdr + 4, idx1_data_size);
    fwrite(idx1_hdr, 1, 8, s_seg.fp);

    for (int i = 0; i < s_seg.idx.count; i++) {
        uint8_t entry[16];
        memcpy(entry, "00dc", 4);                              /* dwChunkId          */
        put_u32(entry + 4,  AVIIF_KEYFRAME);                   /* dwFlags            */
        put_u32(entry + 8,  s_seg.idx.entries[i].offset + 4);  /* dwOffset (from movi)*/
        put_u32(entry + 12, s_seg.idx.entries[i].size);         /* dwSize             */
        fwrite(entry, 1, 16, s_seg.fp);
    }

    /* Patch RIFF size: file_size - 8 */
    long file_size = ftell(s_seg.fp);
    if (file_size > 0) {
        uint8_t riff_size[4];
        put_u32(riff_size, (uint32_t)(file_size - 8));
        fseek(s_seg.fp, 4, SEEK_SET);
        fwrite(riff_size, 1, 4, s_seg.fp);

        /* Patch movi LIST size */
        uint8_t movi_size[4];
        put_u32(movi_size, s_seg.movi_data_size + 4); /* +4 for "movi" tag */
        fseek(s_seg.fp, AVI_RIFF_HDR_SIZE + AVI_HDRL_TOTAL + 4, SEEK_SET);
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
/*  Recording task                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 录像任务主循环，持续采集摄像头帧并写入AVI分段文件
 */
static void recording_task(void *arg)
{
    cam_config_t *cfg = config_get();
    uint16_t w, h;
    resolution_dims(cfg->resolution, &w, &h);
uint8_t fps = cfg->fps > 0 ? cfg->fps : 10;
uint8_t tl_mode = cfg->timelapse_mode;
bool timelapse = tl_mode > 0;
uint8_t playback_fps = timelapse ? 15 : fps;
const char *prefix = tl_mode == 2 ? "DTL_" : timelapse ? "TLM_" : "REC_";

    bool segment_open = false;
    uint32_t total_bytes = 0;

    /* Register with task watchdog */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Recording task started: mode=%d %ux%u @ %u fps, segment=%u s",
    tl_mode, w, h, playback_fps, cfg->segment_sec);

    while (s_state == RECORDER_RECORDING || s_state == RECORDER_PAUSED) {
        /* Feed task watchdog each iteration */
        esp_task_wdt_reset();

        if (s_state == RECORDER_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Re-read config in case it changed */
        cfg = config_get();
fps = cfg->fps > 0 ? cfg->fps : 10;
tl_mode = cfg->timelapse_mode;
timelapse = tl_mode > 0;
playback_fps = timelapse ? 15 : fps;
prefix = tl_mode == 2 ? "DTL_" : timelapse ? "TLM_" : "REC_";

        /* Open first segment */
        if (!segment_open) {
            if (open_segment(w, h, playback_fps, prefix) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to open segment — trying cleanup");
                storage_cleanup();
                /* Retry once after cleanup */
                if (open_segment(w, h, playback_fps, prefix) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open segment after cleanup, retrying in 5 s");
                    storage_set_unavailable();
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
            segment_open = true;
            total_bytes  = 0;
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
         * motion detection holds the fb for 200-500ms. */
        size_t jpeg_data_len = frame.len;
        uint8_t *jpeg_copy = heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM);
        const uint8_t *jpeg_data;

        if (jpeg_copy) {
            memcpy(jpeg_copy, frame.buf, frame.len);
            jpeg_data = jpeg_copy;
            camera_return_fb(&frame);
        } else {
            ESP_LOGW(TAG, "PSRAM alloc %zu failed, holding camera fb", frame.len);
            jpeg_data = frame.buf;
        }

        /* Publish frame to streamer subscribers (RTSP, MJPEG) */
        fbroadcast_publish(jpeg_data, jpeg_data_len);

/* Dynamic timelapse: process motion detection */
uint16_t dynamic_interval_sec = cfg->timelapse_interval_sec;
if (tl_mode == 2 && s_state == RECORDER_RECORDING) {
    motion_result_t mr;
    esp_err_t merr = motion_detector_process(
        jpeg_data, jpeg_data_len,
        cfg->motion_sensitivity,
        cfg->motion_active_interval_sec,
        cfg->motion_idle_interval_sec,
        &mr);
    if (merr == ESP_OK) {
        dynamic_interval_sec = mr.current_interval_sec;
    } else {
        dynamic_interval_sec = cfg->motion_idle_interval_sec;
    }
}

        /* Smart frame dropping: skip write if cycle exceeds threshold.
         * NOTE: For dynamic timelapse (tl_mode==2), motion detection itself
         * takes ~500ms, so use a higher threshold (2000ms) to avoid dropping
         * every single frame. Regular continuous mode uses 500ms threshold. */
        int64_t drop_threshold_us = (tl_mode == 2) ? 2000000 : 500000;
        int64_t cycle_elapsed_us = esp_timer_get_time() - cycle_start_us;
        if (cycle_elapsed_us > drop_threshold_us && cfg->frame_drop_enabled) {
            if (jpeg_copy) {
                free(jpeg_copy);
            } else {
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
                int drop_delay_ms = tl_mode == 2 ? dynamic_interval_sec * 1000 : 1000 / fps;
                while (drop_delay_ms > 0 && s_state == RECORDER_RECORDING) {
                    int chunk_ms = (drop_delay_ms > 5000) ? 5000 : drop_delay_ms;
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(chunk_ms));
                    drop_delay_ms -= chunk_ms;
                }
            }
            continue;
        }

        /* Write frame to AVI */
        err = write_avi_frame(jpeg_data, jpeg_data_len);
        if (jpeg_copy) {
            free(jpeg_copy);
        } else {
            camera_return_fb(&frame);
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD write failed — closing segment, attempting cleanup");
            close_segment();  /* Save what we have */
            segment_open = false;

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
            storage_set_unavailable();
            s_state = RECORDER_ERROR;
            led_set_status(LED_ERROR);
            break;
        }

        total_bytes += jpeg_data_len;

        /* Flush to SD: every frame in timelapse (sparse), every ~1s in continuous */
static int flush_counter = 0;
if (timelapse) {
    fflush(s_seg.fp);
} else if (++flush_counter >= 10) {
    fflush(s_seg.fp);
    flush_counter = 0;
}

        /* Track stack high-water mark */
        s_stack_hwm = uxTaskGetStackHighWaterMark(NULL);

        /* Check segment duration */
        int64_t elapsed_ms = s_seg_elapsed_ms() - s_seg.start_ms;
        if (elapsed_ms >= (int64_t)cfg->segment_sec * 1000) {
            /* Close current segment */
            char completed_file[128];
            strncpy(completed_file, s_current_file, sizeof(completed_file) - 1);
            completed_file[sizeof(completed_file) - 1] = '\0';
            uint32_t completed_size = total_bytes;

            close_segment();
            segment_open = false;

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

        /* Frame rate control — break long sleeps to feed watchdog (TWDT=30s) */
        {
            int delay_ms;
            if (tl_mode == 2) {
                delay_ms = dynamic_interval_sec * 1000;
            } else if (timelapse) {
                delay_ms = (cfg->timelapse_interval_sec > 0) ? cfg->timelapse_interval_sec * 1000 : 30000;
            } else {
                delay_ms = 1000 / fps;
            }
            /* Sleep in 5-second chunks, feeding watchdog each iteration */
            while (delay_ms > 0 && s_state == RECORDER_RECORDING) {
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
    s_state = RECORDER_RECORDING;  /* Set state BEFORE creating task to avoid race */

/* Initialize motion detector if dynamic timelapse mode */
if (config_get()->timelapse_mode == 2) {
    motion_detector_init();
}

    BaseType_t ret = xTaskCreatePinnedToCore(
        recording_task,
        "recorder",
        10240,   /* increased for motion detection JPEG decode */
        NULL,
        configMAX_PRIORITIES - 2,   /* priority 5 on ESP-IDF default */
        &s_task_handle,
        0                            /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recording task");
        s_state = RECORDER_IDLE;
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Recording started");
    LOG_EVENT(LOG_EVENT_REC_STARTED, "recording started");
    return ESP_OK;
}

/**
 * @brief 停止录像，将状态设为IDLE并等待任务自行退出
 */
esp_err_t recorder_stop(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state != RECORDER_RECORDING && s_state != RECORDER_PAUSED) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    s_state = RECORDER_IDLE;   /* task checks this and exits */
    xSemaphoreGive(s_mutex);

    /* Wait for task to finish — recording uses 5s watchdog chunks, so wait at least 8s */
    int waited = 0;
    while (s_task_handle != NULL && waited < 8000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }

/* Cleanup motion detector */
motion_detector_deinit();

    if (s_task_handle != NULL) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Recording stopped");
    LOG_EVENT(LOG_EVENT_REC_STOPPED, "recording stopped");
    return ESP_OK;
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
 * @brief 喂任务看门狗，防止录像任务被看门狗复位
 */
void recorder_watchdog_feed(void)
{
    esp_task_wdt_reset();
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
        if (st.st_size > 0 && (size_t)st.st_size < (AVI_RIFF_HDR_SIZE + AVI_HDRL_TOTAL + 12)) {
            should_delete = true;
            ESP_LOGI(TAG, "Found undersized AVI (%ld bytes): %s", (long)st.st_size, fullpath);
        }

        /* Check RIFF header and size=0 (incomplete write) */
        if (!should_delete && n == 8 && memcmp(hdr, "RIFF", 4) == 0) {
            uint32_t riff_size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
            if (riff_size == 0) {
                should_delete = true;
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
