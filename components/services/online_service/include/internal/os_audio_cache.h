/*
 * os_audio_cache.h —— SD 上的音频下载缓存（按魔数定扩展名 / 命中查找 / LRU 清理）
 *
 * 原文件里的名字：ext_from_sniff（1164–1183，已移到纯逻辑的 os_util.h）、
 * find_cached_audio（1186–1211）、audio_cache_trim（1214–1257）。
 *
 * 播放缓存：`/sdcard/online/audio/<歌曲ID>.<ext>`，**扩展名按嗅探文件头决定**
 * （音频播放链路与本地文件一致，靠扩展名选解码器）。
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
 * 在音频缓存目录里找这首歌已下载的文件（扩展名不定，所以按 `<id>.` 前缀找）。
 * 命中返回 true 并把完整路径写进 out；只认 ≥ 32 KB 的普通文件。
 */
bool os_find_cached_audio(uint32_t song_id, char *out, size_t cap);

/*
 * 缓存太多就删最旧的几个（SD 卡很大，但别无限涨）。
 * ⚠ 现实现是 O(n²)（每次删除都 rewinddir + 全量 stat），SD 上很慢 ——
 *   拆分时按"零行为变化"照搬；优化（收集 → 排序 → 删前 N）另开一项（蓝图 R9）。
 */
void os_audio_cache_trim(void);

#ifdef __cplusplus
}
#endif
