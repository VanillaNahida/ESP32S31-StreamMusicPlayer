/*
 * mm_id3.h —— ID3v2（MP3）帧遍历：封面帧 APIC/PIC、歌词帧 USLT/SYLT、文本编码
 *
 * 实现见 mm_id3.cc；封面提取的编排在 mm_cover.cc，这里只做"从 FILE* 里把某一帧
 * 的 body 取出来"。所有函数只读输入文件，不持有句柄、不分配长期资源。
 *
 * 文本编码转换（mm_id3_text_to_utf8 / mm_id3_sylt_to_lrc）是纯逻辑，
 * 可脱离真机单测（拆分蓝图 §6.3）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 取 ID3v2 里的专辑图（v2.2 的 PIC / v2.3、v2.4 的 APIC）。
 * 成功时 *out 是 malloc 的图片字节（调用方 free），*out_len 是长度，
 * *is_png 是按 MIME/格式字段猜的（**仅供日志**，解码按魔数分派）。
 * （原 media_meta.cc 的 id3_cover()）
 */
esp_err_t mm_id3_cover(FILE *f, uint8_t **out, size_t *out_len, bool *is_png);

/*
 * 取 ID3v2 里的歌词原文：USLT 优先，没有 USLT 就用 SYLT 转成 LRC 文本。
 * 成功时 *out 是 malloc 的 UTF-8 文本（调用方 free）。
 * （原 media_meta.cc 的 id3_lyrics()）
 */
esp_err_t mm_id3_lyrics(FILE *f, char **out);

/*
 * ID3 文本 → UTF-8，返回写入字节数（保证 dst NUL 结尾）。
 * enc: 0=Latin1, 1=UTF-16(带 BOM), 2=UTF-16BE, 3=UTF-8
 * （原 static id3_text_to_utf8()）
 */
size_t mm_id3_text_to_utf8(uint8_t enc, const uint8_t *src, size_t src_len,
                           char *dst, size_t dst_cap);

/* SYLT（同步歌词）→ LRC 文本；失败返回 NULL（原 static sylt_to_lrc()） */
char *mm_id3_sylt_to_lrc(const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif
