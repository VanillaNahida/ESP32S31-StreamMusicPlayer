/*
 * os_util.h —— 纯逻辑工具（UTF-8 安全截断 / ID 提取 / 百分号编码 / 音频扩展名嗅探）
 *
 * 这一组**不依赖 ESP-IDF、不依赖文件系统与 LVGL**（只用 <string.h> 与 snprintf），
 * 可以直接在 host 侧编两遍做单测（拆分蓝图 §6.3）。
 * 需要 ESP-IDF 的那几个（PSRAM 分配 / 文件判定 / 目录操作）在 os_fs.h 里。
 *
 * 实现见 os_util.cc。原文件里的名字：str_copy / id_from_url / url_encode /
 * ext_from_sniff（分别来自 online_service.cc 的 234、261、1046、1164 行）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UTF-8 安全截断拷贝：装不下就退到多字节字符边界，绝不切在字符中间。
 * dst 为空或 cap == 0 直接返回；src 为 NULL 时写空串。
 */
void os_str_copy(char *dst, size_t cap, const char *src);

/* 从 meting 的 url 字段里抠出歌曲 id：...&id=123456（找不到 / url 为 NULL → 0） */
uint32_t os_id_from_url(const char *url);

/* UTF-8 关键词 → 百分号编码（只放行 RFC3986 的 unreserved 字符）。
 * 中文关键词会被编码成 %E5%91%A8...，服务器那边按 UTF-8 解。 */
void os_url_encode(const char *src, char *dst, size_t cap);

/* 16 字节魔数 → 音频扩展名（含 '.'），默认 ".mp3"。
 * ⚠ 判定顺序与原 ext_from_sniff 完全一致（ASP 靠扩展名选解码器），不要调整。 */
void os_ext_from_sniff(const uint8_t *sniff, char *ext, size_t cap);

#ifdef __cplusplus
}
#endif
