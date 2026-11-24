/*
 * os_audio_cache.h —— SD 上的音频下载缓存（按魔数定扩展名 / 命中查找 / LRU 清理）
 *
 * 原文件里的名字：ext_from_sniff（1164–1183，已移到纯逻辑的 os_util.h）、
 * find_cached_audio（1186–1211）、audio_cache_trim（1214–1257）。
 *
 * 播放缓存：`/sdcard/online/audio/<歌曲ID>.<ext>`，**扩展名按嗅探文件头决定**
 * （音频播放链路与本地文件一致，靠扩展名选解码器）。
 *
 * ⚠ 这个目录**按需创建**（在 online_song_prepare 里，即真的要兜底下载时）：
 *   正常播放是纯流式，不落 SD、也不建目录。查找/清理在这里对"目录不存在"都是安全的
 *   （opendir 失败 = 没有缓存）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音频缓存最多留多少个文件，超了删最旧的 */
#define AUDIO_CACHE_MAX    48

/*
 * 在音频缓存目录里找这首歌已下载的文件（扩展名不定，所以按 `<key>.` 前缀找）。
 * 命中返回 true 并把完整路径写进 out；只认 ≥ 32 KB 的普通文件。
 *
 * key 是**字符串**而不是歌曲 ID：QQ 音乐的 songid 与网易云的歌曲 ID 是两套编号，
 * 同一个数字可能对应两首完全不同的歌（"下载兜底"会把 A 平台的音频喂给 B 平台）。
 * 调用方按 `<id>`（网易云）/ `q<id>`（QQ）生成 key，见 os_media_cache.cc。
 */
bool os_find_cached_audio(const char *key, char *out, size_t cap);

/*
 * 缓存太多就删最旧的几个（SD 卡很大，但别无限涨）。
 * ⚠ 现实现是 O(n²)（每次删除都 rewinddir + 全量 stat），SD 上很慢 ——
 *   拆分时按"零行为变化"照搬；优化（收集 → 排序 → 删前 N）另开一项（蓝图 R9）。
 */
void os_audio_cache_trim(void);

#ifdef __cplusplus
}
#endif
