/*
 * os_http_client.cc —— 自管 302 的同步 HTTP 实现（esp_http_client）
 *
 * 逐字搬自原 online_service.cc 的 HTTP 那一节，只改了两处**名字/结构**上的东西：
 *   1) `s_last_err` 的直接写入改成 os_state_set_last_error()（同一块内存、同一语义）；
 *   2) "相对 Location 拼回主机"两处重复代码抽成 os_http_join_location()
 *      （两个分支的输出逐字等价：loc[0]=='/' 时拼 scheme://host，否则原样拷贝）。
 * 其余 URL、请求头、超时值、日志文案、错误文案一字未动。
 */
#include "internal/os_http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "internal/os_state.h"
#include "internal/os_util.h"
#include "online_service.h"

/*
 * 定义在 os_account.cc；声明与 online_service.h 里的 `extern "C"` 契约一致
 * （该头文件把 online_netease_api_base 也包在 extern "C" 里），
 * 这样无论编译器给 C++ 起什么修饰名，符号都是 C 链接、一定能对上。
 */
extern "C" const char *online_netease_api_base(void);

static const char *TAG = "online";

/*
 * 自建 API 服务的访问鉴权（对应服务端 .env 的 AUTH_TOKEN）。
 * 服务端要求请求头 `Authorization: Bearer <Token>`（实测不带就是 HTTP 401），
 * Token 从 sdkconfig 的 CONFIG_ONLINE_NETEASE_API_TOKEN 来。
 */
#ifdef CONFIG_ONLINE_NETEASE_API_TOKEN
#define NET_AUTH_TOKEN CONFIG_ONLINE_NETEASE_API_TOKEN
#else
#define NET_AUTH_TOKEN ""
#endif

bool sink_reserve(http_sink_t *s, size_t need)
{
    if (need <= s->cap) {
        return true;
    }
    if (need > s->limit) {
        s->overflow = true;
        return false;
    }
    size_t cap = s->cap ? s->cap : HTTP_SINK_MIN;
    while (cap < need) {
        cap *= 2;
        if (cap > s->limit) {
            cap = s->limit;
            break;
        }
    }
    char *nb = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) {
        /* PSRAM 没起来时兜底走内部 RAM（块大时通常也拿不到，尽力而为） */
        nb = (char *)malloc(cap);
    }
    if (!nb) {
        s->overflow = true;
        return false;
    }
    if (s->buf) {
        memcpy(nb, s->buf, s->len);
        free(s->buf);
    }
    s->buf = nb;
    s->cap = cap;
    s->buf[s->len] = '\0';
    return true;
}

esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (!e->user_data) {
        return ESP_OK;
    }
    http_sink_t *s = (http_sink_t *)e->user_data;

    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (e->header_key && e->header_value &&
            strcasecmp(e->header_key, "Location") == 0) {
            snprintf(s->location, sizeof(s->location), "%s", e->header_value);
        }
        return ESP_OK;
    }
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        if (!sink_reserve(s, s->len + (size_t)e->data_len + 1)) {
            return ESP_FAIL;              /* 超上限/没内存：让上层收尾报错 */
        }
        memcpy(s->buf + s->len, e->data, (size_t)e->data_len);
        s->len += (size_t)e->data_len;
        s->buf[s->len] = '\0';
    }
    return ESP_OK;
}

/* 拼 "Bearer xxx"（没有 token 时返回空串）。只对我们自己的 API 地址加这个头 ——
 * 绝不能把 token 泄露给 meting 那种第三方代理。 */
const char *net_auth_hdr(void)
{
    static char hdr[192];
    if (!hdr[0] && NET_AUTH_TOKEN[0]) {
        snprintf(hdr, sizeof(hdr), "Bearer %s", NET_AUTH_TOKEN);
        ESP_LOGI(TAG, "netease api auth: enabled (%u char token)", (unsigned)strlen(NET_AUTH_TOKEN));
    }
    return hdr;
}

/*
 * 打日志用的 URL：**砍掉查询串**。
 *
 * ★ 账号类接口把 cookie 放在 `?cookie=MUSIC_U=…`（登录凭证，~1.6 KB）。
 *   失败日志里原样打印 = 把账号送人：用户把日志贴出来（贴给同事/贴到群里/贴给 AI）
 *   就等于泄露登录态（2026-10 真机日志里就这样出现过）。
 *   调试需要的只是"哪个接口挂了"，查询串一律不要打。
 *
 * 返回静态缓冲，只保证"取完立刻用"（HTTP 都在同一个工作线程里跑）。
 */
const char *url_for_log(const char *url)
{
    static char buf[192];
    const size_t full = url ? strlen(url) : 0;
    const char *q = url ? strchr(url, '?') : NULL;
    size_t n = q ? (size_t)(q - url) : full;
    if (n > sizeof(buf) - 1) {
        n = sizeof(buf) - 1;
    }
    if (url) {
        memcpy(buf, url, n);
    }
    buf[n] = '\0';
    return buf;
}

void os_http_join_location(const char *cur, const char *loc, char *out, size_t cap)
{
    if (loc[0] == '/') {
        /* 相对跳转：挂回原主机 */
        char host[256] = {0};
        char tail[512] = {0};
        const char *scheme_end = strstr(cur, "://");
        const char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
        if (path) {
            size_t n = (size_t)(path - cur);
            if (n >= sizeof(host)) {
                n = sizeof(host) - 1;
            }
            memcpy(host, cur, n);
            host[n] = '\0';
        }
        const int loc_n = snprintf(tail, sizeof(tail), "%s", loc);
        snprintf(out, cap, "%.*s%.*s", (int)sizeof(host) - 1, host,
                 (loc_n > 0) ? loc_n : 0, tail);
    } else {
        snprintf(out, cap, "%s", loc);
    }
}

/*
 * 哪些失败值得"换一条新连接再发一次"。
 *
 * ★ 2026-11 真机（用户报"网易云封面有时出不来"）：HTTPS 偶发在**握手成功后的
 *   第一次读**就失败，日志是
 *     I esp-x509-crt-bundle: Certificate validated
 *     E esp-tls-mbedtls: read error :-0x0087
 *     E transport_base: esp_tls_conn_read error, errno=I/O error
 *     E HTTP_CLIENT: Incomlete data received, ret=-135, 0/8967 bytes
 *   -0x0087 = -135 = PSA_ERROR_INVALID_ARGUMENT，落在 **PSA 的错误空间**里
 *   （mbedtls 的 SSL 错误码最短也只到 -0x7080，而且 mbedtls_print_error_msg()
 *   对它打不出任何人类可读的解释 —— 日志里那行是空的正是这个原因）。也就是
 *   失败点在 mbedtls 解 TLS record → PSA → ESP 硬件 AES/GCM 驱动那段预编译库，
 *   **不是**网络不通、URL 不对、请求头缺失：同一时刻别的 HTTPS 请求能成，
 *   服务器侧（主机 curl / node）取同一个地址是 200 + 43 KB 正常图片。
 *
 *   所以这里的判据只有两类：①"连接/读写"层面的瞬时失败；② 我们自己的笼统失败。
 *   服务器**明确表态**的失败（非 200、重定向缺 Location、超上限、没内存）不重试 ——
 *   那些重试也只是白白多打三次请求，把失败延迟拖长三倍。
 */
static bool get_err_retryable(esp_err_t e)
{
    switch (e) {
    case ESP_ERR_HTTP_CONNECT:            /* TCP/TLS 建链失败 */
    case ESP_ERR_HTTP_FETCH_HEADER:       /* ← 上面那条 -0x0087 走的就是它 */
    case ESP_ERR_HTTP_INCOMPLETE_DATA:    /* 读到一半断流 / ret=-135 */
    case ESP_ERR_HTTP_EAGAIN:             /* 读数据超时（见 HTTP_TIMEOUT_MS 的注释） */
    /*
     * 2026-11 真机（用户报"歌曲拉取失败"）：TLS **写**请求失败，但根因在内存 ——
     *     E esp-aes: Failed to allocate memory
     *     E esp-tls-mbedtls: write error :-0x0084
     *     E HTTP_CLIENT: Error write request
     * 那会儿 Wi-Fi 刚连上、蓝牙正在连 A2DP，内部 RAM 见底，esp-aes 给 DMA 申请
     * 16 字节对齐的内部缓冲失败（esp_aes_dma_core.c 的 heap_caps_aligned_alloc）。
     * 这是瞬时压力而不是服务器表态，隔几百毫秒再来一次往往就过了 —— 值得重试。
     */
    case ESP_ERR_HTTP_WRITE_DATA:
    case ESP_ERR_TIMEOUT:
    case ESP_FAIL:                        /* esp_http_client_init/perfom 的笼统失败 */
        return true;
    default:
        return false;
    }
}

/* 这条 URL 是不是指向**我们自己的 API 服务**（base 地址见 os_account.cc）。
 * 只有对自家服务才发 Authorization / cookie —— 302 跳到 CDN 时绝不能带出去。 */
static bool is_own_api(const char *url)
{
    const char *base = online_netease_api_base();
    const size_t n = strlen(base);
    return n > 0 && strncmp(url, base, n) == 0;
}

/*
 * 一次 GET = 最多四跳（自管 302，见文件头）。
 * `cur` 是**输入输出**：重试时可以从已经解析出来的最终地址继续，
 * 不必每次都从 http 重新跳一次 https（历史上 meting 就是这么 302 的）。
 * `cookie` 非空时给**自家 base** 加 `X-Netease-Cookie` 头（语义见头文件）。
 */
/*
 * 从响应体里挑一句人能看懂的原因（**只用于报错**，不做完整 JSON 解析）：
 *   1) 优先取 {"msg":"…"} —— /qq 与网易云两套外壳都有这个字段；
 *   2) 取不到就把 body 开头压成一行。
 * ⚠ body 不一定以 NUL 结尾、也可能不是文本，所以先限长拷进栈上小缓冲再解析。
 */
static void http_reason_of(const char *body, size_t len, char *out, size_t cap)
{
    if (!out || cap < 8) {
        return;
    }
    out[0] = '\0';
    if (!body || len == 0) {
        return;
    }
    char head[160];
    size_t hn = (len < sizeof(head) - 1) ? len : sizeof(head) - 1;
    memcpy(head, body, hn);
    head[hn] = '\0';

    const char *src = head;
    const char *p = strstr(head, "\"msg\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                src = p + 1;
            }
        }
    }
    /* 正文里的控制字符压成空格：这一行要进串口日志与 Toast */
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        const char c = src[i];
        if (src != head && c == '"') {
            break;                      /* msg 的结束引号 */
        }
        if (src == head && (c == '\n' || c == '\r')) {
            break;
        }
        out[i] = (c < 0x20) ? ' ' : c;
    }
    out[i] = '\0';
}

static esp_err_t http_get_hops(char *cur, size_t cur_cap, http_verb_t verb,
                               const char *hdr1_name, const char *hdr1_val,
                               const char *hdr2_name, const char *hdr2_val,
                               char **out, size_t *out_len, size_t max_len)
{
    for (int hop = 0; hop < 4; hop++) {
        http_sink_t sink = {};
        sink.limit = max_len;          /* 上限只当门限：缓冲按需增长（见 sink_reserve） */
        /* sink.buf 先不分配 —— 第一段数据到了再按需从 PSRAM 建 */

        esp_http_client_config_t cfg = {};
        cfg.url                   = cur;
        cfg.event_handler         = http_evt;
        cfg.user_data             = &sink;
        cfg.timeout_ms            = HTTP_TIMEOUT_MS;
        cfg.crt_bundle_attach     = esp_crt_bundle_attach;
        cfg.disable_auto_redirect = true;
        cfg.keep_alive_enable     = false;
        /*
         * ★ buffer_size 默认只有 512 字节，而它要装下**整个请求行 + 请求头**。
         *   账号接口把 cookie 塞在 URL 里（MUSIC_U 一个值就 ~550 字符），稍不留神
         *   就超过 512 → 日志 `E HTTP_CLIENT: Out of buffer` + `GET failed (ESP_FAIL)`，
         *   表现为"扫码 803 成功了，但账号信息/歌单全拿不到"（2026-10 真机踩了两轮）。
         *   给到 4 KB，和 static url[NET_URL_MAX = 4096] 对齐。
         *
         * ★ 2026-11 服务端改策略后 cookie 从查询串搬到 `X-Netease-Cookie` 头
         *   （最长 NET_COOKIE_SEND = 2 KB），请求头整段于是又长了一截 ——
         *   再抬到 8 KB，别在"登录成功但所有账号接口都发不出去"上再踩第三次。
         */
        cfg.buffer_size           = 8192;
        /*
         * ★★ 光是 buffer_size 不够！IDF 的 esp_http_client.c:1783 把**请求行**
         *   （method + path + "?" + query）拼进 `client->buffer_size_tx`，超了就报
         *   `E HTTP_CLIENT: Out of buffer` 并直接返回 -1 —— 而 **buffer_size_tx 是独立
         *   字段**，默认只有 512，跟 buffer_size 无关。查询串长度现在回归正常
         *   （cookie 不再拼在 URL 上），4 KB 绰绰有余。
         */
        cfg.buffer_size_tx        = 4096;

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            free(sink.buf);
            return ESP_FAIL;
        }
        /*
         * 访问鉴权 + 账号 cookie：两者都**只对自家 API 基地址**发（见 net_auth_hdr /
         * is_own_api）。重定向到 CDN 或第三方时，cookie 一分一毫都不能带出去。
         */
        if (is_own_api(cur)) {
            const char *auth = net_auth_hdr();
            if (auth[0]) {
                esp_http_client_set_header(cli, "Authorization", auth);
            }
            /* 调用方指定的头（QQ 音乐凭证是两个；网易云只有一个 cookie 头） */
            if (hdr1_name && hdr1_name[0] && hdr1_val && hdr1_val[0]) {
                esp_http_client_set_header(cli, hdr1_name, hdr1_val);
            }
            if (hdr2_name && hdr2_name[0] && hdr2_val && hdr2_val[0]) {
                esp_http_client_set_header(cli, hdr2_name, hdr2_val);
            }
            /* POST：参数全在查询串里，body 为空（服务端 /qq 就是这个契约） */
            if (verb == HTTP_VERB_POST) {
                esp_http_client_set_method(cli, HTTP_METHOD_POST);
            }
        }
        /*
         * ★ 2026-11 服务端新策略：cookie 只能走 header（以前拼 `?cookie=<urlencoded>`
         *   会被 400 拒掉）。调用方通过 hdr_name/hdr_val 传入 —— 网易云是
         *   `X-Netease-Cookie: <压缩后的原始串>`，QQ 音乐是
         *   `X-QQMusic-Credential: base64(JSON)`，两者共用同一条发送路径。
         */
        esp_err_t ret = esp_http_client_perform(cli);
        const int status = esp_http_client_get_status_code(cli);
        if (status == 401 || status == 403) {
            /* 这一行就是"日志没别的信息"的答案：Token 没填/填错时，所有接口都会 401 */
            ESP_LOGE(TAG, "API 鉴权失败 (HTTP %d)：请在 sdkconfig 填 CONFIG_ONLINE_NETEASE_API_TOKEN", status);
        }

        const bool is_redirect = (status == 301 || status == 302 || status == 303 ||
                                  status == 307 || status == 308);
        char loc[512] = {0};
        if (is_redirect) {
            snprintf(loc, sizeof(loc), "%s", sink.location);
        }
        esp_http_client_cleanup(cli);

        if (is_redirect) {
            free(sink.buf);                        /* 这一跳的 body 不要了 */
            if (!loc[0]) {
                os_state_set_last_error("重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            os_http_join_location(cur, loc, cur, cur_cap);
            ESP_LOGI(TAG, "redirect -> %s", cur);
            continue;
        }

        if (ret != ESP_OK) {
            /* 超上限/没内存时 sink_reserve 会置 overflow，给一句能看懂的话 */
            if (sink.overflow) {
                ESP_LOGW(TAG, "GET too large (limit %u KB, got %u bytes): %s",
                         (unsigned)(max_len / 1024), (unsigned)sink.len, url_for_log(cur));
                char msg[64];
                snprintf(msg, sizeof(msg), "响应过大或内存不足(%u KB)", (unsigned)(sink.len / 1024));
                os_state_set_last_error(msg);
            } else {
                /*
                 * ⚠ url_for_log()：**不能**打整条 URL —— 账号接口的查询串里是登录 cookie。
                 * 顺手带上堆余量：TLS 的失败里有一半其实是"内部 RAM 没了"
                 * （-0x0084 AES DMA 分不出缓冲 / -0x008D ssl_setup 内存不足），
                 * 有这四列就能当场和网络错误区分开，不用再让用户重烧一遍加日志。
                 */
                ESP_LOGW(TAG, "GET failed (%s): %s | int free=%u max=%u dma max=%u psram=%u", 
                         esp_err_to_name(ret), url_for_log(cur),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                char msg[64];
                snprintf(msg, sizeof(msg), "网络错误 %s", esp_err_to_name(ret));
                os_state_set_last_error(msg);
            }
            free(sink.buf);
            return ret;
        }
        if (status != 200) {
            /*
             * ★ 2026-11：把**服务端那句原因**带出来。
             *   只报"HTTP 422"的时候，界面上只有"收藏失败：服务器返回 HTTP 422"，
             *   根本看不出是参数不对还是凭证过期（QQ 的 add_songs 就踩过：
             *   sidecar 的 list[int] 只认 song_id=，axios 默认写成 song_id[]=，
             *   于是每次都 422，而设备侧一个字都看不见）。
             *   两家接口的外壳都是 {"code":N,"msg":"…"}，优先取 msg；取不到就把
             *   body 开头压成一行（截断到 last_error 的 64 字节里放得下）。
             */
            char reason[40];
            http_reason_of(sink.buf, sink.len, reason, sizeof(reason));
            ESP_LOGW(TAG, "GET status %d: %s | %s", status, url_for_log(cur),
                     reason[0] ? reason : "-");
            char msg[64];
            if (reason[0]) {
                snprintf(msg, sizeof(msg), "HTTP %d：%s", status, reason);
            } else {
                snprintf(msg, sizeof(msg), "服务器返回 HTTP %d", status);
            }
            os_state_set_last_error(msg);
            free(sink.buf);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (sink.len == 0) {
            os_state_set_last_error("服务器返回空数据");
            free(sink.buf);
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out = sink.buf;                           /* 所有权交给调用方 */
        if (out_len) {
            *out_len = sink.len;
        }
        return ESP_OK;
    }
    os_state_set_last_error("重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}

/*
 * GET 的重试外壳（判据与根因见上面 get_err_retryable 的注释）。
 *
 * 三条纪律：
 *   1) **只重试瞬时失败**，服务器明确表态的（非 200 / 缺 Location / 超上限 / 没内存）
 *      立刻返回，别把失败延迟拖三倍；
 *   2) **保留已经解析出来的 URL**：meting 的 http 会 302 到 https，每次重新从 http
 *      起跳等于多打一个请求（首跳虽然是明文、很快，但也是一次完整建链）；
 *   3) **慢失败不再重试**：`timeout_ms` 是"两次读之间"的上限，一次读超时就是 30 s，
 *      叠三次等于 90 s —— 而起播任务 / 歌单刷新都在这条线程上等着。
 */
static esp_err_t http_request_retry(http_verb_t verb, const char *url,
                                    const char *hdr1_name, const char *hdr1_val,
                                    const char *hdr2_name, const char *hdr2_val,
                                    char **out, size_t *out_len, size_t max_len, int attempts)
{
    if (!url || !out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (attempts < 1) {
        attempts = 1;
    }

    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", url);
    os_state_set_last_error("");

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0) {
            const unsigned wait_ms = (unsigned)(HTTP_GET_RETRY_BACKOFF_MS * attempt);
            /* ⚠ url_for_log()：查询串里可能有登录 cookie，不能原样打（见它的注释） */
            ESP_LOGW(TAG, "GET retry %d/%d (%s, wait %u ms): %s", attempt, attempts - 1,
                     esp_err_to_name(ret), wait_ms, url_for_log(cur));
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
        const int64_t t0 = esp_timer_get_time();
        ret = http_get_hops(cur, sizeof(cur), verb, hdr1_name, hdr1_val, hdr2_name, hdr2_val,
                            out, out_len, max_len);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (!get_err_retryable(ret)) {
            return ret;
        }
        if ((esp_timer_get_time() - t0) / 1000 > HTTP_GET_RETRY_FAST_MS) {
            return ret;
        }
    }
    return ret;
}

esp_err_t http_get_text_hdr_ex(const char *url, const char *cookie, char **out,
                               size_t *out_len, size_t max_len, int attempts)
{
    return http_request_retry(HTTP_VERB_GET, url, "X-Netease-Cookie", cookie, NULL, NULL,
                              out, out_len, max_len, attempts);
}

esp_err_t http_request_ext(http_verb_t verb, const char *url,
                           const char *hdr1_name, const char *hdr1_val,
                           const char *hdr2_name, const char *hdr2_val,
                           char **out, size_t *out_len, size_t max_len)
{
    return http_request_retry(verb, url, hdr1_name, hdr1_val, hdr2_name, hdr2_val,
                              out, out_len, max_len, HTTP_GET_ATTEMPTS);
}

esp_err_t http_get_text_ex(const char *url, char **out, size_t *out_len, size_t max_len,
                           int attempts)
{
    return http_get_text_hdr_ex(url, NULL, out, out_len, max_len, attempts);
}

esp_err_t http_get_text_hdr(const char *url, const char *cookie, char **out, size_t *out_len,
                            size_t max_len)
{
    return http_get_text_hdr_ex(url, cookie, out, out_len, max_len, HTTP_GET_ATTEMPTS);
}

esp_err_t http_get_text(const char *url, char **out, size_t *out_len, size_t max_len)
{
    return http_get_text_hdr_ex(url, NULL, out, out_len, max_len, HTTP_GET_ATTEMPTS);
}

esp_err_t http_download(const char *url, const char *dest, size_t min_size,
                        uint8_t sniff[16])
{
    if (sniff) {
        memset(sniff, 0, 16);
    }
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", url);
    os_state_set_last_error("");

    /* 同样自己处理重定向（音频地址是 302 到 https CDN，跨主机 + 跨协议）。
     * 先探测拿到最终地址，再去下载；避免"下载函数里再套一层循环"把文件写乱。 */
    for (int hop = 0; hop < 4; hop++) {
        http_sink_t probe = {};        /* 只要 Location，不接 body（buf 留 NULL） */
        esp_http_client_config_t cfg = {};
        cfg.url                   = cur;
        cfg.timeout_ms            = HTTP_TIMEOUT_MS;
        cfg.crt_bundle_attach     = esp_crt_bundle_attach;
        cfg.disable_auto_redirect = true;
        cfg.keep_alive_enable     = false;
        /*
         * ★ buffer_size 默认只有 512 字节，而它要装下**整个请求行 + 请求头**。
         *   账号接口把 cookie 塞在 URL 里（MUSIC_U 一个值就 ~550 字符），稍不留神
         *   就超过 512 → 日志 `E HTTP_CLIENT: Out of buffer` + `GET failed (ESP_FAIL)`，
         *   表现为"扫码 803 成功了，但账号信息/歌单全拿不到"（2026-10 真机踩了两轮）。
         *   给到 4 KB，和 static url[NET_URL_MAX = 4096] 对齐。
         */
        cfg.buffer_size           = 4096;
        /*
         * ★★ 光是 buffer_size 不够！IDF 的 esp_http_client.c:1783 把**请求行**
         *   （method + path + "?" + query）拼进 `client->buffer_size_tx`，超了就报
         *   `E HTTP_CLIENT: Out of buffer` 并直接返回 -1 —— 而 **buffer_size_tx 是独立
         *   字段**，默认只有 512，跟 buffer_size 无关。账号接口把整条 cookie 放在 query
         *   里（~1 KB），必须显式把它也放大，否则请求根本发不出去（真机连踩两轮）。
         */
        cfg.buffer_size_tx        = 4096;
        cfg.event_handler         = http_evt;
        cfg.user_data             = &probe;

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            return ESP_FAIL;
        }
        esp_err_t ret = esp_http_client_open(cli, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "open url failed: %s (%s)", esp_err_to_name(ret), url_for_log(cur));
            char msg[64];
            snprintf(msg, sizeof(msg), "网络错误 %s", esp_err_to_name(ret));
            os_state_set_last_error(msg);
            esp_http_client_cleanup(cli);
            return ret;
        }
        esp_http_client_fetch_headers(cli);
        const int status = esp_http_client_get_status_code(cli);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char loc[512] = {0};
            snprintf(loc, sizeof(loc), "%s", probe.location);
            esp_http_client_cleanup(cli);
            if (!loc[0]) {
                os_state_set_last_error("重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            os_http_join_location(cur, loc, cur, sizeof(cur));
            ESP_LOGI(TAG, "download redirect -> %s", cur);
            continue;
        }

        if (status != 200) {
            ESP_LOGW(TAG, "download status %d: %s", status, url_for_log(cur));
            char msg[64];
            snprintf(msg, sizeof(msg), "服务器返回 HTTP %d（版权受限？）", status);
            os_state_set_last_error(msg);
            esp_http_client_cleanup(cli);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const int64_t content_len = esp_http_client_get_content_length(cli);
        if (content_len > 0 && (uint64_t)content_len > AUDIO_MAX_BYTES) {
            esp_http_client_cleanup(cli);
            os_state_set_last_error("文件过大");
            return ESP_ERR_INVALID_SIZE;
        }

        FILE *f = fopen(dest, "wb");
        if (!f) {
            ESP_LOGE(TAG, "open %s failed", dest);
            os_state_set_last_error("无法写入 SD 卡");
            esp_http_client_cleanup(cli);
            return ESP_FAIL;
        }
        uint8_t buf[HTTP_BUF];
        uint64_t total = 0;
        bool sniffed = false;
        ret = ESP_OK;
        for (;;) {
            const int r = esp_http_client_read(cli, (char *)buf, sizeof(buf));
            if (r < 0) {
                ret = ESP_FAIL;
                break;
            }
            if (r == 0) {
                break;                     /* 正常结束 */
            }
            if (total + (uint64_t)r > AUDIO_MAX_BYTES) {
                ret = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (!sniffed && sniff) {
                memcpy(sniff, buf, (size_t)(r < 16 ? r : 16));
                sniffed = true;
            }
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
                ret = ESP_FAIL;            /* SD 写失败（卡满/拔出） */
                os_state_set_last_error("SD 卡写入失败");
                break;
            }
            total += (uint64_t)r;
        }
        esp_http_client_cleanup(cli);
        fclose(f);

        if (ret != ESP_OK || total < min_size) {
            ESP_LOGW(TAG, "download incomplete: %s (%llu bytes)",
                     dest, (unsigned long long)total);
            unlink(dest);
            if (ret == ESP_OK) {
                char msg[64];
                snprintf(msg, sizeof(msg), "下载不完整（%llu 字节）", (unsigned long long)total);
                os_state_set_last_error(msg);
                return ESP_ERR_INVALID_SIZE;
            }
            return ret;
        }
        return ESP_OK;
    }
    os_state_set_last_error("重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}
