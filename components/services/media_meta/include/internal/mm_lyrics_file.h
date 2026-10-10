/*
 * mm_lyrics_file.h —— 内嵌歌词提取 + 歌词三级查找 + /lrc 落盘
 *
 * 实现见 mm_lyrics_file.cc。对外契约（优先级、失败码、所有权）与拆分前完全一致：
 *   1) 歌曲同目录同名 .lrc
 *   2) <SD 挂载点>/lrc/<同名>.lrc
 *   3) 内嵌 ID3 歌词（取到后**自动落盘**到 2 档的位置）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "media_meta.h"     /* lrc_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 内嵌歌词原文（UTF-8，malloc 的）；只做"取出来"这一件事，不落盘（原 media_meta_embedded_lyrics） */
esp_err_t mm_embedded_lyrics(const char *audio_path, char **text);

/* 释放内嵌歌词文本（原 media_meta_lyrics_free） */
void mm_lyrics_free(char *text);

/* 三级查找（原 media_meta_lyrics_load；内嵌那档会写 SD） */
esp_err_t mm_lyrics_load(const char *audio_path, lrc_t *out);

/* 读一个独立的 .lrc 文件（在线流式歌词，原 media_meta_lyrics_load_file） */
esp_err_t mm_lyrics_load_file(const char *lrc_path, lrc_t *out);

/* "/sdcard/xx/yy.mp3" → "/sdcard/lrc"（纯路径处理） */
bool mm_lyrics_dir_for(const char *audio_path, char *dir, size_t cap);

/* "/sdcard/xx/yy.mp3" → "/sdcard/lrc/yy.lrc"（纯路径处理） */
bool mm_lyrics_cache_path(const char *audio_path, char *path, size_t cap);

#ifdef __cplusplus
}
#endif
