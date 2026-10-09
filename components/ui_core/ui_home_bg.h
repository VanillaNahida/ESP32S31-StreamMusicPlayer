/*
 * ui_home_bg.h —— 首页背景图（assets/background.jpg，编译期嵌进 ui_core）
 *
 * 内部头（不进 include/）：只有 ui_core.cc 与 ui_home.cc 用。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 起后台解码任务（幂等）。在 ui_core_init() 里尽早调一次：
 * 800x480 的 JPEG 软解要几百毫秒，放后台就能与其它初始化并行 ——
 * 首页建出来时（甚至更早）图已经好了，用户看不到"先纯色再跳出壁纸"。
 */
void ui_home_bg_start(void);

/*
 * 绑定首页 screen：建首页时传它，首页销毁时传 NULL。
 * 图已解好就立刻贴上；还没解好则由解码任务完成后补上（那条路会自己加 LVGL 锁）。
 * ⚠ 必须在 LVGL 任务上下文调用（或已持 esp_lv_adapter_lock）。
 */
void ui_home_bg_bind(lv_obj_t *home_scr);

#ifdef __cplusplus
}
#endif
