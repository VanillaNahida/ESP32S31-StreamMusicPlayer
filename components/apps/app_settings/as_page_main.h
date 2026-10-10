/*
 * as_page_main.h —— 设置一级页（主页）的构建入口
 *
 * 只有 `page_settings` 需要被外面看到：`app_settings.cc` 要把它注册进 ui_app_registry。
 * 其余（摘要 tick / 删除回调 / s_sum[]）都是本页私有的。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 构建设置主页（一级页）。注册表里的 create 回调。 */
lv_obj_t *page_settings(void);

#ifdef __cplusplus
}
#endif
