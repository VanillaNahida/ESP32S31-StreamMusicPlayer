/*
 * ao_pages.h —— **通用 LVGL 层**：页面骨架（三页 tab + 左右滑动切页）+ 控件工厂
 *
 * 页序（**必须与 PG_* 常量、动画下标一致**，加页时三处一起改）：
 *   0 = 我的歌单（网易云账号） / 1 = 我添加的歌单 / 2 = 搜索
 *
 * 三个页面容器（s_page_acct / s_page_pl / s_page_srch）由本模块持有：
 * 切页动画要按下标同时平移它们；各页自己的控件（列表/提示/返回行）由各自的 .cc 持有。
 *
 * 控件工厂（ao_make_row / 占位 / 翻页行 / 返回行）也在这里：它们本来就是"跟哪一页无关"
 * 的通用件，四个列表长得一模一样就靠它，别再各写一份。
 *
 * ⚠ 2026-10 拆分说明：蓝图里 `ao_widgets.cc`（约 180 行）与 `ao_pages.cc`（约 120 行）
 *   是两份；实际落地后两份都远不到"每个 .cc 300~600 行"的下限，而它们同属"通用 LVGL
 *   层"，合并成一份既满足行数带又不增加耦合 —— 这是对蓝图的**微调**，职责没变。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "ao_common.h"

/* ---------------------------------------------------------------- 骨架 / 切页 */

/* 给控件挂 LV_EVENT_GESTURE（屏幕绝大部分面积是行，横滑多从行上起手） */
void ao_pager_attach(lv_obj_t *o);

/* 建顶部三个 tab 标签（文字即标题，点一下切页）并登记 */
void ao_pages_build_tabs(lv_obj_t *tabs);
/* 建三页的宿主（负责裁剪，页宽也从它取）+ 挂手势 */
lv_obj_t *ao_pages_build_host(lv_obj_t *scr);

void ao_pages_set_root(lv_obj_t *root);
void ao_pages_set_page(int which, lv_obj_t *page);

/* 切页：夹页号、改页签样式、离开搜索页收键盘、220 ms 缓出平移 */
void ao_page_show(int which, bool animate);
/* 当前页号（poll_cb 判断"本页可见吗"用） */
int  ao_pages_current(void);

/* online_on_delete()：清掉骨架里所有控件指针 */
void ao_pages_clear(void);

/* ---------------------------------------------------------------- 第 0 页：我的歌单 */

lv_obj_t *ao_acct_build(lv_obj_t *parent);
void      ao_acct_rebuild(void);
void      ao_acct_clear(void);
lv_obj_t *ao_acct_list(void);
/* 重新载入当前歌单详情的第 1 页（删除歌曲成功后用；每日推荐/无详情时什么都不做） */
void      ao_acct_detail_refresh(void);

/* ---------------------------------------------------------------- 第 1 页：我添加的歌单 */

lv_obj_t *ao_pl_build(lv_obj_t *parent);
void      ao_pl_rebuild(void);
void      ao_pl_clear(void);
lv_obj_t *ao_pl_list(void);
/* 长按菜单（页面销毁前必须关；点行进详情时也要先关） */
void      ao_pl_menu_close(void);
/* 重命名目标（pl_menu_close() 会清 s_menu_idx，所以先存到这份里） */
int       ao_pl_rename_idx(void);
void      ao_pl_rename_idx_set(int idx);

/* ---------------------------------------------------------------- 第 2 页：搜索 */

lv_obj_t *ao_srch_build(lv_obj_t *parent);
void      ao_srch_rebuild(void);
void      ao_srch_clear(void);
lv_obj_t *ao_srch_list(void);
/* page_online() 里 layout 之后、首次 page_show 之前的一次性复位 */
void      ao_srch_reset_view(void);
void      ao_srch_set_busy(bool busy);
void      ao_srch_set_has_res(bool has);
/* 拼音键盘上的对勾 / 回车（ao_modal.cc 按上下文分派过来） */
void      ao_srch_ime_ready(lv_event_t *e);

/* ---------------------------------------------------------------- 通用控件工厂 */

/* 通用列表行（图标块 + 标题 + 右侧副标题），返回行对象 */
lv_obj_t *ao_make_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                      const char *title, const char *sub);

/* 歌曲列表（歌单详情 / 搜索结果 / 账号歌单共用）：画 s_songs[first, first+count) */
void ao_fill_song_rows_range(lv_obj_t *list, size_t first, size_t count);

/* 列表内容整段换掉之后回到顶部（不归零的话新内容会从中间开始显示） */
void ao_list_scroll_top(lv_obj_t *list);

/* "‹ 上一页 / 下一页 ›"两行（页码提示由各页自己写在 hint 里） */
void ao_page_rows(lv_obj_t *list, int which, int page, int pages);

/* 详情页顶部"‹ 返回"行（**必须挂在页容器上，不放进可滚动列表**，见实现里的说明） */
lv_obj_t *ao_make_back_label(lv_obj_t *parent, const char *text, lv_event_cb_t cb);

/* 统一"转圈 + 一行字"占位 */
void ao_loading_rows(lv_obj_t *list, const char *text);

/* 详情占位：转圈；超 DETAIL_TIMEOUT_MS 换成"重试"入口（reload_cb 可为 NULL） */
void ao_detail_wait_rows(lv_obj_t *list, lv_event_cb_t reload_cb);

/* 点歌行 → 起播（搜索页的历史行/结果行共用） */
void ao_online_row_clicked(lv_event_t *e);
