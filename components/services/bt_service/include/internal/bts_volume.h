/*
 * bts_volume.h —— bt_service 内部：AVRCP 绝对音量（esp_bt_audio_vol.h）
 *
 * 为什么单独一个文件：蓝牙音量有两条完全不同的路，别混在一起
 *   1. **AVRCP 绝对音量**（本文件）：我们把 0~100 的音量发给耳机/音箱，
 *      由**对端**去衰减。这是 A2DP 源的标准做法（手机也是这么干的），
 *      好处是对端的音量条/指示灯跟着动，且不占我们 CPU。
 *   2. 软件缩放（audio_output.cc 的 pcm16_scale）：AVRCP 不可用时（CT 没连上、
 *      对端不支持）才退回这条路，逐样本乘系数。
 * 两条路**同时只能有一条生效**，否则音量会被衰减两次（滑到 50% 听感像 25%）。
 *
 * 开关判据就是 bts_volume_avrcp_ok()：它反映"上一次 esp_bt_audio_vol_set_absolute()
 * 是否成功"。esp_bt_audio 在没有 CT ops（没连上 AVRCP Controller）时返回
 * ESP_ERR_INVALID_STATE，我们据此退回软件缩放。
 *
 * ⚠ 本文件的函数可能在**任意任务**调用；esp_bt_audio_vol_set_absolute() 内部只是
 *   往 BTC 投一条 AVRCP 命令（非阻塞），可以在这里直接调。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bt_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 清掉"AVRCP 可用"标记（断开/关机时调），保留应用设定的音量值 */
void bts_volume_reset(void);

/* 应用要改音量（0~100）：缓存 + 立刻发给对端（没连上就只缓存，连上后再发） */
void bts_volume_set(int percent);

/* 把缓存的音量再发一次：A2DP 流启动、或刚从扬声器切到蓝牙时调 */
void bts_volume_reapply(void);

/* 对端通过 AVRCP 改了音量（事件回调里调）：同步缓存并通知上层 */
void bts_volume_on_peer(int percent);

int  bts_volume_get(void);
bool bts_volume_avrcp_ok(void);

/* 上层注册的"对端改了音量"回调（跑在蓝牙栈任务上下文） */
typedef void (*bts_volume_cb_t)(int percent, void *user);
void bts_volume_set_peer_cb(bts_volume_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
