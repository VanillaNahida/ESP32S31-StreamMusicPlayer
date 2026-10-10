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
#include <sys/stat.h>      /* stat()：给"容器探测没覆盖"的本地文件补平均码率 */

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
static bool              s_stream_dur_fixed;    /**< 在线总时长本会话已定稿（别再重算，防抖） */
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
    s_stream_dur_fixed = false;      /* 新会话：在线总时长重新定稿一次 */
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

    /* 逐段日志：真机两次 tlsf 双重释放都发生在这一段里，日志能把范围缩到某一次 free */
    ESP_LOGI(TAG, "session stop: espaudiocore_stop() ...");
    esp_err_t err = espaudiocore_stop();     /* 阻塞至解码任务退出 */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "session stop: espaudiocore_stop() done (%s)", esp_err_to_name(err));
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

/*
 * pause / resume 也要持会话锁：它们同样是"先判 s_ctx 再写 s_ctx"的两步操作
 * （库里 `audio_player_pause()` 是 `if (!s_ctx) return …; s_ctx->pause_req = true;`），
 * 而 `engine_stop_locked()` 会在持锁状态下把那个对象释放掉 ——
 * 不持锁就可能在两步之间被换曲/停止拆掉，踩到已释放内存（2026-10 同类崩溃的第二种形态）。
 *
 * 锁内不会回头进本模块：on_event 只置标志 + 开关功放，不取锁。
 */
esp_err_t audio_engine_pause(void)
{
    audio_core_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_session_active) {
        err = espaudiocore_pause();
    }
    audio_core_unlock();
    return err;
}

esp_err_t audio_engine_resume(void)
{
    audio_core_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_session_active) {
        err = espaudiocore_resume();
    }
    audio_core_unlock();
    return err;
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
    if (!out) {
        return false;
    }
    /*
     * 顺手做一次"在线时长纠偏"（幂等、只在真的需要改时才动 + 只打一行日志）。
     *
     * 为什么要挂在这里：上层（app 的 tick）就是靠轮询 `get_info()` 拿总时长的，
     * 而纠偏必须在起播后几秒内自己生效 —— 不能等用户拖动才第一次调用
     * `audio_engine_bitrate()`。挂在 get_info() 上就等于"谁问时长，谁顺便纠正"。
     * ⚠ **只对在线流**：本地文件的时长由 media_meta 探测，别去碰（见 audio_engine_bitrate）。
     */
    if (s_is_stream && !s_probe_bps) {
        (void)audio_engine_bitrate();
    }
    if (!s_info_valid) {
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

/*
 * 在线流"平均码率"的合理区间（用于时间↔字节换算）。
 *
 * 为什么需要这道检查（2026-10 用户报"在线 seek 支持还不是很好"）：
 * 在线码率是 `总字节 × 8000 ÷ 时长` 算出来的，而**时长来自在线元数据**。
 * 网易云对 VIP 歌下发的是 **30 秒试听片段**（实测 Content-Length 只有 439 KB ≈
 * 128 kbps 的 27 秒），而元数据里的时长是**整首**（~200 s）→ 算出来 17 kbps。
 * 拿这个码率做映射，拖到 50% 会落到片段中间某个位置（听起来就是"拖了但落点不对"），
 * 而且进度条按 200 s 慢慢爬、永远走不到头，还会被 app 的"提前结束"判据误判成
 * 流被掐断 → 反复重刷重连。
 *
 * 落在区间外时改用**解码器报的码率**（它看的是真实帧，跟元数据无关）反推这段流的
 * 真实时长：`duration = 总字节 × 8 ÷ 码率`。音乐流不会低于 24 kbps，也不高于 512 kbps。
 */
#define AUDIO_STREAM_BPS_MIN  24000u
#define AUDIO_STREAM_BPS_MAX  512000u

/*
 * 实测码率 = **解码器已吃掉的字节** ÷ 已播时长。
 *
 * ★ 必须是"已吃掉"而不是"已下到"（2026-10 修："在线总时长不对"的根因）：
 *   下载前沿跑在播放前面（4 MB 缓冲经常整首装下），拿 `downloaded` 除以已播时长，
 *   得到的是个**越来越小**的假码率 —— 乘回总字节算时长，就会"一边播一边往上爬"。
 *   已吃掉的字节 = 已下到的 − 环形缓冲里还没被取走的（audio_http_ring_filled）。
 *
 * 其余两条门禁：
 *   - 起播头 5 秒不算（那会儿在抢读预读，缓冲区还没进入稳态）；
 *   - 只对**在线流**生效（本地文件有自己的探测码率，见 audio_engine_bitrate）。
 */
static uint32_t measured_stream_bps(void)
{
    if (!s_is_stream) {
        return 0;
    }
    const uint32_t pos = audio_output_position_ms();
    if (pos < 5000) {
        return 0;
    }
    const uint64_t got = audio_http_downloaded();
    const uint64_t held = audio_http_ring_filled();
    if (got <= held) {
        return 0;
    }
    const uint64_t consumed = got - held;
    const uint32_t bps = (uint32_t)((consumed * 8000ull) / pos);
    return (bps >= AUDIO_STREAM_BPS_MIN && bps <= AUDIO_STREAM_BPS_MAX) ? bps : 0;
}

uint32_t audio_engine_bitrate(void)
{
    /*
     * 本地文件：只用 media_meta 探测出来的平均码率。
     * ⚠ 本地**绝不能**走下面那段在线纠偏逻辑（2026-10 修）：探测失败的文件
     *   （M4A / OGG 这类还没做探测的容器）`s_probe_bps` 是 0，而 HTTP 层里可能
     *   还留着**上一次在线播放**的 total/downloaded —— 拿它去算"实测码率"会把
     *   时长改成一个垃圾值（真机现象：本地歌的总时长乱跳/变成别的数）。
     */
    if (!s_is_stream) {
        return s_probe_bps;
    }
    if (s_probe_bps) {
        return s_probe_bps;
    }
    /* 在线：总字节 ÷ 时长（时长由上层告知，见 audio_service_set_duration_ms） */
    const uint64_t total = audio_http_total();
    const uint32_t ms = s_dur_hint_ms ? s_dur_hint_ms : s_info.duration_ms;
    const uint32_t by_size = (total && ms) ? (uint32_t)((total * 8000ull) / ms) : 0;

    /*
     * 纠偏：两者不一致时以**实测**为准，并把时长提示改成"这段流真正的长度"
     * （`duration = 总字节 × 8 ÷ 实测码率`）—— 进度条、seek 映射、"提前结束"判据
     * 三处都跟着一起对。
     *
     * ⚠ **每个会话只纠一次**（s_stream_dur_fixed）：VBR 流的实测值本来就会小幅浮动，
     *   每 tick 重算会让界面上的总时长跟着抖。
     */
    const uint32_t meas = measured_stream_bps();
    if (meas && !s_stream_dur_fixed) {
        const bool size_ok = (by_size >= AUDIO_STREAM_BPS_MIN && by_size <= AUDIO_STREAM_BPS_MAX);
        const bool agree = size_ok && meas <= by_size + by_size / 4 && meas + meas / 4 >= by_size;
        if (!agree) {
            const uint32_t eff_ms = total ? (uint32_t)((total * 8000ull) / meas) : 0;
            if (eff_ms) {
                ESP_LOGW(TAG, "stream duration: %u ms -> %u ms (by-size %u bps 不可信，"
                              "实测 %u bps，已按实测定稿)",
                         (unsigned)ms, (unsigned)eff_ms, (unsigned)by_size, (unsigned)meas);
                s_dur_hint_ms       = eff_ms;
                s_info.duration_ms  = eff_ms;  /* get_info() 也报纠正值：进度条跟着对 */
                s_info_valid        = true;
                s_stream_dur_fixed  = true;
            }
        } else {
            s_stream_dur_fixed = true;     /* 两者一致：不用改，也别再算了 */
        }
    }
    return by_size ? by_size : meas;
}

/*
 * 本地文件：总时长未知时向**解码库**要一次（说明见 audio_internal.h）。
 *
 * 覆盖的是 `media_meta` 没做探测的容器（M4A / OGG / Opus）—— 那些文件以前总时长
 * 恒为 0：界面 `--:--`、进度条不动、拖不动，看起来像"播放器没识别出这首歌"。
 * 由输出任务在会话锁内每块调用一次；一旦拿到就再也不问（`s_info.duration_ms != 0`）。
 */
void audio_engine_poll_duration(void)
{
    if (s_is_stream || !s_session_active || s_info.duration_ms != 0) {
        return;
    }
    const int64_t d = espaudiocore_get_duration_ms();
    if (d <= 0) {
        return;
    }
    s_info.duration_ms = (uint32_t)d;
    s_info_valid       = true;

    /*
     * 顺手把**平均码率**补上：`media_meta` 没探测这些容器（M4A/OGG/Opus），
     * 码率同样是 0 —— 而 `audio_seek_supported()` 要求码率非 0，否则本地也拖不动。
     * 平均码率 = 文件字节 × 8 ÷ 时长（VBR 也成立，正是 seek 的"时间→字节"要用的值）。
     */
    if (s_probe_bps == 0 && s_cur_path[0]) {
        struct stat st = {};
        if (stat(s_cur_path, &st) == 0 && st.st_size > 0) {
            const uint64_t bps = ((uint64_t)st.st_size * 8000ull) / (uint64_t)d;
            if (bps >= 8000 && bps <= 4000000) {
                s_probe_bps = (uint32_t)bps;
            }
        }
    }
    ESP_LOGI(TAG, "duration from decoder: %u ms, bitrate %u bps (容器探测没覆盖这个格式)",
             (unsigned)d, (unsigned)s_probe_bps);
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
