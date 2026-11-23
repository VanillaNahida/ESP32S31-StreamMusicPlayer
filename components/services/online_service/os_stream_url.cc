/*
 * os_stream_url.cc —— 302 解析链：拿到音频最终直链（+ 估时长）
 *
 * 逐字搬自原 online_service.cc：
 *   mp3_bitrate_bps          1504–1524
 *   online_song_stream_info  1532–1654
 *   online_song_stream_url   1656–1741
 *
 * ★ 这条链路上的东西**一律不许改**（拆分要求点名）：
 *   - `esp_http_client_open` + `fetch_headers` + `get_status_code` + 手动重定向；
 *   - 每条错误文案（"重定向缺少 Location" / "服务器返回 HTTP %d（版权受限？）" /
 *     "重定向次数过多"）——它们会经 online_last_error() 变成 UI 的 Toast；
 *   - 四处 `http_sink_t probe` 的 cfg（timeout 15000 / buffer_size 4096 /
 *     buffer_size_tx 4096 / disable_auto_redirect / keep_alive false）。
 *
 * ⚠ 与 http_get_text/http_download 的**已知不一致**（蓝图 R2）：这里（还有
 *   online_song_stream_url）**不处理相对 Location**，直接用 Location 原文。
 *   本次拆分**照搬这个不一致**（零行为变化），不在重构里顺手统一 —— 要统一得单独
 *   评估并单测 `Location: /x/y`（网易云 CDN 目前给的都是绝对地址）。
 */
#include "internal/os_stream_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "internal/os_fs.h"
#include "internal/os_http_client.h"
#include "internal/os_qq.h"
#include "internal/os_state.h"
#include "online_service.h"

static const char *TAG = "online";

/* 定义在 os_account.cc（带 cookie 取直链，对外契约见 online_service.h） */
extern "C" esp_err_t online_netease_song_url(const char *song_id, char *out, size_t cap,
                                             int64_t *out_bytes, uint32_t *out_dur_ms);

/* MP3 帧头 → 码率(bps)，解析失败返回 0。表格照 ISO/IEC 11172-3 Layer III。 */
int os_mp3_bitrate_bps(const uint8_t *p, size_t n)
{
    static const int v1l3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int v2l3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    for (size_t i = 0; i + 4 <= n; i++) {
        if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        const int ver = (p[i + 1] >> 3) & 0x03;    /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
        const int layer = (p[i + 1] >> 1) & 0x03;  /* 1 = Layer III */
        const int br_idx = (p[i + 2] >> 4) & 0x0F;
        if (layer != 1 || br_idx == 0 || br_idx == 15) {
            continue;
        }
        const int kbps = (ver == 3) ? v1l3[br_idx] : v2l3[br_idx];
        if (kbps > 0) {
            return kbps * 1000;
        }
    }
    return 0;
}

/*
 * 流地址 + 时长估算：解析 302 拿到最终地址，并顺手读一段前缀拿 MP3 码率，
 * 配合响应里的 Content-Length 估出总时长（在线流式播放拿不到 duration_ms，
 * GMF 的 music info 一直是 "0 bps, 0 ms"，所以列表/播放器页以前只显示 --:--）。
 * 只多读 4 KB，代价可以忽略。
 */
esp_err_t online_song_stream_info(const online_song_t *song, char *out, size_t cap,
                                  int64_t *out_bytes, uint32_t *out_dur_ms)
{
    if (out_bytes) {
        *out_bytes = 0;
    }
    if (out_dur_ms) {
        *out_dur_ms = 0;
    }
    if (!song || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * ★ QQ 音乐是**另一条**取流链路（2026-11）：
     *   服务端 /qq/song/{mid}/play_url 已经把 CDN 拼好了，直接就是最终地址，
     *   既没有 302、也没有"码率×长度"可算 —— 时长直接用元数据里的 dur_sec。
     *   拿不到音源（无版权 / 非 VIP）返回 ESP_ERR_NOT_FOUND，由上层提示，
     *   **不要**再退回下面的网易云逻辑（那个 id 是 QQ 的，会取到别人的歌）。
     */
    if (song->src == ONLINE_SRC_QQ) {
        return online_qq_song_url(song, out, cap, out_bytes, out_dur_ms);
    }

    /*
     * ★★ 一律先走"**当前登录 CK + 自建中间件**"拿直链（2026-10 用户要求）。
     *
     * 为什么不能再用 meting 的 url：它是**匿名**请求，会员 / 数字专辑歌曲只会下发
     * **30 秒试听片段**（真机实测 Content-Length 439 KB ≈ 128 kbps 的 27 秒，
     * 而元数据时长是整首 —— 顺带把 seek 映射、进度条、"提前结束"判据全带偏了）。
     * 带 CK 请求 `/song/url/v1` 拿到的才是**完整**直链，会员歌就能整首听。
     *
     * ⚠ 2026-11：meting 代理已下线（/meting/api* 一律 404），所以这里的兜底
     * 只剩"歌记录里存的旧 audio_url"—— 那是旧歌单文件（`/sdcard/online/pl_*.bin`）
     * 里带的历史字段，**新抓的歌单不会再填它**。保留它只是为了让老歌单里的歌
     * 还能放（那种地址会过期，403 由 app 层重刷链路兜）。
     */
    if (song->id != 0) {
        char sid[16];
        snprintf(sid, sizeof(sid), "%u", (unsigned)song->id);
        if (online_netease_song_url(sid, out, cap, out_bytes, out_dur_ms) == ESP_OK) {
            ESP_LOGI(TAG, "stream: 账号直链(CK) id=%s, %lld B, %u ms", sid,
                     (long long)(out_bytes ? *out_bytes : 0),
                     (unsigned)(out_dur_ms ? *out_dur_ms : 0));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stream: 账号直链(CK) 拿不到 id=%s，退回歌记录里的旧地址", sid);
    }

    if (!song->audio_url[0]) {
        /* 既没有 CK 直链、也没有旧地址：只能报"找不到" */
        return ESP_ERR_NOT_FOUND;
    }
    out[0] = '\0';
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    char cur[512];
    snprintf(cur, sizeof(cur), "%s", song->audio_url);
    os_state_set_last_error("");

    for (int hop = 0; hop < 4; hop++) {
        http_sink_t probe = {};
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
            ESP_LOGW(TAG, "stream probe open failed: %s (%s)", esp_err_to_name(ret), cur);
            char msg[64];
            snprintf(msg, sizeof(msg), "网络错误 %s", esp_err_to_name(ret));
            os_state_set_last_error(msg);
            esp_http_client_cleanup(cli);
            return ret;
        }
        const int64_t total = esp_http_client_fetch_headers(cli);
        const int status = esp_http_client_get_status_code(cli);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char loc[512] = {0};
            snprintf(loc, sizeof(loc), "%s", probe.location);
            esp_http_client_cleanup(cli);
            if (!loc[0]) {
                os_state_set_last_error("重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            snprintf(cur, sizeof(cur), "%s", loc);
            continue;
        }
        if (status != 200) {
            esp_http_client_cleanup(cli);
            char msg[64];
            snprintf(msg, sizeof(msg), "服务器返回 HTTP %d（版权受限？）", status);
            os_state_set_last_error(msg);
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* 读 4 KB 前缀：只为了拿 MP3 码率，读完就断开。
         * PSRAM 优先：4 KB 正好卡在 SPIRAM_MALLOC_ALWAYSINTERNAL(4096) 的边界上，
         * 用 malloc 会落内部 RAM；这块只是"读一遍就扔"的批量数据（2026-11-16）。 */
        uint8_t *head = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!head) {
            head = (uint8_t *)malloc(4096);
        }
        int got = 0;
        if (head) {
            got = esp_http_client_read(cli, (char *)head, 4096);
            if (got < 0) {
                got = 0;
            }
            if (out_dur_ms && total > 0 && got > 0) {
                const int br = os_mp3_bitrate_bps(head, (size_t)got);
                if (br > 0) {
                    *out_dur_ms = (uint32_t)((uint64_t)total * 8ULL * 1000ULL / (uint64_t)br);
                    ESP_LOGI(TAG, "stream duration est: %lld B @ %d bps -> %u ms",
                             (long long)total, br, (unsigned)*out_dur_ms);
                }
            }
            free(head);
        }
        esp_http_client_cleanup(cli);

        if (out_bytes) {
            *out_bytes = total;
        }
        snprintf(out, cap, "%s", cur);
        return ESP_OK;
    }
    os_state_set_last_error("重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t online_song_stream_url(const online_song_t *song, char *out, size_t cap)
{
    if (!song || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* QQ 音乐：一条道路走到底（见 online_song_stream_info 里的说明） */
    if (song->src == ONLINE_SRC_QQ) {
        return online_qq_song_url(song, out, cap, NULL, NULL);
    }

    /* ★ 同上：先用"登录 CK + 中间件"拿直链，失败才退回旧地址（说明见 stream_info） */
    if (song->id != 0) {
        char sid[16];
        snprintf(sid, sizeof(sid), "%u", (unsigned)song->id);
        if (online_netease_song_url(sid, out, cap, NULL, NULL) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stream url: 账号直链(CK) 拿不到 id=%s，退回歌记录里的旧地址", sid);
    }
    if (!song->audio_url[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    out[0] = '\0';
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    char cur[512];
    snprintf(cur, sizeof(cur), "%s", song->audio_url);
    os_state_set_last_error("");

    for (int hop = 0; hop < 4; hop++) {
        http_sink_t probe = {};           /* 只要 Location，不接 body */
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
            ESP_LOGW(TAG, "stream probe open failed: %s (%s)", esp_err_to_name(ret), cur);
            char msg[64];
            snprintf(msg, sizeof(msg), "网络错误 %s", esp_err_to_name(ret));
            os_state_set_last_error(msg);
            esp_http_client_cleanup(cli);
            return ret;
        }
        esp_http_client_fetch_headers(cli);
        const int status = esp_http_client_get_status_code(cli);
        esp_http_client_cleanup(cli);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (!probe.location[0]) {
                os_state_set_last_error("重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            snprintf(cur, sizeof(cur), "%s", probe.location);
            ESP_LOGI(TAG, "stream url -> %s", cur);
            continue;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "stream probe status %d: %s", status, cur);
            char msg[64];
            snprintf(msg, sizeof(msg), "服务器返回 HTTP %d（版权受限？）", status);
            os_state_set_last_error(msg);
            return ESP_ERR_INVALID_RESPONSE;
        }
        snprintf(out, cap, "%s", cur);
        return ESP_OK;
    }
    os_state_set_last_error("重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}
