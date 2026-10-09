/*
 * splash.cc —— 开机启动图（assets/init.jpg）
 *
 * 需求（2026-10 用户）：
 *   "使用 assets/init.jpg 图片作为启动加载初始化图，开机后初始化完屏幕就显示这个
 *    图片和 Toast，Toast 内容为：正在初始化... 初始化完成后，经过一个过渡动画，
 *    加载首页。"
 *
 * 几个刻意的设计：
 *
 * 1. **图片是 JPEG，运行时软解**（不是编译期烘成 C 数组）。本工程没有图片编码工具
 *    （`tools/font/venv` 里没有 PIL，见 AGENTS §9），把 355 KB 的 JPEG 编译成 .c
 *    源码会膨胀到 ~6 MB、还拖慢编译。这里走现成的解码服务
 *    `media_meta_image_mem_rgb565()`（封面那条路用的同一套 JPEG/PNG 解码器），
 *    输出 RGB565 + PSRAM，正好是 LVGL 要的格式。
 *
 * 2. **解码在后台任务里、与后面的初始化并行**（钉在 core 0；core 1 是 LVGL 任务，
 *    别让几百毫秒的软解去抢它的帧）。等屏幕起来时图多半已经好了，于是"屏幕一亮
 *    就是这张图"，中间不会先黑一下再闪出来。
 *
 * 3. **盖在 lv_layer_top() 的最上层**：状态栏 / 返回键 / 首页都在它下面，所以它一
 *    出现就是整屏图。Toast 是**后调**的 —— `ui_toast()` 内部会
 *    `lv_obj_move_foreground()` 把 Toast 提到最前，于是"图在最下面、Toast 在最上面"
 *    自动成立（顺序不能反）。
 *
 * 4. **过渡动画**：初始化完成后，启动图淡出（整棵子树的 opa，见 ui_core 里同一套做法）
 *    同时轻轻上移，露出下面已经建好的首页；动画跑完再删对象、还回那 ~770 KB PSRAM。
 *
 * 5. **两头都兜住**：最短显示 SPLASH_MIN_MS（太快一闪而过像故障），最长
 *    SPLASH_MAX_MS（后台初始化卡住也不能一直盖着首页）。
 */
#include "splash.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "ui_core.h"
#include "ui_metrics.h"
#include "media_meta.h"

static const char *TAG = "splash";

/*
 * 构建时嵌进来的 JPEG（main/CMakeLists.txt 里 target_add_binary_data ... BINARY
 * RENAME_TO boot_splash_jpg）。BINARY 方式**不会**在末尾补 '\0'，所以长度要用
 * start/end 两个符号相减。
 */
extern "C" const uint8_t _binary_boot_splash_jpg_start[];
extern "C" const uint8_t _binary_boot_splash_jpg_end[];

#define SPLASH_MIN_MS     1100     /* 至少显示这么久 */
#define SPLASH_MAX_MS     8000    /* 上限：后台初始化再慢也不能一直盖着首页 */
#define SPLASH_FADE_MS    460     /* 淡出 + 上滑时长 */
#define SPLASH_SLIDE_PX   UI_PX_H(28)   /* 淡出时上移的距离（"首页从下面顶上来"） */
#define SPLASH_POLL_MS    100     /* 轮询"图解码好了吗 / 初始化完了吗" */
#define SPLASH_TOAST_MS   6000    /* Toast 时长；真正收掉是在过渡开始时（ui_toast_hide） */
#define SPLASH_DECODE_STACK (8 * 1024)  /* 与 app_music 的封面解码任务同量级 */

static uint8_t        *s_px;          /* 解码后的 RGB565 像素（PSRAM，media_meta_cover_free 释放） */
static int             s_px_w;
static int             s_px_h;
static volatile bool   s_decoded;     /* 解码任务已结束（成功与否都算结束） */
static volatile bool   s_init_done;   /* 后台初始化完成标志 */
static lv_obj_t       *s_scr;         /* 全屏启动图容器 */
static lv_obj_t       *s_img;         /* 里面的图（解码好才建） */
static lv_image_dsc_t *s_dsc;         /* 图片描述符（堆上，随浮层释放） */
static lv_timer_t     *s_tick;
static uint32_t        s_shown_tick;
static bool            s_fading;
static bool            s_decode_started;

/* ---------------------------------------------------------------- 解码（后台任务） */

static void splash_decode_task(void *arg)
{
    (void)arg;
    const size_t len = (size_t)(_binary_boot_splash_jpg_end - _binary_boot_splash_jpg_start);
    uint8_t *px = NULL;
    int w = 0;
    int h = 0;
    const int64_t t0 = esp_timer_get_time();
    /*
     * max_dim 传 UI_W(800)：图正好是 800x480，解码器在 `long_side <= max_dim` 时
     * **不缩放**（media_meta.cc 的判定），所以拿到的是原始分辨率、只做一次解码。
     */
    esp_err_t err = media_meta_image_mem_rgb565(_binary_boot_splash_jpg_start, len, UI_W,
                                                &px, &w, &h);
    if (err == ESP_OK && px && w > 0 && h > 0) {
        s_px   = px;
        s_px_w = w;
        s_px_h = h;
        ESP_LOGI(TAG, "init.jpg decoded %dx%d in %lld ms", w, h,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGW(TAG, "init.jpg decode failed: %s (%u bytes) —— 启动图只显示背景色",
                 esp_err_to_name(err), (unsigned)len);
    }
    s_decoded = true;
    vTaskDelete(NULL);
}

void boot_splash_decode_start(void)
{
    if (s_decode_started) {
        return;
    }
    s_decode_started = true;
    /* 钉 core 0（app_main 所在核）：core 1 跑 LVGL，别让软解抢帧 */
    if (xTaskCreatePinnedToCore(splash_decode_task, "splash_dec", SPLASH_DECODE_STACK,
                                NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "splash decode task create failed");
        s_decoded = true;          /* 不挡住后续流程 */
    }
}

/* ---------------------------------------------------------------- 显示（LVGL 任务） */

/* 解码好了就把图贴上去（建 lv_image + 描述符）。只在 LVGL 任务里调用。 */
static void splash_attach(void)
{
    if (s_img || !s_px || !s_scr) {
        return;
    }
    s_dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
    if (!s_dsc) {
        ESP_LOGW(TAG, "splash dsc alloc failed");
        return;
    }
    s_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc->header.w      = (uint32_t)s_px_w;
    s_dsc->header.h      = (uint32_t)s_px_h;
    s_dsc->header.stride = (uint32_t)s_px_w * 2u;
    s_dsc->data_size     = (uint32_t)s_px_w * 2u * (uint32_t)s_px_h;
    s_dsc->data          = s_px;

    s_img = lv_image_create(s_scr);
    lv_image_set_src(s_img, s_dsc);
    lv_obj_center(s_img);
}

static void splash_timer_stop(void)
{
    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
}

/* 淡出：整棵子树的透明度 + 轻轻上移（与 ui_core 的 popup 动画同一套做法） */
static void splash_fade_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    if (v < 0) {
        v = 0;
    } else if (v > 255) {
        v = 255;
    }
    lv_obj_set_style_opa(o, (lv_opa_t)v, LV_PART_MAIN);
    lv_obj_set_style_translate_y(o, (int32_t)((255 - v) * -SPLASH_SLIDE_PX / 255), LV_PART_MAIN);
}

/* 动画回调里不能同步删对象（LVGL 正在遍历动画/对象），统一走 async */
static void splash_free_cb(void *arg)
{
    (void)arg;
    free(s_dsc);
    s_dsc = NULL;
    if (s_px) {
        media_meta_cover_free(s_px);      /* 还回那 ~770 KB PSRAM */
        s_px = NULL;
    }
    ESP_LOGI(TAG, "splash resources released");
}

static void splash_out_done_cb(lv_anim_t *a)
{
    (void)a;
    splash_timer_stop();
    if (s_img) {
        /* 先把图源摘掉：LVGL 立刻关掉解码器/缓存项，之后才谈得上释放像素与描述符 */
        lv_image_set_src(s_img, NULL);
    }
    if (s_scr) {
        lv_obj_delete_async(s_scr);
        s_scr = NULL;
    }
    s_img = NULL;
    /*
     * 释放排在删除之后（lv_obj_delete_async 与 lv_async_call 是同一条异步队列，
     * 按调用顺序执行）：等对象真的删掉了，再放描述符和像素。
     */
    lv_async_call(splash_free_cb, NULL);
    ESP_LOGI(TAG, "splash gone —— 首页可见");
}

static void splash_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_scr) {
        splash_timer_stop();
        return;
    }
    if (!s_img && s_decoded) {
        splash_attach();
    }
    if (s_fading) {
        return;
    }
    const uint32_t el = lv_tick_elaps(s_shown_tick);
    const bool time_up = (s_init_done && el >= SPLASH_MIN_MS) || (el >= SPLASH_MAX_MS);
    if (!time_up) {
        return;
    }

    s_fading = true;
    ui_toast_hide();          /* "正在初始化..." 跟着一起收掉，别飘在首页上 */
    if (el >= SPLASH_MAX_MS && !s_init_done) {
        ESP_LOGW(TAG, "splash timeout %u ms (后台初始化还没报完成)", (unsigned)el);
    } else {
        ESP_LOGI(TAG, "splash out after %u ms", (unsigned)el);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_scr);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, SPLASH_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_ready_cb(&a, splash_out_done_cb);
    lv_anim_start(&a);
}

void boot_splash_show(void)
{
    if (s_scr) {
        return;
    }
    lv_obj_t *top = lv_layer_top();
    s_scr = lv_obj_create(top);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, LV_PCT(100), LV_PCT(100));
    /* 图没解码完时先铺背景色（比纯黑更接近 App 观感），图一好就盖上 */
    lv_obj_set_style_bg_color(s_scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    /* 吃掉触摸：启动期间手指落在屏幕上的那几下不该点到底下的首页 */
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);

    s_shown_tick = lv_tick_get();
    s_fading     = false;

    splash_attach();          /* 解码已就绪就直接贴（否则由 splash_tick 补） */
    splash_timer_stop();
    s_tick = lv_timer_create(splash_tick, SPLASH_POLL_MS, NULL);

    /*
     * ⚠ 顺序不能反：Toast 必须**在启动图之后**调 —— ui_toast() 内部会
     *   lv_obj_move_foreground() 把自己提到最前，于是"图盖住状态栏、Toast 盖住图"。
     */
    ui_toast("正在初始化...", SPLASH_TOAST_MS);
    ESP_LOGI(TAG, "splash shown (decode done=%d)", (int)s_decoded);
}

void boot_splash_init_done(void)
{
    s_init_done = true;
}

bool boot_splash_active(void)
{
    return s_scr != NULL;
}
