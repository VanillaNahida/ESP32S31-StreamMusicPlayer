/*
 * audio_engine.cc —— espaudiocore 播放会话
 *
 * 职责：
 *   1. 建/停会话（本地文件走 begin_rb，压缩流走 begin_stream_rb，**都不用 I2S 输出**）
 *   2. 把 espaudiocore 的事件映射成本工程的 audio_evt_t 并上报
 *   3. 维护"曲目信息"（采样率/声道/位深/时长/码率）与当前路径
 *
 * ── 为什么不用 espaudiocore 的 I2S 输出模式（begin / begin_stream）────────────
 * 本板的 I2S 是 **TDM**（`components/gen_bmgr_codes/gen_board_periph_config.c:49`
 * 是 `I2S_COMM_MODE_TDM`），而 espaudiocore 的 I2S sink 用 STD 的
 * `i2s_channel_reconfig_std_clock()/_slot()`（`src/audio_sink_i2s.cpp:402/476`），
 * IDF 对非 STD 句柄直接返回 ESP_ERR_INVALID_ARG —— 采样率一变就配不下去，
 * 44.1k 的音源会按 48k 时钟放（快 8.8%，明显变调）。
 * 所以统一走 **ringbuf 输出**：解码后的 PCM 交给 audio_output.cc，
 * 由它做重采样 → 48k/16/2 → `esp_codec_dev_write()`（TDM 通道归板级管理器所有，
 * 我们只是消费者，与 GMF 时代完全一致）。
 *
 * ── 与 GMF 时代的三个语义差异（app 层依赖，别改坏）─────────────────────────
 *   - `audio_service_is_playing()` **不含暂停态**，所以这里自己维护 s_running，
 *     不能直接用 `espaudiocore_is_running()`（它含暂停态）；
 *   - **没有 MUSIC_INFO 事件了**：曲目参数改成"起播时自己探测 + 轮询 get_format()"，
 *     码率自带探测（在线用 Content-Length ÷ 时长）；
 *   - seek 不再有"码率未知就拖不动"：本地文件由库内解码器 seek，恒可用。
 */
#include "audio_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp.h"
#include "espaudiocore.h"
#include "media_meta.h"

static const char *TAG = "audio_eng";

/* ===========================================================================
 * 状态
 * =========================================================================*/

static SemaphoreHandle_t s_lock;
static uint32_t          s_gen;                 /**< 会话代数（每次起播/停止 +1） */
static volatile bool     s_session_active;      /**< 有活跃会话（含暂停态） */
static volatile bool     s_running;             /**< 管线在跑（**不含暂停**） */
static volatile bool     s_is_stream;           /**< 本会话是压缩流（日志/诊断用） */
static bool              s_info_valid;
static audio_info_t      s_info;
static char              s_cur_path[AUDIO_PATH_MAX];
static volatile uint32_t s_dur_hint_ms;
static uint32_t          s_probe_bps;           /**< media_meta 探到的平均码率 */

uint32_t audio_gen_current(void)
{
    return s_gen;
}

uint32_t audio_gen_next(void)
{
    return ++s_gen;
}

void audio_core_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void audio_core_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ===========================================================================
 * espaudiocore 回调（**解码任务上下文，禁止阻塞、禁止 LVGL**）
 * =========================================================================*/

static void on_event(void *user, espaudiocore_event_t ev, const char *msg)
{
    (void)user;
    /* 迟到的回调（stop 已经清了标志）直接丢掉 —— 见 audio_internal.h 的会话代数说明 */
    if (!s_session_active) {
        return;
    }

    switch (ev) {
    case ESPAUDIOCORE_EVT_STARTED:
        s_running = true;
        if (audio_output_route() == AUDIO_OUT_SPEAKER) {
            audio_output_pa(true);
        }
        ESP_LOGI(TAG, "started (gen %u)", (unsigned)s_gen);
        audio_notify(AUDIO_EVT_STARTED);
        break;

    case ESPAUDIOCORE_EVT_PAUSED:
        s_running = false;
        audio_notify(AUDIO_EVT_PAUSED);
        break;

    case ESPAUDIOCORE_EVT_RESUMED:
        s_running = true;
        audio_notify(AUDIO_EVT_RESUMED);
        break;

    case ESPAUDIOCORE_EVT_SEEKED:
        ESP_LOGD(TAG, "seeked");
        break;

    case ESPAUDIOCORE_EVT_EOS:
        /*
         * 数据读到末尾了，但 ringbuf 里还有最多 ~340 ms 没播完。
         * 这里**不**立刻报 FINISHED：交给输出任务把积压播完再报
         * （否则 app 会在还有声音的时候就开始下一首）。
         */
        ESP_LOGI(TAG, "end of stream (waiting for output drain)");
        audio_output_note_eos(s_gen);
        break;

    case ESPAUDIOCORE_EVT_STOPPED:
        /* EOS / ERROR 之后必然补发一次；主动 stop 时也会来。两种都不需要上报 */
        ESP_LOGD(TAG, "stopped (%s)", msg ? msg : "");
        break;

    case ESPAUDIOCORE_EVT_ERROR:
        s_running = false;
        ESP_LOGE(TAG, "player error: %s", msg ? msg : "unknown");
        audio_output_pa(false);
        audio_notify(AUDIO_EVT_ERROR);
        break;

    default:
        break;
    }
}

/* 元数据：只需要 "tlen"（解码中估算出的总时长）。封面/歌词走 media_meta，
 * 这里**不**拷贝 APIC —— meta->data 只在回调期间有效，拷贝几百 KB 会挤爆解码任务栈。 */
static void on_meta(void *user, const espaudiocore_meta_t *m)
{
    (void)user;
    if (!m || m->is_binary || !m->type || !m->data) {
        return;
    }
    if (strcmp(m->type, "tlen") == 0 && s_dur_hint_ms == 0) {
        long v = strtol(m->data, NULL, 10);
        if (v > 0) {
            s_info.duration_ms = (uint32_t)v;
            s_info_valid = true;
        }
    }
}

/* ===========================================================================
 * 起播
 * =========================================================================*/

static espaudiocore_cfg_t make_cfg(void)
{
    espaudiocore_cfg_t cfg = {};
    /* LVGL 在 core 1（见 main.cc 的 esp_lv_adapter_init），解码钉到 core 0，
     * 优先级取 6：高于 LVGL(4)、低于蓝牙协议栈(BTC≈19)与 Wi-Fi 任务 */
    cfg.task_core     = 0;
    cfg.task_priority = 6;
    cfg.task_stack    = 0;                 /* 用解码器推荐值（Opus 16384，其余 8192） */
    cfg.io_timeout_ms = 0;                 /* 无限等待：阻塞点都能被 stop 唤醒 */
    /*
     * 统一输出 16-bit：
     *   - S31 的**硬件 ASRC 只吃 16-bit**（`esp_asrc/hw/caps/esp32s31/asrc_caps_check.c`
     *     里 IS_VALID_BITS 只有 16），统一到 16-bit 才有机会走硬件重采样；
     *   - 24-bit 源在这里被右移截断（无抖动），链路下游（ES8389/蓝牙 SBC）本来
     *     也只跑 16-bit，少一层软件位宽转换。
     */
    cfg.output_bits = AUDIO_OUT_BITS;
    cfg.volume_db   = 0.0f;                /* 音量在 audio_output 层做，不叠加两套 */
    cfg.on_event    = on_event;
    cfg.on_meta     = on_meta;
    return cfg;
}

static void reset_info(const char *path)
{
    memset(&s_info, 0, sizeof(s_info));
    s_info_valid = false;
    s_probe_bps = 0;
    s_dur_hint_ms = 0;
    s_cur_path[0] = '\0';
    if (path) {
        strlcpy(s_cur_path, path, sizeof(s_cur_path));
    }
}

/* 停止会话的**无锁**实现（调用方必须已经持有会话锁，或明确不需要互斥） */
static esp_err_t engine_stop_locked(void)
{
    /* 先落标志：stop() 期间 espaudiocore 还会派发 STOPPED，见 on_event 的门禁 */
    s_session_active = false;
    s_running = false;
    s_is_stream = false;

    esp_err_t err = espaudiocore_stop();     /* 阻塞至解码任务退出 */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop: %s", esp_err_to_name(err));
    }
    audio_output_session_end();              /* 停消费 + 抽干 PCM */
    return ESP_OK;
}

esp_err_t audio_engine_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t audio_engine_start_file(const char *path)
{
    if (!path || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    engine_stop_locked();

    reset_info(path);

    /*
     * 曲目信息：自己探测（GMF 时代由 MUSIC_INFO 事件喂，现在没有那个事件了）。
     * media_meta_probe_audio() 按魔数分派 MP3/FLAC/WAV/ADTS，只读几十字节 + 少量 seek，
     * 同步执行（几十 ms）—— 与 GMF 时代在事件回调里同步读 SD 相比只快不慢。
     */
    audio_probe_t p = {};
    if (media_meta_probe_audio(path, &p) == ESP_OK) {
        s_info.bitrate     = p.bitrate;
        s_info.duration_ms = p.duration_ms;
        s_info.sample_rate = p.sample_rate;
        s_info.channels    = p.channels;
        s_probe_bps        = p.bitrate;
        s_info_valid       = (p.sample_rate != 0 || p.duration_ms != 0 || p.bitrate != 0);
        ESP_LOGI(TAG, "probe: %u bps, %u ms, %u Hz, %uch",
                 (unsigned)p.bitrate, (unsigned)p.duration_ms,
                 (unsigned)p.sample_rate, p.channels);
    } else {
        ESP_LOGD(TAG, "probe unsupported for %s (时长/码率交给解码器或上层)", path);
    }

    const uint32_t gen = audio_gen_next();
    s_session_active = true;
    s_running        = false;
    s_is_stream      = false;

    /* 先让输出任务开始消费，再起解码 —— 否则解码器第一块 PCM 会撞上没人取的 ringbuf */
    audio_output_session_begin(gen);

    espaudiocore_cfg_t cfg = make_cfg();
    int dec_err = 0;
    esp_err_t err = espaudiocore_begin_rb(path, audio_output_pcm_ring(), &cfg, &dec_err);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin_rb('%s') failed: %s (dec_err=%d)",
                 path, esp_err_to_name(err), dec_err);
        s_session_active = false;
        audio_output_session_end();
        return err;
    }

    ESP_LOGI(TAG, "file session started (gen %u): %s", (unsigned)gen, path);
    return ESP_OK;
}

esp_err_t audio_engine_start_stream(int fmt)
{
    engine_stop_locked();

    reset_info(audio_http_url());

    const uint32_t gen = audio_gen_next();
    s_session_active = true;
    s_running        = false;
    s_is_stream      = true;

    audio_output_session_begin(gen);

    espaudiocore_cfg_t cfg = make_cfg();
    int dec_err = 0;
    esp_err_t err = espaudiocore_begin_stream_rb(audio_http_ring(),
                                                (espaudiocore_format_t)fmt,
                                                audio_output_pcm_ring(), &cfg, &dec_err);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin_stream_rb(fmt=%d) failed: %s (dec_err=%d)",
                 fmt, esp_err_to_name(err), dec_err);
        s_session_active = false;
        audio_output_session_end();
        return err;
    }

    ESP_LOGI(TAG, "stream session started (gen %u, fmt %d)", (unsigned)gen, fmt);
    return ESP_OK;
}

esp_err_t audio_engine_stop(void)
{
    return engine_stop_locked();
}

esp_err_t audio_engine_pause(void)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return espaudiocore_pause();
}

esp_err_t audio_engine_resume(void)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return espaudiocore_resume();
}

esp_err_t audio_engine_seek_ms(int64_t ms)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_is_stream) {
        return ESP_ERR_NOT_SUPPORTED;   /* 流式会话的定位由 audio_seek.cc 重开会话实现 */
    }
    return espaudiocore_seek_ms(ms);
}

bool audio_engine_running(void)
{
    return s_running;
}

bool audio_engine_flow_gate(void)
{
    return s_session_active && s_running;
}

bool audio_engine_session_active(void)
{
    return s_session_active;
}

bool audio_engine_is_stream(void)
{
    return s_is_stream && s_session_active;
}

bool audio_engine_info_get(audio_info_t *out)
{
    if (!out || !s_info_valid) {
        return false;
    }
    *out = s_info;
    return true;
}

const char *audio_engine_cur_path(void)
{
    return s_cur_path;
}

void audio_engine_set_duration_hint(uint32_t ms)
{
    s_dur_hint_ms = ms;
    if (ms && s_info.duration_ms == 0) {
        s_info.duration_ms = ms;
        s_info_valid = true;
    }
}

uint32_t audio_engine_duration_hint(void)
{
    return s_dur_hint_ms;
}

uint32_t audio_engine_bitrate(void)
{
    if (s_probe_bps) {
        return s_probe_bps;             /* 本地：media_meta 探测的平均码率 */
    }
    /* 在线：总字节 ÷ 时长（时长由上层告知，见 audio_service_set_duration_ms） */
    const uint64_t total = audio_http_total();
    const uint32_t ms = s_dur_hint_ms ? s_dur_hint_ms : s_info.duration_ms;
    if (total && ms) {
        return (uint32_t)((total * 8000ull) / ms);
    }
    return 0;
}

void audio_engine_output_drained(uint32_t gen)
{
    if (gen != s_gen || !s_session_active) {
        return;                          /* 迟到的输出任务回调 */
    }
    s_session_active = false;            /* 会话已自然结束，后续事件不再上报 */
    s_running = false;
    audio_output_pa(false);
    ESP_LOGI(TAG, "playback finished (gen %u)", (unsigned)gen);
    audio_notify(AUDIO_EVT_FINISHED);
    /* 不在这里 stop 会话：让上层（app_music）决定是下一首还是停下 */
}
