/*
 * music_player_bg.cc —— 播放器页背景图
 *
 * 需求（2026-10 用户）："将 assets/player_background.jpg 作为播放器背景图"。
 *
 * 做法与首页背景（ui_core/ui_home_bg.cc）保持一致，理由也一样：
 *   1. 图当 screen 的 bg_image（原尺寸居中 = 满屏），不额外建铺满屏幕的对象；
 *   2. 压暗烘进像素：播放器页左封面、右歌词/标题都是浅色，背景太亮会看不清。
 *      解码完在 PSRAM 里一次性乘上暗化系数，之后只做普通 RGB565 贴图，
 *      不再用 bg_image_recolor_opa（那会让 LVGL 每帧走 recolor_only()）；
 *   3. 800x480 JPEG 软解要几百毫秒 → 后台任务（core 0）解，解完持 LVGL 锁贴；
 *      像素缓冲常驻 PSRAM（约 770 KB），进页面不再重解。
 *
 * 与本文件同目录的 music_cover.cc 是"专辑封面"（每首歌不同、会换），
 * 两者不要混：那个是可变的，这个是页面墙纸。
 */
#include "music_player_bg.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lv_adapter.h"
#include "ui_metrics.h"
#include "media_meta.h"

static const char *TAG = "app_music";

/* 构建时嵌进来的 JPEG（app_music/CMakeLists.txt 的 target_add_binary_data ... RENAME_TO） */
extern "C" const uint8_t _binary_player_bg_jpg_start[];
extern "C" const uint8_t _binary_player_bg_jpg_end[];

/*
 * 压暗程度：0 = 原图，255 = 全黑。
 * 120（约 47%）与首页壁纸同一档 —— 再亮歌词/标题会糊，再暗就看不出图了。
 * 这个值只在解码后一次性使用，不再参与每帧绘制。
 */
#define PLAYER_BG_DIM_OPA   120
#define PLAYER_BG_STACK     (8 * 1024)
#define PLAYER_BG_LOCK_MS   2000

/* 把压暗一次性烘进 RGB565 像素（等价于按 opa 叠一层黑）。只在解码任务里跑一次。 */
static void dim_rgb565_in_place(uint8_t *px, int w, int h, int opa)
{
    if (!px || w <= 0 || h <= 0 || opa <= 0) {
        return;
    }
    if (opa > 255) {
        opa = 255;
    }
    const uint32_t keep = 255u - (uint32_t)opa;
    uint16_t *p = (uint16_t *)px;
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const uint16_t v = p[i];
        uint32_t r = (v >> 11) & 0x1F;
        uint32_t g = (v >> 5) & 0x3F;
        uint32_t b = v & 0x1F;
        r = (r * keep + 127u) / 255u;
        g = (g * keep + 127u) / 255u;
        b = (b * keep + 127u) / 255u;
        p[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static uint8_t        *s_px;
static lv_image_dsc_t *s_dsc;
static lv_obj_t       *s_scr;      /* 当前播放器页 screen（NULL = 页面不在） */
static bool            s_started;

/* 把图贴到播放器页 screen 上（没有图或没有页面就什么都不做）。持 LVGL 锁调用。 */
static void player_bg_apply(void)
{
    if (!s_scr || !s_dsc) {
        return;
    }
    lv_obj_set_style_bg_image_src(s_scr, s_dsc, LV_PART_MAIN);
    lv_obj_set_style_bg_image_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_image_tiled(s_scr, false, LV_PART_MAIN);   /* 原尺寸居中 = 满屏 */
    /* 压暗已经烘进像素（见 dim_rgb565_in_place），这里不再设 recolor。 */
}

/* 页面销毁时把指针清掉，否则解码任务会往已释放的 screen 上贴图（use-after-free） */
static void player_bg_deleted(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE && s_scr == lv_event_get_target(e)) {
        s_scr = NULL;
    }
}

static void player_bg_task(void *arg)
{
    (void)arg;
    const size_t len = (size_t)(_binary_player_bg_jpg_end - _binary_player_bg_jpg_start);
    uint8_t *px = NULL;
    int w = 0;
    int h = 0;

    /* max_dim 传 UI_W：图是 800x480，解码器在长边 <= max_dim 时不缩放 */
    esp_err_t err = media_meta_image_mem_rgb565(_binary_player_bg_jpg_start, len, UI_W,
                                                &px, &w, &h);
    if (err != ESP_OK || !px || w <= 0 || h <= 0) {
        ESP_LOGW(TAG, "player_background.jpg decode failed: %s -- 页面用纯色背景",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    /* 一次性压暗：之后每帧只做普通 RGB565 贴图，不再走 LVGL 的 recolor_only()。 */
    dim_rgb565_in_place(px, w, h, PLAYER_BG_DIM_OPA);

    s_px = px;
    s_dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
    if (!s_dsc) {
        ESP_LOGW(TAG, "player background dsc alloc failed");
        media_meta_cover_free(px);
        s_px = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc->header.w      = (uint32_t)w;
    s_dsc->header.h      = (uint32_t)h;
    s_dsc->header.stride = (uint32_t)w * 2u;
    s_dsc->data_size     = (uint32_t)w * 2u * (uint32_t)h;
    s_dsc->data          = s_px;
    ESP_LOGI(TAG, "player background decoded %dx%d", w, h);

    /* 贴图是 LVGL 操作，本任务不是 LVGL 任务 —— 按 AGENTS 第 6 节走适配器锁 */
    if (esp_lv_adapter_lock(PLAYER_BG_LOCK_MS) == ESP_OK) {
        player_bg_apply();
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGW(TAG, "player background: LVGL lock timeout（下次进页面再贴）");
    }
    vTaskDelete(NULL);
}

void music_player_bg_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    /* 优先级 2：与首页壁纸同档，低于启动图解码；开机后台解，进页面就有 */
    if (xTaskCreatePinnedToCore(player_bg_task, "player_bg", PLAYER_BG_STACK, NULL, 2,
                                NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "player_bg task create failed -- 页面用纯色背景");
    }
}

void music_player_bg_attach(lv_obj_t *scr)
{
    s_scr = scr;
    if (scr) {
        lv_obj_add_event_cb(scr, player_bg_deleted, LV_EVENT_DELETE, NULL);
    }
    player_bg_apply();
}
