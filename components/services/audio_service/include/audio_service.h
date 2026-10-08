/*
 * audio_service.h —— 播放服务（对外统一接口）
 *
 * M2 实现：esp_audio_simple_player（官方基于 ESP-GMF 的播放门面）
 *   解码 → (重采样到 48k) → out 回调 → esp_codec_dev → ES8389 → NS4150B
 *
 * 业务层只认这个头文件，不直接碰 GMF / esp_codec_dev。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_EVT_STARTED = 0,   /* 开始播放 */
    AUDIO_EVT_PAUSED,        /* 暂停 */
    AUDIO_EVT_RESUMED,       /* 恢复 */
    AUDIO_EVT_FINISHED,      /* 自然播完 */
    AUDIO_EVT_ERROR,         /* 出错（文件不存在 / 解码失败 / 格式不支持） */
} audio_evt_t;

typedef void (*audio_event_cb_t)(audio_evt_t evt, void *user);

/* 初始化：打开 codec 输出（48k/2ch/16bit）、建播放实例 */
esp_err_t audio_service_init(void);

/*
 * 播放一个本地文件。
 * @param vfs_abs_path  VFS 绝对路径，如 "/sdcard/Music/a.mp3"
 *                      内部会转成 GMF 需要的 URI 形式 "file:///sdcard/Music/a.mp3"
 */
esp_err_t audio_service_play_path(const char *vfs_abs_path);

/*
 * 播放一个 URI：传本地绝对路径（"/sdcard/..."）等价于上面那个；
 * 传 http(s):// 则走 GMF 的 HTTP reader IO **流式播放**
 * （工程已开 CONFIG_ESP_AUDIO_SIMPLE_PLAYER_HTTP_EN，池子里注册了该 IO）。
 * 流式没有本地文件：时长探测与拖动定位能力受限，拿不到就由上层提示。
 */
esp_err_t audio_service_play_uri(const char *uri);

esp_err_t audio_service_pause(void);
esp_err_t audio_service_resume(void);
esp_err_t audio_service_toggle(void);
esp_err_t audio_service_stop(void);
bool      audio_service_is_playing(void);

/* 按时间定位（拖动进度条）。见实现里的限制说明：
 * 只有拿到 MUSIC_INFO（码率）之后才能换算字节位置，且只对流式容器可靠。 */
esp_err_t audio_service_seek_ms(uint32_t pos_ms);

/* ------------------------------------------------------------------ 输出通道 */

typedef enum {
    AUDIO_OUT_SPEAKER = 0,   /* ES8389 → NS4150B ×2 → 左右扬声器 */
    AUDIO_OUT_BLUETOOTH,     /* 经典蓝牙 A2DP（需要 GMF bt_audio，当前未接入） */
    AUDIO_OUT_NONE,          /* 静音：静音 + 关功放 */
} audio_out_t;

/*
 * 切换输出通道（开发指南 5.12 的防爆音时序在切换管线时才有意义；
 * 这里目前只有"扬声器 / 静音"两条真实通路）。
 * AUDIO_OUT_BLUETOOTH 会返回 ESP_ERR_NOT_SUPPORTED —— A2DP 还没接，
 * 不假装成功，也不把声音偷偷留在扬声器上。
 */
esp_err_t   audio_service_set_output(audio_out_t out);
audio_out_t audio_service_get_output(void);

/* 主音量 0~100（走 ES8389 数字音量寄存器） */
esp_err_t audio_service_set_volume(int percent);
int       audio_service_get_volume(void);
esp_err_t audio_service_set_mute(bool mute);

/* 事件回调（在播放器任务上下文被调用，**不要在里面做重活**） */
void audio_service_set_event_cb(audio_event_cb_t cb, void *user);

/* 当前曲目的流信息（由 ESP_ASP_EVENT_TYPE_MUSIC_INFO 事件填充，
 * bitrate/duration_ms 在 GMF 报 0 时会用自读帧头的探测结果兜底） */
typedef struct {
    int      sample_rate;
    int      bitrate;      /* bit/s；可用于用文件大小估算总时长 */
    uint8_t  channels;
    uint8_t  bits;
    uint32_t duration_ms;  /* 0 = 未知；有 Xing/Info 帧数时是准确值，比估算可靠 */
} audio_info_t;

bool audio_service_get_info(audio_info_t *out);

/*
 * 在线流式播放的**缓冲进度**（需求："缓冲的时候…显示加载百分比"）。
 *
 * 返回 true 时 *percent 是 0~100（已下载字节 / Content-Length）；返回 false 表示
 * 现在算不出来 —— 还没开始流过、或者服务端用分块传输（没有 Content-Length）。
 * 调用方（UI）判 false 就别显示百分比，退回"缓冲中…"即可。
 *
 * 语义：这是**下载**进度，不是播放进度。流式播放的环形缓冲放在 PSRAM
 * （`STREAM_RING_BYTES`，4 MB，见 audio_service.cc 的 ensure_stream_io），
 * 条子满了/歌下完了就会到 100%（一首 3~4 MB 的歌基本上一口气就拉完）。
 * 纯查询，任何任务里调都安全。
 */
bool audio_service_stream_progress(int *percent);

/*
 * 本首是否**已经真的出声**（有 PCM 写到 codec / 蓝牙）。
 * 起播与换曲时清零（见 play_uri_locked），由 out 回调置位。
 *
 * UI 用它决定"缓冲中 xx%"什么时候换回歌词：只看 audio_service_is_playing()
 * 不行 —— pipeline 一 run() 状态就是 playing，那时还没声音。
 */
bool audio_service_output_active(void);

#ifdef __cplusplus
}
#endif