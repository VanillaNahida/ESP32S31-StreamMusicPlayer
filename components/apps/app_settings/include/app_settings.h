/*
 * app_settings.h —— 设置 APP 注册入口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 "settings" 桌面项（覆盖 ui_home 的占位页）；在 ui_core_init 之前调 */
void app_settings_register_apps(void);

#ifdef __cplusplus
}
#endif
