/*
 * app_settings.h —— 设置 APP 注册入口
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 "settings" 桌面项（覆盖 ui_home 的占位页）；在 ui_core_init 之前调 */
void app_settings_register_apps(void);

/*
 * 直接跳到设置里的**蓝牙页**（在任意页面/事件回调里调，内部走 ui_router_push）。
 * 用途：音乐页长按输出设备按钮 → 去连蓝牙耳机。
 * 返回 ESP_OK 表示已压栈；栈满等情况下返回错误，调用方可以 toast 一下。
 */
esp_err_t app_settings_open_bt(void);

#ifdef __cplusplus
}
#endif
