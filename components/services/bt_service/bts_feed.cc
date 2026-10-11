/*
 * bts_feed.cc —— bt_service 内部：PCM 环形缓冲 + feed 任务 + write_pcm 流控
 *
 * 从 bt_service.cc 674–883（bt_send_one_frame / feed_task / bt_service_write_pcm /
 * bt_service_dropped_bytes）与 init 里的"PCM 缓冲 + feed 任务"那一段（1065–1079）
 * 原样搬来。**节拍与丢帧策略一字未改**：
 *   - write_pcm 的 200 ms 阻塞发送（上游流控，见下面的长注释）；
 *   - feed_task 的"带超时的阻塞取"（不是"看一眼不够就睡 5 ms"）。
 *
 * 诊断计数（acq_retry / enc_us / feed_idle …）全都是本文件 static：它们是 1000 帧
 * 窗口计数（打日志后清零），累计口径只有 bts_dropped 一个（在 bts_state.cc）。
 */

#include "bts_feed.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "bts_audio.h"
#include "bts_state.h"

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

/* PCM 环形缓冲（存 PSRAM，≈340 ms @48k/16b/2ch） */
StreamBufferHandle_t bts_pcm;
static TaskHandle_t  s_feed_task;

/* 馈送路径诊断计数（2026-10 加）：用来判断"发不够"到底卡在哪一段 */
static volatile uint32_t s_acq_retry;      /* acquire_write 拿不到缓冲的次数 */
static volatile uint32_t s_acq_sleep_ms;   /* 为此睡掉的总毫秒 */
static volatile uint32_t s_rel_timeout;    /* release_write 超时（丢帧）次数 */
static volatile uint64_t s_enc_us;         /* SBC 编码累计耗时（us） */
static volatile uint32_t s_feed_idle;      /* feed_task 因"没数据"空转的次数 */

static uint32_t s_tx_frames;     /* 已发出的编码帧数（统计） */
static uint32_t s_tx_bytes;      /* 已发出的编码字节数（统计） */
static uint32_t s_asrc_err_logged;/* ASRC 失败计数（限频打日志用） */
static uint32_t s_asrc_probe;     /* 前几次 ASRC 转换的"进/出"探测日志 */
static int64_t  s_tx_t0;          /* 吞吐统计窗口起点（us） */
static uint32_t s_last_drop_log_ms;/* 环形缓冲满的上次告警时刻 */

/* ------------------------------------------------------------------ 媒体时钟节拍
 *
 * ★★ 2026-10 修：**切到蓝牙后整条链跑到 3.8 倍实时，并把 CPU1 的 IDLE 饿死**。
 *
 * 现场日志：
 *   a2dp tx: 149000 frames, 1301 f/s (need 345 f/s), ... acq_retry 0, rel_to 0, idle 0, ring 62464 B
 *   play stats: PCM 708 KB/s | 产 18880 ms / 墙 5000 ms (377.6%) | route 1
 *   task_wdt: IDLE1 (CPU 1) 没喂狗；CPU 1 正在跑 bt_feed
 *
 * 根因：这条链原来的节拍**完全依赖 A2DP 协议栈回压**（`acquire_write` 拿不到发送窗口
 * 就等）。而协议栈只保证"不超过链路能力"，**不保证"就是媒体速率"** —— 链路有余量时
 * 它照单全收（2.4 GHz 空闲时 ACL 能扛 ~1 Mbps 以上，SBC 只要 ~250 kbps）。
 * 于是 feed 全速编码发送 → 环形缓冲一直被抽干 → 上游（解码/输出）被拖着全速跑：
 *   - 对端收到 3.8 倍的帧，多余的只能丢 → 听感是噪音/变调；
 *   - 上游 PCM 708 KB/s，IDLE 被饿死 → task_wdt（10 s 阈值）刷寄存器；
 *   - 环形缓冲永远是满的，端到端白白多出 ~340 ms 延迟。
 * （以前"卡顿"那轮把"没数据就睡 5 ms"改掉是对的，但那只解决了"发不够"；
 *   "发太多"这一侧一直没有节拍。）
 *
 * 现在**自己按媒体时钟发**：每秒 bts_rate ÷ 每帧采样数 帧。实现用 tick 槽 + 累加器
 * （1000 Hz 下 345 f/s = 0.345 帧/tick → 大多数槽 0 帧、偶尔 1 帧），长期精确到帧，
 * 且每帧只睡 1 ms（us 级忙等会把一个核烧掉，不能用）。
 * 编码器还没就绪（帧率未知）时不限制，退回原行为。
 */
static uint32_t   s_media_fps;      /* 对端帧率（帧/秒）；0 = 未知/不限制 */
static TickType_t s_pace_tick;      /* 上次结算的 tick */
static uint32_t   s_pace_acc;       /* 帧率累加器（单位：帧 × TICK_HZ） */
static uint32_t   s_pace_quota;     /* 本 tick 剩下的发送额度（帧） */

/** 编码器刚就绪时调一次：按协商结果算对端帧率 */
static void bts_pace_init(void)
{
    const uint32_t per_frame = (uint32_t)(bts_enc_in_bytes /
                                (bts_channels > 0 ? bts_channels * 2 : 2));
    s_media_fps  = (per_frame && bts_rate > 0) ? ((uint32_t)bts_rate / per_frame) : 0;
    s_pace_tick  = 0;
    s_pace_acc   = 0;
    s_pace_quota = 0;
    if (s_media_fps) {
        ESP_LOGI(TAG, "media clock: %u f/s (每 %u us 一帧)", (unsigned)s_media_fps,
                 (unsigned)(1000000u / s_media_fps));
    }
}

/** 取一个"发送额度"；额度用完返回 false（调用方睡到下一个 tick） */
static bool bts_pace_take(void)
{
    if (s_media_fps == 0) {
        return true;                     /* 帧率未知：不限制 */
    }
    const TickType_t now = xTaskGetTickCount();
    if (now != s_pace_tick) {
        s_pace_tick  = now;
        s_pace_acc  += s_media_fps;                       /* 本槽应发 acc / TICK_HZ 帧 */
        s_pace_quota = s_pace_acc / configTICK_RATE_HZ;
        s_pace_acc  -= s_pace_quota * configTICK_RATE_HZ; /* 余数留给下一槽 → 长期不漂 */
        /*
         * 卡顿（刚开编码器、被抢 CPU、对端暂停）之后**不许攒额度**：
         * 上限 = 每槽均值 + 2 帧。否则一次卡顿会换来一串"补发"的突发，
         * 对端那边就是一段噪音。
         */
        const uint32_t cap = s_media_fps / configTICK_RATE_HZ + 2;
        if (s_pace_quota > cap) {
            s_pace_quota = cap;
            s_pace_acc   = 0;
        }
    }
    if (s_pace_quota == 0) {
        return false;
    }
    s_pace_quota--;
    return true;
}

/* 编码暂存区里的一帧并交给 A2DP 源（一包 = 一个编码帧） */
static void bts_send_one_frame(esp_bt_audio_stream_handle_t h)
{
    esp_audio_enc_in_frame_t in = {};
    in.buffer = bts_stage;
    in.len    = (uint32_t)bts_enc_in_bytes;
    esp_audio_enc_out_frame_t out = {};
    out.buffer = bts_enc_frame;
    out.len    = (uint32_t)bts_enc_out_bytes;

    const int64_t t_enc0 = esp_timer_get_time();
    if (esp_audio_enc_process(bts_enc, &in, &out) != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        bts_dropped = bts_dropped + (uint32_t)bts_enc_in_bytes;
    } else {
        s_enc_us = s_enc_us + (uint64_t)(esp_timer_get_time() - t_enc0);
        /*
         * 发送池被占满时（NO_MEM）稍等重试；队列满（release 超时）就丢掉这一帧。
         * 都只等很短的时间 —— 在这里长时间等会把 48k→A2DP 的差速全部压到
         * 环形缓冲上，最终表现为大面积丢帧。
         *
         * ★ 重试次数与睡掉的毫秒都记账（s_acq_retry / s_acq_sleep_ms）：
         *   "发不够帧率"时靠这两个数区分是**协议栈/链路**给不出缓冲，
         *   还是我们自己算得慢（后者看 s_enc_us）。
         */
        for (int retry = 0; retry < 20; retry++) {
            esp_bt_audio_stream_packet_t pkt = {};
            if (esp_bt_audio_stream_acquire_write(h, &pkt, out.encoded_bytes) != ESP_OK || !pkt.data) {
                s_acq_retry = s_acq_retry + 1;
                s_acq_sleep_ms = s_acq_sleep_ms + 2;
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            memcpy(pkt.data, bts_enc_frame, out.encoded_bytes);
            pkt.size = out.encoded_bytes;
            if (esp_bt_audio_stream_release_write(h, &pkt, TX_WAIT_MS) != ESP_OK) {
                bts_dropped = bts_dropped + out.encoded_bytes;
                s_rel_timeout = s_rel_timeout + 1;
            } else {
                s_tx_frames++;
                s_tx_bytes += out.encoded_bytes;
                if (s_tx_frames == 1) {
                    /* 第一帧：一线"通了"的最短判据（排查没声音时先看这一行） */
                    ESP_LOGI(TAG, "first a2dp frame sent (%u B)", (unsigned)out.encoded_bytes);
                }
                if ((s_tx_frames % FEED_LOG_EVERY_FRAMES) == 0) {
                    /* 带上时间窗口算实际帧率：A2DP 每秒必须发够 rate/128 帧，
                     * 少了就是"慢放"（对端欠载），多了会被丢（对端过载/噪音） */
                    const int64_t now = esp_timer_get_time();
                    const uint32_t per_frame = (uint32_t)(bts_enc_in_bytes /
                                              (bts_channels > 0 ? bts_channels * 2 : 2));
                    const double secs = (s_tx_t0 && now > s_tx_t0) ? (double)(now - s_tx_t0) / 1e6 : 0.0;
                    if (secs > 0.0) {
                        const uint32_t ring = (uint32_t)xStreamBufferBytesAvailable(bts_pcm);
                        ESP_LOGI(TAG, "a2dp tx: %u frames, %.0f f/s (need %.0f f/s), dropped %u B | "
                                 "acq_retry %u (%u ms slept), rel_to %u, enc %u us/frame, idle %u, ring %u B",
                                 (unsigned)s_tx_frames,
                                 (double)FEED_LOG_EVERY_FRAMES / secs,
                                 (double)bts_rate / (double)(per_frame ? per_frame : 1),
                                 (unsigned)bts_dropped,
                                 (unsigned)s_acq_retry, (unsigned)s_acq_sleep_ms,
                                 (unsigned)s_rel_timeout,
                                 (unsigned)(s_enc_us / (FEED_LOG_EVERY_FRAMES ? FEED_LOG_EVERY_FRAMES : 1)),
                                 (unsigned)s_feed_idle, (unsigned)ring);
                        s_acq_retry    = 0;
                        s_acq_sleep_ms = 0;
                        s_rel_timeout  = 0;
                        s_enc_us       = 0;
                        s_feed_idle    = 0;
                    } else {
                        ESP_LOGI(TAG, "a2dp tx: %u frames, dropped %u B",
                                 (unsigned)s_tx_frames, (unsigned)bts_dropped);
                    }
                    s_tx_t0 = now;
                }
            }
            break;
        }
    }

    /* 用掉一个编码帧的输入，把余量搬到前面 */
    if (bts_stage_len > (uint32_t)bts_enc_in_bytes) {
        const uint32_t rest = bts_stage_len - (uint32_t)bts_enc_in_bytes;
        memmove(bts_stage, bts_stage + bts_enc_in_bytes, rest);
        bts_stage_len = rest;
    } else {
        bts_stage_len = 0;
    }
}

static void feed_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_bt_audio_stream_handle_t h = bts_stream;
        if (!h || !bts_streaming || !bts_pcm) {
            /* 流没起来 / 已停：把编码器/重采样器收掉，等下次 STARTED 重建 */
            if (bts_enc) {
                bts_audio_close();
            }
            s_media_fps = 0;             /* 编码器没了：节拍状态一并作废 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!bts_enc && !bts_enc_open(h)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_media_fps == 0) {
            bts_pace_init();             /* 编码器刚就绪（或刚重建）：算对端帧率 */
        }

        /* 暂存里够一个编码帧就先编码发送 —— 但要**按媒体时钟**发（见 bts_pace_take）：
         * 协议栈不会按实时速率回压，不自己节拍就会全速发送（真机实测 3.8 倍）。 */
        if (bts_stage_len >= (uint32_t)bts_enc_in_bytes) {
            if (!bts_pace_take()) {
                vTaskDelay(1);           /* 额度用完：让出 1 ms（FREERTOS_HZ=1000） */
                continue;
            }
            bts_send_one_frame(h);
            continue;
        }

        /* 不需要重采样（对端就是 48 kHz）：直接按编码帧长度取 PCM。
         * ★ 用**带超时的阻塞取**而不是"看一眼不够就睡 5 ms"：后者等于给馈送速率设了
         *   一个地板（每次最多 512 B / 5 ms ≈ 100 KB/s < 需要的 192 KB/s），
         *   缓冲一旦被抽空就再也追不上 —— 听感就是持续的"一卡一卡"。
         *   阻塞取在有数据时**立刻**返回，没数据才等（最多 5 ms）。 */
        if (!bts_asrc) {
            const size_t got = xStreamBufferReceive(bts_pcm, bts_stage + bts_stage_len,
                                                    (size_t)bts_enc_in_bytes, pdMS_TO_TICKS(5));
            if (got == 0) {
                s_feed_idle = s_feed_idle + 1;
                continue;
            }
            bts_stage_len += (uint32_t)got;
            continue;
        }

        /* 需要重采样：先取一段 48 kHz PCM，转成对端采样率再进暂存区。
         * ASRC 要的是**整段**，取不满就白取，所以这里只能"够不够"再取；
         * 但等待改成 1 ms（原来 5 ms 同样是个速率地板）。 */
        if (xStreamBufferBytesAvailable(bts_pcm) < (size_t)bts_asrc_in_bytes) {
            s_feed_idle = s_feed_idle + 1;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (xStreamBufferReceive(bts_pcm, bts_asrc_in, bts_asrc_in_bytes, 0) != bts_asrc_in_bytes) {
            continue;
        }
        uint32_t in_samples  = bts_asrc_in_bytes / bts_asrc_in_smp;
        uint32_t out_samples = bts_asrc_out_cap_samples;
        if (esp_asrc_process(bts_asrc, bts_asrc_in, in_samples, bts_asrc_out, &out_samples) !=
                ESP_ASRC_ERR_OK || out_samples == 0) {
            bts_dropped = bts_dropped + bts_asrc_in_bytes;
            /* 别静默失败：重采样挂了等于又回到"没声音"，必须能看见 */
            if ((++s_asrc_err_logged % 200) == 1) {
                ESP_LOGW(TAG, "asrc process failed (%u in-samples, first at #%u)",
                         (unsigned)in_samples, (unsigned)s_asrc_err_logged);
            }
            continue;
        }
        /* 前几次把"进多少、出多少"打出来：48k→44.1k 应该是 128 进 ≈ 117~118 出。
         * 这个比例不对（比如出=进，或者出=0）就是重采样没生效，声音必然不对。 */
        if (s_asrc_probe < 5) {
            s_asrc_probe++;
            ESP_LOGI(TAG, "asrc probe #%u: %u in -> %u out frames",
                     (unsigned)s_asrc_probe, (unsigned)in_samples, (unsigned)out_samples);
        }
        bts_stage_append(bts_asrc_out, out_samples * bts_asrc_out_smp);
    }
}

/* init 用：建 PCM 缓冲（PSRAM 优先）+ feed 任务（幂等） */
esp_err_t bts_feed_start(void)
{
    if (!bts_pcm) {
        bts_pcm = xStreamBufferCreateWithCaps(PCM_BUF_BYTES, PCM_BUF_TRIGGER, MALLOC_CAP_SPIRAM);
        if (!bts_pcm) {
            ESP_LOGW(TAG, "PSRAM stream buffer failed, fall back to internal");
            bts_pcm = xStreamBufferCreate(PCM_BUF_BYTES, PCM_BUF_TRIGGER);
        }
        if (!bts_pcm) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_feed_task) {
        xTaskCreate(feed_task, "bt_feed", FEED_TASK_STACK, NULL, 6, &s_feed_task);
    }
    return ESP_OK;
}

/* deinit 用：**先删任务**（此时它可能正在编码/发送），再删环形缓冲 */
void bts_feed_stop(void)
{
    if (s_feed_task) {
        vTaskDelete(s_feed_task);
        s_feed_task = NULL;
    }
    if (bts_pcm) {
        vStreamBufferDelete(bts_pcm);
        bts_pcm = NULL;
    }
}

bool bts_feed_created(void)
{
    return bts_pcm != NULL || s_feed_task != NULL;
}

esp_err_t bt_service_write_pcm(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 这段 PCM 在 48k/2ch/16bit 下代表多少毫秒（用于没处可写时"按时间睡"） */
    const uint32_t dur_ms = (uint32_t)(((uint64_t)len * 1000ULL) /
                                       ((uint64_t)A2DP_PCM_RATE * A2DP_PCM_CH * (A2DP_PCM_BITS / 8)));

    if (!bts_pcm || !bts_streaming) {
        /*
         * 蓝牙流还没起来 / 已经停了：丢掉这段音频，但**按它的播放时长睡一下**。
         * 不睡的话 GMF 解码管线会全速空转（见下面流控的说明），
         * 等流一起来，环形缓冲里只剩"最新的那一小段"，声音必然是抽稀过的噪音。
         */
        vTaskDelay(pdMS_TO_TICKS(dur_ms > 0 ? dur_ms : 1));
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★ 蓝牙通路的流控就在这一行：**阻塞**等环形缓冲腾出位置。
     *
     * 扬声器通路的节拍由 esp_codec_dev_write() 提供 —— 它写 I2S/codec 时会被
     * DMA 缓冲挡住，GMF 解码任务自然按 48 kHz 的实时速度推进。
     * 蓝牙这边如果"满了就丢帧并立刻返回成功"，GMF 会以为写成功了，
     * 于是全速解码：实测 8 秒灌进 1.5 MB，而链路每秒只吃得下 176 KB ——
     * 我们只消费到环形缓冲里最新的一小段，等于把音乐抽稀成 1/20 再按 44.1 kHz
     * 发出去，对端听到的就是**噪音**（日志里的 "pcm ring full: dropped ..." 就是它）。
     * 阻塞式发送把节拍还给解码管线，缓冲就不会被灌爆。
     *
     * 上限 200 ms：万一 A2DP 侧真的卡死，也只丢这一段，不会把管线永久挂住。
     *
     * （上面这段是 GMF 时代写下的，语义现在完全一样：上游从 GMF 的解码任务换成了
     *   audio_service 的输出级 —— 见 docs/audio-stack.md §1，仍然是"阻塞回压"。
     *   **别**改成"满了丢帧立刻返回"。）
     */
    const size_t sent = xStreamBufferSend(bts_pcm, data, len, pdMS_TO_TICKS(200));
    if (sent != len) {
        bts_dropped = bts_dropped + (uint32_t)(len - sent);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_drop_log_ms > 2000) {
            s_last_drop_log_ms = now_ms;
            ESP_LOGW(TAG, "pcm ring full after 200ms: dropped %u B (total %u B)",
                     (unsigned)(len - sent), (unsigned)bts_dropped);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t bt_service_dropped_bytes(void) { return bts_dropped; }
