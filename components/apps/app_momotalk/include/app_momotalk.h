/*
 * app_momotalk.h —— MomoTalk 聊天 APP（测试用 echo 对话）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 注册/替换 "momotalk" APP 描述符（需在 ui_core_init 之前调用） */
void app_momotalk_register_apps(void);

#ifdef __cplusplus
}
#endif
