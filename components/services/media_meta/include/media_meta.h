/*
 * media_meta.h —— 歌词（LRC / ID3 内嵌）与内嵌封面
 *
 * 歌词优先级（开发指南 5.10 + 需求）：
 *   歌曲同目录同名 .lrc → <SD>/lrc/<同名>.lrc → 内嵌 ID3 歌词（Meting 在线歌词后续补）
 *   内嵌歌词取到后会自动落盘到 <SD>/lrc，之后走第二档。
 *
 * LRC 时间标签形如 [mm:ss.xx]，**一行可能有多个标签**，且可能乱序，解析后要按时间排序。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LRC_TEXT_MAX 160

typedef struct {
    uint32_t time_ms;
    char     text[LRC_TEXT_MAX];
} lrc_line_t;

typedef struct {
    lrc_line_t *lines;
    size_t      count;
    size_t      cap;
    /* false = 纯文本歌词（整段没有 [mm:ss] 时间标签，例如内嵌 USLT 的纯文本）：
     * 界面只静态展示，不做高亮与"当前行居中"滚动 */
    bool        timed;
} lrc_t;

/* 清空 / 释放 */
void media_meta_lrc_reset(lrc_t *lrc);

/*
 * 加载音频同名的 .lrc 边文件（把扩展名换成 .lrc）。
 * 找不到返回 ESP_ERR_NOT_FOUND，歌词为空不算错误。
 */
esp_err_t media_meta_lrc_load_sidecar(const char *audio_path, lrc_t *out);

/* 直接从字符串解析（在线歌词用）。没有时间标签时按"纯文本歌词"逐行收下，
 * 并把 out->timed 置 false。 */
esp_err_t media_meta_lrc_parse(const char *text, lrc_t *out);

/* 读一个**独立的 .lrc 文件**并解析（在线流式播放用：音频在网络上、没有本地
 * 同名文件，歌词是我们自己缓存的 .lrc，路径与音频名无关）。 */
esp_err_t media_meta_lyrics_load_file(const char *lrc_path, lrc_t *out);

/*
 * 按优先级给一首歌找歌词（需求：同目录 → /lrc → 内嵌并落盘）：
 *   1) 歌曲同目录的同名 .lrc
 *   2) <SD 挂载点>/lrc/<同名>.lrc
 *   3) 内嵌歌词（MP3 的 ID3v2 USLT；没有 USLT 就把 SYLT 转成 LRC）
 *      → **自动写入 <SD 挂载点>/lrc/<同名>.lrc**，下次直接命中第 2 档
 * 三级都没有时返回 ESP_ERR_NOT_FOUND（歌词为空不算错误）。
 */
esp_err_t media_meta_lyrics_load(const char *audio_path, lrc_t *out);

/* 内嵌歌词原文（UTF-8，malloc 的）；只做"取出来"这一件事，不落盘 */
esp_err_t media_meta_embedded_lyrics(const char *audio_path, char **text);
void      media_meta_lyrics_free(char *text);

/* 返回 pos_ms 时刻应高亮的行下标；无歌词返回 -1 */
int media_meta_lrc_index_at(const lrc_t *lrc, uint32_t pos_ms);

/* ------------------------------------------------------------------ 内嵌封面 */

/*
 * 一次调用拿到"能直接喂给 LVGL"的封面位图：
 *   取内嵌专辑图（MP3 = ID3v2 的 APIC/PIC 帧，FLAC = PICTURE 元数据块）
 *   → 用 esp_new_jpeg 解成 **RGB565 little-endian**
 *   → 按 1/2^n 降到长边不超过 max_dim（省内存，也省 LVGL 每帧的缩放量）。
 *
 * 成功时 *pixels 由本模块分配（16 字节对齐），调用方用完必须调
 * media_meta_cover_free()。失败码：
 *   ESP_ERR_NOT_FOUND     文件里没有封面（或容器不支持）
 *   ESP_ERR_NOT_SUPPORTED 封面是 PNG / 不认识（本版只解 JPEG）
 *   ESP_ERR_NO_MEM / ESP_FAIL  OOM / 解码失败
 *
 * ⚠ 这是**同步阻塞**调用（读文件 + 软解 JPEG，几十~几百 ms），必须在
 *   工作任务的上下文里调用，**不要**在 LVGL 任务里直接调。
 */
esp_err_t media_meta_cover_rgb565(const char *audio_path, int max_dim,
                                  uint8_t **pixels, int *out_w, int *out_h);

/* 释放 media_meta_cover_rgb565() 分配的像素缓冲（传 NULL 安全） */
void media_meta_cover_free(uint8_t *pixels);

/*
 * 独立 JPEG 文件 → RGB565（在线歌曲封面走这条路：Meting 给的 pic 是一张单独的
 * .jpg，不在音频文件里）。输出格式与 media_meta_cover_rgb565() 完全一致，
 * 同样用 media_meta_cover_free() 释放。
 *
 * 失败码：ESP_ERR_NOT_FOUND 打不开；ESP_ERR_INVALID_SIZE 非 JPEG 或超过 2 MB；
 *        ESP_FAIL 解码失败。
 */
esp_err_t media_meta_jpeg_file_rgb565(const char *jpg_path, int max_dim,
                                      uint8_t **pixels, int *out_w, int *out_h);

/*
 * 容器头部长度（音频数据起始偏移，字节）。
 *   MP3：ID3v2 标签总长度（10 + synchsafe size），无标签为 0
 *   其它：0
 * 拖动定位时用它把"时间"换算成"字节"：byte = offset + ms * bitrate / 8000
 */
size_t media_meta_audio_data_offset(const char *audio_path);

/* ------------------------------------------------------------------ 音频参数探测 */

typedef struct {
    uint32_t bitrate;      /* bit/s；0 = 未知 */
    uint32_t duration_ms;  /* 0 = 未知（有 Xing/Info 帧数时是准的，否则用大小/码率估） */
    uint32_t sample_rate;
    uint8_t  channels;     /* 1 / 2；0 = 未知 */
} audio_probe_t;

/*
 * 自己读 MP3 帧头补参数（CBR 用帧码率；有 Xing/Info 头时用帧数算**准确时长**
 * 与平均码率，VBR 也准）。
 *
 * 为什么需要：esp_audio_simple_player 只在管线 REPORT_INFO 时给一次 MUSIC_INFO，
 * 而 GMF 对 MP3 往往只填采样率、bitrate 是 0 —— 于是"文件大小 / 码率"估不出
 * 总时长：进度条永远停在 0，拖动定位也没有字节位置可用。
 *
 * 只处理 .mp3；其它扩展名返回 ESP_ERR_NOT_SUPPORTED（调用方保持原逻辑）。
 * 同步读文件（最多 32 KB），别在 LVGL 任务里调用。
 */
esp_err_t media_meta_probe_mp3(const char *audio_path, audio_probe_t *out);

#ifdef __cplusplus
}
#endif