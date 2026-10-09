/*
 * ui_home_bg.cc —— 首页背景图（assets/background.jpg）
 *
 * 需求（2026-10 用户）："将这个 assets/background.jpg 作为主界面背景图"。
 *
 * 实现要点：
 *
 * 1. **图直接当 screen 的背景图**（`bg_image_src`），不额外建一个铺满屏幕的对象：
 *    LVGL 的 bg_image 在 `bg_image_tiled = false` 时按**原尺寸居中**画在对象自己的
 *    区域上（见 lv_draw_rect.c 的 `draw_bg_img`），图正好 800x480 = 整屏，于是天然
 *    满屏；也就不用操心 screen 的 pad_top 会让子对象坐标偏移那件事。
 *
 * 2. **压暗一层**：这张壁纸是亮色天空，而首页的时钟/图标名都是浅色（0xEDE6D8 /
 *    UI_COLOR_FG），直接压上去看不清。用 `bg_image_recolor` = 黑 + 一定 opa 做一层
 *    暗色滤镜（同一段源码里 recolor 是传给图片绘制的），一个常量就能调浓淡，
 *    比再叠一个半透明对象省一层绘制。
 *
 * 3. **后台解码**：800x480 的 JPEG 软解要几百毫秒（与启动图同一套 media_meta 解码器）。
 *    解码任务钉 core 0（core 1 是 LVGL），解完按 AGENTS §6 用 `esp_lv_adapter_lock()`
 *    把图贴到首页；首页还没建好（或已经销毁）就先放着，等 ui_home_bg_bind() 那边贴。
 *
 * 4. 像素缓冲**一直留着**（≈770 KB PSRAM）：背景图是常驻的，释放了还得重解。
 */
#include "ui_home_bg.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lv_adapter.h"
#include "ui_metrics.h"
#include "media_meta.h"

static const char *TAG = "ui_home_bg";

/* 构建时嵌进来的 JPEG（ui_core/CMakeLists.txt 的 target_add_binary_data ... RENAME_TO） */
extern "C" const uint8_t _binary_home_bg_jpg_start[];
extern "C" const uint8_t _binary_home_bg_jpg_end[];

/*
 * 壁纸压暗程度：0 = 原图（亮色天空，浅色文字看不清），255 = 全黑。
 * ≈47% 是我按"壁纸还看得出、时钟和图标名读得清"取的中间值；嫌暗/嫌亮改这一个数。
 */
#define HOME_BG_DIM_OPA     120
#define HOME_BG_TASK_STACK  (8 * 1024)   /* 与 app_music 的封面解码任务同量级 */
#define HOME_BG_LOCK_MS     2000         /* 等 LVGL 锁的上限（等不到就放弃这次贴图） */

static uint8_t        *s_px;
static int             s_px_w;
static int             s_px_h;
static lv_image_dsc_t *s_dsc;
static bool            s_started;
static lv_obj_t       *s_scr;        /* 当前首页 screen（NULL = 首页不在） */

/* 把已经解好的图贴到首页（没有图或没有首页就什么都不做）。LVGL 任务/持锁上下文。 */
static void home_bg_apply(void)
{
    if (!s_scr || !s_dsc) {
        return;
    }
    lv_obj_set_style_bg_image_src(s_scr, s_dsc, LV_PART_MAIN);
    lv_obj_set_style_bg_image_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_image_tiled(s_scr, false, LV_PART_MAIN);   /* 原尺寸居中 = 满屏 */
    /* 亮色壁纸压暗一层，保证浅色时钟/图标名的可读性 */
    lv_obj_set_style_bg_image_recolor(s_scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_image_recolor_opa(s_scr, HOME_BG_DIM_OPA, LV_PART_MAIN);
}

static void home_bg_task(void *arg)
{
    (void)arg;
    const size_t len = (size_t)(_binary_home_bg_jpg_end - _binary_home_bg_jpg_start);
    uint8_t *px = NULL;
    int w = 0;
    int h = 0;

    /* max_dim 传 UI_W：图正好 800x480，解码器在 long_side <= max_dim 时不缩放 */
    esp_err_t err = media_meta_image_mem_rgb565(_binary_home_bg_jpg_start, len, UI_W,
                                                &px, &w, &h);
    if (err != ESP_OK || !px || w <= 0 || h <= 0) {
        ESP_LOGW(TAG, "background.jpg decode failed: %s —— 首页用纯色背景",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_px   = px;
    s_px_w = w;
    s_px_h = h;
    s_dsc  = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
    if (!s_dsc) {
        ESP_LOGW(TAG, "background dsc alloc failed —— 首页用纯色背景");
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
    ESP_LOGI(TAG, "home background decoded %dx%d", w, h);

    /*
     * 贴图这一步是 LVGL 操作，而本任务不是 LVGL 任务 —— 按 AGENTS §6 走适配器锁
     * （首页此刻多半已经建好；没建好就等 ui_home_bg_bind() 那边贴）。
     */
    if (esp_lv_adapter_lock(HOME_BG_LOCK_MS) == ESP_OK) {
        home_bg_apply();
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGW(TAG, "LVGL lock timeout —— 背景图等首页重建时再贴");
    }
    vTaskDelete(NULL);
}

void ui_home_bg_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    /* 优先级 2：比启动图解码（3）低一点 —— 用户先看到的是启动图 */
    if (xTaskCreatePinnedToCore(home_bg_task, "home_bg", HOME_BG_TASK_STACK, NULL, 2,
                                NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "home_bg task create failed —— 首页用纯色背景");
    }
}

void ui_home_bg_bind(lv_obj_t *home_scr)
{
    s_scr = home_scr;
    /* 图还没解好 / 解码失败 / 首页已销毁 —— home_bg_apply() 自己会判空跳过；
     * 解好那一刻若首页已经在，解码任务会自己贴上来（见 home_bg_task）。 */
    home_bg_apply();
}
