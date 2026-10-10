/*
 * bts_settings.h —— bt_service 内部：开关/上次设备的 NVS 记忆 + 开机自动连定时器
 *
 * 对应 NVS 命名空间 bt_cfg：`on`（总开关，默认开）、`last`（上次连上的设备，6 字节 blob）。
 * 键名与拆分前完全一致。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 总开关（NVS 记忆；关掉时控制器 + 协议栈一起 deinit 释放资源） */
extern volatile bool bts_enabled;
/* 是否已经从 NVS 读过开关（UI 在服务尚未 init 时也要能显示正确的开关状态） */
extern bool          bts_settings_loaded;

/* 读 bt_cfg/on 与 bt_cfg/last（没存过 = 默认开、没有上次设备） */
void bts_settings_load(void);

/* 存总开关 */
void bts_settings_save_enabled(bool on);

/* 存"上次连上的设备"并同步内存（开机自动连用） */
void bts_settings_save_last(const uint8_t bda[6]);

/* 开机自动连：有"上次设备"就（重）起 3 s 一次性定时器，没有就什么都不做 */
void bts_auto_connect_arm(void);

/* 停掉自动连定时器（deinit 用） */
void bts_auto_connect_stop(void);

#ifdef __cplusplus
}
#endif
