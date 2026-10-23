/*
 * gallery_view.cc —— 图库：图片查看器（显示 + 手势 + 信息弹窗）
 *
 * 交互（需求：双指放大缩小 / 双击放大还原 / 左右滑动切换 / 右上角 ⓘ 看详情）：
 *
 *   双指捏合   LV_EVENT_GESTURE + lv_event_get_gesture_type() == PINCH
 *              缩放 = pinch_base * lv_event_get_pinch_scale()（累计比例）
 *   双击       LVGL 没有内置双击，这里自己判：两次 PRESSED 间隔 < 350 ms 且落点
 *              相距 < 40 px → 在"适应屏幕"与"2×适应屏幕"之间切换
 *   单指拖动   放大后（zoom > fit）用 lv_indev_get_vect() 平移；平移量按图片
 *              与容器的大小差夹紧，拖不出边界
 *   左右滑     未放大时（zoom == fit）才翻页 —— 放大后单指是"平移"，不能又翻页
 *              否则拖过头就换图了。用 lv_indev_get_gesture_dir()（经典手势，
 *              不需要 LV_USE_GESTURE_RECOGNITION；那套只用来认 PINCH）
 *
 * 缩放实现：lv_image_set_scale(obj, 256) = 100%。**不用 transform_scale** ——
 * 那是绘制期变换、要单独一层 ARGB8888 缓冲，2000 px 的图每帧都重算会卡死；
 * lv_image 自己的 scale 是"绘制时按比例取样"，便宜得多（GIF 也一样，它是
 * lv_image 的子类，内部就是拿 draw_buf 当图源）。
 */
#include "gallery_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "media_meta.h"
#include "ui_ba.h"
#include "ui_metrics.h"

static const char *TAG = "gallery";

/* 相对"适应屏幕"最多放大几倍；再大就是马赛克 */
#define GAL_MAX_MULT      4

/*
 * GIF 的画布上限。1280 = 我们给 LVGL 的 AnimatedGIF.h 打补丁后的 MAX_WIDTH
 * （原值 480，gif.c 里硬判 iCanvasWidth > MAX_WIDTH 就 GIF_TOO_WIDE 直接失败）。
 * ⚠ 托管组件更新会把那个补丁还原成 480，届时宽 GIF 会又开始"load failed" ——
 *   下面的日志会把画布尺寸打出来，一眼能看出是不是这个原因。见 docs/gallery.md §12。
 */
#define GAL_GIF_MAX_W     1280
/* draw_buf 是整张画布的 RGB565：限到 ~2.3 MP（4.6 MB），再大 PSRAM 吃不住 */
#define GAL_GIF_MAX_PIXELS (UI_W * UI_H * 6)
/* 双击放大到"适应屏幕"的几倍 */
#define GAL_DBL_MULT      2
/* 双击判定 */
#define GAL_DBL_MS        350
#define GAL_DBL_DIST      40

static lv_obj_t       *s_stage;
static lv_obj_t       *s_img;        /* 静图对象（NULL = 当前不是静图） */
static lv_obj_t       *s_gif;        /* GIF 对象（NULL = 当前不是 GIF） */
static lv_obj_t       *s_hint;
static lv_obj_t       *s_caption;

static lv_image_dsc_t  s_img_dsc;    /* 静图描述符（data 指向 s_px，换图时改字段） */
static lv_image_dsc_t *s_gif_dsc;    /* GIF 描述符（要跟 gif 字节同生共死，所以 malloc） */

static uint8_t *s_px;                /* 静图像素（media_meta_cover_free 释放） */
static uint8_t *s_gif_bytes;         /* GIF 原始字节（free 释放） */
static int      s_src_w;             /* 图像本身的像素尺寸（解码后） */
static int      s_src_h;

static int  s_fit  = 256;            /* 适应屏幕的缩放（1/256） */
static int  s_zoom = 256;
static int  s_pan_x;
static int  s_pan_y;
static bool s_pinching;
static int  s_pinch_base;

static gal_item_t s_item;            /* 当前图片的条目（ⓘ 弹窗用） */
static bool       s_has_item;
static int        s_orig_w;          /* 原始分辨率（文件头读出来的） */
static int        s_orig_h;

static gal_page_cb_t s_page_cb;
static void         *s_page_user;
static lv_obj_t     *s_btn_prev;
static lv_obj_t     *s_btn_next;

/* 单击（切全屏）：先记时间戳，由 app 的轮询取走（见 gallery_view_take_pending_tap） */
static uint32_t      s_tap_ms;
/*
 * 必须**大于**双击窗口（GAL_DBL_MS = 350）：否则"双击放大"的第一下可能在
 * 第二下按到之前就被当成单击、把全屏切了（400 = 350 + 轮询粒度 50 的余量）。
 */
#define GAL_TAP_DELAY_MS  400

static uint32_t   s_last_press_ms;
static lv_point_t s_last_press_pt;

static ui_ba_dialog_t s_info_dlg;

/* ------------------------------------------------------------------ 缩放/平移 */

static lv_obj_t *active_obj(void)
{
    return s_gif ? s_gif : s_img;
}

static void pan_clamp(void)
{
    if (!s_stage) {
        return;
    }
    const int sw = lv_obj_get_content_width(s_stage);
    const int sh = lv_obj_get_content_height(s_stage);
    int max_x = (s_src_w * s_zoom / 256 - sw) / 2;
    int max_y = (s_src_h * s_zoom / 256 - sh) / 2;
    if (max_x < 0) {
        max_x = 0;
    }
    if (max_y < 0) {
        max_y = 0;
    }
    if (s_pan_x > max_x) {
        s_pan_x = max_x;
    }
    if (s_pan_x < -max_x) {
        s_pan_x = -max_x;
    }
    if (s_pan_y > max_y) {
        s_pan_y = max_y;
    }
    if (s_pan_y < -max_y) {
        s_pan_y = -max_y;
    }
}

static void apply_zoom(void)
{
    lv_obj_t *o = active_obj();
    if (!o) {
        return;
    }
    lv_image_set_scale(o, (uint32_t)s_zoom);
    pan_clamp();
    lv_obj_align(o, LV_ALIGN_CENTER, s_pan_x, s_pan_y);
}

/* 适应屏幕的缩放：小图不放大（最多 1:1），大图按比例缩到能整张看见 */
static void fit_compute(void)
{
    if (!s_stage || s_src_w <= 0 || s_src_h <= 0) {
        s_fit = 256;
        return;
    }
    const int sw = lv_obj_get_content_width(s_stage);
    const int sh = lv_obj_get_content_height(s_stage);
    if (sw <= 0 || sh <= 0) {
        s_fit = 256;
        return;
    }
    int fx = sw * 256 / s_src_w;
    int fy = sh * 256 / s_src_h;
    s_fit = (fx < fy) ? fx : fy;
    if (s_fit > 256) {
        s_fit = 256;      /* 不放大：小图居中显示 */
    }
    if (s_fit < 16) {
        s_fit = 16;       /* 超大图也别小到看不见 */
    }
}

static void zoom_reset(void)
{
    fit_compute();
    s_zoom  = s_fit;
    s_pan_x = 0;
    s_pan_y = 0;
    s_pinching = false;
    apply_zoom();
}

static void zoom_set(int zoom)
{
    const int lo = s_fit;
    const int hi = s_fit * GAL_MAX_MULT;
    if (zoom < lo) {
        zoom = lo;
    }
    if (zoom > hi) {
        zoom = hi;
    }
    s_zoom = zoom;
    apply_zoom();
}

/* ------------------------------------------------------------------ 释放 */

/* 只放内存（像素/GIF 字节/描述符），不碰 LVGL 对象 */
static void free_pixels(void)
{
    if (s_gif_dsc) {
        free(s_gif_dsc);
        s_gif_dsc = NULL;
    }
    if (s_gif_bytes) {
        free(s_gif_bytes);
        s_gif_bytes = NULL;
    }
    if (s_px) {
        media_meta_cover_free(s_px);
        s_px = NULL;
    }
    s_src_w = s_src_h = 0;
    s_zoom = s_fit = 256;
    s_pan_x = s_pan_y = 0;
}

/* 换图：先删掉旧的图像对象，再放内存 */
static void free_content(void)
{
    if (s_gif) {
        lv_obj_delete(s_gif);
        s_gif = NULL;
    }
    if (s_img) {
        lv_obj_delete(s_img);
        s_img = NULL;
    }
    free_pixels();
}

/* ------------------------------------------------------------------ 手势 */

static void on_gesture(lv_event_t *e)
{
    if (!s_stage) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    const lv_indev_gesture_type_t gt = lv_event_get_gesture_type(e);

    if (gt == LV_INDEV_GESTURE_PINCH) {
        if (!active_obj()) {
            return;      /* 还没有图（载入中/打不开）：捏合没意义 */
        }
        const lv_indev_gesture_state_t st =
            lv_event_get_gesture_state(e, LV_INDEV_GESTURE_PINCH);
        if (st == LV_INDEV_GESTURE_STATE_ENDED || st == LV_INDEV_GESTURE_STATE_CANCELED) {
            s_pinching = false;
            return;
        }
        const float scale = lv_event_get_pinch_scale(e);
        if (!s_pinching) {
            s_pinching   = true;
            s_pinch_base = s_zoom;
        }
        int z = (int)((float)s_pinch_base * scale + 0.5f);
        zoom_set(z);
        return;
    }

    /*
     * 左右滑**不再翻页**（2026-10 用户要求）：单指左右滑与双指缩放/拖动会打架 ——
     * 放大后单指本来就在平移，滑到头就变成"翻到下一张"，手感很糟。
     * 现在翻页只走左右两侧的箭头按钮。
     */
    (void)indev;
}

static void on_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_stage || !active_obj() || s_zoom <= s_fit + 4) {
        return;      /* 没放大：单指拖动不动画（留给翻页手势） */
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) {
        return;
    }
    s_pan_x += v.x;
    s_pan_y += v.y;
    apply_zoom();
}

static void on_pressed(lv_event_t *e)
{
    if (!s_stage || !active_obj()) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    const uint32_t now = lv_tick_get();
    const bool     dbl = (s_last_press_ms != 0) &&
                         (now - s_last_press_ms < GAL_DBL_MS) &&
                         (abs(p.x - s_last_press_pt.x) < GAL_DBL_DIST) &&
                         (abs(p.y - s_last_press_pt.y) < GAL_DBL_DIST);

    if (dbl) {
        s_last_press_ms = 0;    /* 吃掉这次，避免三击又触发 */
        s_tap_ms = 0;           /* 双击不是单击：别让 app 把全屏切了 */
        if (s_zoom > s_fit + 4) {
            zoom_reset();                              /* 已放大 → 还原 */
        } else {
            s_pan_x = 0;
            s_pan_y = 0;
            zoom_set(s_fit * GAL_DBL_MULT);            /* 未放大 → 放大 */
        }
    } else {
        s_last_press_ms = now;
        s_last_press_pt = p;
    }
}

/* 单击（松手且没拖动）：先记时间戳，等 app 的轮询确认"不是双击"后再切全屏 */
static void on_short_click(lv_event_t *e)
{
    (void)e;
    s_tap_ms = lv_tick_get();
}

static void on_size_changed(lv_event_t *e)
{
    (void)e;
    /* 容器大小变了（旋转/重新布局）：重算适应比例。放大状态保留相对倍数 */
    const int old_fit = s_fit;
    fit_compute();
    if (old_fit > 0 && old_fit != s_fit) {
        s_zoom = s_zoom * s_fit / old_fit;
        apply_zoom();
    }
}

/* ------------------------------------------------------------------ 信息弹窗 */

static void fit_bytes(uint32_t n, char *buf, size_t cap)
{
    if (n == 0) {
        snprintf(buf, cap, "未知");   /* 图还没解完（扫描阶段不 stat） */
        return;
    }
    if (n >= 1024u * 1024u) {
        snprintf(buf, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
    } else if (n >= 1024u) {
        snprintf(buf, cap, "%.1f KB", (double)n / 1024.0);
    } else {
        snprintf(buf, cap, "%u B", (unsigned)n);
    }
}

static void fit_time(uint32_t t, char *buf, size_t cap)
{
    if (t == 0) {
        snprintf(buf, cap, "未知（FAT 未记录）");
        return;
    }
    time_t    tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void info_close_cb(lv_event_t *e)
{
    ui_ba_dialog_t *d = (ui_ba_dialog_t *)lv_event_get_user_data(e);
    if (d) {
        ui_ba_close(d);
    }
}

void gallery_view_show_info(void)
{
    if (!s_has_item) {
        return;
    }
    if (s_info_dlg.card) {
        return;      /* 已经开着 */
    }

    char res[96];
    if (s_orig_w > 0 && s_orig_h > 0) {
        if (s_src_w > 0 && (s_src_w != s_orig_w || s_src_h != s_orig_h)) {
            snprintf(res, sizeof(res), "%d x %d（解码 %d x %d）",
                     s_orig_w, s_orig_h, s_src_w, s_src_h);
        } else {
            snprintf(res, sizeof(res), "%d x %d", s_orig_w, s_orig_h);
        }
    } else if (s_src_w > 0) {
        snprintf(res, sizeof(res), "%d x %d", s_src_w, s_src_h);
    } else {
        snprintf(res, sizeof(res), "未知");
    }

    char size[32];
    char mtime[40];
    fit_bytes(s_item.size, size, sizeof(size));
    fit_time(s_item.mtime, mtime, sizeof(mtime));

    s_info_dlg = ui_ba_open("图片信息", 62, LV_ALIGN_CENTER, 0);
    if (!s_info_dlg.card) {
        return;
    }

    lv_obj_t *t = lv_label_create(s_info_dlg.body);
    lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(BA_TEXT_BODY), LV_PART_MAIN);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_text_fmt(t,
                          "文件名    %s\n"
                          "分辨率    %s\n"
                          "大小      %s\n"
                          "格式      %s\n"
                          "修改日期  %s",
                          s_item.name, res, size, gallery_kind_name(s_item.kind), mtime);

    /* 路径单独一行：长，允许换行 */
    lv_obj_t *p = lv_label_create(s_info_dlg.body);
    lv_obj_set_style_text_font(p, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(p, lv_color_hex(BA_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(p, LV_PCT(100));
    lv_label_set_text_fmt(p, "%s", s_item.path);

    ui_ba_btn(s_info_dlg.btn_row, "好的", true, info_close_cb, &s_info_dlg);
}

/* ------------------------------------------------------------------ 对外 */

/* 一侧的翻页箭头：圆钮 + 符号，压在图像左/右边缘中间 */
static void on_arrow_clicked(lv_event_t *e)
{
    const int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_page_cb) {
        s_page_cb(dir, s_page_user);
    }
}

static lv_obj_t *make_arrow(lv_obj_t *parent, const char *sym, int dir)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, UI_PX_H(44), UI_PX_H(44));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_PART_MAIN);
    lv_obj_align(btn, (dir < 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 (dir < 0) ? UI_PX_W(6) : -UI_PX_W(6), 0);
    lv_obj_add_event_cb(btn, on_arrow_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)dir);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *gallery_view_create(lv_obj_t *parent)
{
    s_stage = lv_obj_create(parent);
    lv_obj_remove_style_all(s_stage);
    lv_obj_set_size(s_stage, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_stage, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_stage, UI_PX_H(12), LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_stage, true, LV_PART_MAIN);
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);   /* 自己实现平移，别让 LVGL 滚 */
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_PRESS_LOCK);      /* 手指滑出边界也别断手势 */

    lv_obj_add_event_cb(s_stage, on_pressed,   LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_stage, on_pressing,  LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_stage, on_gesture,   LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_stage, on_short_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_stage, on_size_changed, LV_EVENT_SIZE_CHANGED, NULL);

    /* 居中提示（载入中 / 没有图片 / 打不开）：暗底药丸，压在图上也不难读 */
    s_hint = lv_label_create(s_stage);
    lv_obj_set_style_text_font(s_hint, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hint, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_hint, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_hint, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_radius(s_hint, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_center(s_hint);
    lv_label_set_text(s_hint, "");

    /* 底部说明：文件名 + 序号 */
    s_caption = lv_label_create(s_stage);
    lv_obj_set_style_text_font(s_caption, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_caption, UI_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_caption, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_caption, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_caption, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_caption, UI_PX_H(3), LV_PART_MAIN);
    lv_obj_set_style_radius(s_caption, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(6));
    lv_label_set_text(s_caption, "");

    /* 左右箭头：半透明圆钮压在图像上（用户要求用它代替左右滑动手势） */
    s_btn_prev = make_arrow(s_stage, LV_SYMBOL_LEFT, -1);
    s_btn_next = make_arrow(s_stage, LV_SYMBOL_RIGHT, +1);

    return s_stage;
}

void gallery_view_set_still(uint8_t *px, int w, int h)
{
    free_content();
    if (!s_stage || !px || w <= 0 || h <= 0) {
        if (px) {
            media_meta_cover_free(px);
        }
        return;
    }
    s_px = px;
    s_src_w = w;
    s_src_h = h;

    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w      = (uint32_t)w;
    s_img_dsc.header.h      = (uint32_t)h;
    s_img_dsc.header.stride = (uint32_t)w * 2u;
    s_img_dsc.data_size     = (uint32_t)w * 2u * (uint32_t)h;
    s_img_dsc.data          = s_px;

    s_img = lv_image_create(s_stage);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);   /* 手势归 stage */
    zoom_reset();
}

/* GIF 逻辑屏幕描述符：宽/高各 2 字节小端，在文件头第 6..9 字节 */
static bool gif_canvas_size(const uint8_t *g, size_t len, int *w, int *h)
{
    if (!g || len < 13 || g[0] != 'G' || g[1] != 'I' || g[2] != 'F') {
        return false;
    }
    *w = (int)g[6] | ((int)g[7] << 8);
    *h = (int)g[8] | ((int)g[9] << 8);
    return (*w > 0 && *h > 0);
}

bool gallery_view_set_gif(uint8_t *gif, size_t len)
{
    free_content();
    if (!s_stage || !gif || len == 0) {
        if (gif) {
            free(gif);
        }
        return false;
    }

    /*
     * 先自己读画布尺寸做预检 —— LVGL 的 lv_gif 失败时只有一句 "Couldn't load the source"，
     * 各种原因（太宽/太大/不是 GIF）长得一模一样。这里先把原因分清楚，日志和提示都给具体值。
     */
    int cw = 0, ch = 0;
    if (!gif_canvas_size(gif, len, &cw, &ch)) {
        ESP_LOGW(TAG, "gif: bad header (%u bytes)", (unsigned)len);
        free(gif);
        gallery_view_set_hint("GIF 文件损坏");
        return false;
    }
    if (cw > GAL_GIF_MAX_W) {
        ESP_LOGW(TAG, "gif too wide: canvas %dx%d > %d (AnimatedGIF MAX_WIDTH)", cw, ch,
                 GAL_GIF_MAX_W);
        free(gif);
        gallery_view_set_hint("GIF 太宽（宽度上限 1280）");
        return false;
    }
    if ((int64_t)cw * ch > (int64_t)GAL_GIF_MAX_PIXELS) {
        ESP_LOGW(TAG, "gif too large: canvas %dx%d (%d KB RGB565)", cw, ch,
                 cw * ch * 2 / 1024);
        free(gif);
        gallery_view_set_hint("GIF 太大");
        return false;
    }

    s_gif_bytes = gif;

    s_gif_dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
    if (!s_gif_dsc) {
        free(s_gif_bytes);
        s_gif_bytes = NULL;
        return false;
    }
    s_gif_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    s_gif_dsc->data         = s_gif_bytes;
    s_gif_dsc->data_size    = (uint32_t)len;

    s_gif = lv_gif_create(s_stage);
    /* RGB565 帧缓冲：ARGB8888 的 800x480 要 1.5 MB，RGB565 只要 750 KB；
     * 必须在 set_src 之前设，否则会先按默认格式分配一次再重来 */
    lv_gif_set_color_format(s_gif, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(s_gif, s_gif_dsc);
    lv_obj_remove_flag(s_gif, LV_OBJ_FLAG_CLICKABLE);

    if (!lv_gif_is_loaded(s_gif)) {
        ESP_LOGW(TAG, "gif load failed (canvas %dx%d, %u bytes)", cw, ch, (unsigned)len);
        gallery_view_set_hint("GIF 打不开");
        lv_obj_delete(s_gif);
        s_gif = NULL;
        free(s_gif_dsc);
        s_gif_dsc = NULL;
        free(s_gif_bytes);
        s_gif_bytes = NULL;
        return false;
    }

    /* 画布尺寸：lv_gif 会把对象调成画布大小 */
    s_src_w = lv_obj_get_width(s_gif);
    s_src_h = lv_obj_get_height(s_gif);
    if (s_src_w <= 0 || s_src_h <= 0) {
        s_src_w = 256;
        s_src_h = 256;
    }
    zoom_reset();
    return true;
}

void gallery_view_set_hint(const char *text)
{
    if (!s_hint) {
        return;
    }
    lv_label_set_text(s_hint, text ? text : "");
    if (text && text[0]) {
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_hint);
        lv_obj_move_foreground(s_hint);
    } else {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void gallery_view_set_caption(const char *text)
{
    if (s_caption) {
        lv_label_set_text(s_caption, text ? text : "");
    }
}

void gallery_view_set_orig_size(int w, int h)
{
    s_orig_w = (w > 0) ? w : 0;
    s_orig_h = (h > 0) ? h : 0;
}

/* 扫描阶段不 stat（O(N²) 的坑），所以这两个值在解码完成时由工作线程补上 */
void gallery_view_note_file_stat(uint32_t size, uint32_t mtime)
{
    if (size) {
        s_item.size = size;
    }
    if (mtime) {
        s_item.mtime = mtime;
    }
}

void gallery_view_set_item(const gal_item_t *item)
{
    if (item) {
        s_item = *item;
        s_has_item = true;
    } else {
        memset(&s_item, 0, sizeof(s_item));
        s_has_item = false;
    }
    s_orig_w = s_orig_h = 0;
}

void gallery_view_set_page_cb(gal_page_cb_t cb, void *user)
{
    s_page_cb   = cb;
    s_page_user = user;
}

void gallery_view_set_pager_visible(bool visible)
{
    lv_obj_t *btns[2] = { s_btn_prev, s_btn_next };
    for (int i = 0; i < 2; i++) {
        if (!btns[i]) {
            continue;
        }
        if (visible) {
            lv_obj_remove_flag(btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(btns[i]);
        } else {
            lv_obj_add_flag(btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void gallery_view_set_caption_visible(bool visible)
{
    if (!s_caption) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_caption, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_caption, LV_OBJ_FLAG_HIDDEN);
    }
}

bool gallery_view_take_pending_tap(void)
{
    if (!s_tap_ms) {
        return false;
    }
    if (lv_tick_elaps(s_tap_ms) < GAL_TAP_DELAY_MS) {
        return false;
    }
    s_tap_ms = 0;
    return true;
}

void gallery_view_destroy(void)
{
    /*
     * ⚠ 这里**绝对不能**再 lv_obj_delete(s_img/s_gif)：本函数是从最外层 screen 的
     *   LV_EVENT_DELETE 里调的，而 LVGL 删对象是先删子对象、再给父对象发 DELETE ——
     *   轮到我们时 s_img/s_gif 已经是被释放的内存，再去删就是 use-after-free。
     *   所以只放**我们自己 malloc 的**像素/GIF 字节/描述符，指针一律清空。
     */
    free_pixels();
    s_img = NULL;
    s_gif = NULL;
    s_stage = NULL;
    s_hint = NULL;
    s_caption = NULL;
    s_has_item = false;
    s_btn_prev = NULL;
    s_btn_next = NULL;
    s_page_cb   = NULL;
    s_page_user = NULL;
    s_tap_ms = 0;
    memset(&s_info_dlg, 0, sizeof(s_info_dlg));
}
