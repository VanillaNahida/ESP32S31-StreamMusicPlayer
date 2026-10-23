/*
 * media_meta.cc —— 媒体元数据（歌词 + 内嵌封面 + 音频参数探测）：**对外门面**
 *
 * 本文件只剩"转发"：每个 media_meta_* 入口一行调用对应的 mm_*.cc 实现，
 * 签名与语义与拆分前完全一致（include/media_meta.h 是对外契约，未改动）。
 * 实现分布（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 *   mm_bits.cc         字节序 / synchsafe / MIME 判定（纯，无 IO）
 *   mm_lrc.cc          LRC 解析、时间轴索引、编码归一化（UTF-8/UTF-16/GBK）
 *   mm_id3.cc          ID3v2：APIC/PIC 封面帧、USLT/SYLT 歌词、文本编码
 *   mm_cover.cc        封面提取编排 + 独立图文件 / 内存缓冲入口
 *   mm_image.cc        JPEG/PNG → RGB565 + 盒式降采样
 *   mm_probe.cc        MP3/FLAC/WAV/ADTS 参数探测 + 魔数分派
 *   mm_lyrics_file.cc  歌词三级查找 + /lrc 落盘
 *
 * 歌词解析规则：
 *   - 行首可能有**多个** [mm:ss.xx] / [mm:ss] 标签，每个标签都要生成一行
 *   - 标签之后是歌词文本；[ti:] [ar:] [al:] 等元信息标签忽略
 *   - 解析完按时间排序（LRC 允许乱序）
 *   - 整段没有任何时间标签时按"纯文本歌词"逐行收下，timed=false
 *
 * 歌词来源优先级（media_meta_lyrics_load）：
 *   歌曲同目录同名 .lrc → <SD>/lrc/<同名>.lrc → 内嵌 ID3（USLT，或 SYLT 转 LRC）
 *   最后一档取到后会**自动写入 <SD>/lrc/<同名>.lrc**，下次直接命中第二档。
 *
 * 封面：MP3 的 ID3v2 APIC/PIC、FLAC 的 PICTURE 块 → 解成 RGB565（LVGL 可直接用）。
 * JPEG 走 esp_new_jpeg，PNG 走官方 libpng 组件；两条路在 mm_image_to_rgb565() 里
 * 按文件魔数分派（APIC 的 MIME 字段常常是错的，不能拿它当依据）。
 *
 * ⚠ 所有权（一个字都没变）：像素一律 media_meta_cover_free()，
 *   歌词文本一律 media_meta_lyrics_free()，lrc_t 内部缓冲一律 media_meta_lrc_reset()。
 */
#include "media_meta.h"

#include "internal/mm_container.h"
#include "internal/mm_cover.h"
#include "internal/mm_image.h"      /* mm_photo_file_rgb565（图库的大图解码） */
#include "internal/mm_lrc.h"
#include "internal/mm_lyrics_file.h"

/* ------------------------------------------------------------------ 歌词 */

void media_meta_lrc_reset(lrc_t *lrc)
{
    mm_lrc_reset(lrc);
}

esp_err_t media_meta_lrc_load_sidecar(const char *audio_path, lrc_t *out)
{
    return mm_lrc_load_sidecar(audio_path, out);
}

esp_err_t media_meta_lrc_parse(const char *text, lrc_t *out)
{
    return mm_lrc_parse(text, out);
}

esp_err_t media_meta_lyrics_load_file(const char *lrc_path, lrc_t *out)
{
    return mm_lyrics_load_file(lrc_path, out);
}

esp_err_t media_meta_lyrics_load(const char *audio_path, lrc_t *out)
{
    return mm_lyrics_load(audio_path, out);
}

esp_err_t media_meta_embedded_lyrics(const char *audio_path, char **text)
{
    return mm_embedded_lyrics(audio_path, text);
}

void media_meta_lyrics_free(char *text)
{
    mm_lyrics_free(text);
}

int media_meta_lrc_index_at(const lrc_t *lrc, uint32_t pos_ms)
{
    return mm_lrc_index_at(lrc, pos_ms);
}

/* ------------------------------------------------------------------ 内嵌封面 */

esp_err_t media_meta_cover_rgb565(const char *audio_path, int max_dim,
                                  uint8_t **pixels, int *out_w, int *out_h)
{
    return mm_cover_from_audio(audio_path, max_dim, pixels, out_w, out_h);
}

/* ⚠ 必须用 jpeg_free_align 释放（像素是 jpeg_calloc_align 分配的，见 mm_image.cc） */
void media_meta_cover_free(uint8_t *pixels)
{
    mm_cover_free(pixels);
}

esp_err_t media_meta_image_file_rgb565(const char *img_path, int max_dim,
                                       uint8_t **pixels, int *out_w, int *out_h)
{
    return mm_cover_from_file(img_path, max_dim, pixels, out_w, out_h);
}

esp_err_t media_meta_image_mem_rgb565(const uint8_t *data, size_t len, int max_dim,
                                      uint8_t **pixels, int *out_w, int *out_h)
{
    return mm_cover_from_mem(data, len, max_dim, pixels, out_w, out_h);
}

/* 大图（图库）：JPEG 走解码期 scale，别整解再降采样 —— 见 mm_image.cc 的说明 */
esp_err_t media_meta_photo_file_rgb565(const char *img_path, int max_dim,
                                       uint8_t **pixels, int *out_w, int *out_h)
{
    return mm_photo_file_rgb565(img_path, max_dim, pixels, out_w, out_h);
}

/* ------------------------------------------------------------------ 音频参数探测 */

size_t media_meta_audio_data_offset(const char *audio_path)
{
    return mm_audio_data_offset(audio_path);
}

esp_err_t media_meta_probe_mp3(const char *audio_path, audio_probe_t *out)
{
    return mm_probe_mp3(audio_path, out);
}

/* ⚠ 本地曲目的时长/码率主来源（audio_service 在 MUSIC_INFO 里调），行为不能变 */
esp_err_t media_meta_probe_audio(const char *audio_path, audio_probe_t *out)
{
    return mm_probe_audio(audio_path, out);
}
