/*
 * audio_http.cc —— 在线播放的传输层（自研，替代 GMF 的 esp_gmf_io_http）
 *
 * 为什么必须自研：espaudiocore 只提供"ringbuf 输入"这一种流式入口，
 * HTTP/TLS、Range 续传、断线重连、进度、下载速率它一概不管
 * （见 libs/espaudiocore/API.md 的能力表）。而工程原来的这些能力全部来自
 * `esp_gmf_io_http`，换库后必须在这里补齐。
 *
 * 数据通路：
 *
 *   HTTP(S) ── esp_http_client_read ──> 压缩 ringbuf（4 MB，PSRAM）
 *                                          └─ espaudiocore_begin_stream_rb() 解码
 *
 * 关键设计：
 *   1. **读满就停**：ringbuf 满了就阻塞在 xRingbufferSend 上，等于自动"预读满即停读
 *      网络"（与 GMF 的 4 MB 环一致），一首 3~4 MB 的歌基本一口气拉完。
 *   2. **断线续传**：读到错误就带 `Range: bytes=<已下载>-` 重连，连续失败
 *      AUDIO_HTTP_MAX_RETRY 次才上报 AUDIO_EVT_ERROR（app 层据此重刷过期的签名地址）。
 *   3. **Range 重开**：seek 走 stop + start(from_byte)，服务端返回 206 时
 *      `总长 = from_byte + Content-Length`；返回 200（不支持 Range）时本地丢弃
 *      from_byte 字节（慢，但功能是对的，与 GMF 的 `skip_bytes` 兜底等价）。
 *   4. **格式嗅探**：ringbuf 输入不能回退探测，所以先按 URL 扩展名猜，
 *      猜不出就在建连后读头 2 KB 嗅探魔数，并把这两 KB 原样喂进 ringbuf。
 *      Vorbis/Opus/M4A 需要可 seek 的源，**不能**流式解码 → streamable() 返回 false，
 *      由上层改走"先下载到 SD 再当本地文件播"。
 */
#include "audio_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "espaudiocore.h"

static const char *TAG = "audio_http";

#define HTTP_TIMEOUT_MS      8000
#define HTTP_MAX_RETRY       5        /* 连续重连失败上限（成功读一次就清零） */
#define SNIFF_BYTES          2048
/**
 * 起播前预读的字节数。
 *
 * 解码器 `open()` 必须能凑出一个完整容器帧，否则直接报 PARSE 起不来
 * （MP3 一帧最大 1441 B @320 kbps；AAC/FLAC 的帧更小）。取一个读取块（8 KB）
 * 有 5 倍余量，同时不会明显拖慢起播（慢网下还有 STREAM_PREBUF_TIMEOUT_MS 兜底）。
 */
#define STREAM_PREBUF_BYTES  AUDIO_STREAM_CHUNK
/** 预读的最长等待：超时就用手里这些起播，别把用户卡在"缓冲中" */
#define STREAM_PREBUF_TIMEOUT_MS  2500
/** 速率统计窗口 */
#define SPEED_WINDOW_MS      250

static char             s_url[AUDIO_PATH_MAX];
static RingbufHandle_t  s_ring;
static TaskHandle_t     s_task;
static volatile bool    s_run;          /**< 馈送任务是否应当继续 */
static volatile bool    s_ready;        /**< 连接已建立 + 嗅探完成 */
static volatile bool    s_eof;          /**< 本次连接已读到正常结尾 */
/*
 * ⚠ 这两个计数器是**跨任务**读的（馈送任务写、UI/LVGL 任务读进度）。
 * 故意用 **32 位**：RV32 上 64 位读不是原子的，撕裂读会让进度百分比跳成垃圾值
 * （音频文件不可能超过 4 GB，32 位足够）。
 */
static volatile uint32_t s_downloaded;
static volatile uint32_t s_total;
static volatile int     s_fmt;          /**< espaudiocore_format_t */
static volatile bool    s_conn_failed;

/* 速率窗口（只在馈送任务里更新） */
static uint64_t s_speed_bytes;
static uint32_t s_speed_ms;
static uint32_t s_speed_kbps;

/* ===========================================================================
 * 小工具
 * =========================================================================*/

static void ring_drain(void)
{
    if (!s_ring) {
        return;
    }
    for (;;) {
        size_t got = 0;
        uint8_t *it = (uint8_t *)xRingbufferReceiveUpTo(s_ring, &got, 0, 4096);
        if (!it) {
            break;
        }
        vRingbufferReturnItem(s_ring, it);
    }
}

/** 从 URL 路径部分（切掉 ?query）猜容器格式；猜不出返回 UNKNOWN */
static int format_from_url(const char *url)
{
    const char *q = strchr(url, '?');
    const size_t len = q ? (size_t)(q - url) : strlen(url);
    const char *dot = NULL;
    for (size_t i = len; i > 0; i--) {
        if (url[i - 1] == '.') {
            dot = url + i - 1;
            break;
        }
        if (url[i - 1] == '/') {
            break;
        }
    }
    if (!dot) {
        return ESPAUDIOCORE_FMT_UNKNOWN;
    }
    const char *e = dot + 1;
    const size_t elen = len - (size_t)(e - url);
    struct {
        const char *ext;
        int         fmt;
    } tab[] = {
        {"mp3", ESPAUDIOCORE_FMT_MP3},   {"aac", ESPAUDIOCORE_FMT_AAC},
        {"flac", ESPAUDIOCORE_FMT_FLAC}, {"wav", ESPAUDIOCORE_FMT_WAV},
        {"wv", ESPAUDIOCORE_FMT_WAVPACK},{"ogg", ESPAUDIOCORE_FMT_VORBIS},
        {"oga", ESPAUDIOCORE_FMT_VORBIS},{"opus", ESPAUDIOCORE_FMT_OPUS},
        {"m4a", ESPAUDIOCORE_FMT_M4A},   {"mp4", ESPAUDIOCORE_FMT_M4A},
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strlen(tab[i].ext) == elen && strncasecmp(e, tab[i].ext, elen) == 0) {
            return tab[i].fmt;
        }
    }
    return ESPAUDIOCORE_FMT_UNKNOWN;
}

/** 按魔数嗅探容器（拿不到扩展名时用） */
static int sniff_format(const uint8_t *d, size_t n)
{
    if (n >= 4 && memcmp(d, "fLaC", 4) == 0) {
        return ESPAUDIOCORE_FMT_FLAC;
    }
    if (n >= 4 && memcmp(d, "OggS", 4) == 0) {
        return ESPAUDIOCORE_FMT_VORBIS;
    }
    if (n >= 4 && memcmp(d, "wvpk", 4) == 0) {
        return ESPAUDIOCORE_FMT_WAVPACK;
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) {
        return ESPAUDIOCORE_FMT_WAV;
    }
    if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
        return ESPAUDIOCORE_FMT_M4A;
    }
    if (n >= 3 && memcmp(d, "ID3", 3) == 0) {
        return ESPAUDIOCORE_FMT_MP3;         /* 带 ID3v2 的 MP3 */
    }
    if (n >= 2 && d[0] == 0xFF) {
        if ((d[1] & 0xF6) == 0xF0) {
            return ESPAUDIOCORE_FMT_AAC;     /* ADTS：1111 1111 1111 0xxx */
        }
        if ((d[1] & 0xE0) == 0xE0) {
            return ESPAUDIOCORE_FMT_MP3;     /* MPEG 音频同步字 */
        }
    }
    return ESPAUDIOCORE_FMT_UNKNOWN;
}

static void speed_account(size_t bytes)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_speed_ms == 0) {
        s_speed_ms = now;
    }
    s_speed_bytes += bytes;
    const uint32_t dt = now - s_speed_ms;
    if (dt >= SPEED_WINDOW_MS) {
        const uint32_t kbps = (uint32_t)(s_speed_bytes * 8ull / dt);
        /* 1:1 平滑（原来 GMF 是 1:9，这里窗口更短，平滑不用那么重） */
        s_speed_kbps = s_speed_kbps ? (s_speed_kbps + kbps) / 2 : kbps;
        s_speed_bytes = 0;
        s_speed_ms = now;
    }
}

/** 把一块压缩数据写进 ringbuf（满了就等解码器消费；stop 时立刻退出） */
static bool ring_put(const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        if (!s_run) {
            return false;
        }
        if (xRingbufferSend(s_ring, data + sent, len - sent, pdMS_TO_TICKS(200)) == pdTRUE) {
            sent = len;
            break;
        }
    }
    return true;
}

/* ===========================================================================
 * 连接与读取
 * =========================================================================*/

/** 建立连接；from_byte > 0 时带 Range */
static esp_err_t http_open(esp_http_client_handle_t *out, uint64_t from_byte, int64_t *body_len)
{
    esp_http_client_config_t cfg = {};
    cfg.url               = s_url;
    cfg.timeout_ms        = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   /* https 也要校验证书 */
    cfg.buffer_size       = 4096;
    cfg.buffer_size_tx    = 512;
    cfg.keep_alive_enable = true;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return ESP_ERR_NO_MEM;
    }
    if (from_byte > 0) {
        char rng[48];
        snprintf(rng, sizeof(rng), "bytes=%llu-", (unsigned long long)from_byte);
        esp_http_client_set_header(cli, "Range", rng);
    }

    if (esp_http_client_open(cli, 0) != ESP_OK) {
        ESP_LOGW(TAG, "open failed (from %llu)", (unsigned long long)from_byte);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }
    const int64_t len = esp_http_client_fetch_headers(cli);
    const int status = esp_http_client_get_status_code(cli);

    if (status == 206) {
        /* 部分内容：Content-Length 是**剩余**长度，整首总长要加回起点 */
        *body_len = len;
        s_total = (uint32_t)(from_byte + (len > 0 ? (uint64_t)len : 0));
        ESP_LOGI(TAG, "HTTP 206 from %llu, remaining %lld (total %llu)",
                 (unsigned long long)from_byte, (long long)len,
                 (unsigned long long)s_total);
    } else if (status == 200) {
        *body_len = len;
        s_total = len > 0 ? (uint32_t)len : 0;
        if (from_byte > 0) {
            ESP_LOGW(TAG, "server ignored Range (200), skipping %llu bytes locally",
                     (unsigned long long)from_byte);
        }
    } else {
        ESP_LOGE(TAG, "HTTP status %d (URL 可能已过期)", status);
        esp_http_client_cleanup(cli);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 服务端不支持 Range：本地把前 from_byte 字节读掉丢弃 */
    if (status == 200 && from_byte > 0) {
        uint8_t *skip = (uint8_t *)malloc(4096);
        uint64_t left = from_byte;
        if (!skip) {
            esp_http_client_cleanup(cli);
            return ESP_ERR_NO_MEM;
        }
        while (left > 0) {
            const int r = esp_http_client_read(cli, (char *)skip, 4096);
            if (r <= 0) {
                break;
            }
            left -= (uint64_t)r;
        }
        free(skip);
        ESP_LOGW(TAG, "skip done, %llu bytes left to skip", (unsigned long long)left);
    }

    *out = cli;
    return ESP_OK;
}

static void feed_exit(void)
{
    /* ⚠ 必须先清 s_task 再删自己：否则 audio_http_stop() 会拿着已失效的句柄
     * 去 vTaskDelete()（那是 use-after-free，现场表现是随机的 LoadProhibited） */
    s_task = NULL;
    s_run = false;
    vTaskDelete(NULL);
}

static void feed_task(void *arg)
{
    const uint64_t from_byte = (uint64_t)(uintptr_t)arg;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(AUDIO_STREAM_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc(AUDIO_STREAM_CHUNK);
    }
    if (!buf) {
        ESP_LOGE(TAG, "no buffer");
        s_conn_failed = true;
        s_ready = true;
        feed_exit();
    }

    int64_t body_len = 0;
    esp_http_client_handle_t cli = NULL;
    if (http_open(&cli, from_byte, &body_len) != ESP_OK) {
        s_conn_failed = true;
        s_ready = true;
        free(buf);
        feed_exit();
    }

    s_downloaded = (uint32_t)from_byte;
    s_eof = false;

    /*
     * ---- 起播前预读（**任何连接都要做**，包括 seek 重开的）----
     *
     * ★ 为什么必须（2026-10 真机定位）：MP3 解码器 open() 里是两步 ——
     *     fill_valid_frame()  找同步字（可能是帧数据里的**假同步**）
     *     decode_frame()      真正校验这一帧
     *   而 open() 对 decode_frame() 的失败**没有重试**，直接返回 PARSE
     *   （`begin_stream_rb failed: ESP_ERR_INVALID_RESPONSE (dec_err=-2)`）。
     *   所以"解码器打开时手里数据太少、凑不出一个完整帧"= 这首歌起不来。
     *   实测：首次连接预读过 2 KB 能起（320 kbps 一帧 1441 B 刚好塞得下），
     *   而 seek 重开的连接当时**没有预读**，只剩几百字节 → 必然失败。
     *
     * 嗅探仍然只在**首次**连接做：seek 重开的连接是从歌中间开始的，
     * 那几 KB 里没有容器头，嗅出来的只能是垃圾（会把已定好的格式改坏）。
     */
    {
        size_t got = 0;
        bool sniffed = (from_byte != 0);   /* 非首次连接 = 不需要嗅探，直接算已完成 */
        const uint32_t deadline =
            (uint32_t)(esp_timer_get_time() / 1000) + STREAM_PREBUF_TIMEOUT_MS;

        while (got < STREAM_PREBUF_BYTES && s_run) {
            const int r = esp_http_client_read(cli, (char *)buf + got,
                                               (int)(STREAM_PREBUF_BYTES - got));
            if (r <= 0) {
                break;
            }
            got += (size_t)r;
            if (!sniffed && got >= SNIFF_BYTES) {
                const int s = sniff_format(buf, got);
                if (s != ESPAUDIOCORE_FMT_UNKNOWN && s != s_fmt) {
                    ESP_LOGW(TAG, "container sniffed as %d (扩展名猜的是 %d)，以嗅探为准",
                             s, (int)s_fmt);
                    s_fmt = s;
                }
                sniffed = true;
            }
            if ((uint32_t)(esp_timer_get_time() / 1000) >= deadline) {
                break;      /* 慢网：有多少算多少，别把起播卡住 */
            }
        }
        /* 连 2 KB 都没凑齐就超时：用手里这些尽力嗅一次 */
        if (!sniffed) {
            const int s = sniff_format(buf, got);
            if (s != ESPAUDIOCORE_FMT_UNKNOWN && s != s_fmt) {
                s_fmt = s;
            }
        }

        if (got == 0) {
            ESP_LOGE(TAG, "no data for prebuffer (from %llu)",
                     (unsigned long long)from_byte);
            s_conn_failed = true;
            s_ready = true;
            esp_http_client_cleanup(cli);
            free(buf);
            feed_exit();
        }
        s_downloaded += (uint32_t)got;
        speed_account(got);
        if (!ring_put(buf, got)) {
            esp_http_client_cleanup(cli);
            free(buf);
            feed_exit();
        }
        ESP_LOGI(TAG, "prebuf %u bytes (from %llu) -> format %d", (unsigned)got,
                 (unsigned long long)from_byte, (int)s_fmt);
    }

    s_ready = true;
    ESP_LOGI(TAG, "streaming %s (fmt %d, total %llu)", s_url, (int)s_fmt,
             (unsigned long long)s_total);

    int retry = 0;
    while (s_run) {
        const int n = esp_http_client_read(cli, (char *)buf, AUDIO_STREAM_CHUNK);
        if (n > 0) {
            retry = 0;                       /* 读到数据就清空连续失败计数 */
            s_downloaded += (uint32_t)n;
            speed_account((size_t)n);
            if (!ring_put(buf, (size_t)n)) {
                break;                       /* 被 stop 打断 */
            }
            continue;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "download complete: %llu bytes", (unsigned long long)s_downloaded);
            s_eof = true;
            break;
        }

        /* n < 0：连接出错（CDN 掐长连接很常见）→ 带 Range 续传 */
        if (++retry > HTTP_MAX_RETRY) {
            ESP_LOGE(TAG, "reconnect failed %d times, giving up", retry - 1);
            audio_notify(AUDIO_EVT_ERROR);
            break;
        }
        ESP_LOGW(TAG, "read error, reconnecting #%d from %llu", retry,
                 (unsigned long long)s_downloaded);
        esp_http_client_cleanup(cli);
        cli = NULL;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (http_open(&cli, (uint64_t)s_downloaded, &body_len) != ESP_OK) {
            if (!s_run) {
                break;
            }
            continue;
        }
    }

    if (cli) {
        esp_http_client_cleanup(cli);
    }
    free(buf);

    /* 正常读完就让任务歇着等 stop（保持任务存在，seek 只要 stop+start 即可） */
    while (s_run) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_ready = false;
    feed_exit();
}

/* ===========================================================================
 * 对外（组件内部）接口
 * =========================================================================*/

esp_err_t audio_http_prepare(const char *url)
{
    if (!url || strncmp(url, "http", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* ringbuf 只建一次并长期保留（GMF 时代同样是 4 MB 常驻），避免每次切歌
     * 都向 PSRAM 要 4 MB —— 碎片化与失败风险都不值得省这点内存 */
    if (!s_ring) {
        s_ring = xRingbufferCreateWithCaps(AUDIO_STREAM_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            ESP_LOGW(TAG, "PSRAM ringbuf failed, try internal RAM");
            s_ring = xRingbufferCreate(256 * 1024, RINGBUF_TYPE_BYTEBUF);
        }
        if (!s_ring) {
            ESP_LOGE(TAG, "stream ringbuf create failed");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "stream ringbuf ready (%d KB)", AUDIO_STREAM_RING_BYTES / 1024);
    }

    strlcpy(s_url, url, sizeof(s_url));
    s_downloaded = 0;
    s_total = 0;
    s_ready = false;
    s_eof = false;
    s_conn_failed = false;
    s_speed_kbps = 0;
    s_speed_bytes = 0;
    s_speed_ms = 0;
    s_fmt = format_from_url(url);
    ring_drain();

    ESP_LOGI(TAG, "prepare: %s (ext fmt %d)", url, (int)s_fmt);
    return ESP_OK;
}

int audio_http_format(void)
{
    return s_fmt;
}

bool audio_http_streamable(void)
{
    switch (s_fmt) {
    case ESPAUDIOCORE_FMT_MP3:
    case ESPAUDIOCORE_FMT_AAC:
    case ESPAUDIOCORE_FMT_FLAC:
    case ESPAUDIOCORE_FMT_WAV:
    case ESPAUDIOCORE_FMT_WAVPACK:
        return true;
    default:
        /* Vorbis / Opus / M4A 的封装库要求可 seek 的源，ringbuf 输入不支持
         * （libs/espaudiocore/README.md 的"已知限制"），必须整段下载后再当本地文件播 */
        return false;
    }
}

RingbufHandle_t audio_http_ring(void)
{
    return s_ring;
}

esp_err_t audio_http_start(uint64_t from_byte)
{
    if (!s_ring) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_http_stop();

    s_run = true;
    s_ready = false;
    s_conn_failed = false;
    s_eof = false;
    ring_drain();

    /*
     * 馈送任务：core 0、优先级 5（**低于**解码任务的 6 —— 下载不能饿着解码）。
     * 栈 8 KB 且**放 PSRAM**：TLS 握手 + esp_http_client 的栈需求不小，而内部 RAM
     * 要留给射频（main.cc 有 RADIO_MIN_INTERNAL 检查）；GMF 时代的 io_http 也是
     * `stack_in_ext = true`，这条路在本板已经验证过。
     * （`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` 才能用 WithCaps 版本。）
     */
    if (xTaskCreatePinnedToCoreWithCaps(feed_task, "audio_http", 8192,
                                        (void *)(uintptr_t)from_byte, 5, &s_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool audio_http_wait_ready(int timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!s_ready) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            ESP_LOGW(TAG, "wait_ready timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !s_conn_failed;
}

void audio_http_stop(void)
{
    if (!s_task) {
        s_run = false;
        ring_drain();
        return;
    }
    s_run = false;
    /* 等任务自己退出（它可能正阻塞在 xRingbufferSend 的 200 ms 超时里） */
    for (int i = 0; i < 200 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task) {
        /*
         * 走到这里说明任务连 2 秒都没退 —— 它多半正阻塞在 socket 读里
         * （网络卡住，esp_http_client_read 的超时是 8 s）。
         *
         * ⚠ 强制删除是**不得已**：任务若正持着 lwip/mbedtls 内部的锁被删掉，
         * 那把锁就永远不还，网络栈会僵住。所以：
         *   1) 等待时间给足（见上面的循环：2 s，覆盖"ringbuf 满 → 200 ms 超时退出"
         *      与"socket 读被 8 s 超时打断"之间的绝大多数情况）；
         *   2) 真删的时候把日志打醒，事后排查能看见；
         *   3) 删之前先 s_run=false，任务即使被删也不会再主动用 s_ring/s_task。
         * 想彻底避免就得把 socket 读改成带超时的非阻塞轮询（esp_http_client 没有
         * 提供），那是另一个量级的改动，先按现状 + 醒目日志。
         */
        ESP_LOGW(TAG, "feed task did not exit in 2s, force delete (socket 可能未关闭)");
        vTaskDelete(s_task);
        s_task = NULL;
    }
    ring_drain();
    s_ready = false;
}

bool audio_http_progress(int *percent)
{
    const uint32_t total = s_total;
    const uint32_t got = s_downloaded;
    if (!percent || total == 0) {
        return false;
    }
    uint64_t p = (uint64_t)got * 100ull / total;
    if (p > 100) {
        p = 100;
    }
    *percent = (int)p;
    return true;
}

bool audio_http_speed(int *kbps)
{
    if (!kbps || s_speed_kbps == 0) {
        return false;
    }
    *kbps = (int)s_speed_kbps;
    return true;
}

uint64_t audio_http_downloaded(void)
{
    return s_downloaded;
}

/*
 * 环形缓冲里**还没被解码器取走**的字节数。
 *
 * 用途（见 audio_engine 的 measured_stream_bps）：算"这段流的真实码率"必须用
 * **解码器已经吃掉的字节**，而不是"网络已经下到的字节" —— 下载前沿会跑在播放前面
 * （4 MB 缓冲经常能把整首歌装下），拿下载量除以已播时长得到的是"越来越小的假码率"，
 * 于是总时长会**一边播一边往上爬**（2026-10 用户报"在线总时长不对"就是这个）。
 */
uint64_t audio_http_ring_filled(void)
{
    if (!s_ring) {
        return 0;
    }
    const size_t free_sz = xRingbufferGetCurFreeSize(s_ring);
    return (free_sz >= (size_t)AUDIO_STREAM_RING_BYTES)
               ? 0
               : (uint64_t)((size_t)AUDIO_STREAM_RING_BYTES - free_sz);
}

uint64_t audio_http_total(void)
{
    return s_total;
}

const char *audio_http_url(void)
{
    return s_url[0] ? s_url : NULL;
}
