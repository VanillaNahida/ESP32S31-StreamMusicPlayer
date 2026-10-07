/*
 * online_service.cc —— 在线音乐（网易云歌单 / Meting API）实现
 *
 * 二进制歌单文件格式（定长，方便直接 mmap/定点读写，也符合"二进制保存"的需求）：
 *   struct online_pl_hdr_t { uint32_t magic; uint32_t version; uint32_t count; uint32_t rsv; }
 *   紧接着 count 条 online_rec_t（定长 896 字节）
 * 小端（ESP32 本身就是小端，不做额外字节序转换）。
 *
 * HTTP 说明：
 *   - meting 的 API/歌词/封面都支持 **http**，走明文省掉 TLS 与时间同步依赖；
 *   - 音频是 302 到 https CDN，必须 TLS 校验（证书包已开），
 *     所以下载音频前要确认系统时间已经同步（SNTP 在 WiFi 拿到 IP 后启动）。
 */
#include "online_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_service.h"    /* online_has_network() */

static const char *TAG = "online";

#define ONLINE_ROOT        "/sdcard/online"
#define ONLINE_AUDIO_DIR   ONLINE_ROOT "/audio"
#define ONLINE_COVER_DIR   ONLINE_ROOT "/cover"
#define ONLINE_LRC_DIR     ONLINE_ROOT "/lrc"
#define ONLINE_PL_FMT      ONLINE_ROOT "/pl_%s.bin"

#define NVS_NS             "online"
#define NVS_KEY_CNT        "n"
#define NVS_KEY_ID_FMT     "id%d"      /* id0..id7 */

#define API_BASE           "http://meting.xcnahida.cn/api"
/* 搜索接口在另一个路径上，而且关键词放在 id= 里：
 *   http://meting.xcnahida.cn/meting/api?server=netease&type=search&id=<关键词>
 * （/api?type=search&keywords=xxx 在该实例上会忽略关键词、返回固定列表，别用） */
#define API_SEARCH_BASE    "http://meting.xcnahida.cn/meting/api"
#define HTTP_TIMEOUT_MS    15000
#define HTTP_BUF           2048
/* 单个 JSON 响应上限（歌单 1000 首 ≈ 300 KB），别让一个坏响应吃光内存 */
#define JSON_MAX_LEN       (1024u * 1024u)
/* 音频上限（防止下到无穷无尽的流）：250 MB */
#define AUDIO_MAX_BYTES    (250u * 1024u * 1024u)
/* 音频缓存最多留多少个文件，超了删最旧的 */
#define AUDIO_CACHE_MAX    48

#define ONLINE_PL_MAGIC    0x4F504C31u   /* 'OPL1' */
#define ONLINE_PL_VERSION  1u

/* 落盘用的定长记录 */
typedef struct {
    uint32_t id;
    char     title[ONLINE_TITLE_MAX];
    char     artist[ONLINE_ARTIST_MAX];
    char     cover_url[ONLINE_URL_MAX];
    char     audio_url[ONLINE_URL_MAX];
    char     lrc_url[ONLINE_URL_MAX];
    uint8_t  rsv[64];
} online_rec_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t rsv;
} online_pl_hdr_t;

static bool  s_ready;
static char  s_ids[ONLINE_MAX_PLAYLISTS][ONLINE_ID_MAX];
static size_t s_id_count;
/* 最近一次失败的可读原因（给 UI 的 Toast 用） */
static char  s_last_err[64];

/* ---------------------------------------------------------------- 小工具 */

static void str_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;                       /* 不要截在多字节字符中间 */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* 从 meting 的 url 字段里抠出歌曲 id：...&id=123456 */
static uint32_t id_from_url(const char *url)
{
    if (!url) {
        return 0;
    }
    const char *p = strstr(url, "id=");
    if (!p) {
        return 0;
    }
    return (uint32_t)strtoul(p + 3, NULL, 10);
}

static bool file_ok(const char *path, size_t min_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return (size_t)st.st_size >= min_size;
}

static esp_err_t mkdir_if_needed(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    return (mkdir(path, 0777) == 0) ? ESP_OK : ESP_FAIL;
}

/* ---------------------------------------------------------------- HTTP */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    char   location[512];    /* 301/302 的 Location：响应头只能从事件里拿
                              * （esp_http_client_get_header() 取的是**请求**头，
                              *   实测在 perform() 之后取 Location 一直是空的） */
} http_sink_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
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
    if (e->event_id == HTTP_EVENT_ON_DATA && s->buf) {
        if (s->len + (size_t)e->data_len >= s->cap) {
            return ESP_FAIL;              /* 超上限：让上层收尾报错 */
        }
        memcpy(s->buf + s->len, e->data, (size_t)e->data_len);
        s->len += (size_t)e->data_len;
        s->buf[s->len] = '\0';
    }
    return ESP_OK;
}

/*
 * GET 一段文本（JSON / LRC）。成功时 *out 为 malloc 的字符串（NUL 结尾）。
 *
 * ★ 重定向自己处理，不用 esp_http_client 的自动跳转：
 *   这个实例把 http://meting.xcnahida.cn/api?... 302 到 https 同一路径，
 *   而 IDF 的自动跳转在实测里会把查询串弄丢（服务器直接回 400，
 *   日志就是 "GET status 400: http://meting.xcnahida.cn/api...playlist..."）。
 *   我们改成 disable_auto_redirect + 自己读 Location 再发一次，
 *   顺便把每一跳打进日志，方便定位。
 */
static esp_err_t http_get_text(const char *url, char **out, size_t *out_len)
{
    if (!url || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }

    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", url);
    s_last_err[0] = '\0';

    for (int hop = 0; hop < 4; hop++) {
        http_sink_t sink = {};
        sink.cap = JSON_MAX_LEN;
        sink.buf = (char *)malloc(sink.cap);
        if (!sink.buf) {
            return ESP_ERR_NO_MEM;
        }
        sink.buf[0] = '\0';

        esp_http_client_config_t cfg = {};
        cfg.url                   = cur;
        cfg.event_handler         = http_evt;
        cfg.user_data             = &sink;
        cfg.timeout_ms            = HTTP_TIMEOUT_MS;
        cfg.crt_bundle_attach     = esp_crt_bundle_attach;
        cfg.disable_auto_redirect = true;
        cfg.keep_alive_enable     = false;

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            free(sink.buf);
            return ESP_FAIL;
        }
        esp_err_t ret = esp_http_client_perform(cli);
        const int status = esp_http_client_get_status_code(cli);

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
                snprintf(s_last_err, sizeof(s_last_err), "重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (loc[0] == '/') {
                /* 相对跳转：挂回原主机 */
                char host[256] = {0};
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
                snprintf(cur, sizeof(cur), "%.*s%.*s", (int)sizeof(host) - 1, host,
                         (int)sizeof(loc) - 1, loc);
            } else {
                snprintf(cur, sizeof(cur), "%s", loc);
            }
            ESP_LOGI(TAG, "redirect -> %s", cur);
            continue;
        }

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GET failed (%s): %s", esp_err_to_name(ret), cur);
            snprintf(s_last_err, sizeof(s_last_err), "网络错误 %s", esp_err_to_name(ret));
            free(sink.buf);
            return ret;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "GET status %d: %s", status, cur);
            snprintf(s_last_err, sizeof(s_last_err), "服务器返回 HTTP %d", status);
            free(sink.buf);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (sink.len == 0) {
            snprintf(s_last_err, sizeof(s_last_err), "服务器返回空数据");
            free(sink.buf);
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out = sink.buf;                           /* 所有权交给调用方 */
        if (out_len) {
            *out_len = sink.len;
        }
        return ESP_OK;
    }
    snprintf(s_last_err, sizeof(s_last_err), "重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}

/*
 * 下载二进制到文件。自动跟随 302；成功条件是 HTTP 200 且落盘字节数 >= min_size。
 * 返回时把真实内容的前 16 字节放到 sniff（用于判断容器格式）。
 */
static esp_err_t http_download(const char *url, const char *dest, size_t min_size,
                               uint8_t sniff[16])
{
    if (sniff) {
        memset(sniff, 0, 16);
    }
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", url);
    s_last_err[0] = '\0';

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
        cfg.event_handler         = http_evt;
        cfg.user_data             = &probe;

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            return ESP_FAIL;
        }
        esp_err_t ret = esp_http_client_open(cli, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "open url failed: %s (%s)", esp_err_to_name(ret), cur);
            snprintf(s_last_err, sizeof(s_last_err), "网络错误 %s", esp_err_to_name(ret));
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
                snprintf(s_last_err, sizeof(s_last_err), "重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (loc[0] == '/') {
                char host[256] = {0};
                const char *se = strstr(cur, "://");
                const char *p = se ? strchr(se + 3, '/') : NULL;
                if (p) {
                    size_t n = (size_t)(p - cur);
                    if (n >= sizeof(host)) {
                        n = sizeof(host) - 1;
                    }
                    memcpy(host, cur, n);
                    host[n] = '\0';
                }
                snprintf(cur, sizeof(cur), "%.*s%.*s", (int)sizeof(host) - 1, host,
                         (int)sizeof(loc) - 1, loc);
            } else {
                snprintf(cur, sizeof(cur), "%s", loc);
            }
            ESP_LOGI(TAG, "download redirect -> %s", cur);
            continue;
        }

        if (status != 200) {
            ESP_LOGW(TAG, "download status %d: %s", status, cur);
            snprintf(s_last_err, sizeof(s_last_err), "服务器返回 HTTP %d（版权受限？）", status);
            esp_http_client_cleanup(cli);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const int64_t content_len = esp_http_client_get_content_length(cli);
        if (content_len > 0 && (uint64_t)content_len > AUDIO_MAX_BYTES) {
            esp_http_client_cleanup(cli);
            snprintf(s_last_err, sizeof(s_last_err), "文件过大");
            return ESP_ERR_INVALID_SIZE;
        }

        FILE *f = fopen(dest, "wb");
        if (!f) {
            ESP_LOGE(TAG, "open %s failed", dest);
            snprintf(s_last_err, sizeof(s_last_err), "无法写入 SD 卡");
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
                snprintf(s_last_err, sizeof(s_last_err), "SD 卡写入失败");
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
                snprintf(s_last_err, sizeof(s_last_err), "下载不完整（%llu 字节）",
                         (unsigned long long)total);
                return ESP_ERR_INVALID_SIZE;
            }
            return ret;
        }
        return ESP_OK;
    }
    snprintf(s_last_err, sizeof(s_last_err), "重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}

/* ---------------------------------------------------------------- 歌单 NVS */

static void nvs_load_ids(void)
{
    nvs_handle_t h;
    s_id_count = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    if (nvs_get_u8(h, NVS_KEY_CNT, &n) == ESP_OK) {
        if (n > ONLINE_MAX_PLAYLISTS) {
            n = ONLINE_MAX_PLAYLISTS;
        }
        for (uint8_t i = 0; i < n; i++) {
            char key[12];
            snprintf(key, sizeof(key), NVS_KEY_ID_FMT, i);
            size_t len = sizeof(s_ids[i]);
            if (nvs_get_str(h, key, s_ids[i], &len) == ESP_OK && s_ids[i][0]) {
                s_id_count++;
            }
        }
    }
    nvs_close(h);
}

static void nvs_save_ids(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_CNT, (uint8_t)s_id_count);
    for (size_t i = 0; i < ONLINE_MAX_PLAYLISTS; i++) {
        char key[12];
        snprintf(key, sizeof(key), NVS_KEY_ID_FMT, (int)i);
        if (i < s_id_count) {
            nvs_set_str(h, key, s_ids[i]);
        } else {
            nvs_erase_key(h, key);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static int id_index(const char *id)
{
    for (size_t i = 0; i < s_id_count; i++) {
        if (strcmp(s_ids[i], id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- 歌单落盘 */

static void pl_path(const char *id, char *out, size_t cap)
{
    char safe[ONLINE_ID_MAX];
    str_copy(safe, sizeof(safe), id);
    /* 用 %.*s 明确给出长度上限：这样编译器能证明不会截断
     * （直接传任意长度字符串会触发 -Werror=format-truncation） */
    snprintf(out, cap, ONLINE_ROOT "/pl_%.*s.bin", (int)(sizeof(safe) - 1), safe);
}

/* 把内存里的歌曲数组写成二进制文件 */
static esp_err_t pl_save(const char *id, const online_song_t *songs, size_t count)
{
    char path[128];
    pl_path(id, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "create %s failed", path);
        return ESP_FAIL;
    }
    online_pl_hdr_t hdr = {};
    hdr.magic   = ONLINE_PL_MAGIC;
    hdr.version = ONLINE_PL_VERSION;
    hdr.count   = (uint32_t)count;
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < count; i++) {
        online_rec_t rec = {};
        rec.id = songs[i].id;
        str_copy(rec.title, sizeof(rec.title), songs[i].title);
        str_copy(rec.artist, sizeof(rec.artist), songs[i].artist);
        str_copy(rec.cover_url, sizeof(rec.cover_url), songs[i].cover_url);
        str_copy(rec.audio_url, sizeof(rec.audio_url), songs[i].audio_url);
        str_copy(rec.lrc_url, sizeof(rec.lrc_url), songs[i].lrc_url);
        if (fwrite(&rec, sizeof(rec), 1, f) != 1) {
            fclose(f);
            unlink(path);
            return ESP_FAIL;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "playlist %s saved: %u songs, %u bytes",
             path, (unsigned)count, (unsigned)(sizeof(hdr) + count * sizeof(online_rec_t)));
    return ESP_OK;
}

/* 读二进制文件到内存数组 */
static esp_err_t pl_load(const char *id, online_song_t **out, size_t *out_count)
{
    char path[128];
    pl_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    online_pl_hdr_t hdr = {};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != ONLINE_PL_MAGIC ||
        hdr.version != ONLINE_PL_VERSION || hdr.count == 0 || hdr.count > 5000) {
        fclose(f);
        ESP_LOGW(TAG, "%s: bad header", path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    online_song_t *arr = (online_song_t *)psram_alloc((size_t)hdr.count * sizeof(online_song_t));
    if (!arr) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        online_rec_t rec = {};
        if (fread(&rec, sizeof(rec), 1, f) != 1) {
            break;
        }
        online_song_t *s = &arr[got];
        memset(s, 0, sizeof(*s));
        s->id = rec.id;
        str_copy(s->title, sizeof(s->title), rec.title);
        str_copy(s->artist, sizeof(s->artist), rec.artist);
        str_copy(s->cover_url, sizeof(s->cover_url), rec.cover_url);
        str_copy(s->audio_url, sizeof(s->audio_url), rec.audio_url);
        str_copy(s->lrc_url, sizeof(s->lrc_url), rec.lrc_url);
        if (s->title[0]) {
            got++;
        }
    }
    fclose(f);
    if (got == 0) {
        free(arr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = arr;
    *out_count = got;
    return ESP_OK;
}

/* ---------------------------------------------------------------- 抓歌单 */

/* 解析 meting 返回的 JSON 数组 → 歌曲数组 */
static esp_err_t parse_songs(const char *json, online_song_t **out, size_t *out_count)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        ESP_LOGW(TAG, "bad JSON from meting");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int n = cJSON_GetArraySize(root);
    if (n <= 0) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    online_song_t *arr = (online_song_t *)psram_alloc((size_t)n * sizeof(online_song_t));
    if (!arr) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(root, i);
        if (!it) {
            continue;
        }
        cJSON *jt = cJSON_GetObjectItem(it, "title");
        cJSON *ja = cJSON_GetObjectItem(it, "author");
        cJSON *jp = cJSON_GetObjectItem(it, "pic");
        cJSON *ju = cJSON_GetObjectItem(it, "url");
        cJSON *jl = cJSON_GetObjectItem(it, "lrc");
        if (!cJSON_IsString(jt) || !cJSON_IsString(ju)) {
            continue;
        }
        online_song_t *s = &arr[got];
        memset(s, 0, sizeof(*s));
        str_copy(s->title, sizeof(s->title), jt->valuestring);
        str_copy(s->artist, sizeof(s->artist), cJSON_IsString(ja) ? ja->valuestring : "");
        str_copy(s->cover_url, sizeof(s->cover_url), cJSON_IsString(jp) ? jp->valuestring : "");
        str_copy(s->audio_url, sizeof(s->audio_url), ju->valuestring);
        str_copy(s->lrc_url, sizeof(s->lrc_url), cJSON_IsString(jl) ? jl->valuestring : "");
        s->id = id_from_url(s->audio_url);
        if (s->title[0]) {
            got++;
        }
    }
    cJSON_Delete(root);
    if (got == 0) {
        free(arr);
        return ESP_ERR_NOT_FOUND;
    }
    *out = arr;
    *out_count = got;
    return ESP_OK;
}

esp_err_t online_playlist_add(const char *id)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    snprintf(url, sizeof(url), API_BASE "?server=netease&type=playlist&id=%s", id);

    char *json = NULL;
    esp_err_t ret = http_get_text(url, &json, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    online_song_t *songs = NULL;
    size_t count = 0;
    ret = parse_songs(json, &songs, &count);
    free(json);
    if (ret != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "歌单解析失败（空歌单或格式变了？）");
        return ret;
    }

    ret = pl_save(id, songs, count);
    free(songs);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 记进 NVS（已存在就只算刷新） */
    if (id_index(id) < 0) {
        if (s_id_count >= ONLINE_MAX_PLAYLISTS) {
            return ESP_ERR_NO_MEM;
        }
        str_copy(s_ids[s_id_count], sizeof(s_ids[0]), id);
        s_id_count++;
        nvs_save_ids();
    }
    ESP_LOGI(TAG, "playlist %s: %u songs", id, (unsigned)count);
    return ESP_OK;
}

esp_err_t online_playlist_remove(const char *id)
{
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }
    const int idx = id_index(id);
    if (idx >= 0) {
        for (size_t i = (size_t)idx + 1; i < s_id_count; i++) {
            str_copy(s_ids[i - 1], sizeof(s_ids[0]), s_ids[i]);
        }
        s_id_count--;
        nvs_save_ids();
    }
    char path[128];
    pl_path(id, path, sizeof(path));
    unlink(path);
    return ESP_OK;
}

esp_err_t online_playlist_load(const char *id, online_song_t **songs, size_t *count)
{
    if (!id || !songs || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = pl_load(id, songs, count);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* 本地没有：抓一次再读（顺带落盘） */
        ret = online_playlist_add(id);
        if (ret == ESP_OK) {
            ret = pl_load(id, songs, count);
        }
    }
    return ret;
}

/* ---------------------------------------------------------------- 搜索 */

/* UTF-8 关键词 → 百分号编码（只放行 RFC3986 的 unreserved 字符）。
 * 中文关键词会被编码成 %E5%91%A8...，服务器那边按 UTF-8 解。 */
static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < cap; p++) {
        const unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

/* 本地兜底：在所有已保存歌单里按子串找（不需要网络） */
static esp_err_t search_local(const char *keyword, online_song_t **out, size_t *count, size_t max,
                              online_song_t *acc)
{
    size_t n = 0;
    for (size_t p = 0; p < s_id_count && n < max; p++) {
        char path[128];
        pl_path(s_ids[p], path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        online_pl_hdr_t hdr = {};
        if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != ONLINE_PL_MAGIC) {
            fclose(f);
            continue;
        }
        for (uint32_t i = 0; i < hdr.count && n < max; i++) {
            online_rec_t rec = {};
            if (fread(&rec, sizeof(rec), 1, f) != 1) {
                break;
            }
            if (!rec.title[0]) {
                continue;
            }
            if (strcasestr(rec.title, keyword) || strcasestr(rec.artist, keyword)) {
                online_song_t *s = &acc[n];
                memset(s, 0, sizeof(*s));
                s->id = rec.id;
                str_copy(s->title, sizeof(s->title), rec.title);
                str_copy(s->artist, sizeof(s->artist), rec.artist);
                str_copy(s->cover_url, sizeof(s->cover_url), rec.cover_url);
                str_copy(s->audio_url, sizeof(s->audio_url), rec.audio_url);
                str_copy(s->lrc_url, sizeof(s->lrc_url), rec.lrc_url);
                n++;
            }
        }
        fclose(f);
    }
    if (n == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = acc;
    *count = n;
    ESP_LOGI(TAG, "local search '%s': %u hit(s)", keyword, (unsigned)n);
    return ESP_OK;
}

/*
 * 搜索歌曲：**先走服务端搜索**（Meting 的 type=search，关键词放 id=），
 * 失败或没网时退化成"扫本地已保存歌单"。
 */
esp_err_t online_search(const char *keyword, online_song_t **out, size_t *count, size_t max)
{
    if (!keyword || !out || !count || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *count = 0;

    online_song_t *acc = (online_song_t *)psram_alloc(max * sizeof(online_song_t));
    if (!acc) {
        return ESP_ERR_NO_MEM;
    }

    if (online_has_network()) {
        char enc[192];
        url_encode(keyword, enc, sizeof(enc));
        char url[320];
        snprintf(url, sizeof(url), API_SEARCH_BASE "?server=netease&type=search&id=%s", enc);
        char *json = NULL;
        if (http_get_text(url, &json, NULL) == ESP_OK) {
            online_song_t *arr = NULL;
            size_t n = 0;
            const esp_err_t ret = parse_songs(json, &arr, &n);
            free(json);
            if (ret == ESP_OK && n > 0) {
                if (n > max) {
                    n = max;                 /* 只要前 max 条 */
                }
                *out = arr;
                *count = n;
                ESP_LOGI(TAG, "search '%s': %u hit(s) from server", keyword, (unsigned)n);
                free(acc);
                return ESP_OK;
            }
            free(arr);
            ESP_LOGW(TAG, "server search failed, fallback to local");
        }
    }

    const esp_err_t ret = search_local(keyword, out, count, max, acc);
    if (ret != ESP_OK) {
        free(acc);
    }
    return ret;
}

/* ---------------------------------------------------------------- 音频/封面/歌词 */

static void ext_from_sniff(const uint8_t *sniff, char *ext, size_t cap)
{
    const char *e = ".mp3";                       /* 默认按 mp3 处理 */
    if (sniff) {
        if (memcmp(sniff, "fLaC", 4) == 0) {
            e = ".flac";
        } else if (memcmp(sniff, "OggS", 4) == 0) {
            e = ".ogg";
        } else if (memcmp(sniff, "RIFF", 4) == 0) {
            e = ".wav";
        } else if (memcmp(sniff + 4, "ftyp", 4) == 0) {
            e = ".m4a";
        } else if (memcmp(sniff, "ID3", 3) == 0 || (sniff[0] == 0xFF && (sniff[1] & 0xE0) == 0xE0)) {
            e = ".mp3";
        } else if (memcmp(sniff, "ADIF", 4) == 0 || (sniff[0] == 0xFF && (sniff[1] & 0xF0) == 0xF0)) {
            e = ".aac";
        }
    }
    snprintf(ext, cap, "%s", e);
}

/* 在音频缓存目录里找这首歌已下载的文件（扩展名不定，所以按前缀找） */
static bool find_cached_audio(uint32_t song_id, char *out, size_t cap)
{
    DIR *d = opendir(ONLINE_AUDIO_DIR);
    if (!d) {
        return false;
    }
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%u.", (unsigned)song_id);
    const size_t plen = strlen(prefix);
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) != 0) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), ONLINE_AUDIO_DIR "/%s", ent->d_name);
        if (file_ok(path, 32 * 1024)) {
            snprintf(out, cap, "%s", path);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* 缓存太多就删最旧的几个（SDL 卡很大，但别无限涨） */
static void audio_cache_trim(void)
{
    DIR *d = opendir(ONLINE_AUDIO_DIR);
    if (!d) {
        return;
    }
    /* 统计 + 找出最旧的那几个：文件数不多（<= 几百），直接两遍扫描 */
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] != '.') {
            n++;
        }
    }
    if (n <= AUDIO_CACHE_MAX) {
        closedir(d);
        return;
    }
    int to_del = n - AUDIO_CACHE_MAX;
    for (int k = 0; k < to_del; k++) {
        rewinddir(d);
        char oldest[320] = "";
        time_t oldest_t = 0;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char path[320];
            snprintf(path, sizeof(path), ONLINE_AUDIO_DIR "/%s", ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }
            if (oldest[0] == '\0' || st.st_mtime < oldest_t) {
                snprintf(oldest, sizeof(oldest), "%s", path);
                oldest_t = st.st_mtime;
            }
        }
        if (oldest[0]) {
            unlink(oldest);
        }
    }
    closedir(d);
}

esp_err_t online_song_prepare(const online_song_t *song, char *audio_path, size_t cap)
{
    if (!song || !audio_path || cap == 0 || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_path[0] = '\0';

    if (find_cached_audio(song->id, audio_path, cap)) {
        ESP_LOGI(TAG, "audio cache hit: %s", audio_path);
        return ESP_OK;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 先下到临时文件，嗅探格式后再改名（扩展名决定用哪个解码器） */
    char tmp[192];
    snprintf(tmp, sizeof(tmp), ONLINE_AUDIO_DIR "/%u.part", (unsigned)song->id);
    uint8_t sniff[16] = {};
    esp_err_t ret = http_download(song->audio_url, tmp, 64 * 1024, sniff);
    if (ret != ESP_OK) {
        return ret;
    }
    char ext[8];
    ext_from_sniff(sniff, ext, sizeof(ext));
    snprintf(audio_path, cap, ONLINE_AUDIO_DIR "/%u%s", (unsigned)song->id, ext);
    unlink(audio_path);
    if (rename(tmp, audio_path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloaded '%s' -> %s", song->title, audio_path);

    audio_cache_trim();

    /* 封面与歌词：失败不影响播放 */
    char aux[192];
    if (online_song_cover(song, aux, sizeof(aux)) != ESP_OK) {
        ESP_LOGD(TAG, "no cover for %u", (unsigned)song->id);
    }
    if (online_song_lyrics(song, aux, sizeof(aux)) != ESP_OK) {
        ESP_LOGD(TAG, "no lyrics for %u", (unsigned)song->id);
    }
    return ESP_OK;
}

esp_err_t online_song_cover(const online_song_t *song, char *jpg_path, size_t cap)
{
    if (!song || !jpg_path || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(jpg_path, cap, ONLINE_COVER_DIR "/%u.jpg", (unsigned)song->id);
    if (file_ok(jpg_path, 1024)) {
        return ESP_OK;
    }
    if (!song->cover_url[0] || !online_has_network()) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 封面 URL 在歌单 JSON 里是 http://p*.music.126.net/...  直接下 */
    uint8_t sniff[16];
    esp_err_t ret = http_download(song->cover_url, jpg_path, 1024, sniff);
    if (ret != ESP_OK) {
        unlink(jpg_path);
        return ret;
    }
    return ESP_OK;
}

esp_err_t online_song_lyrics(const online_song_t *song, char *lrc_path, size_t cap)
{
    if (!song || !lrc_path || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * 歌词与音频**同目录同主名**（<id>.lrc 紧挨着 <id>.mp3）：
     * 这样 app_music 播放时不需要任何在线专用分支 ——
     * media_meta_lyrics_load(audio_path) 的第一档"同目录同名 .lrc"直接命中。
     */
    snprintf(lrc_path, cap, ONLINE_AUDIO_DIR "/%u.lrc", (unsigned)song->id);
    if (file_ok(lrc_path, 8)) {
        return ESP_OK;
    }
    if (!song->lrc_url[0] || !online_has_network()) {
        return ESP_ERR_NOT_FOUND;
    }
    char *text = NULL;
    size_t len = 0;
    esp_err_t ret = http_get_text(song->lrc_url, &text, &len);
    if (ret != ESP_OK || len < 8) {
        free(text);
        return (ret == ESP_OK) ? ESP_ERR_NOT_FOUND : ret;
    }
    FILE *f = fopen(lrc_path, "wb");
    if (!f) {
        free(text);
        return ESP_FAIL;
    }
    fwrite(text, 1, len, f);
    fclose(f);
    free(text);
    ESP_LOGI(TAG, "lyrics cached: %s (%u bytes)", lrc_path, (unsigned)len);
    return ESP_OK;
}

/*
 * 只把重定向解析掉，**不下载**：拿到音频的最终地址（https CDN），交给播放器流式播放。
 * 为什么自己解析：meting 的 type=url 是 302 到别的 CDN 主机，而 GMF 的 HTTP IO
 * 内部用 esp_http_client 的自动跳转（实测会丢查询串 → 服务器回 400）；
 * 先把最终 URL 算出来，播放时就只剩一次直连 GET。
 */
esp_err_t online_song_stream_url(const online_song_t *song, char *out, size_t cap)
{
    if (!song || !out || cap == 0 || !song->audio_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    char cur[512];
    snprintf(cur, sizeof(cur), "%s", song->audio_url);
    s_last_err[0] = '\0';

    for (int hop = 0; hop < 4; hop++) {
        http_sink_t probe = {};           /* 只要 Location，不接 body */
        esp_http_client_config_t cfg = {};
        cfg.url                   = cur;
        cfg.timeout_ms            = HTTP_TIMEOUT_MS;
        cfg.crt_bundle_attach     = esp_crt_bundle_attach;
        cfg.disable_auto_redirect = true;
        cfg.keep_alive_enable     = false;
        cfg.event_handler         = http_evt;
        cfg.user_data             = &probe;

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            return ESP_FAIL;
        }
        esp_err_t ret = esp_http_client_open(cli, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "stream probe open failed: %s (%s)", esp_err_to_name(ret), cur);
            snprintf(s_last_err, sizeof(s_last_err), "网络错误 %s", esp_err_to_name(ret));
            esp_http_client_cleanup(cli);
            return ret;
        }
        esp_http_client_fetch_headers(cli);
        const int status = esp_http_client_get_status_code(cli);
        esp_http_client_cleanup(cli);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (!probe.location[0]) {
                snprintf(s_last_err, sizeof(s_last_err), "重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            snprintf(cur, sizeof(cur), "%s", probe.location);
            ESP_LOGI(TAG, "stream url -> %s", cur);
            continue;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "stream probe status %d: %s", status, cur);
            snprintf(s_last_err, sizeof(s_last_err), "服务器返回 HTTP %d（版权受限？）", status);
            return ESP_ERR_INVALID_RESPONSE;
        }
        snprintf(out, cap, "%s", cur);
        return ESP_OK;
    }
    snprintf(s_last_err, sizeof(s_last_err), "重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}

/*
 * 流式播放前的轻量准备：**只**抓封面与歌词（几十~几百 KB），不碰音频 ——
 * 音频由播放器边下边播。
 */
esp_err_t online_song_prepare_stream(const online_song_t *song)
{
    if (!song || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char aux[192];
    const esp_err_t c = online_song_cover(song, aux, sizeof(aux));
    const esp_err_t l = online_song_lyrics(song, aux, sizeof(aux));
    ESP_LOGI(TAG, "stream meta for '%s': cover=%s lyrics=%s",
             song->title, esp_err_to_name(c), esp_err_to_name(l));
    return ESP_OK;
}

esp_err_t online_cache_clear(void)
{
    const char *dirs[] = { ONLINE_AUDIO_DIR, ONLINE_COVER_DIR };
    for (size_t k = 0; k < sizeof(dirs) / sizeof(dirs[0]); k++) {
        DIR *d = opendir(dirs[k]);
        if (!d) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            /* 目录名 + '/' + FATFS 最长 255 字节文件名：给够空间，别让编译器担心截断 */
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", dirs[k], ent->d_name);
            unlink(path);
        }
        closedir(d);
    }
    return ESP_OK;
}

bool online_has_network(void)
{
    return wifi_service_has_ip();
}

/* ---------------------------------------------------------------- init */

esp_err_t online_service_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    mkdir_if_needed(ONLINE_ROOT);
    mkdir_if_needed(ONLINE_AUDIO_DIR);
    mkdir_if_needed(ONLINE_COVER_DIR);
    mkdir_if_needed(ONLINE_LRC_DIR);
    nvs_load_ids();
    s_ready = true;
    ESP_LOGI(TAG, "online service ready: %u playlist id(s)", (unsigned)s_id_count);
    return ESP_OK;
}

size_t online_playlist_count(void)
{
    return s_id_count;
}

const char *online_last_error(void)
{
    return s_last_err[0] ? s_last_err : NULL;
}

const char *online_playlist_id(size_t idx)
{
    return (idx < s_id_count) ? s_ids[idx] : NULL;
}
