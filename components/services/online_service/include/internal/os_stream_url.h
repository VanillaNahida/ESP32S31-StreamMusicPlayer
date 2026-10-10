/*
 * os_stream_url.h —— 播放地址 302 解析（+ MP3 码率→时长估算）
 *
 * 原文件里的名字：mp3_bitrate_bps（1504–1524，纯逻辑）、
 * online_song_stream_info（1532–1654）、online_song_stream_url（1656–1741）。
 *
 * ★ 契约（**不能变**，见 docs/audio-stack.md §5 与 online_service.h）：
 *   这两个 API **只解析地址、不下载**：解析 302 拿到音频最终 https 地址，
 *   交给播放端边下边播。`online_song_stream_info` 额外读 **4 KB 前缀**拿 MP3 码率，
 *   配合 Content-Length 估总时长（在线流拿不到 duration_ms）。
 *
 * ★ 幂等、可重复调用、**不加缓存**：网易云的直链带时间戳签名，放一阵就 403；
 *   app_music 的 online_url_refresh() 依赖"每次调用都能拿到新签名地址"（蓝图 R3）。
 *
 * 账号歌（audio_url 为空）走 online_netease_song_url()（带 cookie 现取直链）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MP3 帧头 → 码率(bps)，解析失败返回 0。表格照 ISO/IEC 11172-3 Layer III。 */
int os_mp3_bitrate_bps(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif
