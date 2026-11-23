/*
 * ao_pages.cc —— **通用 LVGL 层**：页面骨架（三页 tab + 左右滑动切页）+ 控件工厂
 *
 * 网易云音乐是**三页左右滑动**：第 0 页"我的歌单"（网易云账号）、第 1 页"我添加的
 * 歌单"、第 2 页"歌曲搜索"，顶部那三段文字既是标题也是 tab。
 *
 * 实现：三页并排放在 s_host 里（LVGL 默认把子对象裁剪在父对象范围内，所以超出部分
 * 自动看不见），切页 = 一条 lv_anim 把三页的 x 一起平移，220 ms 缓出 —— 和 ui_router
 * 的页面动画同节奏，视觉上是一套。
 *
 * ⚠ 页序（0/1/2）必须与 ao_common.h 的 PG_* 常量、slide_exec_cb 里的下标三处一致：
 *   之前漏改就出现过"点歌曲搜索跳到了我添加的歌单"。
 *
 * 控件工厂部分有三条容易踩的坑（拆分时原样保留，别"顺手统一"）：
 *   1) `ao_make_row()` 会给行挂 `ao_pager_attach()`（横滑切页）——屏幕绝大部分面积
 *      是行，横滑多从行上起手；
 *   2) 详情页的"‹ 返回"行必须由**页容器**持有（`ao_make_back_label`），不能放进
 *      可滚动列表：放进列表就会跟着歌曲列表滚走（用户 2026-10 报"返回按钮为什么
 *      会跟着滑动"）；
 *   3) `ao_fill_song_rows_range()` 给行挂的下标是**绝对下标**（含分页偏移），
 *      点行即播靠它。
 */
#include "ao_pages.h"

#include "app_music.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_account.h"
#include "ao_overlay.h"
#include "ao_model.h"
#include "ao_provider.h"

#define PAGE_ANIM_MS  220        /* 三页左右滑动的动画时长（与 ui_router 同节奏） */

static lv_obj_t *s_root;         /* 页面 screen */
static lv_obj_t *s_tab_acct;     /* 顶部文字：我的歌单（网易云账号，兼 tab） */
static lv_obj_t *s_tab_pl;       /* 顶部文字：我添加的歌单（兼 tab） */
static lv_obj_t *s_tab_srch;     /* 顶部文字：搜索（兼 tab） */
static lv_obj_t *s_host;         /* 三页的宿主（负责裁剪，页宽也从它取） */
static lv_obj_t *s_page_acct;    /* 第 0 页容器 */
static lv_obj_t *s_page_pl;      /* 第 1 页容器 */
static lv_obj_t *s_page_srch;    /* 第 2 页容器 */
static int       s_page;         /* 当前页 0/1/2 */
static int32_t   s_page_off;     /* 当前平移量（px）—— 动画的起始值 */

/* ================================================================ 骨架 / 切页 */

/*
 * 左右滑动切页。
 *
 * 为什么挂 GESTURE 而不是自己算 PRESSED/RELEASED 位移：LVGL 只在"被按的对象
 * **无法**在该方向滚动"时才发 LV_EVENT_GESTURE，而发过 gesture 之后它会置
 * gesture_sent，**松手不再补发 CLICKED** —— 正好避免"滑一下顺手把歌点开了"。
 * 本页的三个列表都只允许纵向滚动（LV_DIR_VER），所以列表上的横滑会冒到这里。
 */
static void pager_gesture(lv_event_t *e)
{
    (void)e;
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        ao_page_show(s_page + 1, true);      /* 手指向左划 → 看右边那一页 */
    } else if (dir == LV_DIR_RIGHT) {
        ao_page_show(s_page - 1, true);      /* 手指向右划 → 看左边那一页 */
    }
}

/* 每个列表行、宿主各挂一份：屏幕绝大部分面积是行，横滑多从这里起手 */
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
    /*
     * 三页必须每帧都摆正：只挪"当前 + 目标"两页时，被跳过的那页会停在
     * 边界 x=±w；它的子对象/阴影/动画一旦越界，就可能和可见页叠在一起。
     * （2026-11 用户报"点卡片切换元素重叠"，这里回退为全量摆位。）
     */
    lv_obj_set_x(s_page_acct, -v);
    lv_obj_set_x(s_page_pl, w - v);
    lv_obj_set_x(s_page_srch, 2 * w - v);
}

static void tab_style(lv_obj_t *l, bool active)
{
    lv_obj_set_style_text_color(l, active ? UI_COLOR_ACCENT : UI_COLOR_FG_DIM, LV_PART_MAIN);
}

void ao_page_show(int which, bool animate)
{
    if (!s_host) {
        return;
    }
    if (which < 0) {
        which = 0;
    } else if (which > 2) {
        which = 2;
    }
    s_page = which;
    tab_style(s_tab_acct, s_page == 0);
    tab_style(s_tab_pl, s_page == 1);
    tab_style(s_tab_srch, s_page == 2);
    /* 离开搜索页就把键盘收了：别让它继续盖在别的页上 */
    if (s_page != 2) {
        ao_ime_hide();
    }
    if (s_page == 0) {
        /* 进账号页刷新一次：登录态 / 歌单可能在别处变了 */
        ao_acct_rebuild();
    }

    /* 页宽 = 宿主的宽；万一还没 layout（宽 0）就用屏幕宽兜底，别让两页叠一起 */
    int32_t w = lv_obj_get_width(s_host);
    if (w <= 0) {
        w = lv_display_get_horizontal_resolution(NULL);
    }
    const int32_t target = (int32_t)s_page * w;
    if (!animate || target == s_page_off) {
        slide_exec_cb(NULL, target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    /* var 只是 anim 的载体，exec 里按下标同时挪三页 */
    lv_anim_set_var(&a, &s_page);
    lv_anim_set_values(&a, s_page_off, target);
    lv_anim_set_duration(&a, PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, slide_exec_cb);
    lv_anim_start(&a);
}

int ao_pages_current(void)
{
    return s_page;
}

/* ⚠ 页序是三页：0 = 我的歌单（账号）、1 = 我添加的歌单、2 = 歌曲搜索。
 *   加页时这几个下标必须跟着改 —— 之前漏改就出现"点歌曲搜索跳到了我添加的歌单"。 */
static void tab_acct_clicked(lv_event_t *e) { (void)e; ao_page_show(0, true); }
static void tab_pl_clicked(lv_event_t *e)   { (void)e; ao_page_show(1, true); }
static void tab_srch_clicked(lv_event_t *e) { (void)e; ao_page_show(2, true); }

void ao_pages_build_tabs(lv_obj_t *tabs)
{
    /* 第 0 页页签：我的歌单（网易云账号）—— 必须在 pl 之前建，保证页签从左到右的次序 */
    s_tab_acct = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_acct, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_tab_acct, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_tab_acct, "我的歌单");
    lv_obj_add_flag(s_tab_acct, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_acct, UI_PX_W(10));
    lv_obj_add_event_cb(s_tab_acct, tab_acct_clicked, LV_EVENT_CLICKED, NULL);

    s_tab_pl = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_pl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_tab_pl, "我添加的歌单");
    lv_obj_add_flag(s_tab_pl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_pl, UI_PX_H(12));   /* 文字本身太小，扩大点击区 */
    lv_obj_add_event_cb(s_tab_pl, tab_pl_clicked, LV_EVENT_CLICKED, NULL);

    s_tab_srch = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_srch, UI_FONT_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_tab_srch, "搜索");
    lv_obj_add_flag(s_tab_srch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_srch, UI_PX_H(12));
    lv_obj_add_event_cb(s_tab_srch, tab_srch_clicked, LV_EVENT_CLICKED, NULL);
}

lv_obj_t *ao_pages_build_host(lv_obj_t *scr)
{
    /* ---- 三页的宿主：吃掉剩余高度，并把三页裁剪在可视范围内 ---- */
    s_host = lv_obj_create(scr);
    lv_obj_remove_style_all(s_host);
    lv_obj_set_width(s_host, LV_PCT(100));
    lv_obj_set_flex_grow(s_host, 1);
    lv_obj_remove_flag(s_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_host, LV_OBJ_FLAG_CLICKABLE);
    ao_pager_attach(s_host);            /* 点空白处起手横滑也能切页 */
    return s_host;
}

void ao_pages_set_root(lv_obj_t *root)
{
    s_root = root;
}

void ao_pages_set_page(int which, lv_obj_t *page)
{
    if (which == 0) {
        s_page_acct = page;
    } else if (which == 1) {
        s_page_pl = page;
    } else {
        s_page_srch = page;
    }
}

void ao_pages_clear(void)
{
    s_root = NULL;
    s_tab_acct = NULL;
    s_tab_pl = NULL;
    s_tab_srch = NULL;
    s_host = NULL;
    s_page_acct = NULL;
    s_page_pl = NULL;
    s_page_srch = NULL;
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

/* 歌曲列表（歌单详情 / 搜索结果 / 账号歌单共用）：画 s_songs[first .. first+count) */
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
         *   （长按之后也补），那样"长按收藏"会顺手把这首歌放起来。SHORT_CLICKED 只在
         *   没长按的短点里发 —— 与「我添加的歌单」那页的列表行同一套处理。
         */
        lv_obj_add_event_cb(row, ao_online_row_clicked, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)idx);
        lv_obj_add_event_cb(row, song_row_long_pressed, LV_EVENT_LONG_PRESSED,
                            (void *)(intptr_t)idx);
    }
}

/* 列表内容整段换掉之后回到顶部。
 * 列表是 lv_obj_clean + 重建的，滚动位置会留在原处 —— 进详情/翻页后若不归零，
 * 新内容会从中间开始显示（用户看到的像是"漏了前几首"）。 */
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

/* "‹ 上一页 / 下一页 ›"两行（页码提示由各页自己写在 hint 里） */
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

/*
 * 详情页顶部的"‹ 返回上一级"行。
 *
 * ★ 必须由**页容器**（s_page_acct / s_page_pl）持有，**不能放进可滚动的列表里**：
 *   2026-10 用户报"歌单的返回按钮为什么会跟着滑动？导致无法快速返回上一级" ——
 *   返回行原来是 s_*_list 的子对象，而列表是 flex_grow + LV_DIR_VER 可滚动的，
 *   一往下翻歌就把返回入口滚出屏幕外了，只能滑回顶部才能点。
 *   现在它是页面 flex 列里的**第一行**（在 hint 之上），列表滚动与它无关。
 *
 * 隐藏时 flex 会自动跳过它（lv_flex.c 里 `LV_OBJ_FLAG_HIDDEN` 的对象不参与布局），
 * 所以"歌单列表"状态下不会留一条空白。
 */
lv_obj_t *ao_make_back_label(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, UI_PX_H(14));   /* 手指点得准一点 */
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);      /* 默认藏起来，进详情才显示 */
    return l;
}

/*
 * "等东西"的统一视觉：无限转圈 + 一行字。
 * lv_spinner 默认就是无限循环（LV_SPINNER_TYPE_SPINNING_ARC），不用自己起 timer。
 * 歌单详情 / 账号歌单 / 搜索 / 账号页都走它，四个地方长得一样。
 */
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
    lv_label_set_text(h, text ? text : "载入中…");
}

/*
 * 详情页"还没拿到自己那份数据"时的占位：转圈；超过 DETAIL_TIMEOUT_MS 就换成一个
 * "重试"入口（reload_cb 是这套详情对应的重新载入回调）。
 * 为什么要有超时出口：请求万一没被工作线程收下（队列满）就永远没有结果，
 * 只给转圈的话用户只能退出重进。
 */
void ao_detail_wait_rows(lv_obj_t *list, lv_event_cb_t reload_cb)
{
    if (lv_tick_elaps(ao_detail_tick()) < DETAIL_TIMEOUT_MS) {
        ao_loading_rows(list, "载入中…");
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
