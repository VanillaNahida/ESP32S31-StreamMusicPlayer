/*
 * bts_events.h —— bt_service 内部：蓝牙栈事件回调（发现/连接/流状态/媒体键）
 *
 * 对应 bt_service.cc 887–1020。回调跑在蓝牙栈（BTC）任务上下文：
 * **别做重活、别碰 LVGL**（远端媒体键转发出去的回调也一样，见 bt_service.h）。
 */
#pragma once

#include "bt_service.h"
#include "esp_bt_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册给 esp_bt_audio_config_t.event_cb */
void bts_audio_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data);

/* 远端媒体键回调（注册在 LVGL 任务、调用在 BT 回调任务） */
extern bt_media_ctrl_cb_t bts_ctrl_cb;
extern void              *bts_ctrl_user;

#ifdef __cplusplus
}
#endif
