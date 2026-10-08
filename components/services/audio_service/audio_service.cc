/*
 * audio_service.cc —— 播放服务实现（ESP-GMF / esp_audio_simple_player）
 *
 * 管线：file reader → decoder → (resample 48k / ch / bit) → out 回调
 *       → esp_codec_dev_write() → I2S/TDM → ES8389 → NS4150B ×2 → 左右扬声器
 *
 * 关键事实（见 AGENTS.md）：
 *   - 本板 I2S 固定 48 kHz / 16 bit / 2ch TDM，所以把 GMF 重采样目标设成 48k
 *   - GMF file IO 的 URI 解析会剥掉 "scheme://"，所以必须写成 "file:///sdcard/..."
 *   - 播放与录音共用同一组 I2S/TDM，播放时不能录音
 */
#include "audio_service.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"   /* get_pipeline / register_io */
#include "esp_gmf_io_http.h"                   /* 自己注册 http IO：放大流缓冲 */
#include "esp_http_client.h"                   /* 事件钩子里读 Content-Length */
#include "esp_crt_bundle.h"                    /* 音频流也做证书校验 */
#include "esp_codec_dev.h"
#include "bsp.h"
#include "media_meta.h"
#include "bt_service.h"
#include "play_state.h"    /* 音量记忆（NVS） */

static const char *TAG = "audio_svc";

/* 板级固定输出格式 */
#define OUT_RATE_HZ   (48000)
#define OUT_CHANNELS  (2)
#define OUT_BITS      (16)

static esp_asp_handle_t  s_player;
static SemaphoreHandle_t s_lock;
static audio_event_cb_t  s_evt_cb;
static void             *s_evt_user;
static volatile bool     s_running;
/* 当前音量：开机时取 NVS 里的记忆值（没有记录才用默认 55） */
static int               s_volume = PLAY_STATE_VOL_DEFAULT;
static audio_info_t      s_info;
static volatile bool     s_info_valid;
static size_t            s_data_offset;   /* 音频数据起始偏移（MP3 = ID3v2 标签长度） */
static audio_out_t       s_out = AUDIO_OUT_SPEAKER;   /* 当前输出通道（out 回调用它分流） */
/* 当前播放的 VFS 路径：MUSIC_INFO 里若带不出码率，用它回文件里探测帧头 */
#define AUDIO_PATH_MAX   (600)
static char              s_cur_path[AUDIO_PATH_MAX];
/* 本首是否已经真的出声（PCM 写到过 codec/蓝牙）。out_write_cb 里置位、
 * 每次换曲清零；UI 用它把"缓冲中 xx%"换回歌词。 */
static volatile bool     s_out_flowing;
/* 在线流式的缓冲进度统计（见 stream_evt_hook / audio_service_stream_progress）：
 *   s_stream_total/s_stream_got —— 事件钩子从 Content-Length 与收到的数据里数出来的；
 *   s_stream_io                —— 我们注册进 ASP 池子的那个 http IO，进度优先问它
 *                                 （esp_gmf_io_get_pos 是"真读到多少字节"，更准）。 */
static volatile uint32_t s_stream_total;
static volatile uint32_t s_stream_got;
static esp_gmf_io_handle_t s_stream_io;

/* ---------------------------------------------------------------- out 回调 */

/*
 * GMF 把解码（并重采样）后的 PCM 交给我们。按当前输出通道分两条路：
 *   扬声器 → esp_codec_dev_write（ES8389）
 *   蓝牙   → bt_service_write_pcm（A2DP 源，非阻塞，满了丢帧）
 * 蓝牙这条路**必须**返回 data_size：返回短了 GMF 会当成写失败/结束，
 * 宁可丢掉一帧也不能让解码管线停。
 */
static int out_write_cb(uint8_t *data, int data_size, void *ctx)
{
    /* 有 PCM 真的送到 codec 了 —— 这就是"缓冲结束、开始出声"的信号，
     * UI 用它把"缓冲中 xx%"换回歌词（见 audio_service_output_active） */
    s_out_flowing = true;
    if (s_out == AUDIO_OUT_BLUETOOTH) {
        (void)bt_service_write_pcm(data, (size_t)data_size);
        return data_size;
    }
    esp_codec_dev_handle_t dev = (esp_codec_dev_handle_t)ctx;
    if (!dev) {
        return -1;
    }
    return esp_codec_dev_write(dev, data, data_size);
}

/* ---------------------------------------------------------------- 事件 */

static void notify(audio_evt_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt, s_evt_user);
    }
}

static int player_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (!pkt || !pkt->payload) {
        return 0;
    }

    if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
        esp_asp_music_info_t info = {0};
        if (pkt->payload_size >= (int)sizeof(info)) {
            memcpy(&info, pkt->payload, sizeof(info));
            s_info.sample_rate = info.sample_rate;
            s_info.bitrate     = info.bitrate;
            s_info.channels    = (uint8_t)info.channels;
            s_info.bits        = (uint8_t)info.bits;
            s_info.duration_ms = 0;
            /*
             * GMF 对 MP3 经常只填采样率，bitrate 报 0 —— 那样"文件大小 / 码率"
             * 估不出总时长，进度条永远是 0、拖动定位也没有换算系数。
             * 这里回文件里读帧头补上（同一次事件里做，之后不再重复）。
             */
            if (s_info.bitrate <= 0 && s_cur_path[0]) {
                audio_probe_t probe = {};
                if (media_meta_probe_mp3(s_cur_path, &probe) == ESP_OK) {
                    s_info.bitrate     = (int)probe.bitrate;
                    s_info.duration_ms = probe.duration_ms;
                }
            }
            s_info_valid       = true;
            ESP_LOGI(TAG, "music info: %d Hz %uch %ubit %d bps, %u ms",
                     info.sample_rate, info.channels, info.bits, s_info.bitrate,
                     (unsigned)s_info.duration_ms);
        }
        return 0;
    }

    if (pkt->type != ESP_ASP_EVENT_TYPE_STATE) {
        return 0;
    }

    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    memcpy(&st, pkt->payload,
           pkt->payload_size < (int)sizeof(st) ? pkt->payload_size : (int)sizeof(st));

    switch (st) {
    case ESP_ASP_STATE_RUNNING:
        s_running = true;
        bsp_audio_pa_enable(true);      /* 有声音了才开功放，避免静音期底噪 */
        notify(AUDIO_EVT_STARTED);
        break;
    case ESP_ASP_STATE_PAUSED:
        notify(AUDIO_EVT_PAUSED);
        break;
    case ESP_ASP_STATE_FINISHED:
        s_running = false;
        bsp_audio_pa_enable(false);
        notify(AUDIO_EVT_FINISHED);
        break;
    case ESP_ASP_STATE_STOPPED:
        s_running = false;
        bsp_audio_pa_enable(false);
        break;
    case ESP_ASP_STATE_ERROR:
        s_running = false;
        bsp_audio_pa_enable(false);
        ESP_LOGE(TAG, "player error state");
        notify(AUDIO_EVT_ERROR);
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------- 生命周期 */

esp_err_t audio_service_init(void)
{
    if (s_player) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* 音量记忆：必须在开 codec 之前就取出来，否则开机先是默认音量、
     * 再"跳"一下变到记忆值（听感上就是开机爆一下） */
    s_volume = play_state_volume_get(PLAY_STATE_VOL_DEFAULT);

    esp_err_t ret = bsp_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 播放前先把 codec 以板级格式打开（GMF 重采样目标与之一致） */
    ret = bsp_audio_open_output(OUT_RATE_HZ, OUT_CHANNELS, OUT_BITS);
    if (ret != ESP_OK) {
        return ret;
    }
    bsp_audio_set_volume(s_volume);
    bsp_audio_set_mute(true);          /* 未播放先静音，防爆音 */
    bsp_audio_pa_enable(false);

    /* 注意：C++ 不支持嵌套的指定初始化列表（.in.cb = ...），必须逐字段赋值 */
    esp_asp_cfg_t cfg = {};
    cfg.in.cb        = NULL;
    cfg.in.user_ctx  = NULL;
    cfg.out.cb       = out_write_cb;
    cfg.out.user_ctx = bsp_audio_codec();
    cfg.task_prio    = 5;
    cfg.task_stack   = 6 * 1024;
    /*
     * 双核分工：播放管线（读文件 → 解码 → 重采样 → 写 codec）放 **core 0**，
     * LVGL 任务在 main.cc 里固定 core 1。两者各占一核，播放时 UI 不会被
     * 解码任务抢占。
     *
     * 注意这里**不能**写 1：esp_gmf_task.c 里是
     *     if (cfg->thread.core) { 用它 } else { 用 DEFAULT_ESP_GMF_TASK_CORE }
     * 而 DEFAULT_ESP_GMF_TASK_CORE 正好是 0 —— 传 0 会走 else 分支落到 core 0，
     * 传 1 则显式钉在 core 1（和 LVGL 抢核）。
     */
    cfg.task_core    = 0;

    esp_gmf_err_t err = esp_audio_simple_player_new(&cfg, &s_player);
    if (err != ESP_GMF_ERR_OK || !s_player) {
        ESP_LOGE(TAG, "player new failed: %d", err);
        s_player = NULL;
        return ESP_FAIL;
    }

    err = esp_audio_simple_player_set_event(s_player, player_event_cb, NULL);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "set event failed: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "audio service ready (%d Hz / %uch / %ubit)",
             OUT_RATE_HZ, OUT_CHANNELS, OUT_BITS);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 播放控制 */

/*
 * 内部的统一入口：uri 是给 GMF 的（file:///... 或 http(s)://...），
 * local_path 只在本地文件时非空（用来探测容器头/记进度路径）。
 */
static esp_err_t play_uri_locked(const char *uri, const char *local_path)
{
    /* 先停掉上一首，避免复用旧管线（会崩） */
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    if (st == ESP_ASP_STATE_RUNNING || st == ESP_ASP_STATE_PAUSED) {
        esp_audio_simple_player_stop(s_player);
    }

    /* 记下当前"路径"：本地是文件路径；在线流式记 URI（MUSIC_INFO 兜底探测用） */
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", local_path ? local_path : uri);

    /* 换曲：流缓冲的进度计数清零（钩子会重新填 total/got），出声标志也清掉 */
    s_stream_total = 0;
    s_stream_got   = 0;
    s_out_flowing  = false;

    /* 播放前解除静音并开功放由事件回调负责 */
    s_info_valid = false;   /* 旧曲目的流信息作废 */
    /* 容器头部长度：MP3 要跳过 ID3v2 标签，拖动定位的字节换算才不会整体偏后。
     * 在线流式没有本地文件，只能给 0（拖动定位对流的支持本来就有限）。 */
    s_data_offset = local_path ? media_meta_audio_data_offset(local_path) : 0;
    bsp_audio_set_mute(false);
    bsp_audio_set_volume(s_volume);

    esp_gmf_err_t err = esp_audio_simple_player_run(s_player, uri, NULL);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "run '%s' failed: %d", uri, err);
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        notify(AUDIO_EVT_ERROR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "playing %s", uri);
    return ESP_OK;
}

esp_err_t audio_service_play_path(const char *vfs_abs_path)
{
    if (!s_player || !vfs_abs_path || vfs_abs_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    /* GMF 会剥掉 "scheme://"，这里补成 "file:///sdcard/..." 才是绝对路径。
     * 缓冲必须放得下整条 UTF-8 长路径：截断出来的"短路径"会让 GMF 打不开文件 */
    char uri[600];
    int n = snprintf(uri, sizeof(uri), "file://%s", vfs_abs_path);
    if (n <= 0 || n >= (int)sizeof(uri)) {
        ESP_LOGE(TAG, "path too long for URI (%d bytes)", n);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t ret = play_uri_locked(uri, vfs_abs_path);
    xSemaphoreGive(s_lock);
    return ret;
}

/*
 * 在线流式播放：把 http(s):// 直接交给简单播放器（工程里
 * CONFIG_ESP_AUDIO_SIMPLE_PLAYER_HTTP_EN=y，池子里注册了 GMF 的 HTTP reader IO，
 * 它按 URI 的 scheme 选 IO）。
 *
 * 与 audio_service_play_path() 的区别只有两点：
 *   - 没有本地文件，所以容器头偏移给 0；
 *   - s_cur_path 记的是 URI（进度记忆/日志用；在线曲目的"续播"由上层按歌曲 ID 处理）。
 * 传进来是绝对路径时自动走本地那条路，调用方不用判断。
 */
/*
 * 流式播放的缓冲：GMF 的 HTTP IO 默认只有 20 KB 环形缓冲（头文件里写死的宏，
 * 没有 Kconfig 可调），Wi-Fi 一抖就容易 `Run timeout`。这里注册一个自己的
 * http IO 覆盖池里那个默认的：
 *   - 环形缓冲 **4 MB**：GMF 的分配器在 CONFIG_SPIRAM_BOOT_INIT=y 时优先用
 *     PSRAM（esp_gmf_oal_mem.c 里就是 heap_caps_malloc(MALLOC_CAP_SPIRAM)），
 *     所以这块缓冲落在 PSRAM，不占内部 RAM。4 MB ≈ 320 kbps 的 100 秒音频 ——
 *     相当于"一次缓冲一大段"，网络抖动时靠这段顶着，不再几秒一卡。
 *   - 每次读 8 KB（默认 3 KB），减少 TLS 记录边界上的往返；
 *   - 挂上事件钩子：**统计已收字节 / 总字节**，给 UI 显示"缓冲中 xx%"；
 *   - 顺手挂上证书包：音频流以前是不校验的。
 * 只在第一次流式播放前做一次；失败就退回 ASP 默认的 IO（不影响播放）。
 */
#define STREAM_RING_BYTES   (4 * 1024 * 1024)

/*
 * HTTP IO 的事件钩子。返回值语义（见 esp_gmf_io_http.h）：
 *   0 = 不干预，继续正常收发；-1 = 让 HTTP 客户端停下。
 * 我们只**旁观**，永远返回 0。
 *
 *   HTTP_STREAM_FINISH_REQUEST：响应头已到手，此时能从 http_client 拿到
 *                               Content-Length（分块传输时是 -1，那就显示不了百分比）；
 *   HTTP_STREAM_ON_RESPONSE   ：收到一段数据（buffer_len 是本段字节数）。
 */
static int stream_evt_hook(http_stream_event_msg_t *msg)
{
    if (!msg) {
        return 0;
    }
    switch (msg->event_id) {
    case HTTP_STREAM_FINISH_REQUEST: {
        const int64_t len = esp_http_client_get_content_length(
                                (esp_http_client_handle_t)msg->http_client);
        s_stream_total = (len > 0) ? (uint32_t)len : 0;
        ESP_LOGI(TAG, "stream size %" PRId32 " bytes", (int32_t)len);
        break;
    }
    case HTTP_STREAM_ON_RESPONSE:
        if (msg->buffer_len > 0) {
            s_stream_got += (uint32_t)msg->buffer_len;
        }
        break;
    default:
        break;
    }
    return 0;
}

static void ensure_stream_io(void)
{
    static bool done;
    if (done || !s_player) {
        return;
    }
    done = true;

    http_io_cfg_t cfg = HTTP_STREAM_CFG_DEFAULT();
    cfg.dir               = ESP_GMF_IO_DIR_READER;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handle      = stream_evt_hook;
    cfg.io_cfg.buffer_cfg.io_size     = 8 * 1024;
    cfg.io_cfg.buffer_cfg.buffer_size = STREAM_RING_BYTES;
    cfg.io_cfg.thread.stack_in_ext    = true;

    esp_gmf_io_handle_t io = NULL;
    if (esp_gmf_io_http_init(&cfg, &io) != ESP_GMF_ERR_OK || !io) {
        ESP_LOGW(TAG, "custom http io init failed, keep default");
        return;
    }
    if (esp_audio_simple_player_register_io(s_player, io) != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "register http io failed, keep default");
        return;
    }
    s_stream_io = io;                 /* 进度查询要用（见 audio_service_stream_progress） */
    ESP_LOGI(TAG, "stream io ready: ring %d KB (PSRAM), read %d KB",
             (int)(cfg.io_cfg.buffer_cfg.buffer_size / 1024),
             (int)(cfg.io_cfg.buffer_cfg.io_size / 1024));
}

bool audio_service_stream_progress(int *percent)
{
    if (!s_stream_io) {
        return false;
    }
    /* 总长度：钩子从 Content-Length 拿到的优先，拿不到就问 IO 自己 */
    uint32_t total = s_stream_total;
    if (total == 0) {
        uint64_t size = 0;
        if (esp_gmf_io_get_size(s_stream_io, &size) == ESP_GMF_ERR_OK && size > 0) {
            total = (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size;
        }
    }
    if (total == 0) {
        return false;                 /* 分块传输/未知长度：显示不了百分比 */
    }

    /*
     * 已收字节：**优先问 IO 的读位置** —— 它按"实际读到多少字节"推进，是准的。
     *
     * 为什么不用钩子里的 s_stream_got 当主：ON_RESPONSE 是在**读之前**派发的，
     * msg->buffer_len 是"这次想读多少"（我们的 io_size = 8 KB），不是真读到的字节数。
     * 网络一慢（一次 TLS 记录只回来 1~2 KB），按 8 KB 累加就会冲到 100% 而实际远没下完。
     * 所以那个计数只当兜底。
     */
    uint32_t got = 0;
    uint64_t pos = 0;
    if (esp_gmf_io_get_pos(s_stream_io, &pos) == ESP_GMF_ERR_OK && pos > 0) {
        got = (pos > total) ? total : (uint32_t)pos;
    } else {
        got = s_stream_got;
        if (got > total) {
            got = total;
        }
    }
    if (percent) {
        *percent = (int)((uint64_t)got * 100 / total);
    }
    return true;
}

bool audio_service_output_active(void)
{
    return s_out_flowing;
}

esp_err_t audio_service_play_uri(const char *uri)
{
    if (!s_player || !uri || !uri[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uri[0] != '/') {
        ensure_stream_io();          /* 流式播放：换成大缓冲（PSRAM）的 http IO */
    }
    if (uri[0] == '/') {
        return audio_service_play_path(uri);
    }
    if (strncmp(uri, "http://", 7) != 0 && strncmp(uri, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t ret = play_uri_locked(uri, NULL);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t audio_service_pause(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gmf_err_t err = esp_audio_simple_player_pause(s_player);
    return (err == ESP_GMF_ERR_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_service_resume(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gmf_err_t err = esp_audio_simple_player_resume(s_player);
    if (err == ESP_GMF_ERR_OK) {
        notify(AUDIO_EVT_RESUMED);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/*
 * 按时间定位（拖动进度条）。
 *
 * 限制（都是 GMF / 容器的硬约束，不是这里偷懒）：
 *   - esp_audio_simple_player 没有 seek 门面，只能拿到 pipeline 再调
 *     esp_gmf_pipeline_seek()，而它**只接受字节位置**，且要求 pipeline 处于
 *     PAUSED/STOPPED/FINISHED。所以这里"暂停 → 定位 → 恢复"。
 *   - 字节位置只能用 **平均码率** 估算（byte = offset + ms*bitrate/8000），
 *     等价于把文件当 CBR；VBR 文件会有几秒误差。MP3/AAC/TS 这类流式格式
 *     解码器能在帧同步字上自己重新对齐，M4A(MP4 容器) 不行。
 *   - 时长/码率未知（MUSIC_INFO 还没来）时直接返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t audio_service_seek_ms(uint32_t pos_ms)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_info_valid || s_info.bitrate <= 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    const bool was_running = (st == ESP_ASP_STATE_RUNNING);
    if (was_running) {
        if (audio_service_pause() != ESP_OK) {
            return ESP_FAIL;
        }
    }

    esp_gmf_pipeline_handle_t pipe = NULL;
    esp_gmf_err_t gerr = esp_audio_simple_player_get_pipeline(s_player, &pipe);
    if (gerr != ESP_GMF_ERR_OK || !pipe) {
        if (was_running) {
            audio_service_resume();
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint64_t byte = (uint64_t)s_data_offset +
                          ((uint64_t)pos_ms * (uint64_t)s_info.bitrate) / 8000ULL;
    gerr = esp_gmf_pipeline_seek(pipe, byte);
    if (was_running) {
        audio_service_resume();
    }
    if (gerr != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "seek %ums (byte %llu) failed: %d", (unsigned)pos_ms,
                 (unsigned long long)byte, (int)gerr);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "seek -> %ums (byte %llu)", (unsigned)pos_ms, (unsigned long long)byte);
    return ESP_OK;
}

esp_err_t audio_service_toggle(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    if (st == ESP_ASP_STATE_RUNNING) {
        return audio_service_pause();
    }
    if (st == ESP_ASP_STATE_PAUSED) {
        return audio_service_resume();
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t audio_service_stop(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gmf_err_t err = esp_audio_simple_player_stop(s_player);
    bsp_audio_set_mute(true);
    bsp_audio_pa_enable(false);
    s_running = false;
    return (err == ESP_GMF_ERR_OK) ? ESP_OK : ESP_FAIL;
}

bool audio_service_is_playing(void)
{
    if (!s_player) {
        return false;
    }
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    return st == ESP_ASP_STATE_RUNNING;
}

/* ---------------------------------------------------------------- 输出通道 */

esp_err_t audio_service_set_output(audio_out_t out)
{
    switch (out) {
    case AUDIO_OUT_NONE:
        s_out = out;
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        ESP_LOGI(TAG, "output -> none (静音 + 关功放)");
        return ESP_OK;
    case AUDIO_OUT_SPEAKER:
        /* 从蓝牙切回来：先停掉 A2DP 源流，再把本地功放接回来 */
        if (s_out == AUDIO_OUT_BLUETOOTH) {
            bt_service_media_stop();
        }
        s_out = out;
        bsp_audio_set_volume(s_volume);
        bsp_audio_set_mute(s_volume == 0);
        if (s_running) {
            bsp_audio_pa_enable(true);
        }
        ESP_LOGI(TAG, "output -> speaker (%d%%)", s_volume);
        return ESP_OK;
    case AUDIO_OUT_BLUETOOTH:
        if (!bt_service_is_connected()) {
            ESP_LOGW(TAG, "no bluetooth audio device connected");
            return ESP_ERR_INVALID_STATE;
        }
        /*
         * 开发指南 5.12：切到蓝牙要**先静音 + 拉低 PA_CTRL**，
         * 否则本地喇叭和耳机同时出声。
         */
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        if (!bt_service_is_streaming()) {
            bt_service_media_start();       /* 异步；流起来之前的 PCM 会被丢掉 */
        }
        s_out = out;
        ESP_LOGI(TAG, "output -> bluetooth (%s)", bt_service_peer_name());
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

audio_out_t audio_service_get_output(void)
{
    return s_out;
}

esp_err_t audio_service_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    const bool changed = (percent != s_volume);
    s_volume = percent;
    /* 主音量走 ES8389 数字音量寄存器 */
    esp_err_t ret = bsp_audio_set_volume(percent);
    /*
     * 音量归零自动静音；反过来，从 0 调大时必须自动解除静音，
     * 否则用 VOL+ 按键把音量从 0 加上去会"没声音"，看着像按键没生效。
     * （开机时的静音由 audio_service_init / play_path 自己管理，不走这里。）
     */
    if (percent == 0) {
        bsp_audio_set_mute(true);
    } else {
        bsp_audio_set_mute(false);
    }
    /*
     * 音量记忆（需求）：这里是**唯一**的音量写入点，所有改音量的路径
     * （播放器页音量条、硬件 VOL±、控制中心滑条）都会经过它。
     * 落盘由 play_state 合并防抖，不会因为长按 VOL+ 而狂写 flash。
     */
    if (changed) {
        play_state_volume_set(percent);
    }
    return ret;
}

int audio_service_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_service_set_mute(bool mute)
{
    return bsp_audio_set_mute(mute);
}

void audio_service_set_event_cb(audio_event_cb_t cb, void *user)
{
    s_evt_cb = cb;
    s_evt_user = user;
}

bool audio_service_get_info(audio_info_t *out)
{
    if (!out || !s_info_valid) {
        return false;
    }
    *out = s_info;
    return true;
}