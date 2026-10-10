/*
 * mm_container.h —— 音频容器参数探测（MP3 / FLAC / WAV / ADTS-AAC）
 *
 * 实现见 mm_probe.cc。这里只有"读文件 + 解析头部"：
 *   - 纯解析部分（mm_mp3_parse_header / mm_probe_fill_bitrate）不碰 IO，
 *     可以脱离真机单测（拆分蓝图 §6.3）；
 *   - mm_probe_audio() 按**魔数**分派（不看扩展名 —— 用户的文件扩展名经常撒谎），
 *     是 media_meta_probe_audio() 的实现。
 *
 * 对外的 media_meta_probe_mp3 / media_meta_probe_audio / media_meta_audio_data_offset
 * 在 media_meta.cc 里转发。audio_service 起播时靠它拿本地曲目的时长/码率，
 * 行为不能变。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_meta.h"     /* audio_probe_t */

#ifdef __cplusplus
extern "C" {
#endif

/* MPEG 音频帧头解析结果（原 media_meta.cc 的 mp3_frame_t） */
typedef struct {
    int      mpeg_ver;      /* 帧头原值：3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
    int      layer;         /* 1 / 2 / 3 */
    uint32_t sample_rate;
    uint32_t bitrate;       /* bit/s */
    uint32_t frame_size;    /* 整帧字节数 */
    uint32_t samples;       /* 每帧样本数 */
    int      channels;      /* 1 / 2 */
} mm_mp3_frame_t;

/* 解析 MPEG 帧头（纯逻辑，原 static mp3_parse_header） */
bool mm_mp3_parse_header(const uint8_t *p, mm_mp3_frame_t *f);

/* 码率兜底：音频字节数 × 8 ÷ 时长（纯逻辑，原 static probe_fill_bitrate） */
void mm_probe_fill_bitrate(uint32_t audio_bytes, audio_probe_t *out);

/* MP3 的 ID3v2 标签总长；非 MP3 / 无标签返回 0（原 media_meta_audio_data_offset） */
size_t mm_audio_data_offset(const char *audio_path);

/* 只处理 .mp3（其它扩展名 ESP_ERR_NOT_SUPPORTED，原 media_meta_probe_mp3） */
esp_err_t mm_probe_mp3(const char *audio_path, audio_probe_t *out);

/* 按魔数分派（FLAC/WAV/ADTS；.mp3 转 mm_probe_mp3，原 media_meta_probe_audio） */
esp_err_t mm_probe_audio(const char *audio_path, audio_probe_t *out);

#ifdef __cplusplus
}
#endif
