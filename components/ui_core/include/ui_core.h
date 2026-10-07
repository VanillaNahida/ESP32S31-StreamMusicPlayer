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