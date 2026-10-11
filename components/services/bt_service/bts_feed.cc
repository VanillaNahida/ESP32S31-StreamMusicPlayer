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

/*
 * feed 侧是否已经能消费（SBC 编码器 + 重采样都开好了）。
 *
 * ★ 为什么要这个标志：`bt_service_media_start()` 返回之后，feed 任务还要先开编码器
 *   和软件 ASRC，真机实测这一段要 **330 ms**（日志：media start 62408 → first frame
 *   62742），而环形缓冲只有 **340 ms** 的容量。这期间要是照常往里写 PCM，缓冲立刻
 *   被灌满，第一次阻塞发送必然超时丢帧 —— 而那次丢帧的字节数还很可能不是一帧的
 *   整数倍（真机就是 `dropped 3749 B`，3749 = 4×937+1），后面的 PCM 从此整体错位，
 *   整首歌都是沙沙声（见 bt_service_write_pcm 的说明）。
 *   有了这个标志，就绪前 write_pcm 只按音频时长睡一下、一个字都不写：开场不溢出，
 *   而且第一帧就是"当下"的音频，不是 340 ms 前的陈货。
 */
static volatile bool s_feed_ready;

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

/* ------------------------------------------------------------------ 媒体时钟限速
 *
 * ★★ 2026-10 现状：**节拍由协议栈负责，我们只做"别跑太快"的限速**。
 *
 * esp_bt_audio 的 A2DP 源自己就是节拍器：20 ms 定时器（A2DP_SRC_SEND_INTERVAL_MS）
 * 按 `frames_per_tick`（= 采样率 ÷ 每帧采样数，44.1k 是 6.89/20ms = 344.5 f/s）从
 * **20 包**的队列里取数据发出去（bt_audio_a2dp_src.c 的 calc_frames_to_send 用的是
 * "累计应发 − 已发"的口径，落后会补发；队列不足 max_batch_size 时那一拍干脆不发）。
 *
 * 但它对**上游没有任何回压**：队列满时我们的 release_write 只是超时丢帧后立刻返回，
 * 于是上游一旦不限速，整条链会跑到好几倍实时（真机实测过 3.8 倍：环形缓冲一直被抽干、
 * CPU1 的 IDLE 被饿死、对端收到一堆多余的帧）。
 *
 * 所以这里必须留一层限速，关键是**用累计口径**（与协议栈 calc_frames_to_send 同一个
 * 口径）：拿"媒体时钟建立以来应该发多少帧"和"实际发了多少帧"比 ——
 *   - 落后 → 立即放行（补发，绝不丢额度）；
 *   - 已经跑到前面 → 返回 false，调用方睡 1 ms。
 *
 * ★ 不要再改回"每个 tick 加一次 fps、来不及发的额度直接丢掉"的写法：任务被抢占/长
 *   阻塞之后，没发出去的额度会被**清零**，放行速率就长期低于媒体速率（真机实测
 *   309/345 = 89.6%，而当时 acq_retry=0、ring 是满的、enc 也才 20% —— 全是被这个
 *   漏加吃掉的）。协议栈那个 20 包队列只有 58 ms 的深度，被慢慢抽干就是"一卡一卡"。
 *
 * 现在这层**只会在自己跑快了的时候限速，永远不会少发**，因此和协议栈的节拍不再
 * 互相打架：真正决定"何时发出去"的仍然是协议栈的 20 ms 定时器。
 */
static uint32_t s_media_fps;        /* 对端帧率（帧/秒，仅用于日志/预算）；0 = 未知/不限制 */
/*
 * 帧率用**千分位定点**存：44100/128 = 344.53，整数存成 344 会慢 0.15%
 * （每分钟差 0.09 秒，长期会把对端缓冲抽干 → 偶尔咔哒）。
 */
static uint32_t s_media_fps_x1000;  /* 帧率 × 1000（344531 = 344.531 f/s）*/
static int64_t  s_pace_us;          /* 媒体时钟起点（esp_timer，µs）；0 = 还没建立 */
static uint32_t s_pace_frames;      /* 媒体时钟建立以来已发出的帧数 */

/** 编码器刚就绪时调一次：按协商结果算对端帧率，并把媒体时钟归零 */
static void bts_pace_init(void)
{
    const uint32_t per_frame = (uint32_t)(bts_enc_in_bytes /
                                (bts_channels > 0 ? bts_channels * 2 : 2));
    s_media_fps_x1000 = (per_frame && bts_rate > 0)
                            ? (uint32_t)((uint64_t)bts_rate * 1000u / per_frame) : 0;
    s_media_fps       = s_media_fps_x1000 / 1000u;      /* 只给日志/预算用 */
    s_pace_us         = 0;
    s_pace_frames     = 0;
    if (s_media_fps_x1000) {
        ESP_LOGI(TAG, "media clock: %u.%03u f/s (每 %u us 一帧)",
                 (unsigned)(s_media_fps_x1000 / 1000u), (unsigned)(s_media_fps_x1000 % 1000u),
                 (unsigned)(1000000000ull / s_media_fps_x1000));
    }
}

/** 取一个"发送额度"：还没跑到此刻应该发的帧数就放行，跑在前面则 false（调用方睡 1 ms） */
static bool bts_pace_take(void)
{
    if (s_media_fps == 0) {
        return true;                     /* 帧率未知：不限制 */
    }
    if (s_pace_us == 0) {
        s_pace_us = esp_timer_get_time();/* 第一帧：只对表，不放行额外额度 */
        return true;
    }
    /* 应该发的帧数 = 经过时间(µs) × 帧率(×1000) ÷ 1e9；累计比较 → 不丢额度 */
    const int64_t  elapsed = esp_timer_get_time() - s_pace_us;
    const uint32_t should  = (uint32_t)((uint64_t)elapsed * (uint64_t)s_media_fps_x1000 /
                                        1000000000ull);
    return s_pace_frames < should;
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
                s_pace_frames++;      /* 媒体时钟口径：真正发出去的帧数 */
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
            s_media_fps  = 0;            /* 编码器没了：节拍状态一并作废 */
            s_feed_ready = false;        /* 也不再接 PCM（见上面的说明）*/
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!bts_enc && !bts_enc_open(h)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!s_feed_ready) {
            bts_pace_init();             /* 编码器刚就绪（或刚重建）：算对端帧率 */
            s_feed_ready = true;         /* 编码器 + ASRC 都好了，可以开始接 PCM */
        }

        /* 暂存里够一个编码帧就先编码发送 —— 但要过媒体时钟的限速（见 bts_pace_take）：
         * 协议栈对上游不回压，不限速就会全速发送（真机实测 3.8 倍实时）。 */
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

    /*
     * 一帧 PCM 的字节数（48k/2ch/16bit = 4）。**写进缓冲的字节数必须是它的整数倍**，
     * 原因见下面那段长注释。
     */
    const size_t frame = A2DP_PCM_FRAME_BYTES;
    len -= len % frame;                     /* 调用方本来就按帧给；半帧不处理 */
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 这段 PCM 在 48k/2ch/16bit 下代表多少毫秒（用于没处可写时"按时间睡"） */
    const uint32_t dur_ms = (uint32_t)(((uint64_t)len * 1000ULL) /
                                       ((uint64_t)A2DP_PCM_RATE * A2DP_PCM_CH * (A2DP_PCM_BITS / 8)));

    if (!bts_pcm || !bts_streaming || !s_feed_ready) {
        /*
         * 蓝牙流还没起来 / 已经停了 / **feed 侧还没就绪**：丢掉这段音频，但按它的
         * 播放时长睡一下。不睡的话解码管线会全速空转，等流一起来环形缓冲里只剩
         * "最新的那一小段"，声音必然是抽稀过的噪音。
         *
         * s_feed_ready 这一条是 2026-10 为治沙沙声加的：media_start 之后 feed 还要
         * 先开编码器 + 软件 ASRC（真机 330 ms），而缓冲只有 340 ms 容量 —— 这期间
         * 照写不误必然灌爆（详见 s_feed_ready 的定义）。
         */
        vTaskDelay(pdMS_TO_TICKS(dur_ms > 0 ? dur_ms : 1));
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★★ 2026-10 修（真机"沙沙声"的两个根因都在这一段）。
     *
     * 【一】为什么必须自己按帧切
     *   bts_pcm 是 FreeRTOS StreamBuffer —— 一条**字节流**，不是样本流。原来直接把
     *   (data, len) 交给 xStreamBufferSend()，它超时会**只写进去一部分**，而写进去的
     *   字节数 = 当时的剩余空间，**不保证是一帧的整数倍**。真机日志：
     *       W bt_svc: pcm ring full after 200ms: dropped 3749 B (total 3749 B)
     *   3749 = 4×937 + 1 —— 从那以后读出来的每一帧 16-bit 样本都跨在半个样本上，
     *   高低字节全部互换，听感就是**持续的爆音/沙沙声**，直到下一次 xStreamBufferReset
     *   才会恢复（也就是整首歌都废了）。
     *   所以这里先问剩余空间、向下取整到整帧，只请求这么多：xStreamBufferSend() 要么
     *   把请求量整块写完，要么（只请求 1 帧时）超时 0 字节 —— 写进缓冲的字节数**恒为
     *   帧长的整数倍**，流再也不可能错位。
     *
     * 【二】为什么还留着"阻塞回压"
     *   扬声器通路的节拍由 esp_codec_dev_write() 提供（它写 I2S/codec 会被 DMA 挡住）。
     *   蓝牙这边如果"满了就丢帧并立刻返回成功"，解码端会以为写成功了于是全速解码，
     *   把缓冲灌爆 —— 所以这里仍然**阻塞**等缓冲腾位置，只是总等待上限 200 ms（万一
     *   A2DP 侧真卡死也只丢这一段，不会把管线永久挂住）。
     *   别改成"满了立刻返回"。
     */
    const int64_t t_start  = esp_timer_get_time();
    const int64_t budget_us = 200 * 1000;
    size_t off = 0;
    while (off < len) {
        const int64_t spent = esp_timer_get_time() - t_start;
        if (spent >= budget_us) {
            break;                          /* 200 ms 用完：剩下的整帧丢掉 */
        }
        size_t space = xStreamBufferSpacesAvailable(bts_pcm);
        size_t fit   = space - (space % frame);
        if (fit == 0) {
            /*
             * 一帧都塞不下：阻塞等一帧的位置。只请求 1 帧 → 成功就是整帧、超时就是
             * 0 字节，绝不会写进半帧（这正是上面【一】要避免的）。
             */
            fit = frame;
        }
        size_t want = len - off;
        if (want > fit) {
            want = fit;                     /* fit 恒为帧长整数倍（或正好 1 帧） */
        }
        const uint32_t wait_ms = (uint32_t)((budget_us - spent) / 1000);
        const size_t got = xStreamBufferSend(bts_pcm, data + off, want,
                                             pdMS_TO_TICKS(wait_ms > 0 ? wait_ms : 1));
        off += got;
        if (got < want) {
            break;                          /* 没等到位置 */
        }
    }

    if (off != len) {
        bts_dropped = bts_dropped + (uint32_t)(len - off);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_drop_log_ms > 2000) {
            s_last_drop_log_ms = now_ms;
            ESP_LOGW(TAG, "pcm ring full after 200ms: dropped %u B (total %u B, 已按帧对齐)",
                     (unsigned)(len - off), (unsigned)bts_dropped);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t bt_service_dropped_bytes(void) { return bts_dropped; }
