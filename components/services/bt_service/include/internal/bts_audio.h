/*
 * bts_audio.h —— bt_service 内部：SBC 编码器 + 软件 ASRC + 重采样暂存
 *
 * 原来是 bt_service.cc 445–671 那一整段（编码/重采样打开与关闭）。状态定义在
 * bts_audio.cc，由本头文件 extern 出去给 bts_feed.cc（feed 任务）用。
 *
 * ⚠ 这里的状态（编码器句柄/帧缓冲/ASRC 缓冲/暂存）**只允许 feed 任务碰**
 *   （bt_service_deinit 侧要先用 bts_feed_stop() 把 feed 任务删掉，再 bts_audio_close()）。
 *
 * ⚠ A2DP 源要的是**编码后的帧**，不是 PCM；bitpool 必须"拷贝协商结构体再改"，
 *   ASRC 刻意走软件路径 —— 三条都有历史原因，见 bts_audio.cc 里的注释，别改回去。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_audio_enc.h"
#include "esp_asrc.h"
#include "esp_bt_audio_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本地音频管线送进来的 PCM 格式（audio_service 侧固定，见 bt_service.h） */
#define A2DP_PCM_RATE   (48000)
#define A2DP_PCM_CH     (2)
#define A2DP_PCM_BITS   (16)
/* 一次喂给 ASRC 的采样帧数 = 一个 SBC 帧的时长（128 采样 @48k ≈ 2.7 ms） */
#define ASRC_IN_FRAMES  (128)
/* 重采样后的暂存区：够放"上一次的余量 + 一次 ASRC 的输出"就行 */
#define STAGE_BYTES     (2048)
/* 入队等待上限：队列满就短暂等一下（吸收抖动），但绝不长时间阻塞 ——
 * 真正的节拍已经由 bt_service_write_pcm() 的阻塞发送交给解码管线了 */
#define TX_WAIT_MS      (50)

/*
 * ★ SBC bitpool 上限（2026-10 加，治"放歌一卡一卡"）。
 *
 * 真机日志（本地 FLAC，A2DP 到耳机）：
 *   bt_svc: a2dp tx: 8000 frames, 289 f/s (need 345 f/s), dropped 0 B   ← 只发了需求的 84%
 *   audio_svc: play stats: PCM 157 KB/s (满速应 192)                     ← 整条链按 82% 跑
 *   E BT_L2CAP: l2cab is_cong_cback_context                              ← L2CAP 拥塞回调
 * `dropped 0 B` 说明**不是我们丢帧**：环形缓冲从没溢出、acquire 也没超时，
 * 而是链路（或对端）**持续**只吃得下约 84% 的量 → 解码管线被阻塞式发送拖慢 →
 * 听感就是"一卡一卡"，而 CPU 很低（都在等射频）。
 *
 * bitpool 决定 SBC 的码率需求：joint-stereo / 16 block / 8 subband 下，
 * 44.1 kHz 的 bitpool 53 ≈ 328 kbps、bitpool 64 ≈ 400 kbps 上下，
 * 廉价耳机 + 2.4 GHz 与 WiFi 共存时经常吃不下。把 bitpool 压到 40（≈250 kbps）
 * 给链路留出余量，是这类"链路带宽不够"的标准缓解手段。
 *
 * ⚠ 改 bitpool 是**合法**的：SBC 每一帧的帧头里都带 bitpool 字段，对端解码器按
 *   帧头走，不需要重新协商（这也是各家用它做"音质/稳定性"档位的原理）。
 *   代价是码率低一点、听感略糊；对"卡顿"来说这是划算的交换。
 *   想恢复原样把这个宏改成 250（= 不限）即可。
 */
#define A2DP_SBC_BITPOOL_MAX (40)

/* ---- 当前流的句柄与编码/重采样状态（写：BT 回调任务或 init/deinit；用：feed 任务） ---- */
extern esp_bt_audio_stream_handle_t bts_stream;
extern esp_audio_enc_handle_t       bts_enc;          /* 编码器句柄，随流起停开关 */
extern int                          bts_enc_in_bytes;  /* 一个 SBC 帧对应的 PCM 字节数 */
extern int                          bts_enc_out_bytes; /* 编码后一帧的建议缓冲大小 */
extern uint8_t                     *bts_enc_frame;     /* 编码输出缓冲 */

extern esp_asrc_handle_t bts_asrc;          /* 非 NULL = 需要 48k → 协商采样率 */
extern uint16_t          bts_asrc_in_smp;   /* ASRC 一个"采样帧"的输入字节数 */
extern uint16_t          bts_asrc_out_smp;  /* ASRC 一个"采样帧"的输出字节数 */
extern uint32_t          bts_asrc_in_bytes; /* 每次喂给 ASRC 的字节数 */
extern uint8_t          *bts_asrc_in;
extern uint8_t          *bts_asrc_out;
extern uint32_t          bts_asrc_out_cap_samples;
extern uint8_t          *bts_stage;         /* 重采样后的 PCM 暂存（凑够一帧再编码） */
extern uint32_t          bts_stage_len;

/* 关编码器与 ASRC，释放缓冲并清零尺寸变量 */
void bts_audio_close(void);

/* 打开 48 kHz → dest_rate 的**软件** ASRC，并分配对齐缓冲 */
bool bts_asrc_open(uint32_t dest_rate);

/* 按协商到的 codec_cfg 打开编码器（含 bitpool 封顶）；成功返回 true */
bool bts_enc_open(esp_bt_audio_stream_handle_t h);

/* 把重采样后的 PCM 追加到暂存区（凑够一个编码帧才编码） */
void bts_stage_append(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
