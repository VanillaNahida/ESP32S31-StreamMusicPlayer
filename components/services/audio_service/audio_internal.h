/*
 * audio_internal.h —— audio_service 内部各实现文件之间的共享契约
 *
 * 本组件按职责拆成 6 个实现文件，只有 audio_service.h 是对外契约，
 * 本文件是**组件内部**的契约（不要被组件外的代码包含）：
 *
 *   audio_service.cc   对外门面：参数校验、取会话锁、转发、事件上报
 *   audio_engine.cc    espaudiocore 会话：起播/停止/暂停/恢复/事件映射/曲目信息
 *   audio_output.cc    输出级：PCM ringbuf → esp_asrc(48k/16/2) → 扬声器/蓝牙/静音
 *                      并维护"时间轴"（送去硬件的字节数）与播放健康度统计
 *   audio_http.cc      在线传输：自研 HTTP Range 拉流 → 压缩 ringbuf；进度/速度
 *   audio_seek.cc      seek 策略：本地走库内 seek，在线走"Range 重开会话"
 *   audio_internal.h   本文件
 *
 * ── 三条必须遵守的线程纪律（拆模块时最容易破坏的就是它们）─────────────────
 * 1. **会话锁只保护"起播/停止/seek 重开"这类会动会话的流程**，由调用方
 *    （audio_service.cc 的门面，或 audio_seek.cc）显式 audio_core_lock()。
 *    输出任务、事件回调**绝不取这把锁** —— 它们要能在持锁期间照常跑完。
 * 2. **事件回调运行在 espaudiocore 的解码任务上下文**（audio_notify 由
 *    audio_engine.cc 的事件处理调用），禁止阻塞、禁止 LVGL；
 *    `audio_output.cc` 的 FINISHED 上报跑在输出任务上下文，同样禁止 LVGL。
 * 3. **输出级写硬件的 PCM 恒为 48 kHz / 16 bit / 2ch**（见 AUDIO_OUT_*），
 *    所以时间轴可以用 AUDIO_OUT_BYTES_PER_MS 一个常量换算 ——
 *    这条不变式是进度条、看门狗、seek 落点共用的地基，别在别处再算一套。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#include "audio_service.h"

/* ===========================================================================
 * 输出格式与缓冲容量（全工程唯一来源）
 * =========================================================================*/

#define AUDIO_OUT_RATE      48000
#define AUDIO_OUT_CHANNELS  2
#define AUDIO_OUT_BITS      16

/** 送去硬件的 PCM 恒定速率：48000 × 2ch × 2B / 1000 = 192 B/ms */
#define AUDIO_OUT_BYTES_PER_MS \
    ((AUDIO_OUT_RATE / 1000) * AUDIO_OUT_CHANNELS * (AUDIO_OUT_BITS / 8))

/**
 * 解码器 → 输出级的 PCM 环形缓冲。
 *
 * 2026-10 从 32 KB 提到 **64 KB（≈340 ms）**：真机日志显示"取数 + 重采样"这条
 * 生产链会略慢于实时（见 audio_output.cc 的 play stats 里 `产 N ms / 墙 W ms` 那一列），
 * 留深一点让解码端提前跑在前面，输出任务就不必"刚好赶上"。
 * 暂停不受它影响（暂停时输出任务立刻停止消费，缓冲里的音频留着）；
 * seek 会 flush，也不会多播旧位置的声音。
 * 抗抖动的大头仍然在**上游压缩数据的 4 MB 缓冲**（AUDIO_STREAM_RING_BYTES）。
 */
#define AUDIO_PCM_RING_BYTES  (64 * 1024)

/** 路径/URI 缓冲上限（本地文件与在线 URL 共用） */
#define AUDIO_PATH_MAX  600

/** 在线压缩数据环形缓冲（PSRAM）：够深才能"一口气拉完整首歌" */
#define AUDIO_STREAM_RING_BYTES  (4 * 1024 * 1024)
/** 每次向 ringbuf 投递/读取的块大小 */
#define AUDIO_STREAM_CHUNK       8192

/* ===========================================================================
 * 会话代数：用来让"上一个会话的迟到回调"自动失效
 *
 * 每次起播/停止都 +1。回调里带上自己那个会话的 gen，比对不上就直接忽略 ——
 * 这比"靠时序假设"可靠：espaudiocore 的 stop() 虽然保证任务退出后才返回，
 * 但输出任务、HTTP 馈送任务与起播流程是三个上下文，代数比对是唯一便宜的护栏。
 * =========================================================================*/

uint32_t audio_gen_current(void);
uint32_t audio_gen_next(void);

/* ===========================================================================
 * 事件上报（实现在 audio_service.cc）
 * =========================================================================*/

/** 把内部事件转给上层注册的 audio_event_cb_t；在解码任务/输出任务上下文调用 */
void audio_notify(audio_evt_t evt);

/* ===========================================================================
 * 会话锁（实现在 audio_engine.cc）
 * =========================================================================*/

void audio_core_lock(void);
void audio_core_unlock(void);

/* ===========================================================================
 * 引擎：espaudiocore 会话（audio_engine.cc）
 * =========================================================================*/

esp_err_t audio_engine_init(void);

/** 起播本地文件（须持会话锁）。失败时不会留下半开的会话 */
esp_err_t audio_engine_start_file(const char *path);

/** 起播压缩流（须持会话锁）；fmt 是 espaudiocore_format_t，用 int 免暴露库头文件 */
esp_err_t audio_engine_start_stream(int fmt);

/** 停止当前会话并释放（须持会话锁）。无会话时返回 ESP_OK */
esp_err_t audio_engine_stop(void);

esp_err_t audio_engine_pause(void);
esp_err_t audio_engine_resume(void);

/** 本地文件的库内 seek（须持会话锁）；流式会话返回 ESP_ERR_NOT_SUPPORTED */
esp_err_t audio_engine_seek_ms(int64_t ms);

/** 管线是否在跑（**不含暂停态**，与 audio_service_is_playing 语义一致） */
bool audio_engine_running(void);

/**
 * 输出级现在该不该往硬件写（有会话、且没在暂停）。
 *
 * 暂停时**立刻**关闸：输出任务停止消费 PCM ringbuf，缓冲里那点音频留着，
 * 恢复后接着播 —— 所以暂停是"立刻静音"而不是"把 170 ms 播完"，
 * 同时也不会像 flush 那样丢掉一段音频。
 */
bool audio_engine_flow_gate(void);

/** 是否有活跃会话（含暂停态） */
bool audio_engine_session_active(void);

/** 本会话是否是压缩流（在线）；本地文件为 false。决定 seek 走哪条路 */
bool audio_engine_is_stream(void);

/** 本曲流信息（采样率/声道/位深/时长/码率） */
bool audio_engine_info_get(audio_info_t *out);

/** 当前曲目路径/URI（本地是 /sdcard/...，在线是 http(s)://...） */
const char *audio_engine_cur_path(void);

/** 上层告知的本曲时长（在线用；本地由探测填充） */
void     audio_engine_set_duration_hint(uint32_t ms);
uint32_t audio_engine_duration_hint(void);

/** 本曲平均码率（bit/s），0 = 未知。本地来自 media_meta 探测，在线来自总字节÷时长 */
uint32_t audio_engine_bitrate(void);

/**
 * 本地文件：总时长未知时向**解码库**要一次（`espaudiocore_get_duration_ms()`）。
 *
 * 为什么需要：`media_meta` 的容器探测只覆盖 MP3/FLAC/WAV/ADTS，**M4A / OGG / Opus
 * 没有**探测实现 → 这些本地文件的总时长恒为 0（界面 `--:--`、进度条不动）。
 * 而解码库自己能从容器算出来（M4A 的 mvhd、OGG 的末页 granule、MP3 的 Xing）。
 *
 * ⚠ **必须在会话锁内调用**：它读的是库内部的会话对象（线程纪律见本文件开头）。
 * 由输出任务在每块数据前调用（时长未知时才有实际开销）。
 */
void audio_engine_poll_duration(void);

/** 输出级把 PCM 全部播完（EOS 之后）时回调，用来上报 FINISHED */
void audio_engine_output_drained(uint32_t gen);

/* ===========================================================================
 * 输出级（audio_output.cc）
 * =========================================================================*/

esp_err_t audio_output_init(void);

/** 解码器写入 PCM 用的环形缓冲（引擎起播时交给 espaudiocore） */
RingbufHandle_t audio_output_pcm_ring(void);

/** 新会话开始前调用（清时间轴/统计/出声标志，并让输出任务开始消费） */
void audio_output_session_begin(uint32_t gen);

/** 会话结束后调用（停消费 + 抽干残留 PCM），然后才可安全停/换会话 */
void audio_output_session_end(void);

/** 解码侧已到流末尾（ESPAUDIOCORE_EVT_EOS）：输出任务把积压播完再报 FINISHED */
void audio_output_note_eos(uint32_t gen);

/** 丢弃 ringbuf 里的残留 PCM（seek 之后必须调，否则会先播出旧位置的声音） */
void audio_output_flush(void);

/**
 * seek 生效后调用：把时间轴基准挪到目标时刻，并清掉"已出声"标志。
 *
 * 清标志是给 UI 用的：不清的话"缓冲中…"提示刚挂上就被撤掉
 * （app_music 靠 audio_service_output_active() 判缓冲是否结束）。
 */
void audio_output_note_seek(uint32_t pos_ms);

/** 当前播放位置（毫秒）= 时间轴基准 + 已送去硬件的字节 ÷ 192 */
uint32_t audio_output_position_ms(void);

void        audio_output_set_route(audio_out_t out);
audio_out_t audio_output_route(void);

/** 主音量 0~100（扬声器走 ES8389 寄存器，蓝牙走软件缩放） */
void audio_output_set_volume(int percent);
int  audio_output_volume(void);

/** 本会话是否**已经真的出声**（有 PCM 写到了硬件） */
bool audio_output_flowing(void);

/** 本会话已经送去硬件的字节数（48k/16/2），时间轴用 */
uint64_t audio_output_played_bytes(void);

/** 功放使能（扬声器路） */
void audio_output_pa(bool on);

/* ===========================================================================
 * 在线传输（audio_http.cc）
 *
 * 生命周期：prepare（建连、拿 Content-Length、嗅探容器格式）
 *        → start（起馈送任务，往压缩 ringbuf 里灌数据）
 *        → seek_to（关连接、带 Range 重开、清空 ringbuf）
 *        → stop（停任务、关连接、释放）
 * =========================================================================*/

/** 打开 URL 并读够用于嗅探的头部；成功返回 ESP_OK */
esp_err_t audio_http_prepare(const char *url);

/** 嗅探到的容器格式（espaudiocore_format_t）；ESP_OK 之后才有效 */
int audio_http_format(void);

/** 这个容器能不能"边下边解"（Vorbis/Opus/M4A 必须整段文件，返回 false） */
bool audio_http_streamable(void);

/** 压缩数据环形缓冲（交给 espaudiocore_begin_stream_rb）；prepare 之后才非 NULL */
RingbufHandle_t audio_http_ring(void);

/** 起馈送任务，从 from_byte 开始下（断点续传/Range 重开都走这里） */
esp_err_t audio_http_start(uint64_t from_byte);

/** 等"连接已建立 + 头部嗅探完成"；超时返回 false */
bool audio_http_wait_ready(int timeout_ms);

/** 停馈送任务并关连接、清空 ringbuf（**保留** ringbuf 与 URL 供下次复用） */
void audio_http_stop(void);

/** 下载进度（已下载 / Content-Length，0~100）；算不出返回 false */
bool audio_http_progress(int *percent);

/** 下载速率（**Kbps**，UI 自己 ÷8 显示 KB/s）；没数据返回 false */
bool audio_http_speed(int *kbps);

/** 已下载字节数 / 总字节数（0 = 未知） */
uint64_t audio_http_downloaded(void);
uint64_t audio_http_total(void);
/** 压缩流环形缓冲里**还没被解码器取走**的字节数（算实测码率要用"已吃掉"的量） */
uint64_t audio_http_ring_filled(void);
const char *audio_http_url(void);

/* ===========================================================================
 * seek 策略（audio_seek.cc）
 * =========================================================================*/

/** 现在能不能拖动定位（本地文件：会话在跑就能；在线：要能算出码率） */
bool audio_seek_supported(void);

/** 执行定位（内部按需取会话锁） */
esp_err_t audio_seek_to(uint32_t pos_ms);

/** 上一次 seek 的实际落点（本项目不再做落点核对，恒返回 false） */
bool audio_seek_landing_ms(uint32_t *out_ms);
