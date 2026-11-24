/*
 * ao_pages.cc —— **通用 LVGL 层**：页面骨架（四页 tab + 左右滑动切页）+ 控件工厂
 *
 * 版面出处：assets/UIDesign/在线音乐主界面.svg（顶栏 / 页签 / 下划线 / 列表行）。
 *
 * 实现要点（每条都踩过坑）：
 *   1) 四页并排放在 s_host 里（LVGL 把子对象裁剪在父对象范围内），切页 = 一条
 *      lv_anim 把四页的 x 一起平移，220 ms 缓出 —— 与 ui_router 同节奏。
 *   2) **页签下划线**是页签容器里的一个独立对象（IGNORE_LAYOUT，不参与 flex），
 *      每次切页按当前页签的真实坐标重新摆 —— 文字宽度随语言/字号变，不能写死。
 *   3) **详情模式**：页签收起、宿主从 y=118 上移到 y=88（设计稿里详情页的清单就从
 *      88 起）、标题换成"歌单"。此模式下禁止切页（返回键先回列表）。
 *   4) ao_make_row / ao_fill_song_rows_range 这些**旧卡片风格**的工厂保留给搜索页
 *      （搜索页有自己的参考稿，这次不动它）。
 */
#include "ao_pages.h"

#include <stdio.h>
#include <string.h>

#include "app_music.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_account.h"
#include "ao_overlay.h"
#include "ao_model.h"
#include "ao_provider.h"
#include "ao_style.h"

#define PAGE_ANIM_MS  220        /* 四页左右滑动的动画时长（与 ui_router 同节奏） */

/* 设计稿坐标 */
#define D_X(n) UI_PX_W(n)
#define D_Y(n) UI_PX_H(n)

#define HOST_Y_LIST    D_Y(118)                 /* 列表模式：页签 + 说明行之下 */
#define HOST_Y_DETAIL  D_Y(88)                  /* 详情模式：页签收起后 */
#define HOST_H_LIST    (UI_H - HOST_Y_LIST)
#define HOST_H_DETAIL  (UI_H - HOST_Y_DETAIL)

static lv_obj_t *s_root;         /* 页面 screen */
static lv_obj_t *s_tab[4];       /* 顶部 4 个页签（我的歌单 / 我添加的歌单 / 搜索 / 账号） */
static lv_obj_t *s_underline;    /* 强调色下划线（跟着当前页签走） */
static lv_obj_t *s_tabs;         /* 页签容器（详情模式整个收起） */
static lv_obj_t *s_title;        /* 头部标题（详情模式换成"歌单"） */
static lv_obj_t *s_host;         /* 四页的宿主（负责裁剪，页宽也从它取） */
static lv_obj_t *s_page[4];      /* 四个页容器 */
static int       s_page_cur;     /* 当前页 0..3 */
static int32_t   s_page_off;     /* 当前平移量（px）—— 动画的起始值 */
static bool      s_detail;       /* 是否在歌单详情模式 */
static int       s_anim_anchor;  /* 动画载体（内容无意义，只给 lv_anim 用） */

/* ================================================================ 骨架 / 切页 */

static void pager_gesture(lv_event_t *e)
{
    (void)e;
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        ao_page_show(s_page_cur + 1, true);      /* 手指向左划 → 看右边那一页 */
    } else if (dir == LV_DIR_RIGHT) {
        ao_page_show(s_page_cur - 1, true);      /* 手指向右划 → 看左边那一页 */
    }
}

void ao_pager_attach(lv_obj_t *o)
{
    if (o) {
        lv_obj_add_event_cb(o, pager_gesture, LV_EVENT_GESTURE, NULL);
    }
}

static void slide_exec_cb(void *var, int32_t v)
{
    (void)var;
    s_page_off = v;
    int32_t w = lv_obj_get_width(s_host);
    if (w <= 0) {
        w = lv_display_get_horizontal_resolution(NULL);
    }
    /* 四页必须每帧都摆正：只挪"当前 + 目标"两页时，被跳过的那页会停在边界 x=±w */
    for (int i = 0; i < 4; i++) {
        if (s_page[i]) {
            lv_obj_set_x(s_page[i], (int32_t)i * w - v);
        }
    }
}

/* 页签样式：当前页 = 白 100%，其它 = 白 50%（设计稿） */
static void tab_style(lv_obj_t *l, bool active)
{
    if (!l) {
        return;
    }
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(l, active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
}

/*
 * 把下划线摆到当前页签底下。
 *
 * 宽度取 min(页签文字宽, 44 px)：设计稿里 4 个字的"我的歌单"下划线是 44 px 宽，
 * 2 个字的"账号/搜索"是 34 px —— 也就是"不到 44 就跟着文字宽度走"。
 * 坐标用**绝对坐标相减**：页签容器是绝对定位的，直接算相对位置最容易出错。
 */
static void underline_update(void)
{
    if (!s_underline || !s_tabs) {
        return;
    }
    lv_obj_t *tab = s_tab[s_page_cur];
    if (!tab || lv_obj_has_flag(tab, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_flag(s_tabs, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_underline, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_underline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_tabs);

    lv_area_t ta, ba;
    lv_obj_get_coords(tab, &ta);
    lv_obj_get_coords(s_tabs, &ba);

    int32_t w = lv_area_get_width(&ta);
    const int32_t max_w = D_X(44);
    if (w > max_w) {
        w = max_w;
    }
    if (w < D_X(12)) {
        w = D_X(12);
    }
    /* lv_obj_set_pos 是相对**内容区**（扣掉 padding）算的，页签容器有 pad_top/pad_left，
     * 所以先把 padding 减掉，下划线才会真的落在页签文字正下方 */
    const int32_t pad_l = lv_obj_get_style_pad_left(s_tabs, LV_PART_MAIN);
    const int32_t pad_t = lv_obj_get_style_pad_top(s_tabs, LV_PART_MAIN);
    const int32_t x = (ta.x1 - ba.x1) - pad_l + (lv_area_get_width(&ta) - w) / 2;
    const int32_t y = (ta.y2 - ba.y1) - pad_t + D_Y(4);
    lv_obj_set_pos(s_underline, x, y);
    lv_obj_set_size(s_underline, w, D_Y(3));
}

void ao_page_show(int which, bool animate)
{
    if (!s_host) {
        return;
    }
    if (which < 0) {
        which = 0;
    } else if (which > 3) {
        which = 3;
    }
    /* 详情模式下只能靠返回键回去：这里挡掉 tab/横滑的切页请求 */
    if (s_detail && which != s_page_cur) {
        return;
    }
    s_page_cur = which;
    for (int i = 0; i < 4; i++) {
        tab_style(s_tab[i], s_page_cur == i);
    }
    underline_update();
    /* 离开搜索页就把键盘收了：别让它继续盖在别的页上 */
    if (s_page_cur != 2) {
        ao_ime_hide();
    }
    if (s_page_cur == 0) {
        /* 进账号歌单页刷新一次：登录态 / 歌单可能在别处变了 */
        ao_acct_rebuild();
    } else if (s_page_cur == 3) {
        ao_account_rebuild();
    }

    /* 页宽 = 宿主的宽；万一还没 layout（宽 0）就用屏幕宽兜底，别让两页叠一起 */
    int32_t w = lv_obj_get_width(s_host);
    if (w <= 0) {
        w = lv_display_get_horizontal_resolution(NULL);
    }
    const int32_t target = (int32_t)s_page_cur * w;
    if (!animate || target == s_page_off) {
        slide_exec_cb(NULL, target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    /* var 只是 anim 的载体，exec 里按下标同时挪四页 */
    lv_anim_set_var(&a, &s_anim_anchor);
    lv_anim_set_values(&a, s_page_off, target);
    lv_anim_set_duration(&a, PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, slide_exec_cb);
    lv_anim_start(&a);
}

int ao_pages_current(void)
{
    return s_page_cur;
}

bool ao_pages_in_detail(void)
{
    return s_detail;
}

/* 页签点击（user_data = 页号） */
static void tab_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ao_page_show(idx, true);
}

void ao_pages_build_tabs(lv_obj_t *tabs)
{
    static const char *const kNames[4] = { "我的歌单", "我添加的歌单", "搜索", "账号" };
    s_tabs = tabs;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = lv_label_create(tabs);
        lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(l, LV_OPA_50, LV_PART_MAIN);
        lv_label_set_text(l, kNames[i]);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(l, D_X(10));   /* 文字本身太小，扩大点击区 */
        lv_obj_add_event_cb(l, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tab[i] = l;
    }
    /* 下划线：不参与 flex 布局，由 underline_update() 摆 */
    lv_obj_t *u = lv_obj_create(tabs);
    lv_obj_remove_style_all(u);
    lv_obj_set_style_radius(u, D_Y(2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(u, lv_color_hex(AO_C_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(u, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(u, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(u, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(u, LV_OBJ_FLAG_IGNORE_LAYOUT);
    s_underline = u;
}

lv_obj_t *ao_pages_build_host(lv_obj_t *scr)
{
    s_host = lv_obj_create(scr);
    lv_obj_remove_style_all(s_host);
    lv_obj_set_pos(s_host, 0, HOST_Y_LIST);
    lv_obj_set_size(s_host, LV_PCT(100), HOST_H_LIST);
    lv_obj_remove_flag(s_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_host, LV_OBJ_FLAG_CLICKABLE);
    ao_pager_attach(s_host);            /* 点空白处起手横滑也能切页 */
    return s_host;
}

void ao_pages_set_chrome(lv_obj_t *title_label)
{
    s_title = title_label;
}

void ao_pages_set_detail_mode(bool on)
{
    if (s_detail == on) {
        return;
    }
    s_detail = on;
    if (s_tabs) {
        if (on) {
            lv_obj_add_flag(s_tabs, LV_OBJ_FLAG_HIDDEN);
            if (s_underline) {
                lv_obj_add_flag(s_underline, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_remove_flag(s_tabs, LV_OBJ_FLAG_HIDDEN);
            underline_update();
        }
    }
    if (s_title) {
        lv_label_set_text(s_title, on ? "歌单" : ao_provider_title());
    }
    if (s_host) {
        lv_obj_set_y(s_host, on ? HOST_Y_DETAIL : HOST_Y_LIST);
        lv_obj_set_height(s_host, on ? HOST_H_DETAIL : HOST_H_LIST);
    }
}

void ao_pages_sync_detail(int page, bool on)
{
    if (page != s_page_cur) {
        return;
    }
    ao_pages_set_detail_mode(on);
}

void ao_pages_set_root(lv_obj_t *root)
{
    s_root = root;
}

void ao_pages_set_page(int which, lv_obj_t *page)
{
    if (which >= 0 && which < 4) {
        s_page[which] = page;
    }
}

void ao_pages_clear(void)
{
    s_root = NULL;
    for (int i = 0; i < 4; i++) {
        s_tab[i] = NULL;
        s_page[i] = NULL;
    }
    s_underline = NULL;
    s_tabs = NULL;
    s_title = NULL;
    s_host = NULL;
    s_detail = false;
}

/* ================================================================ 通用控件工厂 */

void ao_online_row_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= ao_queue_count()) {
        return;
    }
    /* 播放器接手这份队列之后，被替换下来的旧数组就可以放了 */
    ao_queue_drop_stale();
    app_music_play_online(ao_queue_songs(), ao_queue_count(), (size_t)idx);
    app_music_open_player();       /* 直接进播放器页看封面/歌词 */
}

lv_obj_t *ao_make_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                      const char *title, const char *sub)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    ao_pager_attach(row);               /* 横滑切页（见 pager_gesture 的说明） */

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(40), UI_PX_H(40));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, UI_PCT_X(60));
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, UI_PX_H(52), 0);

    if (sub && sub[0]) {
        lv_obj_t *s = lv_label_create(row);
        lv_obj_set_style_text_font(s, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, UI_PCT_X(22));
        lv_label_set_text(s, sub);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

/*
 * 长按歌曲行 → 「收藏到歌单」（只有 QQ 音乐有：那边提供了 add_songs）。
 *
 * 为什么挂在长按而不是再加一个按钮：歌曲行本来就很窄，多一个按钮会挤掉歌名；
 * 而"点一下起播"是主操作，不能动。LVGL 在派发过 LONG_PRESSED 之后**不会再补
 * CLICKED**，所以长按不会顺手把歌点开。
 */
static int  s_collect_idx = -1;
static bool s_collect_pending;

/* 还按着就下一轮再看（同 ao_page_pl.cc 的长按菜单：按压期间建遮罩会被同一次松手关掉） */
static void collect_deferred(void *arg)
{
    (void)arg;
    if (!s_collect_pending) {
        return;
    }
    const lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lv_async_call(collect_deferred, NULL);
        return;
    }
    const int idx = s_collect_idx;
    s_collect_pending = false;
    s_collect_idx = -1;
    if (idx >= 0 && (size_t)idx < ao_queue_count()) {
        /* 当前列表属于哪个歌单（「删除」要用它的 dirid）；搜索结果/按 ID 的歌单是空串 */
        ao_collect_open(idx, ao_collect_ctx_dirid());
    }
}

static void song_row_long_pressed(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!ao_is_qq()) {
        return;                      /* 网易云侧没有这个能力（服务端没开放加歌） */
    }
    if (idx < 0 || (size_t)idx >= ao_queue_count() || s_collect_pending) {
        return;
    }
    /* ★ 只记录目标 + 排一个异步调用：浮层等手指抬起来再建（见 collect_deferred） */
    s_collect_idx     = idx;
    s_collect_pending = true;
    lv_async_call(collect_deferred, NULL);
}

/* 歌曲列表（搜索结果共用）：画 s_songs[first .. first+count) */
void ao_fill_song_rows_range(lv_obj_t *list, size_t first, size_t count)
{
    online_song_t *songs = ao_queue_songs();
    for (size_t i = 0; i < count; i++) {
        const size_t idx = first + i;
        /* 音源色：保持网易云原来的绿不放，QQ 用自己图标上的绿（略深一点） */
        const uint32_t accent = (songs[idx].src == ONLINE_SRC_QQ) ? 0x2BB673 : 0x3E9E86;
        lv_obj_t *row = ao_make_row(list, LV_SYMBOL_AUDIO, accent,
                                    songs[idx].title, songs[idx].artist);
        /*
         * ★ 点歌用 **SHORT_CLICKED** 而不是 CLICKED：LVGL 松手时会**无条件**补发 CLICKED
         *   （长按之后也补），那样"长按收藏"会顺手把这首歌放起来。
         */
        lv_obj_add_event_cb(row, ao_online_row_clicked, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)idx);
        lv_obj_add_event_cb(row, song_row_long_pressed, LV_EVENT_LONG_PRESSED,
                            (void *)(intptr_t)idx);
    }
}

/* 给"歌曲行"挂点播 + 长按收藏（见头文件说明） */
void ao_song_row_wire(lv_obj_t *row, size_t idx)
{
    if (!row) {
        return;
    }
    lv_obj_add_event_cb(row, ao_online_row_clicked, LV_EVENT_SHORT_CLICKED,
                        (void *)(intptr_t)idx);
    lv_obj_add_event_cb(row, song_row_long_pressed, LV_EVENT_LONG_PRESSED,
                        (void *)(intptr_t)idx);
}

/* 列表内容整段换掉之后回到顶部。 */
void ao_list_scroll_top(lv_obj_t *list)
{
    if (list) {
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
}

/* 翻页行的点击：user_data = (which << 1) | dir（dir: 0 = 上一页, 1 = 下一页） */
static void page_row_clicked(lv_event_t *e)
{
    const int v = (int)(intptr_t)lv_event_get_user_data(e);
    const int which = v >> 1;
    const int dir = (v & 1) ? +1 : -1;
    lv_obj_t *list = NULL;
    if (which == PG_ACCT) {
        ao_acct_page_set(ao_acct_page() + dir);
        ao_acct_rebuild();
        list = ao_acct_list();
    } else if (which == PG_PL) {
        ao_pl_page_set(ao_pl_page() + dir);
        ao_pl_rebuild();
        list = ao_pl_list();
    } else {
        ao_srch_page_set(ao_srch_page() + dir);
        ao_srch_rebuild();
        list = ao_srch_list();
    }
    ao_list_scroll_top(list);
}

void ao_page_rows(lv_obj_t *list, int which, int page, int pages)
{
    if (page > 0) {
        lv_obj_t *r = ao_make_row(list, LV_SYMBOL_LEFT, 0x5B6FB5, "上一页", NULL);
        lv_obj_add_event_cb(r, page_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((which << 1) | 0));
    }
    if (page + 1 < pages) {
        lv_obj_t *r = ao_make_row(list, LV_SYMBOL_RIGHT, 0x5B6FB5, "下一页", NULL);
        lv_obj_add_event_cb(r, page_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((which << 1) | 1));
    }
}

/* 扁平样式的翻页行（歌单详情页用，与列表行同一套视觉） */
void ao_page_rows_flat(lv_obj_t *list, int which, int page, int pages)
{
    if (page > 0) {
        lv_obj_t *r = ao_style_text_row(list, LV_SYMBOL_LEFT "  上一页");
        lv_obj_add_event_cb(r, page_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((which << 1) | 0));
    }
    if (page + 1 < pages) {
        lv_obj_t *r = ao_style_text_row(list, LV_SYMBOL_RIGHT "  下一页");
        lv_obj_add_event_cb(r, page_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((which << 1) | 1));
    }
}

/*
 * 详情页顶部的"‹ 返回上一级"行（保留给"同一页里换内容但不想动头部"的场景）。
 *
 * ★ 必须由**页容器**持有，**不能放进可滚动的列表里**（2026-10 用户报
 *   "歌单的返回按钮为什么会跟着滑动"）。重构后歌单详情改用全局返回钮 +
 *   ui_back_button_set_handler()，这个工厂只剩兼容用途。
 */
lv_obj_t *ao_make_back_label(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, UI_PX_H(14));
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

/* "等东西"的统一视觉：无限转圈 + 一行字。 */
void ao_loading_rows(lv_obj_t *list, const char *text)
{
    lv_obj_t *box = lv_obj_create(list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box, UI_PX_H(28), LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, UI_PX_H(12), LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sp = lv_spinner_create(box);
    lv_obj_set_size(sp, UI_PX_H(48), UI_PX_H(48));
    lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *h = lv_label_create(box);
    lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
    /* 不传文案就用"轮换的那几句"（见 ao_style_loading_text 的说明） */
    lv_label_set_text(h, text ? text : ao_style_loading_text());
}

/*
 * 详情页"还没拿到自己那份数据"时的占位：转圈；超过 DETAIL_TIMEOUT_MS 就换成一个
 * "重试"入口（reload_cb 是这套详情对应的重新载入回调）。
 */
void ao_detail_wait_rows(lv_obj_t *list, lv_event_cb_t reload_cb)
{
    if (lv_tick_elaps(ao_detail_tick()) < DETAIL_TIMEOUT_MS) {
        ao_loading_rows(list, NULL);      /* NULL = 用轮换文案 */
        return;
    }
    lv_obj_t *h = lv_label_create(list);
    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_text(h, "载入超时（网络慢或请求丢了），可以重试一次");
    lv_obj_t *reload = ao_make_row(list, LV_SYMBOL_REFRESH, 0x3E9E86, "重试", NULL);
    if (reload_cb) {
        lv_obj_add_event_cb(reload, reload_cb, LV_EVENT_CLICKED, NULL);
    }
}
