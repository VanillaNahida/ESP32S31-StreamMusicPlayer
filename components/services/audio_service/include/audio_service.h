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

/*
 * 告诉服务"当前这一首有多长"（毫秒，0 = 未知）。
 *
 * 在线曲目**必须**由上层告知：GMF 的 MUSIC_INFO 里 duration 拿不到（服务里一直是 0），
 * 而在线 seek 的"时间→字节"要用**平均码率 = 总字节 × 8000 ÷ 时长** ——
 * 总字节由 HTTP Content-Length 给出，时长只能从在线元数据来。
 * 时机：**必须在 audio_service_play_uri() 之后调**（换曲会把上一首的值清掉）。
 * 本地文件不需要（服务自己会用 media_meta 探测平均码率）。
 */
void audio_service_set_duration_ms(uint32_t dur_ms);

/*
 * 当前这首现在能不能拖动定位（UI 用它决定"拖了到底有没有用"）。
 *   - 本地文件：MUSIC_INFO 到了、且探测到平均码率 → 能；
 *   - 在线流：MUSIC_INFO 到了、且**已知总字节 + 时长**（能算平均码率）→ 能；
 *     只有实测码率（播过一会儿之后才采得到）时也能**向前**拖，见实现里的相对映射。
 * 容器层面的限制（MP4/M4A/OGG 按字节 seek 后对不上帧）在 app_music 里另有一道判断。
 */
bool audio_service_seek_supported(void);

/*
 * 取"上一次 seek 实际落到了哪"（毫秒），**一次性**：读到就清掉标志。
 *
 * 怎么来的：seek 之后第一块 PCM 写出来时，用 `esp_gmf_io_get_pos()` 按**同一个码率**
 * 反推回时间。那个位置是"解码器已经消费到哪"（它的推进点在
 * esp_gmf_io_release_read 里，看源码确认过），所以它就是真实落点。
 * 正常情况下与请求的时间点一致；差得多说明服务端没按我们要的 Range 给数据
 * （少数 CDN/代理会这样），这时上层应把进度/歌词对齐到这个真实位置。
 *
 * 返回 false = 还没有落点（seek 后还没出声，或拿不到 IO 位置）。
 */
bool audio_service_seek_landing_ms(uint32_t *out_ms);

/* ------------------------------------------------------------------ 输出通道 */

typedef enum {
    AUDIO_OUT_SPEAKER = 0,   /* ES8389 → NS4150B ×2 → 左右扬声器 */
    AUDIO_OUT_BLUETOOTH,     /* 经 esp_bt_audio 推 PCM，由耳机自己解码 */
    AUDIO_OUT_USB,           /* USB 声卡（UAC 1.0）：**只有插着时**才允许切 */
    AUDIO_OUT_NONE,          /* 静音：静音 + 关功放 */
} audio_out_t;

/*
 * 切换输出通道。两条通道有前置条件，**不满足就返回 ESP_ERR_INVALID_STATE**
 * （不假装成功，也不把声音偷偷留在原通道上）：
 *   AUDIO_OUT_BLUETOOTH —— 必须 A2DP 已连接；
 *   AUDIO_OUT_USB       —— 必须插着 USB 声卡（`usb_audio_is_ready()`）。
 */
esp_err_t   audio_service_set_output(audio_out_t out);
audio_out_t audio_service_get_output(void);

/*
 * USB 声卡**插入 / 拔出**的 UI 提示（取走即清）。
 *
 * 拔出时的切换是**在 USB 驱动任务里立即完成**的（需求：必须立刻切回内置输出），
 * 这条消息只负责让 UI 补一句话，**不参与**切换本身，所以没 UI 也不影响音频。
 * 消息形如：
 *   "USB 声卡已连接：MCHOSE V9 PRO（24bit）" / "USB 声卡已拔出" /
 *   "USB 声卡已断开，已切回扬声器"。
 * UI 侧由状态栏的定时器消费（见 ui_status_bar.cc 的 poll_usb_audio）。
 */
bool audio_service_take_usb_msg(char *buf, size_t size);

/* 主音量 0~100（走 ES8389 数字音量寄存器） */
esp_err_t audio_service_set_volume(int percent);
int       audio_service_get_volume(void);
esp_err_t audio_service_set_mute(bool mute);

/*
 * 事件回调（**不要在里面做重活，也不要碰 LVGL**）。
 *
 * ⚠ 回调可能在**多个上下文**里被调用，不要在回调里假设线程：
 *   - 解码任务：STARTED / PAUSED / RESUMED / FINISHED（EOS 后输出播完）… ；
 *   - **起播调用者自己的上下文**：`audio_service_play_path()` / `play_uri()` 里
 *     库会同步派发一次 STARTED（也就是说可能在 LVGL 任务里）；
 *   - HTTP 馈送任务：在线流出错时的 ERROR。
 * 正确用法是"只置标志，由定时器/任务去处理"（app_music 就是这么做的）。
 */
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
 * 流式播放的**网络下载速率**，单位 **Kbps（千比特每秒）** —— 注意不是 KB/s，
 * 显示成 KB/s 要自己 ÷8（UI 那边就是这么干的：`缓冲中 20 KB/s`）。
 *
 * 数据源是 GMF 的速度统计（IO 任务每读到一块数据就更新一次，1:9 平滑），
 * 所以它反映的是**网络侧的真实下载速度**，起播缓冲期间也有值 ——
 * 而 `audio_service_stream_progress()` 用的是"解码器消费到哪"，那会儿几乎不动。
 *
 * 返回 false = 没有可用速度（本地文件 / 速度统计没开 / 还没读到任何数据），
 * 这时 UI 应当**不显示速度**，别显示 0。
 * 纯查询，任何任务里调都安全。
 */
bool audio_service_stream_speed(int *kbps);

/*
 * 本首是否**已经真的出声**（有 PCM 写到 codec / 蓝牙）。
 * 起播与换曲时清零，由输出级在写出第一块 PCM 时置位；seek 后也会清零。
 *
 * UI 用它决定"缓冲中 xx%"什么时候换回歌词：只看 audio_service_is_playing()
 * 不行 —— 会话一起来状态就是 playing，那时还没声音。
 */
bool audio_service_output_active(void);

/*
 * 当前播放位置（毫秒）。口径是**送去硬件的 PCM 字节数 ÷ 192 B/ms**：
 * 输出级把一切源统一成 48 kHz/16bit/2ch 才写硬件，所以这个换算恒成立。
 *
 * 与 audio_service_output_active() 配合使用：出声期间它按真实播放推进，
 * 缓冲/暂停期间不涨。想自己累计时间也可以（app_music 就是自己累计的），
 * 这里提供的是"服务侧的真实进度"，用于日志与偏差核对。
 */
uint32_t audio_service_position_ms(void);

/*
 * 当前**解码源**格式：采样率 / 声道 / 位深（0 = 该项未知）。
 *
 * 与 `audio_service_get_info()` 的区别：那个是"**文件/流的信息**"（本地来自
 * media_meta 探测、码率是平均值），**在线流那几项是空的**；这个是**解码器实际上报**
 * 的格式，在线/本地只要有声音就有值 —— 播放器页的"歌曲信息"弹窗用它。
 *
 * 注意 `bits` 是**源**位深（24 表示音源是 24-bit），而送给 codec 的 PCM 恒为 16-bit
 * （ringbuf 与 ASRC 都在 16-bit 域，见 audio-stack.md §2）。
 */
bool audio_service_source_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);

/* 平均码率（bit/s，0 = 未知）：本地来自探测、在线来自"总字节 ÷ 时长"（含实测纠偏） */
uint32_t audio_service_bitrate(void);

#ifdef __cplusplus
}
#endif