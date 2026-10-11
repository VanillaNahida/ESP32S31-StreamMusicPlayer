/*
 * voice_service.cc —— 语音输入的"宿主"：把网络与麦克风接到 lvgl_pinyin_ime 的语音模块上
 *
 * 为什么需要这一层：`lvgl_pinyin_ime` 的语音部分**故意不碰网络、也不碰麦克风**
 * （见 include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h 开头那段），它只做讯飞协议，
 * 要求调用方提供 4 个回调：
 *
 *     ws_open / ws_send_text / ws_close   → 本文件用 esp_websocket_client 实现
 *     audio_read                          → 本文件用板级 audio_adc 器件（ES8389 ADC）实现
 *
 * 数据通路：
 *
 *   ┌──── 组件任务（lvgl_pinyin_ime 的语音任务）────┐
 *   │  每 40 ms 调 audio_read() 拿 1280 B 16k/16bit/1ch
 *   │  ──▶ 签名 + JSON 封帧 ──▶ ws_send_text() ──▶ WebSocket ──▶ 讯飞
 *   │  ◀── lv_pinyin_ime_voice_ws_message()（在本文件的 ws 事件回调里，网络任务）
 *   └──────────────────────────────────────────────┘
 *
 * 几个关键决定：
 *
 *  1. **麦克风走 audio_adc，不走播放那个 audio_dac**。板级 YAML 里它们是**两个独立器件**
 *     （各自一根 I2S：i2s_audio_out / i2s_audio_in，`adc_enabled: true/false`），
 *     所以麦克风可以**原生按 16 kHz 采**（讯飞要的就是这个），既不用软件重采样，
 *     也不影响播放的 48 kHz 通路。
 *
 *  2. **识别期间自动暂停播放、结束自动恢复**（用户要求）：否则扬声器的声音会被
 *     麦克风收进去。恢复只在"本来就在播"时才做（记了 s_resume_needed）。
 *
 *  3. **ws_open 必须等握手完成才返回**（组件紧接着就要发第一帧音频）：
 *     用信号量在 WEBSOCKET_EVENT_CONNECTED 里放行。
 *
 *  4. **JSON 先拷进本文件自己的缓冲再交给组件**：组件会就地解析（把 \uXXXX 解成
 *     UTF-8）并写 `buf[len] = '\0'`，直接拿 ws 库的指针会越界一个字节。
 *
 * 密钥：来自 Kconfig（menuconfig → Component config → lvgl_pinyin_ime → Voice input），
 * `lv_pinyin_ime_voice_cfg_default()` 已经把它们填进 cfg，这里只补 host。
 * 三个都空时会在日志里明确提示，方便定位"为什么点了没反应"。
 */

#include "voice_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_crt_bundle.h"     /* wss 的根证书包（不设就会报 No server verification option set）*/
#include "esp_board_manager.h"
#include "esp_board_manager_includes.h"   /* dev_audio_codec_handles_t */
#include "esp_codec_dev.h"
#include "esp_websocket_client.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

#include "audio_service.h"

static const char *TAG = "voice_svc";

/* 板级 YAML（board_devices.yaml）里的器件名：ES8389 ADC，独立 I2S i2s_audio_in */
#define VOICE_ADC_DEV_NAME   "audio_adc"
/*
 * ★★ 2026-10 实机定位：麦克风必须**按 I2S 总线的时钟采**，不能按讯飞要的 16 kHz 采。
 *
 * 板级 YAML 里 audio_adc / audio_dac 虽然是两个器件，但它们**共用 I2S0（TDM）这一条总线**
 * （日志：`I2S[0] TDM, RX, ws:4, bclk:3, dout:5, din:6` + `Negotiate duplex bus: port=0
 * active=RX mode=TDM peer=TX`）。按 16 kHz 开麦时 ES8389 直接拒绝：
 *     W ADEV_BUS: Negotiate duplex bus: TX and RX clocks do not match
 *     E ADEV_CODEC: Failed to set data interface format, ret=0x106   (ESP_ERR_NOT_SUPPORTED)
 *     E voice_svc: 麦克风打开失败（16000 Hz 1ch 16bit）
 *
 * 所以：**采 48 kHz（与播放一致）+ 软件降采样到 16 kHz**。代价是一点 CPU（每 40 ms
 * 处理 1920 帧，可忽略），换来的是"能和播放同时打开"。
 */
#define VOICE_BUS_RATE       48000   /* I2S 总线采样率，必须与播放一致 */
#define VOICE_BUS_CHANNELS   2       /* TDM 总线是 2ch，跟着走 */
#define VOICE_MIC_BITS       16

/* 讯飞要 16 kHz / 16 bit / 单声道；组件每 40 ms 要 1280 B（= 640 个 16bit 样本） */
#define VOICE_MIC_RATE       16000
#define VOICE_FRAME_BYTES    1280
#define VOICE_DECIM          (VOICE_BUS_RATE / VOICE_MIC_RATE)   /* 3 */
#define VOICE_FRAME_FRAMES   (VOICE_FRAME_BYTES / 2)             /* 640 个输出样本 */
/* 一次要读够 40 ms 的总线数据：640 × 3 帧 × 2ch × 2B = 7680 B */
static int16_t s_mic_raw[VOICE_FRAME_FRAMES * VOICE_DECIM * VOICE_BUS_CHANNELS];
/* 讯飞返回的 JSON 不大，但留足余量；比 buffer_size 小才不会分片 */
#define VOICE_WS_BUF         8192
#define VOICE_WS_TIMEOUT_MS  8000
#define VOICE_SEND_TIMEOUT_MS 2000

static esp_codec_dev_handle_t      s_mic;
static bool                        s_mic_open;
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t           s_ws_connected;
static lv_pinyin_ime_voice_t      *s_voice;      /* 当前键盘上挂着的会话 */
static lv_obj_t                   *s_ime;        /* 那把键盘（麦克风测试页的入口） */

/* 自动暂停/恢复 */
static bool s_paused;
static bool s_resume_needed;

/* 收 JSON 用的缓冲（组件会拷进自己的队列，所以复用一份就够） */
static char s_json[VOICE_WS_BUF];

/* ------------------------------------------------------------------ 麦克风 */

static esp_err_t mic_ensure_open(void)
{
    if (s_mic == NULL) {
        void *handle = NULL;
        /*
         * ★ 器件不会自己就绪：板级管理器只自动初始化 board_info 里登记的那些，
         *   audio_adc 必须**显式 init**，否则 get_device_handle 会报
         *     E BOARD_MANAGER: Device audio_adc not found, error: 801fe
         *     E BOARD_DEVICE: Device audio_adc handle is NULL
         *   （实机踩过）。重复 init 会返回"已初始化"类错误，忽略即可。
         */
        const esp_err_t ierr = esp_board_manager_init_device_by_name(VOICE_ADC_DEV_NAME);
        if (ierr != ESP_OK) {
            ESP_LOGI(TAG, "init %s: %s（已经初始化过就属正常）", VOICE_ADC_DEV_NAME,
                     esp_err_to_name(ierr));
        }
        const esp_err_t err = esp_board_manager_get_device_handle(VOICE_ADC_DEV_NAME, &handle);
        if (err != ESP_OK || handle == NULL) {
            ESP_LOGE(TAG, "板级器件 %s 取不到（%s）——语音输入不可用",
                     VOICE_ADC_DEV_NAME, esp_err_to_name(err));
            return ESP_FAIL;
        }
        s_mic = ((dev_audio_codec_handles_t *)handle)->codec_dev;
    }
    if (!s_mic_open) {
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = VOICE_MIC_BITS;
        fs.channel         = VOICE_BUS_CHANNELS;   /* 跟着总线（见上面那段说明）*/
        fs.sample_rate     = VOICE_BUS_RATE;       /* 48k，不能写 16k，否则时钟不匹配 */
        fs.mclk_multiple   = 256;                  /* 不填会有一条 warning，填上更干净 */
        const esp_err_t err = esp_codec_dev_open(s_mic, &fs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "麦克风打开失败（总线格式 %u Hz %uch %ubit）: %s", (unsigned)VOICE_BUS_RATE,
                     (unsigned)VOICE_BUS_CHANNELS, (unsigned)VOICE_MIC_BITS, esp_err_to_name(err));
            return err;
        }
        s_mic_open = true;
        ESP_LOGI(TAG, "麦克风就绪：总线 %u Hz %uch %ubit → 降采样输出 %u Hz 单声道（板载 MIC1/MIC2）",
                 (unsigned)VOICE_BUS_RATE, (unsigned)VOICE_BUS_CHANNELS, (unsigned)VOICE_MIC_BITS,
                 (unsigned)VOICE_MIC_RATE);
    }
    return ESP_OK;
}

static void mic_close(void)
{
    if (s_mic && s_mic_open) {
        (void)esp_codec_dev_close(s_mic);
        s_mic_open = false;
    }
}

/*
 * 组件每 40 ms 调一次，期望 ime_voice_frame_bytes(16000) = 1280 字节。
 * `esp_codec_dev_read()` 本身就是阻塞读（I2S DMA），所以直接读、不要自己攒缓冲 ——
 * 40 ms 的节拍由组件负责（见 voice.h 的 audio_read 说明）。
 */
static int host_audio_read(void *user, void *buf, size_t len, uint32_t timeout_ms)
{
    (void)user;
    (void)timeout_ms;
    if (mic_ensure_open() != ESP_OK) {
        return -1;                       /* 负值 = 硬件错误，组件会转 ERROR 并上报 ERR_AUDIO */
    }
    if (len != VOICE_FRAME_BYTES) {
        /* 组件的拍长是固定的（ime_voice_frame_bytes(16000) = 1280），别的长度不支持 */
        ESP_LOGW(TAG, "意外的帧长 %u（期望 %u）", (unsigned)len, (unsigned)VOICE_FRAME_BYTES);
        return -1;
    }
    if (esp_codec_dev_read(s_mic, s_mic_raw, (int)sizeof(s_mic_raw)) != ESP_OK) {
        ESP_LOGW(TAG, "麦克风读失败");
        return -1;
    }
    /*
     * 48 kHz 立体声 → 16 kHz 单声道，一次搞定：
     *   每 3 帧取 1 帧（÷3 降采样），同时把这一组的左右声道一起平均掉 ——
     *   这相当于一个 6 抽头的方波低通，能压掉一部分 8 kHz 以上的混叠，比裸抽样干净；
     *   语音识别对这种质量的输入完全够用（讯飞自己还会做降噪/AGC）。
     */
    int16_t *out = (int16_t *)buf;
    for (int i = 0; i < VOICE_FRAME_FRAMES; i++) {
        int32_t acc = 0;
        for (int k = 0; k < VOICE_DECIM; k++) {
            const int16_t *f = &s_mic_raw[(i * VOICE_DECIM + k) * VOICE_BUS_CHANNELS];
            acc += f[0] + f[1];
        }
        out[i] = (int16_t)(acc / (VOICE_DECIM * VOICE_BUS_CHANNELS));
    }
    return VOICE_FRAME_BYTES;
}

/* ------------------------------------------------------------------ WebSocket */

static void ws_event_cb(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接");
        if (s_ws_connected) {
            xSemaphoreGive(s_ws_connected);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        /* op_code 0x1 = 文本帧（讯飞的结果都是文本）；0x8 = 关闭握手 */
        if (d->op_code == 0x01 && d->data_len > 0 && s_voice != NULL) {
            if (d->payload_len != d->data_len || d->payload_offset != 0) {
                /* 分片：一条 JSON 被拆成多次事件。我们把 buffer_size 设得比任何一条
                 * 结果都大，正常不会走到这里 —— 真走到了就明确报出来，别静默丢。 */
                ESP_LOGW(TAG, "WebSocket 分片：offset=%d len=%d payload=%d（这条结果可能不完整）",
                         d->payload_offset, d->data_len, d->payload_len);
            }
            if (d->data_len < (int)sizeof(s_json)) {
                memcpy(s_json, d->data_ptr, (size_t)d->data_len);
                /* ★ 组件会就地解析并写 buf[len] = '\0'，所以要留这个可写字节 */
                s_json[d->data_len] = '\0';
                lv_pinyin_ime_voice_ws_message(s_voice, s_json, (size_t)d->data_len);
            } else {
                ESP_LOGW(TAG, "结果太大（%d 字节），丢掉", d->data_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket 已断开");
        if (s_ws_connected) {
            xSemaphoreGive(s_ws_connected);   /* 别让 ws_open 干等到超时 */
        }
        if (s_voice != NULL) {
            lv_pinyin_ime_voice_ws_closed(s_voice, 1);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket 出错");
        if (s_ws_connected) {
            xSemaphoreGive(s_ws_connected);
        }
        if (s_voice != NULL) {
            lv_pinyin_ime_voice_ws_closed(s_voice, 1);
        }
        break;

    default:
        break;
    }
}

/* 组件要求：**握手完成后才返回**（它紧接着就要发第一帧音频） */
static int host_ws_open(void *user, const char *url)
{
    (void)user;
    if (url == NULL) {
        return -1;
    }
    if (s_ws == NULL) {
        esp_websocket_client_config_t cfg = {};
        cfg.uri                    = url;      /* 每次 open 都是新签名的 URL，下面会重设 */
        cfg.buffer_size            = VOICE_WS_BUF;
        cfg.task_stack             = 6 * 1024;
        cfg.disable_auto_reconnect = true;     /* 重连策略由组件状态机决定，别在底层偷偷重连 */
        cfg.network_timeout_ms     = VOICE_WS_TIMEOUT_MS;
        /* ★ 少了这一行会直接连不上：
         *   E esp-tls-mbedtls: No server verification option set in esp_tls_cfg_t structure
         * 讯飞是 wss（TLS），必须给根证书；IDF 自带的证书包最省事。 */
        cfg.crt_bundle_attach      = esp_crt_bundle_attach;
        s_ws = esp_websocket_client_init(&cfg);
        if (s_ws == NULL) {
            ESP_LOGE(TAG, "websocket client 建不出来");
            return -1;
        }
        (void)esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);
    } else {
        (void)esp_websocket_client_set_uri(s_ws, url);   /* URL 每次都变（带签名/时间戳）*/
    }

    if (s_ws_connected) {
        (void)xSemaphoreTake(s_ws_connected, 0);         /* 清掉上一次残留的信号 */
    }
    const esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start 失败: %s", esp_err_to_name(err));
        return -1;
    }
    if (s_ws_connected &&
        xSemaphoreTake(s_ws_connected, pdMS_TO_TICKS(VOICE_WS_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "websocket 连接超时（%d ms）", VOICE_WS_TIMEOUT_MS);
        (void)esp_websocket_client_stop(s_ws);
        return -1;
    }
    return 0;
}

static int host_ws_send_text(void *user, const char *json)
{
    (void)user;
    if (s_ws == NULL || json == NULL) {
        return -1;
    }
    const int n = esp_websocket_client_send_text(s_ws, json, (int)strlen(json),
                                                pdMS_TO_TICKS(VOICE_SEND_TIMEOUT_MS));
    return (n >= 0) ? 0 : -1;
}

static void host_ws_close(void *user)
{
    (void)user;
    if (s_ws == NULL) {
        return;
    }
    /* 要能容忍"还没连上就关"（组件的失败路径也会调到这里） */
    if (esp_websocket_client_is_connected(s_ws)) {
        (void)esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
    }
    (void)esp_websocket_client_stop(s_ws);
}

static const lv_pinyin_ime_voice_host_t s_host = {
    .user          = NULL,
    .ws_open       = host_ws_open,
    .ws_send_text  = host_ws_send_text,
    .ws_close      = host_ws_close,
    .audio_read    = host_audio_read,
};

/* --------------------------------------------------------- 自动暂停 / 恢复 */

/*
 * 用户要求："开始识别自动暂停播放、结束恢复"。
 *
 * 判据只看会话状态（CONNECTING/LISTENING/FINISHING = 正在忙），不看按键 —— 这样
 * 无论从哪个入口（⌨ 页、语音键）开始，行为一致。DONE/ERROR 之后组件会短暂停留再
 * 回到 IDLE，这里**不等 IDLE**：一离开"忙"就恢复播放，听感更跟手。
 */
static void on_voice_state(lv_event_t *e)
{
    (void)e;
    if (s_voice == NULL) {
        return;
    }
    const lv_pinyin_ime_voice_state_t st = lv_pinyin_ime_voice_state(s_voice);
    const bool busy = (st == LV_PINYIN_IME_VOICE_CONNECTING) ||
                      (st == LV_PINYIN_IME_VOICE_LISTENING) ||
                      (st == LV_PINYIN_IME_VOICE_FINISHING);

    if (busy && !s_paused) {
        s_resume_needed = audio_service_is_playing();
        s_paused        = true;
        if (s_resume_needed) {
            (void)audio_service_pause();
            ESP_LOGI(TAG, "识别开始：已暂停播放");
        }
    } else if (!busy && s_paused) {
        s_paused = false;
        if (s_resume_needed) {
            (void)audio_service_resume();
            ESP_LOGI(TAG, "识别结束：恢复播放");
        }
        s_resume_needed = false;
        mic_close();          /* 会话结束就关麦，省电、也让下一次重新按当前格式打开 */
    }
}

/* ------------------------------------------------------------------ 对外 */

bool voice_service_available(void)
{
    return lv_pinyin_ime_voice_supported();
}

esp_err_t voice_service_init(void)
{
    if (!lv_pinyin_ime_voice_supported()) {
        ESP_LOGW(TAG, "组件没开语音（CONFIG_LV_PINYIN_IME_VOICE_ENABLE=n），语音输入不可用");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_ws_connected == NULL) {
        s_ws_connected = xSemaphoreCreateBinary();
        if (s_ws_connected == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* 密钥提示：三个都空 = 点语音键会立刻报"未配置"，这里先说明白 */
    const lv_pinyin_ime_voice_cfg_t cfg = lv_pinyin_ime_voice_cfg_default();
    if (cfg.appid[0] == '\0' || cfg.apikey[0] == '\0' || cfg.apisecret[0] == '\0') {
        ESP_LOGW(TAG, "讯飞密钥没配：menuconfig → Component config → lvgl_pinyin_ime → "
                      "Voice input (iFlytek IAT)，填 AppID / APIKey / APISecret");
    } else {
        ESP_LOGI(TAG, "语音输入就绪（endpoint=%s language=%s %d Hz）", cfg.endpoint,
                 cfg.language, cfg.sample_rate);
    }
    return ESP_OK;
}

void voice_service_attach_ime(lv_obj_t *ime)
{
    if (ime == NULL || !lv_pinyin_ime_voice_supported()) {
        return;
    }
    /*
     * 先让组件把会话建出来（这一步**还没有 host**，所以只能"能建就建"），
     * 再用 set_config() 把 host 与密钥补上 —— 这正是组件设计好的接入顺序
     * （见 lv_pinyin_ime.c 里 create() 那段注释）。下次 start() 生效。
     */
    lv_pinyin_ime_voice_t *v = lv_pinyin_ime_voice_attach(ime);
    if (v == NULL) {
        ESP_LOGW(TAG, "这把键盘没能接上语音会话");
        return;
    }
    if (s_voice != v) {
        s_voice = v;
        s_ime   = ime;                            /* 麦克风测试页要用它做入口 */
        lv_pinyin_ime_voice_cfg_t cfg = lv_pinyin_ime_voice_cfg_default();
        cfg.host = &s_host;                       /* ★ 四个回调在这里补上 */
        lv_pinyin_ime_voice_set_config(v, &cfg);
        lv_obj_add_event_cb(ime, on_voice_state, lv_pinyin_ime_event_voice_state(), NULL);
        ESP_LOGI(TAG, "键盘已接上语音输入");
    }
}

lv_obj_t *voice_service_ime(void)
{
    return (s_ime != NULL && lv_obj_is_valid(s_ime)) ? s_ime : NULL;
}

lv_pinyin_ime_voice_t *voice_service_voice(void)
{
    return s_voice;
}
