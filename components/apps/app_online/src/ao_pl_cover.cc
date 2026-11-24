/*
 * ao_pl_cover.cc —— 歌单详情左卡片的封面：工作线程抓取/解码 + PSRAM 单槽缓存
 *
 * 线程与产权（照 app_music/music_cover.cc 的老规矩，别改）：
 *   - 只有工作线程写 "out_*" 那组槽位，发布顺序 **先像素/宽高/id，再置 ready，
 *     最后清 busy** —— 反过来 LVGL 任务会读到"ready 已置、像素还没写"的中间态。
 *   - 只有 LVGL 任务释放 s_px / s_dsc（贴图的前提是先摘掉 lv_image 的 src）。
 *   - 请求结构里的 online_song_t 是**拷贝**：工作线程不读调用方（页面/模型）的内存。
 */
#include "ao_pl_cover.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "media_meta.h"
#include "ui_metrics.h"

#include "ao_common.h"

/* 卡片边长 184（设计稿），解码就按这个尺寸 —— 运行期不再让 LVGL 缩放 */
#define PL_COVER_MAX_DIM   UI_PX_H(184)
/* HTTP(TLS) + JPEG 软解都在这个任务里：TLS 握手吃 4~6 KB、软解再吃几 KB，
 * 所以给 12 KB（ao_work 是 16 KB，那边还要跑 JSON 解析）。任务是一次性的，用完就退。 */
#define PL_COVER_STACK     (12 * 1024)

typedef struct {
    int       id;
    int       src;
    char      cover_url[ONLINE_URL_MAX];
} pl_cover_req_t;

static const char *TAG = AO_TAG;

/* ---- 缓存的这一张 ---- */
static int              s_id = -1;      /* 0/负 = 没有 */
static uint8_t         *s_px;
static lv_image_dsc_t  *s_dsc;
static uint32_t         s_last_use;

/* ---- 目标（下一次要哪张） ---- */
static int              s_want_id = -1;
static int              s_want_src;
static char             s_want_url[ONLINE_URL_MAX];

/* ---- 工作线程的产出槽位 ---- */
static volatile bool    s_busy;
static volatile bool    s_ready;
static uint8_t         *s_out_px;
static int              s_out_w;
static int              s_out_h;
static volatile int     s_out_id = -1;

static ao_pl_cover_notify_t s_notify;
/* 屏上正用着这张（详情页开着）：期间不做闲置释放，见 ao_pl_cover_hold */
static bool             s_hold;

void ao_pl_cover_set_notify(ao_pl_cover_notify_t cb)
{
    s_notify = cb;
}

const lv_image_dsc_t *ao_pl_cover_dsc(void)
{
    return s_dsc;
}

int ao_pl_cover_current_id(void)
{
    return s_id;
}

/* 丢掉缓存里的这一张（LVGL 任务上下文；调用方负责先摘掉 lv_image 的 src） */
static void cover_cache_drop(void)
{
    if (s_dsc) {
        free(s_dsc);
        s_dsc = NULL;
    }
    if (s_px) {
        media_meta_cover_free(s_px);
        s_px = NULL;
    }
    s_id = -1;
}

static void cover_task(void *arg)
{
    pl_cover_req_t *req = (pl_cover_req_t *)arg;
    online_song_t song;
    memset(&song, 0, sizeof(song));
    song.id  = (uint32_t)req->id;
    song.src = (req->src == ONLINE_SRC_QQ) ? ONLINE_SRC_QQ : ONLINE_SRC_NETEASE;
    snprintf(song.cover_url, sizeof(song.cover_url), "%s", req->cover_url);

    uint8_t *img = NULL;
    size_t   len = 0;
    uint8_t *px  = NULL;
    int      w = 0, h = 0;

    esp_err_t err = online_song_cover_data(&song, &img, &len);
    if (err == ESP_OK && img && len) {
        err = media_meta_image_mem_rgb565(img, len, PL_COVER_MAX_DIM, &px, &w, &h);
    }
    if (img) {
        free(img);          /* 图片字节是 online_service 给的拷贝，用完就还 */
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "playlist cover %d unavailable (%s)", req->id,
                 esp_err_to_name(err));
        px = NULL;
    }
    s_out_px = px;
    s_out_w  = w;
    s_out_h  = h;
    s_out_id = req->id;
    s_ready  = true;        /* 发布顺序不可改，见文件头 */
    s_busy   = false;
    free(req);
    vTaskDelete(NULL);
}

static void cover_kick(void)
{
    if (s_busy || s_want_id <= 0) {
        return;
    }
    pl_cover_req_t *req = (pl_cover_req_t *)malloc(sizeof(*req));
    if (!req) {
        return;
    }
    req->id  = s_want_id;
    req->src = s_want_src;
    snprintf(req->cover_url, sizeof(req->cover_url), "%s", s_want_url);
    s_want_id = -1;          /* 一次性消费 */

    s_busy = true;
    if (xTaskCreate(cover_task, "pl_cover", PL_COVER_STACK, req, 2, NULL) != pdPASS) {
        s_busy = false;
        free(req);
        ESP_LOGW(TAG, "playlist cover task create failed");
    }
}

void ao_pl_cover_want(const online_song_t *song, uint32_t now)
{
    if (!song || song->id == 0 || !song->cover_url[0]) {
        return;
    }
    const int id = (int)song->id;
    if (id == s_id) {
        s_last_use = now;            /* 命中缓存：刷新"最近用过" */
        return;
    }
    if (s_busy && id == s_out_id) {
        return;                      /* 正在解的就是这张 */
    }
    s_want_id  = id;
    s_want_src = (int)song->src;
    snprintf(s_want_url, sizeof(s_want_url), "%s", song->cover_url);
    cover_kick();
}

void ao_pl_cover_tick(uint32_t now)
{
    /* 1) 上屏：工作线程刚解好的这张 */
    if (s_ready) {
        s_ready = false;
        uint8_t *px = s_out_px;
        const int w = s_out_w;
        const int h = s_out_h;
        const int id = s_out_id;
        s_out_px = NULL;
        s_out_id = -1;

        if (px && w > 0 && h > 0) {
            cover_cache_drop();
            lv_image_dsc_t *dsc = (lv_image_dsc_t *)calloc(1, sizeof(*dsc));
            if (!dsc) {
                media_meta_cover_free(px);
            } else {
                dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
                dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
                dsc->header.w      = (uint32_t)w;
                dsc->header.h      = (uint32_t)h;
                dsc->header.stride = (uint32_t)w * 2u;
                dsc->data_size     = (uint32_t)w * 2u * (uint32_t)h;
                dsc->data          = px;
                s_px  = px;
                s_dsc = dsc;
                s_id  = id;
                s_last_use = now;
                ESP_LOGI(TAG, "playlist cover %dx%d ready (id=%d)", w, h, id);
                if (s_notify) {
                    s_notify();      /* 页面自己去设 src（先摘旧 src 再换指针） */
                }
            }
        } else if (px) {
            media_meta_cover_free(px);
        }
    }

    /* 2) 没有人要这张了：闲置超时释放（需求：资源用一段时间就还给系统） */
    if (!s_hold && s_dsc && (uint32_t)(now - s_last_use) > AO_PL_COVER_IDLE_MS) {
        ESP_LOGI(TAG, "playlist cover idle > %u ms: released", (unsigned)AO_PL_COVER_IDLE_MS);
        if (s_notify) {
            s_notify();              /* 让页面把 src 摘掉，再释放像素 */
        }
        cover_cache_drop();
        s_last_use = now;
    }
}

void ao_pl_cover_hold(bool on)
{
    s_hold = on;
    if (!on) {
        /* 退出详情：闲置计时从"离开那一刻"重新开始（不是从上次贴图那刻） */
        s_last_use = lv_tick_get();
    }
}

void ao_pl_cover_release_all(void)
{
    /* 在途请求：不打断它（工作线程会自己结束），只把目标作废、结果到点丢弃 */
    s_want_id = -1;
    if (s_ready) {
        s_ready = false;
        if (s_out_px) {
            media_meta_cover_free(s_out_px);
            s_out_px = NULL;
        }
        s_out_id = -1;
    }
    cover_cache_drop();
}
