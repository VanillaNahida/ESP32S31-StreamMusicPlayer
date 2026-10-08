/*
 * ui_core.h —— UI 框架层对外接口
 *
 * 全局 UI（状态栏 / 控制中心 / 抽屉 / Toast / 手势）挂在 lv_layer_top()，
 * 跨页面常驻；页面切换走 router 的页面栈，不用 lv_screen_load 裸调。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 页面路由 */

typedef struct page_desc {
    const char *id;
    lv_obj_t  *(*create)(void);   /* 返回一个 lv_obj_create(NULL) 建出的 screen */
    void       (*on_enter)(void);
    void       (*on_exit)(void);
    /*
     * no_back = true：该页**自带**返回钮（或本身就是二级页外壳的一部分），
     * 不要自动叠全局返回键 —— 见 ui_back_button_update() 与 ui_router.cc。
     * 设置页（make_page_shell 自带返回钮）必须置位，否则会出两个返回键。
     */
    bool no_back;
} page_desc_t;

/* 全局 UI 初始化：top layer（状态栏/控制中心/手势/Toast）+ 首页 */
esp_err_t ui_core_init(void);

/* 压栈。**描述符内容会被拷贝**，因此可以安全地传栈上临时量；
 * 但 page->create 返回的 screen 由 router 接管（pop 时销毁）。
 * page->id 必须指向静态存储期字符串（会被日志/查找使用）。 */
esp_err_t ui_router_push(const page_desc_t *page);
esp_err_t ui_router_pop(void);
esp_err_t ui_router_reset_to_home(void);
/* 当前栈深（首页为 1） */
int ui_router_depth(void);

/* ------------------------------------------------------------------ 页面返回键 */

/*
 * 全局返回键（推荐用法）：ui_core_init 时在 lv_layer_top 叠一个圆钮，
 * 点击等价于 ui_router_pop()。ui_router_push/pop 之后会自动刷新它的
 * 显示状态：**栈深 > 1 且当前页 desc.no_back == false 时显示**，否则隐藏。
 * 所以：
 *   - 首页（栈深 1）不显示；
 *   - 普通二级页（播放器 / 在线音乐 / 占位页…）自动就有返回键，页面里
 *     不用再画，标题只需让到 UI_HEADER_TEXT_X 给按钮留位；
 *   - 自带返回钮的页面（设置页 make_page_shell）把 page_desc_t.no_back
 *     置 true 即可。
 * 该键建在 lv_layer_top 上、z 序低于控制中心/抽屉/Toast/遮罩：
 * 模态遮罩打开时它会一并被盖住（点不到，符合预期），关闭后自动恢复。
 */

/*
 * 手动返回键（仅给自带返回钮的页面用，如设置页 make_page_shell）：
 * 点击等价于 ui_router_pop()。
 *
 * @param parent 页头元素：标题所在的那个容器，也可以直接是 screen
 * @param align  按钮的对齐方式，与该页标题保持一致
 *               （标题在容器里用 LV_ALIGN_LEFT_MID；标题直接挂 screen 用 LV_ALIGN_TOP_LEFT）
 * @return 按钮对象，不需要时忽略返回值
 *
 * ⚠ 屏幕最左 UI_EDGE_HOTZONE(24 px) 是返回手势热区，它 z 序高于页面，
 *   所以按钮不能贴 x=0（左半截会被热区吃掉、点不动）。helper 已经把按钮放在
 *   UI_BACK_BTN_X 处，调用方只需把标题右移到 UI_HEADER_TEXT_X。
 * ⚠ 用自动全局返回键的页面**不要再调这个**，否则会出两个返回键。
 */
lv_obj_t *ui_back_button_create(lv_obj_t *parent, lv_align_t align);

/* ------------------------------------------------------------------ 状态栏 */

void ui_status_set_clock(const char *hhmm, const char *date_text);
void ui_status_set_wifi(int level /*0~4*/, bool connected);
void ui_status_set_bt(bool connected, const char *device_name);
void ui_status_set_output(const char *channel_name);
void ui_status_set_sd(bool mounted);

/* ------------------------------------------------------------------ 控制中心 / 抽屉 */

void ui_cc_open(void);
void ui_cc_close(void);
void ui_cc_toggle(void);
bool ui_cc_is_open(void);

void ui_drawer_open(void);
void ui_drawer_close(void);
bool ui_drawer_is_open(void);

/* ------------------------------------------------------------------ 轻提示 */

void ui_toast(const char *text, uint32_t duration_ms);

/* ------------------------------------------------------------------ 音量悬浮条
 *
 * 从屏幕右边缘滑入的竖向音量条，停在**屏幕靠右中间**（见 ui_volume_panel.cc）。
 * 顶部 = 100%、往上拖 = 变大，填充自下而上增长（拖动方向与填充方向一致）。
 * 显示后 2.5 秒无操作自动缩回；拖动即跟手调音量。
 */
void ui_volume_show(void);
void ui_volume_hide(void);

/*
 * 硬件 VOL± 按键步进音量（一格 = 5%）：按一次 +1 / -1。
 *
 * **可以在任意任务上下文调用**（例如 bsp 按键回调里）：这里只做一次原子
 * 累加，真正的音量下发与"弹出音量条"由 ui_volume_panel.cc 的 LVGL 定时器
 * 在 LVGL 任务上下文完成。息屏（LVGL 暂停）期间计数会夹紧，不会积压爆发。
 */
void ui_volume_key_step(int dir);

/* ------------------------------------------------------------------ 息屏（停刷新，音频继续） */

void ui_screen_off(void);
void ui_screen_on(void);
bool ui_screen_is_off(void);

#ifdef __cplusplus
}
#endif