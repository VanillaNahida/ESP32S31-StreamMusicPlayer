/*
 * music_cover.cc —— 封面解码线程 + 像素/描述符产权
 *
 * 搬迁自 app_music.cc 的 122–144（类型与变量）、1872–1968（线程）、2376–2425（贴图）。
 * 注释原样保留（它们记录了栈溢出、LVGL 图片缓存按指针做键这些坑）。
 */
#include "music_cover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "media_meta.h"
#include "storage_service.h"
#include "ui_metrics.h"      /* UI_PX_H：封面显示边长 */

#include "music_state.h"
#include "music_theme.h"     /* 封面主色：主题色跟随封面取色 */

static const char *TAG = "app_music";

/* --- 封面：异步解码（读文件 + 软解 JPEG 不能放在 LVGL 任务里） --- */
#define COVER_MAX_DIM    UI_PX_H(240)   /* 封面显示边长（超出的按 1/2^n 降采样） */
/* JPEG 软解用栈不浅（项目里出过 0xa5a5a5a5 栈溢出，见 11_遇到的问题.md F3），
 * 给足 8 KB；像素缓冲走 PSRAM（CONFIG_SPIRAM_USE_MALLOC），不占内部 RAM */
#define COVER_TASK_STACK (8 * 1024)

typedef struct {
    int  idx;
    bool from_mem;                  /* true = data/len 是内存里的图片字节（在线歌曲） */
    uint8_t *data;                  /* from_mem 时有效：本请求**独占**，用完 free */
    size_t   len;
    char path[STORAGE_PATH_MAX];    /* from_mem=false：本地歌曲音频路径 */
} cover_req_t;

static void cover_task(void *arg);   /* 前向声明：cover_kick 在它上面 */

/*
 * 屏上这张封面属于哪首（-1 = 没有）—— 只有 `music_cover_place()` 会写。
 *
 * 为什么需要它（2026-10 用户报"加载的时候点播放按钮，歌曲封面可能会丢失"）：
 * 起播一首时会先 `cover_show(NULL,0,0)` 把屏上封面释放掉、再重新请求解码。而
 * **同一首重起播**（在线地址过期重连、加载中点了播放导致重来一次）也走这条路径，
 * 于是封面被白白丢掉；更糟的是在线封面的字节缓冲 `on_cover_data` 是**一次性消费**的
 * （起播时已拷走/释放），重来一次就再也拿不到图片 → 封面**永久为空**。
 * 有了这个下标，起播时就能判断"屏上这张就是目标曲目" → 原地留着，不动它。
 */
static int s_disp_idx = -1;

/* 起播 `idx` 时问一句：需要重新解码封面吗？（false = 屏上那张就是它，别动） */
bool music_cover_needs_reload(int idx)
{
    music_state_t *st = music_state();
    if (idx < 0) {
        return true;
    }
    if (s_disp_idx == idx && st->cover_dsc && st->cover_pixels) {
        /* 屏上这张就是目标曲目：把可能排着的"别的下标"的请求也撤掉 */
        if (st->cover_want != idx) {
            st->cover_want = -1;
        }
        return false;
    }
    return true;
}

bool music_cover_have_pixels(void)
{
    music_state_t *st = music_state();
    return st->cover_dsc && st->cover_pixels;
}

void *music_cover_dsc(void)
{
    return music_state()->cover_dsc;
}

uint8_t *music_cover_pixels(void)
{
    return music_state()->cover_pixels;
}

bool music_cover_source_is_online(void)
{
    return music_state()->on_mode;
}

/* 丢掉没人认领的解码结果（换曲太快、页面已退出等） */
void music_cover_reap(void)
{
    music_state_t *st = music_state();
    if (st->cover_ready) {
        st->cover_ready = false;
        if (st->cover_out) {
            media_meta_cover_free(st->cover_out);
            st->cover_out = NULL;
        }
    }
}

int music_cover_want(void)
{
    return music_state()->cover_want;
}

void music_cover_request(int idx)
{
    music_state()->cover_want = idx;
}

/* 有请求且空闲就起解码任务（必须在 LVGL 任务上下文调用） */
void music_cover_kick(void)
{
    music_state_t *st = music_state();
    if (st->cover_busy || st->cover_want < 0) {
        return;
    }
    const int idx = st->cover_want;
    st->cover_want = -1;

    cover_req_t *req = (cover_req_t *)malloc(sizeof(cover_req_t));
    if (!req) {
        return;
    }
    req->idx = idx;
    if (st->on_mode) {
        /* 在线歌曲：封面字节已经在 PSRAM 里（不落 SD）。
         * 把缓冲**转移**给请求：请求独占、解码线程用完释放，
         * 之后切换任何歌曲都不会再碰它。 */
        if (!st->on_cover_data) {
            free(req);              /* 没抓到封面：不排队解码（占位图兜底） */
            return;
        }
        req->from_mem = true;
        req->data     = st->on_cover_data;
        req->len      = st->on_cover_len;
        st->on_cover_data = NULL;
        st->on_cover_len  = 0;
    } else {
        /* 本地歌曲：从音频文件里取内嵌封面。
         * 把路径**拷进请求结构**再交给工作线程：线程只认这块内存，
         * 不再去读 storage_service 的曲目表（扫描任务可能正在重建它） */
        const track_t *t = storage_service_track((size_t)idx);
        if (!t) {
            free(req);
            return;
        }
        req->from_mem = false;
        req->data     = NULL;
        req->len      = 0;
        snprintf(req->path, sizeof(req->path), "%s", t->path);
    }

    st->cover_busy = true;
    if (xTaskCreate(cover_task, "cover", COVER_TASK_STACK, req, 2, NULL) != pdPASS) {
        st->cover_busy = false;
        free(req);
        ESP_LOGW(TAG, "cover task create failed");
    }
}

/*
 * 封面解码任务：只读文件（本地）/ 解内存缓冲（在线）+ 解 JPEG/PNG + 写全局标志，
 * **绝不碰 LVGL**。结果由 tick（LVGL 任务）搬到对象上。
 *
 * ★ 发布顺序不可改：先写像素/宽高/下标 → 再置 ready → **最后**清 busy。
 *   反过来 tick 会读到"ready 已置、像素还没写"的中间态。
 */
static void cover_task(void *arg)
{
    music_state_t *st = music_state();
    cover_req_t *req = (cover_req_t *)arg;
    uint8_t *px = NULL;
    int w = 0, h = 0;

    esp_err_t ret = req->from_mem
                        ? media_meta_image_mem_rgb565(req->data, req->len,
                                                      COVER_MAX_DIM, &px, &w, &h)
                        : media_meta_cover_rgb565(req->path, COVER_MAX_DIM, &px, &w, &h);
    if (req->data) {
        free(req->data);        /* 内存封面：本请求独占的拷贝，用完释放 */
    }
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "no cover for idx=%d (%s)", req->idx, esp_err_to_name(ret));
        px = NULL;
    }
    /*
     * 主题色：**在像素刚刚就绪时算**（同一块 RGB565，PSRAM 里现成的）。
     * 放在这里而不是 LVGL 侧，是因为逐像素统计有几百微秒级开销，
     * 解码线程里做完全不影响出声；放在 cover_ready 之前写，
     * UI 贴新封面时就拿到配套的主题色，不会先亮一下旧颜色。
     */
    st->cover_out     = px;
    st->cover_out_w   = w;
    st->cover_out_h   = h;
    st->cover_out_idx = req->idx;
    st->cover_accent  = (px && w > 0 && h > 0)
                            ? music_theme_rgb_from_cover((const uint16_t *)px, w, h)
                            : 0;
    st->cover_ready   = true;
    st->cover_busy    = false;
    free(req);
    vTaskDelete(NULL);
}

music_cover_pump_t music_cover_pump(int keep_idx, uint8_t **px, int *w, int *h)
{
    music_state_t *st = music_state();
    if (!st->cover_ready) {
        return MUSIC_COVER_PUMP_EMPTY;
    }
    st->cover_ready = false;
    uint8_t *out = st->cover_out;
    const int out_w = st->cover_out_w;
    const int out_h = st->cover_out_h;
    const int out_idx = st->cover_out_idx;
    st->cover_out = NULL;
    if (out_idx != keep_idx) {
        /* 下标对不上：这是上一首的封面，丢 */
        if (out) {
            media_meta_cover_free(out);
        }
        return MUSIC_COVER_PUMP_STALE;
    }
    *px = out;
    *w  = out_w;
    *h  = out_h;
    return MUSIC_COVER_PUMP_OK;
}

void music_cover_place(uint8_t *px, int w, int h,
                       music_cover_set_src_cb set_src,
                       music_cover_set_visible_cb set_visible,
                       void *user)
{
    music_state_t *st = music_state();

    if (set_src) {
        set_src(user, NULL);                     /* 先摘掉旧的，才能安全释放 */
    }
    if (st->cover_dsc) {
        free(st->cover_dsc);
        st->cover_dsc = NULL;
    }
    if (st->cover_pixels) {
        media_meta_cover_free(st->cover_pixels);
        st->cover_pixels = NULL;
    }
    s_disp_idx = -1;      /* 屏上没封面了（下面贴新图时会记上新的下标） */

    if (px && set_src && w > 0 && h > 0) {
        lv_image_dsc_t *dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
        if (!dsc) {
            media_meta_cover_free(px);
            st->cover_accent = 0;       /* 贴不上图了：主题色跟着回落默认蔚蓝 */
            if (set_visible) {
                set_visible(user, false);
            }
            return;
        }
        st->cover_pixels = px;
        dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
        dsc->header.w      = (uint32_t)w;
        dsc->header.h      = (uint32_t)h;
        dsc->header.stride = (uint32_t)w * 2u;
        dsc->data_size     = (uint32_t)w * 2u * (uint32_t)h;
        dsc->data          = st->cover_pixels;
        st->cover_dsc      = dsc;
        /* 记下"屏上这张是谁的封面"：起播时靠它判断要不要重新解码（见文件头） */
        s_disp_idx = st->index;

        set_src(user, dsc);
        if (set_visible) {
            set_visible(user, true);
        }
        return;
    }

    if (px) {
        media_meta_cover_free(px);      /* 页面/对象没了：没人要 */
    }
    /* 没有封面了：主题色回落到默认蔚蓝（下一 tick 的 accent_apply 会重设样式） */
    st->cover_accent = 0;
    if (set_visible) {
        set_visible(user, false);
    }
}
