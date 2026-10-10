/*
 * audio_output.cc —— 输出级：把解码器吐出的 PCM 送到扬声器 / 蓝牙 / 静音
 *
 * 数据通路（spa：espaudiocore 的解码任务 → 本文件的任务）：
 *
 *   espaudiocore 解码任务
 *     └─ AudioSinkRingbuf::write_i32()        ← cfg.output_bits=16，恒定 int16
 *          └─ PCM ringbuf（AUDIO_PCM_RING_BYTES，PSRAM）
 *               └─ out_task（本文件）
 *                    ├─ esp_asrc：源采样率/声道 → 48 kHz / 2ch（16-bit）
 *                    └─ 按 s_route 分流
 *                         ├─ SPEAKER    : esp_codec_dev_write() → ES8389(TDM) → NS4150B
 *                         ├─ BLUETOOTH  : 软件音量 → bt_service_write_pcm()
 *                         └─ NONE       : 只走时间轴，不写硬件
 *
 * ── 为什么必须有这一层（而不是直接用 espaudiocore 的 I2S 输出）────────────────
 * 1. 本板 I2S 是 **TDM**，espaudiocore 的 I2S sink 用 STD API 重配时钟，配不下去
 *    （详见 audio_engine.cc 的说明）；
 * 2. espaudiocore **不做重采样、不做声道转换**，而工程的不变式是
 *    "写硬件的 PCM 恒为 48 kHz/16bit/2ch"（进度条、看门狗、seek 都按 192 B/ms 算）；
 * 3. 输出三路（扬声器/蓝牙/静音）与功放、静音时序是板级需求，库里没有。
 *
 * ── 重采样为什么用 esp_asrc 的**软件**路径 ───────────────────────────────────
 * S31 有硬件 ASRC，但硬件路径要经 GDMA 访问 PSRAM 缓冲，对 cache-line 对齐与
 * 一致性有硬要求；bt_service 已经踩过"先慢放、几秒后全是噪音"的坑并退回软件路径
 * （见 bt_service.cc 里 bt_asrc_open() 的长注释）。这里沿用同样的选择：
 * SW_SPEED + complexity 3，44.1k→48k 立体声每秒不到 200 KB，320 MHz 上余量充足。
 * 想试硬件就把 perf_type 换成 ESP_ASRC_PERF_TYPE_AUTO，并改用
 * esp_asrc_get_buffer_alignment()/esp_asrc_align_alloc() 分配缓冲。
 *
 * ── 时间轴 ─────────────────────────────────────────────────────────────────
 * s_played_bytes 统计的是**送去硬件的 48k/16/2 字节数**，所以
 * `ms = bytes / AUDIO_OUT_BYTES_PER_MS`（192）恒成立。这是全工程唯一的时间轴口径，
 * 不要在别处再按源采样率算一套。
 */
#include "audio_internal.h"

#include <string.h>

#include "esp_asrc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "bsp.h"
#include "bt_service.h"
#include "espaudiocore.h"

static const char *TAG = "audio_out";

/** 一次 ASRC 处理的最大帧数（1024 帧 ≈ 23 ms @44.1k，正好对齐 ringbuf 的块） */
#define ASRC_MAX_FRAMES   2048
/** 健康度日志窗口 */
#define STATS_WINDOW_MS   5000
/** 输出任务栈（只做 memcpy/重采样/写 codec，不要大） */
#define OUT_TASK_STACK    4096
#define OUT_TASK_PRIO     6
#define OUT_TASK_CORE     1     /* 与解码任务（core 0）分开，避免互相饿死 */

/* ===========================================================================
 * 状态
 * =========================================================================*/

static RingbufHandle_t s_pcm_rb;
static TaskHandle_t    s_task;

static volatile uint32_t s_gen;          /**< 0 = 没有会话 */
static volatile bool     s_eos;          /**< 解码侧已到末尾，等输出抽干 */
static volatile bool     s_flowing;      /**< 本会话已经真的写过硬件 */
static volatile uint64_t s_played_bytes; /**< 送去硬件的 48k/16/2 字节数（时间轴） */
static volatile uint32_t s_tl_base_ms;   /**< 时间轴基准（换曲 0；seek 后 = 目标时刻） */
static volatile audio_out_t s_route = AUDIO_OUT_SPEAKER;
static volatile int      s_volume = 55;

/* 重采样 */
static esp_asrc_handle_t s_asrc;
static uint32_t          s_src_rate;
static uint8_t           s_src_ch;
static uint16_t          s_in_smp_bytes;   /**< 源：每帧字节数（ch×2） */
static uint16_t          s_out_smp_bytes;  /**< 目标：每帧字节数（恒 4） */
static uint8_t          *s_in_buf;
static size_t            s_in_cap;         /**< ASRC 输入缓冲容量（字节） */
static uint8_t          *s_out_buf;
static uint32_t          s_out_frames_cap;
static uint32_t          s_asrc_err;
/** mono→stereo 的权重：esp_asrc 硬件路径要求显式给 {1,1} 才认这条路 */
static float             s_weight[2] = {1.0f, 1.0f};

/** 蓝牙路软件音量用的暂存（按需增长） */
static uint8_t          *s_bt_stage;
static size_t            s_bt_stage_cap;

/* 健康度统计 */
static uint32_t s_stats_ms;
static uint64_t s_stats_bytes;
static uint64_t s_stats_codec_us;
static uint32_t s_stats_calls;
static uint32_t s_stats_gap_max_ms;
static uint32_t s_stats_last_ms;
static uint32_t s_codec_err;             /**< 写 codec 失败次数（统计窗口内） */

/* ===========================================================================
 * 小工具
 * =========================================================================*/

static void ring_drain(void)
{
    if (!s_pcm_rb) {
        return;
    }
    for (;;) {
        size_t got = 0;
        uint8_t *it = (uint8_t *)xRingbufferReceiveUpTo(s_pcm_rb, &got, 0, 4096);
        if (!it) {
            break;
        }
        vRingbufferReturnItem(s_pcm_rb, it);
    }
}

/** 16-bit PCM 就地软件音量缩放（带削波保护）。**只给蓝牙路用**：
 *  扬声器路的音量走 ES8389 的数字音量寄存器（更干净、也不占 CPU）。*/
static void pcm16_scale(uint8_t *buf, size_t samples, int percent)
{
    if (percent >= 100) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    int16_t *p = (int16_t *)buf;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = ((int32_t)p[i] * percent) / 100;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        p[i] = (int16_t)v;
    }
}

static uint8_t *bt_stage(size_t need)
{
    if (s_bt_stage_cap >= need) {
        return s_bt_stage;
    }
    uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        return NULL;
    }
    if (s_bt_stage) {
        heap_caps_free(s_bt_stage);
    }
    s_bt_stage = p;
    s_bt_stage_cap = need;
    return s_bt_stage;
}

/* ===========================================================================
 * 重采样（esp_asrc）
 * =========================================================================*/

static void asrc_close(void)
{
    if (s_asrc) {
        esp_asrc_close(s_asrc);
        s_asrc = NULL;
    }
    if (s_in_buf) {
        heap_caps_free(s_in_buf);
        s_in_buf = NULL;
    }
    if (s_out_buf) {
        heap_caps_free(s_out_buf);
        s_out_buf = NULL;
    }
    s_in_cap = 0;
    s_out_frames_cap = 0;
}

static bool asrc_open(uint32_t src_rate, uint8_t src_ch)
{
    esp_asrc_cfg_t cfg = {};
    cfg.src_info.sample_rate     = src_rate;
    cfg.src_info.channel         = src_ch;
    cfg.src_info.bits_per_sample = AUDIO_OUT_BITS;
    cfg.dest_info.sample_rate    = AUDIO_OUT_RATE;
    cfg.dest_info.channel        = AUDIO_OUT_CHANNELS;
    cfg.dest_info.bits_per_sample = AUDIO_OUT_BITS;
    cfg.perf_type = ESP_ASRC_PERF_TYPE_SW_SPEED;
    cfg.complexity = 3;
    cfg.timeout_ms = 1000;
    if (src_ch != AUDIO_OUT_CHANNELS) {
        /* 单声道 → 立体声：显式给权重，硬件路径也认这种配置 */
        cfg.weight = s_weight;
        cfg.weight_len = 2;
        s_weight[0] = 1.0f;
        s_weight[1] = 1.0f;
    }

    if (esp_asrc_open(&cfg, &s_asrc) != ESP_ASRC_ERR_OK || !s_asrc) {
        s_asrc = NULL;
        ESP_LOGE(TAG, "asrc open failed: %u Hz %uch -> %d Hz %dch",
                 (unsigned)src_rate, src_ch, AUDIO_OUT_RATE, AUDIO_OUT_CHANNELS);
        return false;
    }
    if (esp_asrc_get_bytes_per_sample(s_asrc, &s_in_smp_bytes, &s_out_smp_bytes) != ESP_ASRC_ERR_OK ||
        s_in_smp_bytes == 0 || s_out_smp_bytes == 0) {
        asrc_close();
        return false;
    }

    uint32_t out_frames = 0;
    if (esp_asrc_get_out_sample_num(s_asrc, ASRC_MAX_FRAMES, &out_frames) != ESP_ASRC_ERR_OK ||
        out_frames == 0) {
        asrc_close();
        return false;
    }
    /* 非整数比时输出帧数会浮动，留一点余量 */
    s_out_frames_cap = out_frames + 64;
    s_in_cap = (size_t)ASRC_MAX_FRAMES * s_in_smp_bytes;
    const size_t out_sz = (size_t)s_out_frames_cap * s_out_smp_bytes;

    /* 软件路径不要求 cache 对齐；优先放 PSRAM（省内部 RAM），失败退内部 RAM */
    s_in_buf  = (uint8_t *)heap_caps_aligned_alloc(64, s_in_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out_buf = (uint8_t *)heap_caps_aligned_alloc(64, out_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_in_buf) {
        s_in_buf = (uint8_t *)heap_caps_aligned_alloc(64, s_in_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_out_buf) {
        s_out_buf = (uint8_t *)heap_caps_aligned_alloc(64, out_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_in_buf || !s_out_buf) {
        ESP_LOGE(TAG, "asrc buffer alloc failed");
        asrc_close();
        return false;
    }

    ESP_LOGI(TAG, "asrc ready (sw): %u Hz %uch -> %d Hz %dch, %u frames/call",
             (unsigned)src_rate, src_ch, AUDIO_OUT_RATE, AUDIO_OUT_CHANNELS,
             (unsigned)ASRC_MAX_FRAMES);
    return true;
}

/**
 * 采样率/声道随时可能就绪或变化（解码器是异步打开的，AAC 甚至逐帧上报），
 * 所以每轮都问一次；只有真的变了才重建 ASRC。
 *
 * ⚠ **必须持会话锁**：`espaudiocore_get_format()` 读的是库内部的会话对象，
 * 而 `audio_engine_stop()` 会在持锁状态下把它释放掉。不持锁的话存在这样一条窗口：
 * 输出任务刚通过 flow gate 检查 → 引擎关闸并 `espaudiocore_stop()` 释放 ctx →
 * 输出任务才调 `get_format()` → **use-after-free**（现场是随机 LoadProhibited）。
 * 这一段只有几十条指令，锁竞争可以忽略。
 */
static void pump_format(void)
{
    uint32_t rate = 0;
    uint8_t ch = 0, bits = 0;

    audio_core_lock();
    const esp_err_t ferr = espaudiocore_get_format(&rate, &ch, &bits);
    audio_core_unlock();

    if (ferr != ESP_OK || rate == 0 || ch == 0) {
        return;
    }
    if (rate == s_src_rate && ch == s_src_ch) {
        return;
    }

    asrc_close();
    s_src_rate = rate;
    s_src_ch = ch;
    ESP_LOGI(TAG, "source format: %u Hz %uch (bits=%u; ringbuf 恒为 16-bit)", (unsigned)rate, ch, bits);

    if (rate == AUDIO_OUT_RATE && ch == AUDIO_OUT_CHANNELS) {
        return;     /* 直通，不需要重采样 */
    }
    asrc_open(rate, ch);
}

/* ===========================================================================
 * 写硬件
 * =========================================================================*/

static void write_out(const uint8_t *pcm, size_t bytes)
{
    const audio_out_t route = s_route;
    const int64_t t0 = esp_timer_get_time();

    if (route == AUDIO_OUT_SPEAKER) {
        esp_codec_dev_handle_t dev = bsp_audio_codec();
        if (dev && esp_codec_dev_write(dev, (void *)pcm, bytes) != ESP_OK) {
            s_codec_err++;
        }
    } else if (route == AUDIO_OUT_BLUETOOTH) {
        const int vol = s_volume;
        if (vol > 0) {
            if (vol < 100) {
                /* 蓝牙路的音量只能软件做（ES8389 的寄存器对 A2DP 无效） */
                uint8_t *dst = bt_stage(bytes);
                if (dst) {
                    memcpy(dst, pcm, bytes);
                    pcm16_scale(dst, bytes / 2, vol);
                    bt_service_write_pcm(dst, bytes);
                } else {
                    bt_service_write_pcm(pcm, bytes);
                }
            } else {
                bt_service_write_pcm(pcm, bytes);
            }
        }
        /* vol == 0：静音，不推数据，但时间轴照走（与 GMF 时代一致） */
    }
    /* AUDIO_OUT_NONE：只走时间轴 */

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_played_bytes += bytes;
    /*
     * "已出声"标志**无条件**置位（连 AUDIO_OUT_NONE 静音路也算）：
     * 它的语义是"这一首的 PCM 已经流起来了、解码链是通的"，UI 靠它把
     * "缓冲中…"换回歌词。静音时若一直不置位，界面会永远停在"缓冲中"。
     * （GMF 时代的 out_write_cb 也是在函数开头无条件置位的。）
     */
    s_flowing = true;

    s_stats_bytes += bytes;
    s_stats_calls++;
    s_stats_codec_us += (uint64_t)(esp_timer_get_time() - t0);
    if (s_stats_last_ms) {
        const uint32_t gap = now - s_stats_last_ms;
        if (gap > s_stats_gap_max_ms) {
            s_stats_gap_max_ms = gap;
        }
    }
    s_stats_last_ms = now;
}

/** 每 5 秒一行播放健康度：确认"是不是真的在按时出声、卡在哪一段" */
static void stats_tick(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_stats_ms == 0) {
        s_stats_ms = now;
        return;
    }
    const uint32_t win = now - s_stats_ms;
    if (win < STATS_WINDOW_MS) {
        return;
    }
    const uint32_t audio_ms = (uint32_t)(s_stats_bytes / AUDIO_OUT_BYTES_PER_MS);
    const uint32_t kbps = (uint32_t)((uint64_t)s_stats_bytes * 1000ull / win / 1024ull);
    ESP_LOGI(TAG, "play stats: PCM %u KB/s (%llu B in %u ms), audio %u ms, codec %llu ms, "
                  "gap max %u ms, calls %u, route %d, src %u Hz %uch%s%s",
             (unsigned)kbps, (unsigned long long)s_stats_bytes, (unsigned)win,
             (unsigned)audio_ms, (unsigned long long)(s_stats_codec_us / 1000),
             (unsigned)s_stats_gap_max_ms, (unsigned)s_stats_calls, (int)s_route,
             (unsigned)s_src_rate, s_src_ch, s_asrc ? " (asrc)" : "",
             s_codec_err ? " [codec write errors!]" : "");

    s_stats_ms = now;
    s_stats_bytes = 0;
    s_stats_codec_us = 0;
    s_stats_calls = 0;
    s_stats_gap_max_ms = 0;
}

/* ===========================================================================
 * 输出任务
 * =========================================================================*/

static void play_block(uint8_t *data, size_t bytes)
{
    const size_t frame_bytes = (size_t)(s_src_ch ? s_src_ch : AUDIO_OUT_CHANNELS) * 2;
    bytes -= bytes % frame_bytes;      /* 半帧不处理（正常不会出现） */
    if (bytes == 0) {
        return;
    }

    const uint8_t *pcm = data;
    size_t pcm_bytes = bytes;

    if (s_asrc && s_out_buf) {
        const uint32_t in_frames = (uint32_t)(bytes / frame_bytes);
        uint32_t out_frames = s_out_frames_cap;
        if (esp_asrc_process(s_asrc, data, in_frames, s_out_buf, &out_frames) != ESP_ASRC_ERR_OK ||
            out_frames == 0) {
            s_asrc_err = s_asrc_err + 1;      /* volatile 上不能用 ++（C++20 起 -Werror=volatile） */
            if ((s_asrc_err % 200) == 1) {
                ESP_LOGW(TAG, "asrc process failed (%u in-frames, #%u)", (unsigned)in_frames,
                         (unsigned)s_asrc_err);
            }
            return;
        }
        pcm = s_out_buf;
        pcm_bytes = (size_t)out_frames * s_out_smp_bytes;
    }

    write_out(pcm, pcm_bytes);
}

static void out_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "output task started on core %d", xPortGetCoreID());

    for (;;) {
        const uint32_t gen = s_gen;
        if (gen == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* 暂停时不消费：缓冲里那点音频留着，恢复后接着播（见 audio_internal.h） */
        if (!audio_engine_flow_gate()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        pump_format();

        size_t want = s_in_cap ? s_in_cap : 4096;
        size_t got = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_pcm_rb, &got, pdMS_TO_TICKS(100), want);
        if (!item || got == 0) {
            /* 取不到数据：如果是"解码已到末尾"，说明积压播完了 → 上报 FINISHED */
            if (s_eos && gen == s_gen) {
                s_eos = false;
                audio_engine_output_drained(gen);
            }
            stats_tick();
            continue;
        }

        play_block(item, got);
        vRingbufferReturnItem(s_pcm_rb, item);
        stats_tick();
    }
}

/* ===========================================================================
 * 对外（组件内部）接口
 * =========================================================================*/

esp_err_t audio_output_init(void)
{
    if (s_pcm_rb) {
        return ESP_OK;      /* 幂等 */
    }
    /* PCM 缓冲放 PSRAM：内部 RAM 要留给射频（见 main.cc 的 RADIO_MIN_INTERNAL 检查） */
    s_pcm_rb = xRingbufferCreateWithCaps(AUDIO_PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_rb) {
        ESP_LOGW(TAG, "PSRAM ringbuf failed, falling back to internal RAM");
        s_pcm_rb = xRingbufferCreate(AUDIO_PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    }
    if (!s_pcm_rb) {
        ESP_LOGE(TAG, "PCM ringbuf create failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(out_task, "audio_out", OUT_TASK_STACK, NULL, OUT_TASK_PRIO,
                                &s_task, OUT_TASK_CORE) != pdPASS) {
        vRingbufferDelete(s_pcm_rb);
        s_pcm_rb = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

RingbufHandle_t audio_output_pcm_ring(void)
{
    return s_pcm_rb;
}

void audio_output_session_begin(uint32_t gen)
{
    ring_drain();
    asrc_close();
    s_src_rate = 0;
    s_src_ch = 0;
    s_eos = false;
    s_flowing = false;
    s_played_bytes = 0;
    s_tl_base_ms = 0;
    s_asrc_err = 0;
    s_stats_ms = 0;
    s_stats_bytes = 0;
    s_stats_codec_us = 0;
    s_stats_calls = 0;
    s_stats_gap_max_ms = 0;
    s_stats_last_ms = 0;
    s_codec_err = 0;
    if (s_route == AUDIO_OUT_SPEAKER) {
        bsp_audio_set_mute(false);      /* 起播解锁静音（stop 时才会静音） */
    }
    s_gen = gen;                        /* 最后置：置上之后任务才开始消费 */
}

void audio_output_session_end(void)
{
    s_gen = 0;
    s_eos = false;
    asrc_close();
    s_src_rate = 0;
    s_src_ch = 0;
    ring_drain();
}

void audio_output_note_eos(uint32_t gen)
{
    if (gen == s_gen) {
        s_eos = true;
    }
}

void audio_output_flush(void)
{
    ring_drain();
}

void audio_output_note_seek(uint32_t pos_ms)
{
    s_tl_base_ms = pos_ms;
    s_played_bytes = 0;
    s_flowing = false;      /* UI 据此重新挂上"缓冲中…" */
    s_eos = false;
}

uint32_t audio_output_position_ms(void)
{
    return s_tl_base_ms + (uint32_t)(s_played_bytes / AUDIO_OUT_BYTES_PER_MS);
}

void audio_output_set_route(audio_out_t out)
{
    if (out == s_route) {
        return;
    }

    switch (out) {
    case AUDIO_OUT_SPEAKER:
        bt_service_media_stop();
        bsp_audio_set_mute(false);
        audio_output_pa(audio_engine_running());
        break;

    case AUDIO_OUT_BLUETOOTH:
        /* 先掐掉板载出声再起蓝牙（左右共用一个功放使能，同时出声就是回声） */
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        bt_service_media_start();
        break;

    case AUDIO_OUT_NONE:
    default:
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        break;
    }

    s_route = out;
    ESP_LOGI(TAG, "output route -> %d", (int)out);
}

audio_out_t audio_output_route(void)
{
    return s_route;
}

void audio_output_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (s_route == AUDIO_OUT_SPEAKER) {
        bsp_audio_set_volume(percent);   /* 扬声器：ES8389 数字音量寄存器 */
    }
    /* 蓝牙：每次写 PCM 时按 s_volume 做软件缩放，这里不用做 */
}

int audio_output_volume(void)
{
    return s_volume;
}

bool audio_output_flowing(void)
{
    return s_flowing;
}

uint64_t audio_output_played_bytes(void)
{
    return s_played_bytes;
}

void audio_output_pa(bool on)
{
    bsp_audio_pa_enable(on);
}
