/*
 * ao_page_srch.cc —— 第 2 页「搜索」（历史 / 结果分页 / 输入法交互）
 *
 * 列表内容的三条规则（需求原文）：
 *   - 有搜索结果        → 显示结果（"改为显示搜索结果歌曲列表"）
 *   - 输入框为空且键盘关 → 显示历史（"只有输入框为空并关闭输入法的时候才显示历史"）
 *   - 其它（正在输入）  → 只显示一行提示，别把历史塞在键盘上面
 *
 * ★★ 两条不能动的纪律：
 *   1) **绝不能在 LV_EVENT_DEFOCUSED 里收键盘**（2026-10 用户报"点一下键盘它自己就
 *      收了"）：点键盘按键时焦点会从输入框转到按键对象，输入框收到 DEFOCUSED；
 *      早期版本在那个分支里 ao_ime_hide()，于是"按第一个键，键盘就没了"。
 *      收键盘只有三个入口：空输入按对勾、点列表/空白、离开本页。
 *   2) 进 `ao_srch_do_search()` **之前必须 snprintf 拷一份关键词**：调用方可能传
 *      指向 `online_history_at()` 内部存储的指针，而 `online_history_add()` 会把数组
 *      重排（同一个词提到最前会把后面的整体后移）→ 真机现象是"点某条历史，搜出来的
 *      是另一条"（2026-10 踩过，正好错一位）。
 */
#include "lvgl.h"

#include "online_service.h"
#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_common.h"
#include "ao_model.h"
#include "ao_overlay.h"
#include "ao_pages.h"
#include "ao_work.h"

static lv_obj_t *s_srch_ta;      /* 输入框 */
static lv_obj_t *s_srch_hint;    /* 列表上方小标题（历史搜索 / 搜索结果） */
static lv_obj_t *s_srch_clear;   /* 右上角"清除"（只在显示历史时出现） */
static lv_obj_t *s_srch_list;    /* 历史 / 结果 列表 */
static bool      s_srch_has_res; /* 列表里是搜索结果(true) 还是历史(false) */
/*
 * ★ "请求已经发出去、结果还没回来"的中间态。
 * 少了它就会出现用户报的那个怪现象（2026-10）：搜索请求刚发出去时，
 * 为了让结果回来能就地替换，代码先把 s_srch_has_res 置真"占住结果状态"——
 * 可这时列表里还没有任何数据，于是画出来的就是**"没有找到匹配的歌曲"**，
 * 而实际上正在搜。现在这段区间由 s_srch_busy 表示，界面显示"正在搜索…"+转圈。
 */
static bool      s_srch_busy;

/* 待删除的单条搜索记录（确认框弹出时列表可能已经重建，按下标删就够） */
static int s_hist_del_idx = -1;

void ao_srch_clear(void)
{
    s_srch_ta = NULL;
    s_srch_hint = NULL;
    s_srch_clear = NULL;
    s_srch_list = NULL;
    s_srch_has_res = false;
}

lv_obj_t *ao_srch_list(void)
{
    return s_srch_list;
}

void ao_srch_reset_view(void)
{
    s_srch_has_res = false;
}

void ao_srch_set_busy(bool busy)
{
    s_srch_busy = busy;
}

void ao_srch_set_has_res(bool has)
{
    s_srch_has_res = has;
}

static void srch_do_search(const char *kw);

static void srch_row_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_srch_has_res) {
        ao_online_row_clicked(e);       /* 结果行 = 点歌播放 */
        return;
    }
    /* 历史行：把词填回输入框并立刻搜一次 */
    const char *k = online_history_at((size_t)idx);
    if (!k || !k[0]) {
        return;
    }
    /*
     * ★ 必须先拷一份：k 指向 s_hist[] 的内部存储，而 ao_srch_do_search() →
     *   online_history_add() 会把数组重排（同一个词"提到最前"会把后面的整体后移）。
     *   直接带指针进去，等到真正发请求时那个地址上已经是**上一条**记录了 ——
     *   真机现象就是"点某条历史，搜出来的是另一条"（2026-10 踩过，正好错一位）。
     */
    char kw[ONLINE_KW_MAX];
    snprintf(kw, sizeof(kw), "%s", k);
    if (s_srch_ta) {
        lv_textarea_set_text(s_srch_ta, kw);
    }
    srch_do_search(kw);
}

/* 「删除这条搜索记录」二次确认通过后真正删掉 */
static void srch_hist_del_confirmed(void *user)
{
    (void)user;
    const int idx = s_hist_del_idx;
    s_hist_del_idx = -1;
    if (idx < 0) {
        return;
    }
    if (online_history_remove((size_t)idx) == ESP_OK) {
        ui_toast("已删除这条搜索记录", 1200);
        ao_srch_rebuild();
    }
}

static void srch_row_long_pressed(lv_event_t *e)
{
    if (s_srch_has_res) {
        return;                          /* 只对历史生效 */
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const char *k = online_history_at((size_t)idx);
    if (!k || !k[0]) {
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "删除这条搜索记录？\n「%.24s」", k);
    s_hist_del_idx = idx;
    ui_ba_confirm_ex("删 除 记 录", msg, "删除", "取消", true, srch_hist_del_confirmed, NULL);
}

/* 「清除全部搜索记录」二次确认通过后真正清空 */
static void srch_clear_confirmed(void *user)
{
    (void)user;
    online_history_clear();
    ui_toast("搜索记录已清空", 1200);
    ao_srch_rebuild();
}

static void srch_clear_clicked(lv_event_t *e)
{
    (void)e;
    /* 清空是"一次性全没了"，必须有二次确认（用户要求） */
    if (online_history_count() == 0) {
        ui_toast("还没有搜索记录", 1200);
        return;
    }
    ui_ba_confirm_ex("清 除 记 录", "确认清除全部搜索记录吗？", "清除", "取消", true,
                     srch_clear_confirmed, NULL);
}

void ao_srch_rebuild(void)
{
    if (!s_srch_list) {
        return;
    }
    lv_obj_clean(s_srch_list);

    const char *txt = s_srch_ta ? lv_textarea_get_text(s_srch_ta) : "";
    const bool empty = (!txt || !txt[0]);
    const bool show_hist = !s_srch_has_res && !s_srch_busy && empty && !ao_ime_open();

    if (s_srch_hint) {
        lv_label_set_text(s_srch_hint,
                          s_srch_busy ? "正在搜索"
                                      : (s_srch_has_res ? "搜索结果"
                                                        : (show_hist ? "历史搜索"
                                                                     : "输入后按键盘上的对勾搜索")));
    }
    if (s_srch_clear) {
        /* "清除"只在显示历史时出现（需求：右上角"清除"清除搜索记录） */
        if (show_hist && online_history_count() > 0) {
            lv_obj_remove_flag(s_srch_clear, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_srch_clear, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 正在搜索：转圈 + 文案。这一段 LVGL 任务没被占住（请求在工作线程），
     * 所以转圈是真的在转。 */
    if (s_srch_busy) {
        char buf[64];
        snprintf(buf, sizeof(buf), "正在搜索「%.40s」…", txt);
        ao_loading_rows(s_srch_list, buf);
        return;
    }

    if (s_srch_has_res) {
        const size_t song_n = ao_queue_count();
        if (ao_queue_owner() != OWNER_SRCH || song_n == 0) {
            lv_obj_t *h = lv_label_create(s_srch_list);
            lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
            lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
            lv_label_set_text(h, "没有找到匹配的歌曲");
            return;
        }
        /* 搜索结果同样分页（SEARCH_MAX=200 首 → 最多 2 页），一页最多 100 行 */
        const int page = ao_page_clamp(ao_srch_page(), song_n);
        ao_srch_page_set(page);
        {
            const size_t first = ao_page_first(page);
            size_t rows = (song_n > first) ? (song_n - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            ao_fill_song_rows_range(s_srch_list, first, rows);
            ao_page_rows(s_srch_list, PG_SRCH, page, ao_page_count(song_n));
            if (s_srch_hint) {
                lv_label_set_text_fmt(s_srch_hint, "搜索结果 · 第 %d/%d 页 · 共 %u 首",
                                      page + 1, ao_page_count(song_n),
                                      (unsigned)song_n);
            }
        }
        return;
    }

    if (!show_hist) {
        lv_obj_t *h = lv_label_create(s_srch_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_text(h, online_has_network()
                                ? "输入歌名或歌手，按键盘上的对勾（或回车）开始搜索"
                                : "Wi-Fi 未连接：进「设置」连上网络再搜索");
        return;
    }

    const size_t n = online_history_count();
    if (n == 0) {
        lv_obj_t *h = lv_label_create(s_srch_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(h, "还没有搜索记录（长按记录可删除单条）");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *kw = online_history_at(i);
        lv_obj_t *row = ao_make_row(s_srch_list, LV_SYMBOL_EDIT, 0x3E9E86, kw ? kw : "", NULL);
        /* ★ 同上：有长按动作的行必须听 SHORT_CLICKED，否则"长按删记录"会顺带搜一次 */
        lv_obj_add_event_cb(row, srch_row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, srch_row_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

static void srch_do_search(const char *kw)
{
    if (!kw || !kw[0]) {
        return;
    }
    /*
     * ★ 咽喉处再拷一份（防御性）：调用方可能传进来指向 s_hist[] 内部的指针，而下面的
     *   online_history_add() 会 hist_move_to_front() 重排整个数组 —— 不拷的话，
     *   最后 ao_work_submit() 取到的就是被移动后落在那个地址上的**别的关键词**。
     */
    char k[ONLINE_KW_MAX];
    snprintf(k, sizeof(k), "%s", kw);
    if (!k[0]) {
        return;
    }
    if (!online_has_network()) {
        ui_toast("Wi-Fi 未连接", 1500);
        return;
    }
    online_history_add(k);           /* 记历史（服务侧去重 + 提到最前） */
    s_srch_busy    = true;           /* 先进入"正在搜索"态（见 s_srch_busy 的说明） */
    s_srch_has_res = false;          /* 结果没回来之前不算"有结果"，避免误报"没找到" */
    ao_srch_page_set(0);             /* 新结果从第 1 页看起 */
    ao_ime_hide();
    ao_srch_rebuild();
    ao_work_submit(OREQ_SEARCH, k);
}

/*
 * 输入框事件。
 *
 * ★ **绝不能在 LV_EVENT_DEFOCUSED 里收键盘**（2026-10 用户报"点一下键盘它自己就收了"）：
 *   点键盘上的按键时，焦点会从输入框转到那个按键对象上（按键也是可聚焦的 widget），
 *   输入框随即收到 DEFOCUSED —— 早期版本在这个分支里 ao_ime_hide()，
 *   于是"按第一个键，键盘就没了"。
 *   现在收键盘只有三个入口：
 *     1) 输入框为空时按对勾/回车（见 ao_srch_ime_ready）；
 *     2) 点列表/空白区（见 srch_hide_ime_clicked）；
 *     3) 离开本页、或切回"我的歌单"页（online_on_delete / ao_page_show）。
 */
static void srch_ta_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        ao_ime_show_for(s_srch_ta);
        ao_srch_rebuild();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        /* 改了输入内容：旧的搜索结果就不算数了 */
        s_srch_has_res = false;
        ao_srch_rebuild();
    }
}

/*
 * 点列表/空白处 → 收起键盘。顺手重画一次：这样"输入框为空 + 键盘已收起"
 * 三个条件齐了，历史记录会自己回来（见 ao_srch_rebuild 的规则）。
 * 只挂在列表上（它是输入框的**兄弟**节点）：点输入框本身不会走到这里。
 */
static void srch_hide_ime_clicked(lv_event_t *e)
{
    (void)e;
    if (ao_ime_open()) {
        ao_ime_hide();
        ao_srch_rebuild();
    }
}

/* 输入法上的对勾 / 回车（组件在"没有未上屏拼音"时发这个事件） */
void ao_srch_ime_ready(lv_event_t *e)
{
    (void)e;
    const char *txt = s_srch_ta ? lv_textarea_get_text(s_srch_ta) : "";
    if (txt && txt[0]) {
        srch_do_search(txt);
    } else {
        ao_ime_hide();               /* 输入框是空的：对勾 就是"收起键盘" */
        ao_srch_rebuild();
    }
}

lv_obj_t *ao_srch_build(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page, LV_PCT(100), 0);   /* 初始在右边一屏外 */

    s_srch_ta = lv_textarea_create(page);
    lv_obj_set_width(s_srch_ta, LV_PCT(100));
    lv_obj_set_height(s_srch_ta, UI_PX_H(46));
    lv_obj_set_style_text_font(s_srch_ta, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_srch_ta, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_srch_ta, UI_COLOR_FG, LV_PART_MAIN);
    lv_textarea_set_one_line(s_srch_ta, true);
    lv_textarea_set_max_length(s_srch_ta, 60);
    lv_textarea_set_placeholder_text(s_srch_ta, "输入歌名或歌手，按键盘对勾 搜索");
    lv_obj_add_event_cb(s_srch_ta, srch_ta_event, LV_EVENT_ALL, NULL);

    /* 列表小标题（左）+ "清除"（右，只在显示历史时出现） */
    lv_obj_t *srch_head = lv_obj_create(page);
    lv_obj_remove_style_all(srch_head);
    lv_obj_set_size(srch_head, LV_PCT(100), UI_PX_H(26));
    lv_obj_remove_flag(srch_head, LV_OBJ_FLAG_SCROLLABLE);

    s_srch_hint = lv_label_create(srch_head);
    lv_obj_set_style_text_font(s_srch_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_srch_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_srch_hint, "历史搜索");
    lv_obj_align(s_srch_hint, LV_ALIGN_LEFT_MID, 0, 0);

    s_srch_clear = lv_label_create(srch_head);
    lv_obj_set_style_text_font(s_srch_clear, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_srch_clear, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(s_srch_clear, "清除");
    lv_obj_add_flag(s_srch_clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_srch_clear, UI_PX_H(10));
    lv_obj_add_event_cb(s_srch_clear, srch_clear_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_srch_clear, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s_srch_clear, LV_OBJ_FLAG_HIDDEN);

    s_srch_list = lv_obj_create(page);
    lv_obj_remove_style_all(s_srch_list);
    lv_obj_set_width(s_srch_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_srch_list, 1);
    lv_obj_set_style_pad_right(s_srch_list, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_srch_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_srch_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_srch_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_srch_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_srch_list, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_srch_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_srch_list, UI_PX_W(6), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_srch_list, UI_PX_H(3), LV_PART_SCROLLBAR);
    /* 点列表/空白处收键盘（"点别处收起键盘"的手感；输入框自己是列表的兄弟，不受影响） */
    lv_obj_add_flag(s_srch_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_srch_list, srch_hide_ime_clicked, LV_EVENT_CLICKED, NULL);
    return page;
}
