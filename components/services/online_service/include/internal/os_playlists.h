/*
 * os_playlists.h —— 歌单的 NVS 列表 + 抓取 + 落盘编排
 *
 * 原文件里的名字：nvs_load_ids / nvs_save_ids（732–787）、id_index（789–797）、
 * online_playlist_add/remove/load（960–1040）。
 *
 * 状态（s_ids / s_names / s_id_count）统一由 os_state.h 的访问器读写；
 * 二进制格式与读写盘在 os_pl_format.h；JSON 解析在 os_json.h。
 * 对外契约仍是 online_service.h 里的那三个函数（这里只是搬到本文件实现）。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 NVS 读 `n` + `id0..id7` + `nm0..nm7` 到内存表（init 调用一次） */
void os_playlists_nvs_load(void);
/* 全量重写 NVS（多余键 erase 掉）后 commit */
void os_playlists_nvs_save(void);

#ifdef __cplusplus
}
#endif
