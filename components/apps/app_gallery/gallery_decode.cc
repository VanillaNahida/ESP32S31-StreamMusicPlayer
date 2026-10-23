/*
 * gallery_decode.cc —— 图库：解码工作线程
 *
 * 为什么必须有独立线程：一张 4000x3000 的 JPEG 软解要几百毫秒（media_meta 的
 * esp_new_jpeg 路径），在 LVGL 任务里做就是"整屏卡住"。所以：
 *
 *   LVGL 任务                     工作线程（core 0）
 *   gallery_decode_request(i) ──►  s_req_seq++ / 记下路径
 *   （50 ms 轮询）                decode: 静图 -> RGB565（PSRAM）
 *   gallery_decode_take(i) ◄────  结果放进 s_done_*
 *
 * 只保留**最新一次**请求：连续快速翻页时旧的那张解完直接丢掉（take 里比对 seq），
 * 不会排队把用户甩在后面。
 *
 * 静图走 media_meta_image_file_rgb565()（按魔数分派 JPEG/PNG，与封面同一套解码器）；
 * GIF 不在这里解 —— lv_gif 需要**原始字节**自己逐帧解，所以这里只把文件读进 PSRAM
 * 交给 view。
 */
#include "gallery_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "media_meta.h"
#include "ui_metrics.h"

static const char *TAG = "gallery";

/*
 * 静图解码的长边上限：取屏幕宽（800）。
 * media_meta_photo_file_rgb565() 的缩法是"按 1/2^n 降到长边 <= 2*max_dim"，
 * 所以 4000x3000 的照片会解成 1000x750（1.5 MB），1920x1080 解成 960x540 ——
 * 屏上放大到 4 倍也够看，PSRAM 不会被一张照片吃掉。
 */
#define GAL_DECODE_MAX_DIM   (UI_W)
/* GIF 原文件上限：整份读进 PSRAM（lv_gif 按需逐帧解），太小会漏大图，太大怕吃满 */
#define GAL_GIF_MAX_BYTES    (6u * 1024u * 1024u)
#define GAL_DECODE_STACK     (10 * 1024)
#define GAL_DECODE_PRIO      3
#define GAL_DECODE_CORE      0

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;

static int      s_req_idx = -1;
static char     s_req_path[GAL_PATH_MAX];
static gal_kind_t s_req_kind;
static uint32_t s_req_seq;

static uint32_t s_done_seq;
static int      s_done_idx = -1;
static bool     s_done_taken = true;
static bool     s_done_is_gif;
static uint8_t *s_done_px;
static int      s_done_w;
static int      s_done_h;
static uint8_t *s_done_gif;
static size_t   s_done_gif_len;
static int      s_done_ow;    /* 原始分辨率（文件头读出来的，不是解码后的） */
static int      s_done_oh;
static uint32_t s_done_size;  /* 文件大小/修改时间（扫描阶段不取，见 gallery_scan.cc） */
static uint32_t s_done_mtime;
static volatile uint8_t s_last_err;   /* 最近一次失败分类（gal_err_t），成功时清 0 */

static volatile bool s_stop;

/* 锁内：丢掉还没被取走的结果 */
static void done_clear_locked(void)
{
    if (s_done_px) {
        media_meta_cover_free(s_done_px);
        s_done_px = NULL;
    }
    if (s_done_gif) {
        free(s_done_gif);
        s_done_gif = NULL;
    }
    s_done_w = s_done_h = 0;
    s_done_ow = s_done_oh = 0;
    s_done_size = s_done_mtime = 0;
    s_last_err = GAL_ERR_NONE;
    s_done_gif_len = 0;
    s_done_is_gif = false;
    s_done_idx = -1;
    s_done_taken = true;
}

/* 读整个 GIF 文件进 PSRAM（优先 PSRAM，失败退内部 RAM） */
static uint8_t *read_whole_file(const char *path, size_t *out_len)
{
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > GAL_GIF_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc((size_t)sz);
    }
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

static void decode_task(void *arg)
{
    (void)arg;
    uint32_t last_tried = 0xFFFFFFFFu;

    for (;;) {
        if (s_stop) {
            break;
        }

        int        idx = -1;
        gal_kind_t kind = GAL_KIND_UNKNOWN;
        uint32_t   seq = 0;
        char       path[GAL_PATH_MAX];

        xSemaphoreTake(s_lock, portMAX_DELAY);
        idx  = s_req_idx;
        seq  = s_req_seq;
        kind = s_req_kind;
        snprintf(path, sizeof(path), "%s", s_req_path);
        xSemaphoreGive(s_lock);

        if (idx < 0 || seq == last_tried) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_tried = seq;

        uint8_t *px = NULL;
        int      w = 0, h = 0;
        uint8_t *gif = NULL;
        size_t   gif_len = 0;
        int      ow = 0, oh = 0;

        /* 原始分辨率：只读文件头（工作线程里读，别让 LVGL 任务碰 SD） */
        gallery_image_size(path, kind, &ow, &oh);

        /*
         * 文件大小/修改时间只在这里 stat 一次（扫描阶段**不** stat，那是 O(N²)，
         * 见 gallery_scan.cc 的长注释）。反正这张图马上要打开，多这一次可以忽略。
         */
        uint32_t fsize = 0;
        uint32_t fmtime = 0;
        {
            struct stat st;
            if (stat(path, &st) == 0) {
                fsize  = (uint32_t)st.st_size;
                fmtime = (uint32_t)st.st_mtime;
            }
        }

        if (kind == GAL_KIND_GIF) {
            gif = read_whole_file(path, &gif_len);
            if (!gif) {
                ESP_LOGW(TAG, "gif read failed: %s", path);
            }
        } else {
            /* 大图专用：JPEG 优先硬解，PNG 逐行流式，装不下才回落软解 */
            esp_err_t err = media_meta_photo_file_rgb565(path, GAL_DECODE_MAX_DIM, &px, &w, &h);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "decode failed (%s): %s", esp_err_to_name(err), path);
                px = NULL;
                switch (err) {
                case ESP_ERR_INVALID_SIZE:  s_last_err = GAL_ERR_TOO_BIG;     break;
                case ESP_ERR_NOT_SUPPORTED: s_last_err = GAL_ERR_UNSUPPORTED; break;
                case ESP_ERR_NO_MEM:        s_last_err = GAL_ERR_NO_MEM;      break;
                default:                    s_last_err = GAL_ERR_DECODE;      break;
                }
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_req_seq == seq) {
            /* 还是最新那次请求：收下结果（旧的先释放） */
            done_clear_locked();
            s_done_seq     = seq;
            s_done_idx     = idx;
            s_done_taken   = false;
            s_done_is_gif  = (kind == GAL_KIND_GIF);
            s_done_px      = px;
            s_done_w       = w;
            s_done_h       = h;
            s_done_gif     = gif;
            s_done_gif_len = gif_len;
            s_done_ow      = ow;
            s_done_oh      = oh;
            s_done_size    = fsize;
            s_done_mtime   = fmtime;
            px = NULL;
            gif = NULL;
        }
        xSemaphoreGive(s_lock);

        /* 过期的结果（用户已经翻到别的图了）直接扔掉 */
        if (px) {
            media_meta_cover_free(px);
        }
        if (gif) {
            free(gif);
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    done_clear_locked();
    xSemaphoreGive(s_lock);
    s_task = NULL;
    vTaskDelete(NULL);
}

void gallery_decode_start(void)
{
    if (s_task || s_stop) {
        return;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGW(TAG, "decode mutex create failed");
            return;
        }
    }
    if (xTaskCreatePinnedToCore(decode_task, "gal_decode", GAL_DECODE_STACK, NULL,
                                GAL_DECODE_PRIO, &s_task, GAL_DECODE_CORE) != pdPASS) {
        ESP_LOGW(TAG, "decode task create failed");
        s_task = NULL;
    }
}

void gallery_decode_stop(void)
{
    if (!s_task) {
        return;
    }
    s_stop = true;
    /* 等它自己退出（最多几百毫秒：正在解的那张会跑完再看到 s_stop） */
    for (int i = 0; i < 100 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_stop = false;
}

void gallery_decode_request(int idx, const gal_item_t *item)
{
    if (!s_lock || !item) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    done_clear_locked();
    s_req_idx = idx;
    snprintf(s_req_path, sizeof(s_req_path), "%s", item->path);
    s_req_kind = item->kind;
    s_req_seq++;
    xSemaphoreGive(s_lock);
}

uint8_t gallery_decode_last_error(void)
{
    return s_last_err;
}

bool gallery_decode_take(int idx, bool *is_gif, uint8_t **px, int *w, int *h,
                         uint8_t **gif, size_t *gif_len, int *orig_w, int *orig_h,
                         uint32_t *size, uint32_t *mtime)
{
    if (!s_lock) {
        return false;
    }
    bool got = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_done_taken && s_done_idx == idx && s_done_seq == s_req_seq) {
        s_done_taken = true;
        got = true;
        if (is_gif) {
            *is_gif = s_done_is_gif;
        }
        if (px) {
            *px = s_done_px;
        }
        if (w) {
            *w = s_done_w;
        }
        if (h) {
            *h = s_done_h;
        }
        if (gif) {
            *gif = s_done_gif;
        }
        if (gif_len) {
            *gif_len = s_done_gif_len;
        }
        if (orig_w) {
            *orig_w = s_done_ow;
        }
        if (orig_h) {
            *orig_h = s_done_oh;
        }
        if (size) {
            *size = s_done_size;
        }
        if (mtime) {
            *mtime = s_done_mtime;
        }
        s_done_ow = s_done_oh = 0;
        s_done_size = s_done_mtime = 0;
        /* 产权移交：这里不释放，view 换图时自己释放 */
        s_done_px = NULL;
        s_done_gif = NULL;
        s_done_w = s_done_h = 0;
        s_done_gif_len = 0;
    }
    xSemaphoreGive(s_lock);
    return got;
}

void gallery_decode_cancel(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    done_clear_locked();
    s_req_idx = -1;
    s_req_seq++;
    xSemaphoreGive(s_lock);
}
