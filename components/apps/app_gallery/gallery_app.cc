/*
 * gallery_app.cc —— 图库 App：页面骨架 + 列表状态 + 注册到桌面
 *
 * 需求（2026-10 用户）：
 *   "添加图片浏览功能，主界面添加"图库"APP，点开后可浏览存储卡的常见图片格式
 *    （jpg/jpeg/png/gif），支持双指放大缩小双击放大/还原，支持左右滑动切换图片，
 *    支持右上角信息图标查看图片详细信息（分辨率，大小，格式，修改日期）"
 *
 * 分工（见 gallery_internal.h）：
 *   gallery_scan.cc    扫卡（工作线程，不在 LVGL 里做 SD IO）
 *   gallery_decode.cc  解码（工作线程，core 0）
 *   本文件             页面骨架、当前索引、50 ms 轮询把解码结果贴上去、翻页
 *   gallery_view.cc    查看器与手势
 *
 * ⚠ 扫描与解码**都不在 LVGL 任务里**：一张 4000x3000 的 JPEG 软解几百毫秒，
 *   600 张图的目录扫一遍也要几百毫秒，放 LVGL 里就是"整屏卡住"。
 */
#include "app_gallery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gallery_internal.h"
#include "ui_app_registry.h"
#include "ui_core.h"
#include "ui_icons.h"
#include "ui_metrics.h"

static const char *TAG = "gallery";

#define GAL_POLL_MS      50
#define GAL_TOP_BTN      40          /* 右上角 ⓘ 的边长（参考分辨率下） */
#define GAL_SCAN_STACK   (6 * 1024)

static lv_obj_t   *s_scr;
static lv_timer_t *s_poll;

/* 全屏看图（单击切换）：记住要收起来的几个组件 */
static lv_obj_t *s_title;
static lv_obj_t *s_info_btn;
static lv_obj_t *s_body;
static bool      s_immersive;
static int       s_body_h0;        /* 普通模式下 body 的高度，退出全屏时还原 */

static gal_item_t *s_items;          /* malloc 的数组（扫描线程写，UI 只读） */
static int         s_count;
static int         s_index;
static volatile bool s_scan_busy;
static volatile bool s_scan_done;
static bool        s_list_ready;

/*
 * "从文件管理点某张图进来"的目标：只存路径，等列表就绪后再换成下标。
 * 为什么不在打开时就找下标：扫卡是异步的（几百毫秒），那时列表可能还不存在。
 */
static char s_pending_open[GAL_PATH_MAX];

/* 扫描进度提示用（只在数字变化时改标签，见 poll_cb） */
static int s_hint_found = -1;

/* ------------------------------------------------------------------ 扫描线程 */

static void scan_task(void *arg)
{
    (void)arg;
    gal_item_t *items = NULL;
    const int n = gallery_scan(&items, "/sdcard");
    if (n < 0) {
        free(items);
        s_items = NULL;
        s_count = 0;
    } else {
        s_items = items;
        s_count = n;
    }
    s_scan_done = true;        /* 写完全部结果再置标志（UI 侧只读） */
    vTaskDelete(NULL);
}

static void scan_start(bool force)
{
    if (!force && (s_items || s_scan_busy)) {
        return;
    }
    if (s_items) {
        free(s_items);
        s_items = NULL;
    }
    s_count = 0;
    s_scan_done = false;
    s_scan_busy = true;
    s_list_ready = false;
    if (xTaskCreatePinnedToCore(scan_task, "gal_scan", GAL_SCAN_STACK, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "scan task create failed");
        s_scan_busy = false;
    }
}

/* ------------------------------------------------------------------ 显示第 idx 张 */

static void caption_update(void)
{
    if (s_count <= 0) {
        gallery_view_set_caption("");
        return;
    }
    char buf[GAL_NAME_MAX + 64];
    snprintf(buf, sizeof(buf), "%d / %d    %s", s_index + 1, s_count, s_items[s_index].name);
    gallery_view_set_caption(buf);
}

static void show_index(int idx)
{
    if (s_count <= 0) {
        return;
    }
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= s_count) {
        idx = s_count - 1;
    }
    s_index = idx;

    gallery_view_set_item(&s_items[idx]);
    gallery_view_set_orig_size(0, 0);
    gallery_view_set_pager_visible(s_count > 1);   /* 只有 1 张就不画箭头 */
    gallery_view_set_hint("载入中…");
    caption_update();
    gallery_decode_request(idx, &s_items[idx]);
}

/*
 * 把 s_pending_open 解析成 s_index（只有列表就绪后才能算）。
 * 找不到（比如那张图在扫描深度之外、或刚被删）就退回 0，不至于打不开页面。
 */
static void resolve_pending_open(void)
{
    if (!s_pending_open[0]) {
        return;
    }
    if (s_count > 0 && s_items) {
        for (int i = 0; i < s_count; i++) {
            if (strcmp(s_items[i].path, s_pending_open) == 0) {
                s_index = i;
                break;
            }
        }
    }
    s_pending_open[0] = '\0';
}

static void on_page(int dir, void *user)
{
    (void)user;
    if (s_count <= 0) {
        return;
    }
    int next = s_index + dir;
    if (next < 0) {
        next = s_count - 1;      /* 循环：滑到第一张再往左回到最后一张 */
    } else if (next >= s_count) {
        next = 0;
    }
    show_index(next);
}

/* ------------------------------------------------------------------ 全屏看图 */

/*
 * 单击图像在"全屏看图"与"普通视图"之间切换（用户 2026-10 要求：
 * "图库界面应单击隐藏状态栏等其他组件全屏查看，再次单击恢复"）。
 *
 * 全屏 = 收起状态栏 + 页面标题 + ⓘ + 底部文件名条 + 全局返回钮，
 *        并把查看器从"标题栏以下"撑到整个屏幕（含状态栏那一条）。
 * 左右箭头**保留** —— 那是唯一的翻页入口，收起来就没法看下一张了。
 *
 * ⚠ 退出页面时**必须**恢复：状态栏与返回钮是全局的，藏了不还就是整个系统没了。
 */
static void immersive_set(bool on)
{
    if (s_immersive == on) {
        return;
    }
    s_immersive = on;

    lv_obj_t *chrome[2] = { s_title, s_info_btn };
    for (int i = 0; i < 2; i++) {
        if (!chrome[i]) {
            continue;
        }
        if (on) {
            lv_obj_add_flag(chrome[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(chrome[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /*
     * 铺满的实现是**临时把 screen 的 padding 清零**，而不是给 body 算绝对坐标 ——
     * body 平时是"100% 宽 + 底部对齐"，它的 x 其实等于 pad_left(8)、宽等于
     * UI_W-16；直接 set_pos(0,0) 只在"没有 padding"时才对，且 align 的锚点
     * 仍留在 BOTTOM_MID，父容器一改尺寸就又飞回去。
     */
    if (s_scr) {
        if (on) {
            lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_top(s_scr, 0, LV_PART_MAIN);
        } else {
            lv_obj_set_style_pad_all(s_scr, UI_PAGE_PAD, LV_PART_MAIN);
            lv_obj_set_style_pad_top(s_scr, UI_TITLEBAR_Y, LV_PART_MAIN);
        }
    }
    if (s_body) {
        if (on) {
            lv_obj_set_size(s_body, UI_W, UI_H);
            lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 0);
        } else {
            lv_obj_set_size(s_body, LV_PCT(100), s_body_h0);
            lv_obj_align(s_body, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
        lv_obj_update_layout(s_body);
    }
    gallery_view_set_caption_visible(!on);
    ui_status_bar_set_visible(!on);
    ui_back_button_suppress(on);
}

/* ------------------------------------------------------------------ 轮询 */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_scr) {
        return;
    }

    /* 单击（确认不是双击）→ 切全屏 */
    if (gallery_view_take_pending_tap()) {
        immersive_set(!s_immersive);
    }

    if (s_scan_busy && !s_scan_done) {
        /*
         * 扫描期间**把进度显示出来**：扫大卡时原来是屏幕一动不动的一行"正在扫描…"，
         * 体感像卡死；现在数字在涨，用户知道它活着。
         * 只在数量变化时改标签（LVGL 标签订正 + 重排不是免费的，50 ms 一次不该白花）。
         */
        const int found = gallery_scan_progress();
        if (found != s_hint_found) {
            s_hint_found = found;
            char buf[64];
            snprintf(buf, sizeof(buf), "正在扫描存储卡… 已找到 %d 张", found);
            gallery_view_set_hint(buf);
        }
        return;
    }
    if (s_scan_busy && s_scan_done) {
        s_scan_busy = false;
        s_list_ready = true;
        if (s_count <= 0) {
            gallery_view_set_hint("没有找到图片（插卡后在 /sdcard 下放 jpg/png/gif）");
            gallery_view_set_caption("");
            return;
        }
        ESP_LOGI(TAG, "ready: %d image(s)", s_count);
        resolve_pending_open();
        show_index(s_index);
        return;
    }
    if (!s_list_ready || s_count <= 0) {
        return;
    }

    bool     is_gif = false;
    uint8_t *px = NULL;
    uint8_t *gif = NULL;
    size_t   gif_len = 0;
    int      w = 0, h = 0, ow = 0, oh = 0;
    uint32_t fsize = 0, fmtime = 0;

    /*
     * take() 只在"刚解完的正好是当前这张"时返回 true，并把结果（可能为空 =
     * 解码失败）一次性移交过来；返回 false 就是"还没好 / 已经取过 / 用户又翻页了"，
     * 下一拍再看即可。
     */
    if (!gallery_decode_take(s_index, &is_gif, &px, &w, &h, &gif, &gif_len, &ow, &oh,
                             &fsize, &fmtime)) {
        return;
    }

    gallery_view_set_orig_size(ow, oh);
    gallery_view_note_file_stat(fsize, fmtime);   /* ⓘ 里的大小/修改日期（扫描阶段不取） */
    if (is_gif) {
        if (gif) {
            /* set_gif 自己会挂"太宽/太大/损坏"的具体提示，成功时返回 true 才清空 */
            if (gallery_view_set_gif(gif, gif_len)) {
                gallery_view_set_hint("");
            }
        } else {
            gallery_view_set_hint("GIF 读取失败");
        }
    } else if (px) {
        gallery_view_set_still(px, w, h);
        gallery_view_set_hint("");
    } else {
        /* 失败原因分不清的话，用户只会看到同一句"这张打不开" */
        switch (gallery_decode_last_error()) {
        case GAL_ERR_TOO_BIG:
            gallery_view_set_hint("文件太大（超过 12 MB）");
            break;
        case GAL_ERR_UNSUPPORTED:
            gallery_view_set_hint("不支持的格式（WebP / HEIF / 渐进式 JPEG）");
            break;
        case GAL_ERR_NO_MEM:
            gallery_view_set_hint("内存不足，这张解不了");
            break;
        default:
            gallery_view_set_hint("这张打不开（数据可能已损坏）");
            break;
        }
    }
}

/* ------------------------------------------------------------------ 页面 */

/* 右上角 ⓘ：圆形按钮 + 文字 i（项目不用 emoji，也懒得为这一个图标烘位图） */
static void info_btn_cb(lv_event_t *e)
{
    (void)e;
    gallery_view_show_info();
}

static void on_scr_deleted(lv_event_t *e)
{
    (void)e;
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    /*
     * ⚠ 全屏状态下离开页面（点返回 / 边缘返回手势）：状态栏和返回钮是**全局**的，
     *   不还回去整个系统就少一条状态栏、少一个返回键。所以先无条件恢复，再销毁。
     */
    if (s_immersive) {
        s_immersive = false;
        ui_status_bar_set_visible(true);
        ui_back_button_suppress(false);
    }
    gallery_view_destroy();
    gallery_decode_cancel();
    s_scr = NULL;
    s_list_ready = false;
    s_title = NULL;
    s_info_btn = NULL;
    s_body = NULL;
}

lv_obj_t *gallery_page_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_screen_apply_default_bg(scr);   /* 深色不透明底（见 ui_core.h 的说明） */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* 先 pad_all 再 pad_top（顺序反了会把状态栏留白覆盖掉，见播放器页的注释） */
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_TITLEBAR_Y, LV_PART_MAIN);

    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(s_title, "图库");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, UI_HEADER_TEXT_X, ui_titlebar_text_dy());

    lv_obj_t *info = lv_button_create(scr);
    s_info_btn = info;
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, UI_PX_H(GAL_TOP_BTN), UI_PX_H(GAL_TOP_BTN));
    lv_obj_set_style_radius(info, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(info, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(info, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(info, LV_ALIGN_TOP_RIGHT, 0, (UI_TITLEBAR_H - UI_PX_H(GAL_TOP_BTN)) / 2);
    lv_obj_add_event_cb(info, info_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sym = lv_label_create(info);
    lv_obj_set_style_text_font(sym, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(sym, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(sym, "i");
    lv_obj_center(sym);

    /* 查看器：占满标题栏以下的区域（让开返回钮悬出来的 UI_BACK_BTN_DIP） */
    lv_obj_t *body = lv_obj_create(scr);
    s_body = body;
    lv_obj_remove_style_all(body);
    const int body_h = UI_H - UI_TITLEBAR_Y - UI_PAGE_PAD - (UI_TITLEBAR_H + UI_BACK_BTN_DIP);
    lv_obj_set_size(body, LV_PCT(100), body_h > 0 ? body_h : UI_PCT_Y(50));
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* 记下普通模式的高度：从全屏切回来时照原样还原（位置由 BOTTOM_MID 对齐算） */
    s_body_h0 = (body_h > 0) ? body_h : UI_PCT_Y(50);

    gallery_view_create(body);
    gallery_view_set_page_cb(on_page, NULL);
    gallery_view_set_pager_visible(false);   /* 列表就绪/多于 1 张时才显示箭头 */
    s_immersive = false;                     /* 每次进页面都从普通视图开始 */

    s_scr = scr;

    if (!s_poll) {
        s_poll = lv_timer_create(poll_cb, GAL_POLL_MS, NULL);
    }
    gallery_decode_start();

    if (s_items && s_count > 0) {
        /*
         * 列表已经扫过了（同一个页面反复进出走这里）：直接显示，**不要再置 false** ——
         * 以前这里无条件 s_list_ready = false，而 scan_start(false) 看到 s_items 非空
         * 就提前返回，于是永远等不到"扫完"，第二次进图库会一直停在"正在扫描…"。
         */
        s_list_ready = true;
        resolve_pending_open();      /* 从文件管理点进来的那张 */
        show_index(s_index);
    } else {
        s_index = 0;
        s_list_ready = false;
        s_hint_found = -1;
        gallery_view_set_hint("正在扫描存储卡… 已找到 0 张");
        scan_start(false);           /* 首次进来才扫（异步，结果由 poll 取回） */
    }

    lv_obj_add_event_cb(scr, on_scr_deleted, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ------------------------------------------------------------------ 对外 */

void gallery_open_path(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    snprintf(s_pending_open, sizeof(s_pending_open), "%s", path);
    /* 描述符是拷贝进路由栈的，栈上临时量是安全的（同 music_router_open_player） */
    page_desc_t page = { "gallery", gallery_page_build, NULL, NULL, false };
    ui_router_push(&page);
}

/* ------------------------------------------------------------------ 注册 */

void app_gallery_register(void)
{
    static const app_desc_t gal = {
        "gallery", "图库", LV_SYMBOL_IMAGE, 0x6C5CE7,
        gallery_page_build, NULL, NULL, 35, 0,
        &gallery_logo,
    };
    ui_app_register(&gal);
}
