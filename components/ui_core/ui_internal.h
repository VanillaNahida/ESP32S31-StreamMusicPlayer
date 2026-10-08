/*
 * ui_internal.h —— ui_core 组件内部接口
 */
#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 各全局层（都建在 lv_layer_top() 下） */
lv_obj_t *ui_status_bar_create(lv_obj_t *parent);
lv_obj_t *ui_cc_create(lv_obj_t *parent);
lv_obj_t *ui_toast_create(lv_obj_t *parent);
lv_obj_t *ui_volume_panel_create(lv_obj_t *parent);   /* 音量悬浮条（ui_volume_panel.cc） */

/* 手势层：把边缘热区与状态栏下拉接上 */
void ui_gesture_attach(lv_obj_t *status_bar);

/* 面板展开期间挂起/恢复手势热区：热区在 z 序上高于遮罩，
 * 不停用的话点击屏幕最左/最右 24 px 会被热区吃掉，遮罩收不到"点任意处关闭"。 */
void ui_gesture_suspend(bool suspend);

/* 路由内部入口（ui_router.cc） */
esp_err_t ui_router_init_home(void);
/* 不带动画的退栈：给返回手势用（手势已经把顶层页 1:1 滑出去了） */
esp_err_t ui_router_pop_immediate(void);
lv_obj_t *ui_router_top_screen(void);
lv_obj_t *ui_router_under_screen(void);
/* 当前栈顶页是否需要全局返回键（栈深 > 1 且 desc.no_back == false） */
bool ui_router_top_needs_back(void);

/* 全局返回键（ui_core.cc）：建在 lv_layer_top，路由 push/pop 后自动刷新 */
lv_obj_t *ui_back_button_global_create(lv_obj_t *parent);
void ui_back_button_update(void);

/* 首页（ui_home.cc） */
lv_obj_t *ui_home_create(void);

/* 控制中心 / 抽屉对象（ui_control_center.cc） */
lv_obj_t *ui_sheet_root(void);           /* 控制中心 + 抽屉的共同父容器 */
void ui_cc_apply_open(bool open, bool animate);
void ui_drawer_apply_open(bool open, bool animate);
bool ui_cc_state(void);
bool ui_drawer_state(void);

/* 跟手拖动（手势层调用）：dy 为相对按下点的纵向位移，dx 为横向位移（向左为负） */
void ui_cc_drag(int dy);
void ui_cc_drag_end(int dy, int vel);
void ui_drawer_drag(int dx);
void ui_drawer_drag_end(int dx, int vel);

#ifdef __cplusplus
}
#endif