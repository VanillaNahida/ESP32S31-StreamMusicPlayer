/*
 * music_view_output.h —— 输出设备：控制排上的按钮 + 选择弹窗 + 音量键
 *
 * 从 app_music.cc 的 700–705、919–1137、1126–1137 整组搬出来。
 *
 * 实现要点（都踩过坑，别改）：
 *   - 弹窗挂在 lv_layer_top()，不挂在页面 screen 上：页面被 pop（异步销毁）时
 *     弹窗不该跟着页面一起消失（那是未定义行为），页面 DELETE 里显式关掉它；
 *   - 关弹窗一律走 ui_ba_close()（内部是异步删除）：回调是**弹窗自己的子对象**
 *     发出来的，同步删会在 LVGL 事件派发器还在遍历这棵子树时把它抹掉 → hard fault；
 *   - 切换输出只调 audio_service_set_output()，按钮图标由 tick 统一刷新，
 *     避免"点了没反应/图标和实际不一致"。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 控制排上的音量键：从右边缘滑出竖向音量条（ui_volume_panel.cc） */
void music_view_output_on_volume(lv_event_t *e);

/*
 * 造控制排上的输出设备按钮：点 = 打开选择弹窗，长按 = 直接跳到"设置 → 蓝牙"。
 * 返回按钮对象（图标 label 由本模块自己记住）。
 *
 * ★ 短按必须挂 **LV_EVENT_SHORT_CLICKED** 而不是 CLICKED：
 *   indev_proc_release() 里 SHORT_CLICKED 只在 long_pr_sent == 0 时才发，
 *   而 CLICKED 是**无条件**发的 —— 用 CLICKED 的话长按之后松手会
 *   "既跳了设置页、又开出选择弹窗"（同源结论见 AGENTS 14.6）。
 */
lv_obj_t *music_view_output_make_btn(lv_obj_t *parent);

/* 按钮图标跟着实际输出走（由 tick 调用，改动才写 LVGL） */
void music_view_output_sync(void);

/* 页面销毁：显式关掉挂在 top layer 上的弹窗并清掉按钮指针 */
void music_view_output_on_page_delete(void);

#ifdef __cplusplus
}
#endif
