/*
 * mm_lrc.h —— LRC 歌词解析 / 时间轴索引 / 编码归一化（模块内部接口）
 *
 * 实现见 mm_lrc.cc。对外的 media_meta_* 包装在 media_meta.cc 里（门面），
 * 这里只给组件内部与 host 单测用。
 *
 * 除 esp_err 错误码与 esp_log（诊断）外不依赖 ESP-IDF / 硬件：
 * mm_lrc_parse_utf8 / mm_lrc_bytes_to_utf8 / mm_lrc_utf8_valid / mm_lrc_utf8_put
 * 是纯逻辑，可脱离真机单测（拆分蓝图 §6.3）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_meta.h"     /* lrc_t / lrc_line_t：对外契约里的类型 */

#ifdef __cplusplus
extern "C" {
#endif

/* 清空 / 释放 lrc_t 内部缓冲（对外 media_meta_lrc_reset 的实现） */
void mm_lrc_reset(lrc_t *lrc);

/* 解析 UTF-8 文本（不猜编码；内部用） */
esp_err_t mm_lrc_parse_utf8(const char *text, lrc_t *out);

/* 检测编码（BOM / UTF-8 / GBK）后解析（对外 media_meta_lrc_parse 的实现） */
esp_err_t mm_lrc_parse(const char *text, lrc_t *out);

/* 读一个 .lrc 文件并解析（≤256 KB） */
esp_err_t mm_lrc_load_file(const char *path, lrc_t *out);

/* 音频同名 .lrc 边文件（把扩展名换成 .lrc） */
esp_err_t mm_lrc_load_sidecar(const char *audio_path, lrc_t *out);

/* 二分查 pos_ms 时刻应高亮的行下标；无歌词 -1 */
int mm_lrc_index_at(const lrc_t *lrc, uint32_t pos_ms);

/* ---- 纯编码工具（无 IO，host 单测入口） ---- */

/* 码点 → UTF-8（≤3 字节），返回新的写指针 */
char *mm_lrc_utf8_put(char *d, uint32_t cp);

/* 严格校验 UTF-8（过长编码 / 代理区 / 超范围都算非法） */
bool mm_lrc_utf8_valid(const uint8_t *s, size_t n);

/* 原始字节 → UTF-8（BOM / 合法 UTF-8 / GBK）；返回 malloc 的串，调用方 free */
char *mm_lrc_bytes_to_utf8(const uint8_t *raw, size_t n);

#ifdef __cplusplus
}
#endif
