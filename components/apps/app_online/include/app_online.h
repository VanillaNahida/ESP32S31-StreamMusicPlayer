/*
 * app_online.h —— 在线音乐 APP（网易云歌单 + 搜索）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 注册/替换 "online" APP 描述符（需在 ui_core_init 之前调用） */
void app_online_register_apps(void);

#ifdef __cplusplus
}
#endif
