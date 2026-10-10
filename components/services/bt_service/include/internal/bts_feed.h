/*
 * bts_feed.h —— bt_service 内部：PCM 环形缓冲 + feed 任务 + write_pcm 流控
 *
 * 对应 bt_service.cc 674–883（bt_send_one_frame / feed_task / bt_service_write_pcm）。
 *
 * ★ 这个模块是"蓝牙通路的节拍来源"：bt_service_write_pcm() 用 200 ms 阻塞发送把
 *   A2DP 链路的实际吞吐回压给解码管线；feed_task 用**带超时的阻塞取**拿 PCM。
 *   两处都别"优化"成非阻塞/短睡 —— 那样等于给馈送速率设地板，听感就是一卡一卡。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
/* ⚠ FreeRTOS 要求 FreeRTOS.h 必须出现在 stream_buffer.h 之前（否则直接 #error） */
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCM_BUF_BYTES   (64 * 1024)            /* 环形缓冲 ≈ 340 ms @48k/16b/2ch */
/* StreamBuffer 的"触发水位"：只影响 xStreamBufferSend 的返回值语义，
 * 真正的攒帧由 feed_task 按编码器要求（一个 SBC 帧的 PCM 字节数）来做 */
#define PCM_BUF_TRIGGER (512)
#define FEED_TASK_STACK (5 * 1024)
/* 统计日志间隔（编码帧数）：512 B PCM/帧 ≈ 2.7 ms @48k 立体声 → 1000 帧 ≈ 2.7 s */
#define FEED_LOG_EVERY_FRAMES (1000)

/* PCM 环形缓冲（PSRAM 优先）。流 STARTED 时由事件回调 reset，
 * 由 bt_service_write_pcm() 写入、feed_task 取出。 */
extern StreamBufferHandle_t bts_pcm;

/* 建 PCM 缓冲 + feed 任务（幂等；失败返回 ESP_ERR_NO_MEM） */
esp_err_t bts_feed_start(void);

/* 删 feed 任务 + 删 PCM 缓冲（deinit 用；**必须先于 bts_audio_close()**） */
void bts_feed_stop(void);

/* 还留着 PCM 缓冲或 feed 任务吗（deinit 的幂等判据用） */
bool bts_feed_created(void);

#ifdef __cplusplus
}
#endif
