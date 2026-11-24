/*
 * ao_style.cc —— 在线音乐的设计系统实现（见 ao_style.h 的出处说明）
 *
 * 两条实现纪律：
 *   1) **渐变描述符必须有静态/堆存储期**：LVGL 的样式里存的是指针，绘制时才 memcpy
 *      进 draw_dsc。写成函数里的栈局部量 = 出函数就是野指针（画出花屏/崩）。
 *      颜色固定的用文件级 static；参数随调用变的（光晕的透明度）挂堆上，
 *      由对象的 DELETE 事件回收 —— 页面一销毁就跟着释放，不累积。
 *   2) 行/卡片的子对象一律**绝对定位**（去掉 flex），因为设计稿给的是像素坐标；
 *      行本身在高 56 px 的坑里自己排，外层列表仍是 flex 列。
 */
#include "ao_style.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_metrics.h"

#include "ao_pages.h"     /* ao_pager_attach：行上的横滑也要能切页 */

/* 设计稿坐标 → 实际像素 */
#define D_X(n) UI_PX_W(n)
#define D_Y(n) UI_PX_H(n)

/* ================================================================ 渐变 */

/* 封面占位卡：斜向两段（#2FA4E7 → #174E78）。页面底色不再用渐变，见 AO_C_BG。 */
static lv_grad_dsc_t s_cover_grad;
static bool          s_grad_ready;

static void grads_init_once(void)
{
    if (s_grad_ready) {
        return;
    }
    s_grad_ready = true;

    lv_color_t c2[2] = { lv_color_hex(0x2FA4E7), lv_color_hex(0x174E78) };
    static const uint8_t f2[2] = { 0, 255 };
    lv_grad_dsc_t g2;
    memset(&g2, 0, sizeof(g2));
    lv_grad_linear_init(&g2, LV_GRAD_LEFT, LV_GRAD_TOP, LV_GRAD_RIGHT, LV_GRAD_BOTTOM,
                        LV_GRAD_EXTEND_PAD);
    lv_grad_init_stops(&g2, c2, NULL, f2, 2);
    s_cover_grad = g2;
}

/* ================================================================ 背景 */

void ao_style_screen_bg(lv_obj_t *scr)
{
    if (!scr) {
        return;
    }
    /* 等价 ui_screen_apply_default_bg 的第一步，然后铺 #132C3F 单色（2026-11 用户指定）。
     * 层次感交给右上角那团径向光晕（ao_style_glow），不再画渐变。 */
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(AO_C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* 光晕的渐变描述符（每个对象一份，随对象释放） */
typedef struct {
    lv_grad_dsc_t dsc;
} ao_glow_grad_t;

static void glow_grad_free(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        free(lv_event_get_user_data(e));
    }
}

lv_obj_t *ao_style_glow(lv_obj_t *parent, int cx_pct, int cy_pct, lv_opa_t opa)
{
    if (!parent) {
        return NULL;
    }
    ao_glow_grad_t *g = (ao_glow_grad_t *)calloc(1, sizeof(*g));
    if (!g) {
        return NULL;
    }
    /*
     * 半径：设计稿写的是 r=55%，SVG 的百分比半径是相对"归一化对角线"，
     * 即 0.55*sqrt((w^2+h^2)/2) ≈ 0.32*w（800x480 下 256 px）。
     * LVGL 的径向渐变按像素算，这里用"圆心 + 32% 宽度"给出同样的半径。
     */
    lv_color_t cols[2] = { lv_color_hex(AO_C_GLOW), lv_color_hex(AO_C_GLOW) };
    lv_opa_t   opas[2] = { opa, LV_OPA_TRANSP };
    static const uint8_t fracs[2] = { 0, 255 };
    lv_grad_radial_init(&g->dsc, lv_pct(cx_pct), lv_pct(cy_pct),
                        lv_pct(cx_pct + 32), lv_pct(cy_pct), LV_GRAD_EXTEND_PAD);
    lv_grad_init_stops(&g->dsc, cols, opas, fracs, 2);

    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(AO_C_GLOW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad(o, &g->dsc, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /* 不参与父容器的 flex/grid 布局：光晕是铺满的底图，不该占一格 */
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_event_cb(o, glow_grad_free, LV_EVENT_DELETE, g);
    return o;
}

/* ================================================================ 文本 */

lv_obj_t *ao_style_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(l, LV_OPA_50, LV_PART_MAIN);   /* 设计稿 45% */
    lv_label_set_text(l, text ? text : "");
    return l;
}

const lv_font_t *ao_style_title_font(const char *text)
{
    (void)text;
    return UI_FONT_TITLE_22;
}

/*
 * 转圈文案的轮换表。
 *
 * 语气对齐工程里已有的"奋力加载中…"（本地音乐列表 / 图库都用它），
 * 底色是"在干活"，不是卖萌；每句都短到能塞进 496 px 的详情页清单里。
 */
static const char *const k_load_texts[] = {
    "奋力加载中…",
    "正在向云端讨歌…",
    "翻箱倒柜找歌中…",
    "音符正在赶来的路上…",
    "正在把曲目捋顺…",
    "正在偷偷加载好歌…",
};
#define LOAD_TEXT_NUM  ((int)(sizeof(k_load_texts) / sizeof(k_load_texts[0])))
#define LOAD_TEXT_HOLD_MS  6000   /* 同一句至少显示这么久，避免刷新时闪字 */

const char *ao_style_loading_text(void)
{
    static int      s_idx = -1;
    static uint32_t s_tick;
    const uint32_t now = lv_tick_get();
    if (s_idx < 0) {
        s_idx  = 0;
        s_tick = now;
    } else if (lv_tick_elaps(s_tick) >= LOAD_TEXT_HOLD_MS) {
        s_idx  = (s_idx + 1) % LOAD_TEXT_NUM;
        s_tick = now;
    }
    return k_load_texts[s_idx];
}

/* ================================================================ 卡片 */

lv_obj_t *ao_style_card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, 16, LV_PART_MAIN);   /* 设计稿 6% ≈ 16/255 */
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

void ao_style_cover_card(lv_obj_t *box, int radius)
{
    if (!box) {
        return;
    }
    grads_init_once();
    lv_obj_set_style_radius(box, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x2FA4E7), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad(box, &s_cover_grad, LV_PART_MAIN);
}

/* ================================================================ 列表行 */

lv_obj_t *ao_style_text_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), D_Y(44));
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(AO_C_ACCENT), LV_PART_MAIN);
    lv_label_set_text(l, text ? text : "");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, D_X(34), 0);
    return row;
}

lv_obj_t *ao_style_playlist_row(lv_obj_t *parent, const char *title, const char *sub,
                                bool first, bool divider)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), D_Y(56));
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    ao_pager_attach(row);              /* 行上横滑也能切页（屏幕绝大部分是行） */

    if (first) {
        /* 高亮块：白 6% 圆角 10，纵向在 56 px 坑里居中（设计稿 52 px 高） */
        lv_obj_t *hl = lv_obj_create(row);
        lv_obj_remove_style_all(hl);
        lv_obj_set_pos(hl, 0, D_Y(2));
        lv_obj_set_size(hl, LV_PCT(100), D_Y(52));
        lv_obj_set_style_radius(hl, D_Y(10), LV_PART_MAIN);
        lv_obj_set_style_bg_color(hl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hl, 16, LV_PART_MAIN);
        lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
        /* 左侧青点：cx=20 cy=28 r=4 */
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, D_Y(8), D_Y(8));
        lv_obj_set_pos(dot, D_X(16), D_Y(24));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(AO_C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    } else if (divider) {
        lv_obj_t *ln = lv_obj_create(row);
        lv_obj_remove_style_all(ln);
        /* 分割线贴到行底：文字改成垂直居中后，线再留在 y=32 会像"半截悬着" */
        lv_obj_set_pos(ln, D_X(36), D_Y(55));
        lv_obj_set_size(ln, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ln, 20, LV_PART_MAIN);   /* 7% ≈ 20/255 */
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
    }

    /*
     * 歌单名 / 副标题：**在 56 px 的行里垂直居中**。
     *
     * 需求（2026-11 用户）："在线播放的我的歌单主界面，文字没有和列表卡片居中显示"。
     * 原来的坐标是 y=8 的"顶对齐"：16 px 字的身高在 8..28，而行（以及首行那块 52 px
     * 的高亮卡）的中心在 28 —— 文字整整高了 12 px，卡片看着"上沿贴着字、下面空一截"。
     * 现在用 LEFT_MID / RIGHT_MID，字号或行高一变也仍然是居中的。
     */
    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(t, first ? LV_OPA_COVER : LV_OPA_90, LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, D_X(36), 0);
    /* 右侧留出副标题的位置（副标题最长的是"根据你的口味每天更新"这种，~30% 宽） */
    lv_obj_set_width(t, LV_PCT(70));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_label_set_text(t, title ? title : "");

    if (sub && sub[0]) {
        lv_obj_t *s = lv_label_create(row);
        lv_obj_set_style_text_font(s, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(s, LV_OPA_50, LV_PART_MAIN);
        lv_label_set_text(s, sub);
        /* 右端贴 row 右侧 22 px，与歌单名同一条水平中线 */
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, -D_X(22), 0);
    }
    return row;
}

lv_obj_t *ao_style_song_row(lv_obj_t *parent, int no, const char *title, const char *artist,
                            bool first, bool divider)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), D_Y(48));
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    ao_pager_attach(row);              /* 详情里被 ao_page_show 挡掉，不会真的切页 */

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", no);
    lv_obj_t *idx = lv_label_create(row);
    lv_obj_set_style_text_font(idx, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(idx,
                                first ? lv_color_hex(AO_C_ACCENT) : lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_opa(idx, first ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    lv_label_set_text(idx, buf);
    lv_obj_align(idx, LV_ALIGN_LEFT_MID, 0, 0);

    /*
     * 歌名：**超宽时自己跑马灯**（LV_LABEL_LONG_SCROLL_CIRCULAR），行高恒定 48 px。
     *
     * 需求（2026-11 用户）："歌单列表里如果有歌名比较长的歌曲，当用户未手动划列表的时候，
     * 就自动滚动显示长歌曲名的列表项，确保每个项是等高的；手指滑动滚动列表的时候
     * 停止歌曲文字滚动动画"。
     *   - 滚动的是标签**内部**的文本偏移，标签本身尺寸不变 ⇒ 行高不变（等高）；
     *   - 文本没超宽时 LVGL 自己不会起动画（见 lv_label.c 的 hor_anim 判断）；
     *   - "手指滑列表时停止"由清单上的 LV_EVENT_SCROLL 回调负责，
     *     见本文件末尾的 ao_style_marquee_*（标签靠 LV_OBJ_FLAG_USER_1 认出来）。
     */
    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(t, first ? LV_OPA_COVER : LV_OPA_90, LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, D_X(34), 0);
    /* 右侧留给歌手那一列：歌手占 row_w-132 .. row_w-12，歌名到 row_w-140 为止。
     * 用 pad_right 之后**文本区宽度是固定的**（跑马灯需要固定视口）。 */
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_style_pad_right(t, D_X(174), LV_PART_MAIN);
    lv_label_set_long_mode(t, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_add_flag(t, LV_OBJ_FLAG_USER_1);      /* "可跑马灯"的标记 */
    lv_label_set_text(t, title ? title : "");

    if (artist && artist[0]) {
        lv_obj_t *a = lv_label_create(row);
        lv_obj_set_style_text_font(a, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(a, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(a, LV_OPA_50, LV_PART_MAIN);
        lv_label_set_text(a, artist);
        /*
         * 右端内缩 12 px（2026-11 用户："上下滚动提示条太靠左，会和歌手名重叠"）。
         * 滚动条永远画在清单最右沿（LVGL 的 ver_area.x2 = 对象 x2 - pad_right），
         * 所以让它和文字分开只能把文字往左收 —— 收出来的这 12 px 就是滚动条的位置。
         */
        lv_obj_set_width(a, D_X(120));
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_align(a, LV_ALIGN_RIGHT_MID, -D_X(12), 0);
    }

    if (divider) {
        lv_obj_t *ln = lv_obj_create(row);
        lv_obj_remove_style_all(ln);
        lv_obj_set_pos(ln, D_X(34), D_Y(47));
        lv_obj_set_size(ln, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ln, 20, LV_PART_MAIN);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
    }
    return row;
}

lv_obj_t *ao_style_pill(lv_obj_t *parent, int w, int h, int radius, const char *text,
                        uint32_t fill, lv_opa_t fill_opa, uint32_t border,
                        lv_opa_t border_opa, uint32_t text_color)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_radius(p, radius > 0 ? radius : h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p, lv_color_hex(fill), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, fill_opa, LV_PART_MAIN);
    if (border_opa > LV_OPA_TRANSP) {
        lv_obj_set_style_border_width(p, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(p, lv_color_hex(border), LV_PART_MAIN);
        lv_obj_set_style_border_opa(p, border_opa, LV_PART_MAIN);
    }
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(text_color), LV_PART_MAIN);
    lv_label_set_text(l, text ? text : "");
    lv_obj_center(l);
    return p;
}

/* ================================================================ 长歌名跑马灯
 *
 * 需求（2026-11 用户）：歌单列表里长歌名要自动滚动显示，但**手指划列表时停下来**。
 *
 * 机制（不新造动画，全部复用 LVGL 的 label 滚动）：
 *   - ao_style_song_row() 把歌名标签标成 LV_OBJ_FLAG_USER_1，长模式 = SCROLL_CIRCULAR；
 *   - 清单上挂 LV_EVENT_SCROLL：一滚就把这些标签切回 **DOT**（mode 一换，
 *     lv_label_set_long_mode 内部会 lv_anim_delete 掉滚动动画），然后起一个单次 timer；
 *   - 手指停 AO_MARQUEE_IDLE_MS 后 timer 再把它们切回 SCROLL_CIRCULAR，滚动从下一帧
 *     自动重启（lv_label.c 的 draw 阶段发现文本超宽就重新起动画）。
 *
 * ⚠ 清单销毁前必须 ao_style_marquee_detach()，否则 timer / 回调会碰已释放的清单。
 */
#define AO_MARQUEE_IDLE_MS    2400   /* 手指停下多久之后恢复滚动 */
#define AO_MARQUEE_MAX_LISTS  4      /* 同时挂着的清单数（两个页各一个，留余量） */

static lv_obj_t   *s_marquee_lists[AO_MARQUEE_MAX_LISTS];
static lv_timer_t *s_marquee_timer;
static bool        s_marquee_running = true;   /* 文字当前是否在滚 */

/* 把某个清单里所有"可跑马灯"的歌名标签切到 run / stop */
static void marquee_apply(lv_obj_t *list, bool run)
{
    if (!list) {
        return;
    }
    const lv_label_long_mode_t want = run ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                          : LV_LABEL_LONG_DOT;
    const int32_t rows = (int32_t)lv_obj_get_child_count(list);
    for (int32_t i = 0; i < rows; i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        if (!row) {
            continue;
        }
        const int32_t n = (int32_t)lv_obj_get_child_count(row);
        for (int32_t k = 0; k < n; k++) {
            lv_obj_t *o = lv_obj_get_child(row, k);
            if (!o || !lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) {
                continue;
            }
            if (lv_label_get_long_mode(o) != want) {
                lv_label_set_long_mode(o, want);
            }
        }
    }
}

static void marquee_apply_all(bool run)
{
    for (int i = 0; i < AO_MARQUEE_MAX_LISTS; i++) {
        marquee_apply(s_marquee_lists[i], run);
    }
}

/* 停了 AO_MARQUEE_IDLE_MS：恢复滚动（单次，恢复后自己停表） */
static void marquee_resume_cb(lv_timer_t *t)
{
    if (t) {
        lv_timer_pause(t);
    }
    if (s_marquee_running) {
        return;
    }
    s_marquee_running = true;
    marquee_apply_all(true);
}

/* 清单被滚动（手指拖 / 惯性 / lv_obj_scroll_to_y 都算）：先停车，再等手指停稳 */
static void marquee_scroll_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SCROLL) {
        return;
    }
    /*
     * 每帧都无条件"停车"：marquee_apply() 内部只改**模式不同**的标签，幂等且便宜；
     * 而靠 s_marquee_running 短路会有漏（例如列表刚重建、新标签又是 SCROLL_CIRCULAR，
     * 但标志还是 false —— 那一轮滑动就停不下来）。
     */
    s_marquee_running = false;
    marquee_apply_all(false);
    if (s_marquee_timer) {
        lv_timer_reset(s_marquee_timer);
        lv_timer_resume(s_marquee_timer);
    }
}

void ao_style_marquee_attach(lv_obj_t *list)
{
    if (!list) {
        return;
    }
    for (int i = 0; i < AO_MARQUEE_MAX_LISTS; i++) {
        if (s_marquee_lists[i] == list) {
            return;                       /* 已经挂过 */
        }
    }
    for (int i = 0; i < AO_MARQUEE_MAX_LISTS; i++) {
        if (!s_marquee_lists[i]) {
            s_marquee_lists[i] = list;
            break;
        }
    }
    lv_obj_add_event_cb(list, marquee_scroll_cb, LV_EVENT_SCROLL, NULL);
    if (!s_marquee_timer) {
        s_marquee_timer = lv_timer_create(marquee_resume_cb, AO_MARQUEE_IDLE_MS, NULL);
        if (s_marquee_timer) {
            lv_timer_pause(s_marquee_timer);
        }
    }
    /* 刚建好的行都是 SCROLL_CIRCULAR，状态跟着复位 */
    s_marquee_running = true;
}

/*
 * 页面销毁时调用（见头文件）：只清登记表 + 删 timer，**一个控件都不碰**。
 *
 * ⚠ 别在这里加 lv_obj_remove_event_cb()：screen 的 LV_EVENT_DELETE 发生在子对象销毁
 *   之后，那时清单指针已经失效（lv_obj_remove_event_cb 会踩已释放内存）。
 *   清单自己销毁时，挂在它身上的事件回调是跟着对象一起没的，不需要手动摘。
 */
void ao_style_marquee_detach_all(void)
{
    for (int i = 0; i < AO_MARQUEE_MAX_LISTS; i++) {
        s_marquee_lists[i] = NULL;
    }
    if (s_marquee_timer) {
        lv_timer_delete(s_marquee_timer);
        s_marquee_timer = NULL;
    }
    s_marquee_running = true;
}
