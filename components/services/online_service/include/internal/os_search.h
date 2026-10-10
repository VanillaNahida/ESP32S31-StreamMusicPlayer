/*
 * os_search.h —— 在线搜索（服务端 + 本地兜底）
 *
 * 原文件里的名字：search_local（1065–1110）、online_search（1116–1160）。
 * 关键词的百分号编码（url_encode 1046–1062）搬到了纯逻辑的 os_util.h：
 * os_url_encode()。
 *
 * ⚠ 本地兜底的扫盘循环原来自己抄了一份 pl_load 的读盘逻辑（只校验 magic），
 *   已收敛到 os_pl_format.h 的 os_pl_open_for_scan()/os_pl_read_record()
 *   —— 判据保持"只认 magic"不变（蓝图 R8 的行为面）。
 */
#pragma once

#include <stddef.h>

#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本地兜底：在所有已保存歌单里按子串找（不需要网络）。
 * 命中时 *out = acc（调用方给的缓冲，所有权仍归调用方），否则返回错误码。 */
esp_err_t os_search_local(const char *keyword, online_song_t **out, size_t *count,
                          size_t max, online_song_t *acc);

#ifdef __cplusplus
}
#endif
