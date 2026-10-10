/*
 * os_history.h —— 搜索历史（NVS，最多 10 条，新的在最前，按关键词去重）
 *
 * 原文件里的名字：nvs_load_history（1913–1935）、nvs_save_history（1937–1955）、
 * hist_move_to_front（1958–1969）、online_history_*（1971–2027）。
 *
 * 状态（s_hist / s_hist_n）是本文件私有的 static；纯逻辑部分是 hist_move_to_front
 * 与"头插 + 截断"，可 host 单测（蓝图 §6.3）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 NVS 读 `shn` + `sh0..sh9` 到内存表（init 里调一次） */
void os_history_nvs_load(void);

#ifdef __cplusplus
}
#endif
