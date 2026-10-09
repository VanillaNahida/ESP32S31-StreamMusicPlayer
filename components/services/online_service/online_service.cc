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
 *
 * 封面 / 歌词缓存策略（2026-10 改：从 SD 卡搬到 PSRAM）：
 *   之前封面下到 /sdcard/online/cover/、歌词下到 /sdcard/online/audio/<id>.lrc，
 *   切歌时要写/读这些文件 —— SD 卡一忙（日志里一排 sdmmc_req 0x107 超时）切歌
 *   就阻塞，封面还常常下成 0 字节。现在封面字节与歌词文本**只留在 PSRAM**：
 *     - 下载一次，之后同一首歌命中内存缓存（LRU，封面上限 4 MB / 8 首）；
 *     - 播放端拿到的是一份 malloc 的拷贝（online_song_cover_data/lyrics_data），
 *       没有跨线程共享的内存所有权问题；
 *     - SD 上历史遗留的 cover/ 目录与 *.lrc 文件在 init 时清掉（不再 mkdir）。
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

#include <mbedtls/base64.h>   /* 二维码：服务端返回 base64 PNG，见文件尾"网易云账号登录" */
#include "esp_timer.h"        /* esp_timer_get_time()：给二维码请求做缓存破坏参数 */

#define ONLINE_ROOT        "/sdcard/online"
#define ONLINE_AUDIO_DIR   ONLINE_ROOT "/audio"
/* 历史遗留（不再写入，只在 init 时清理）： */
#define ONLINE_COVER_DIR   ONLINE_ROOT "/cover"
#define ONLINE_LRC_DIR     ONLINE_ROOT "/lrc"
#define ONLINE_PL_FMT      ONLINE_ROOT "/pl_%s.bin"

#define NVS_NS             "online"
#define NVS_KEY_CNT        "n"
#define NVS_KEY_ID_FMT     "id%d"      /* id0..id7 */
/* 歌单显示名（用户可重命名；空 = 还没起过名，UI 就显示 ID 或抓到的歌单名） */
#define NVS_KEY_NAME_FMT   "nm%d"      /* nm0..nm7 */
/* 搜索历史（最多 ONLINE_HISTORY_MAX 条，新的在最前，按关键词去重） */
#define NVS_KEY_SH_CNT     "shn"
#define NVS_KEY_SH_FMT     "sh%d"      /* sh0..sh9 */
/* 已登录账号昵称（重启后立刻能显示，不用等网络往返） */
#define NVS_KEY_NICK       "nick"

#define API_BASE           "http://meting.xcnahida.cn/api"
/* 搜索接口在另一个路径上，而且关键词放在 id= 里：
 *   http://meting.xcnahida.cn/meting/api?server=netease&type=search&id=<关键词>
 * （/api?type=search&keywords=xxx 在该实例上会忽略关键词、返回固定列表，别用） */
#define API_SEARCH_BASE    "http://meting.xcnahida.cn/meting/api"
#define HTTP_TIMEOUT_MS    15000
#define HTTP_BUF           2048
/* 单个 JSON 响应上限（歌单 1000 首 ≈ 300 KB），别让一个坏响应吃光内存 */
#define JSON_MAX_LEN       (1024u * 1024u)
/* 封面字节上限。解码端 media_meta 的 MEDIA_META_COVER_FILE_MAX 必须 ≥ 这个值，
 * 否则会出现"下下来了却解不了"。两个都放在 4 MB：
 * 封面正常只有 100~300 KB，但**平台的原图可能大得离谱**（实测某首网易云封面
 * 原图 7.6 MB），所以下面还挂了 ?param= 让服务端先缩图，这个上限只当兜底。 */
#define COVER_MAX_BYTES    (4u * 1024u * 1024u)

/*
 * 封面请求的"服务端缩图"参数：网易云 CDN 认 ?param=WxH（也接受 y 分隔）。
 *
 * ★ 为什么必须加（2026-10 修"部分歌曲封面不显示"）：
 *   实测 https://p2.music.126.net/.../109951169045313754.jpg
 *     - 后缀是 .jpg、Content-Type 也写 image/jpg，**下来的却是 PNG**
 *       （文件头 89 50 4E 47 0D 0A 1A 0A）；
 *     - 而且原图 **7,951,295 字节（7.6 MB）** —— 远超下载上限，封面永远出不来。
 *   加上 ?param=300y300 之后只有 ~100 KB（同样是 PNG），秒下。
 * 界面上的封面框是 240 px、解码目标 COVER_MAX_DIM 也是 240，300 已经够用。
 *
 * ⚠ 两个尺寸**都必须给**，少一个参数会被 CDN 静默忽略、原样返回 7.6 MB 原图
 *   （实测 2026-10 同一条链接）：
 *     ?param=200x200  → 100,539 B  ✓（x / y 分隔符都认）
 *     ?param=200y200  → 100,539 B  ✓
 *     ?param=200      → 7,951,295 B ✗ ← 只有一维 = 没生效，别"简化"成这个写法
 */
#define ONLINE_COVER_PARAM  300

/* 音频上限（防止下到无穷无尽的流）：250 MB */
#define AUDIO_MAX_BYTES    (250u * 1024u * 1024u)
/* 音频缓存最多留多少个文件，超了删最旧的 */
#define AUDIO_CACHE_MAX    48

/* ---- PSRAM 封面/歌词缓存（不落 SD，见文件头说明） ---- */
#define COVER_CACHE_MAX       8
#define COVER_CACHE_BYTES_MAX (4u * 1024u * 1024u)
#define LRC_CACHE_MAX         8
#define LRC_CACHE_BYTES_MAX   (256u * 1024u)

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
/* 显示名（可与 ID 一一对应；空串表示"没起过名"，UI 自己决定显示 ID 还是标题） */
static char  s_names[ONLINE_MAX_PLAYLISTS][ONLINE_NAME_MAX];
static size_t s_id_count;
/* 最近一次失败的可读原因（给 UI 的 Toast 用） */
static char  s_last_err[64];

/* ---------------------------------------------------------------- PSRAM 缓存
 *
 * 封面/歌词下载一次后整段留在 PSRAM，切歌不再碰 SD（见文件头）。
 * 缓存条目归本模块所有；对外 API 返回的是**调用方新 malloc 的拷贝**，
 * 所以这里怎么淘汰都不会踩到播放端正在用的内存。
 */
typedef struct {
    uint32_t id;
    uint8_t *data;
    size_t   len;
    uint32_t last_ms;      /* 最近使用（esp_timer 墙钟，淘汰用） */
} mem_cache_t;

static mem_cache_t s_cover_cache[COVER_CACHE_MAX];
static size_t      s_cover_cache_bytes;
static mem_cache_t s_lrc_cache[LRC_CACHE_MAX];
static size_t      s_lrc_cache_bytes;

static void mem_cache_free_slot(mem_cache_t *e)
{
    if (e->data) {
        free(e->data);
    }
    e->id      = 0;
    e->data    = NULL;
    e->len     = 0;
    e->last_ms = 0;
}

static mem_cache_t *mem_cache_find(mem_cache_t *tab, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (tab[i].id == id && tab[i].data) {
            tab[i].last_ms = (uint32_t)esp_timer_get_time() / 1000u;
            return &tab[i];
        }
    }
    return NULL;
}

/* 找一个空槽；没有就淘汰最旧的一个（LRU） */
static mem_cache_t *mem_cache_slot(mem_cache_t *tab, size_t n, size_t *bytes)
{
    size_t oldest = n;
    uint32_t oldest_ms = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        if (!tab[i].data) {
            return &tab[i];
        }
        if (tab[i].last_ms < oldest_ms) {
            oldest_ms = tab[i].last_ms;
            oldest    = i;
        }
    }
    if (oldest < n) {
        *bytes -= tab[oldest].len;
        mem_cache_free_slot(&tab[oldest]);
        return &tab[oldest];
    }
    return NULL;
}

/* 放进缓存（data 为调用方所有的新缓冲，插入后归缓存管） */
static void mem_cache_put(mem_cache_t *tab, size_t n, size_t *bytes, size_t cap_bytes,
                          uint32_t id, uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        free(data);
        return;
    }
    mem_cache_t *e = mem_cache_slot(tab, n, bytes);
    if (!e) {
        free(data);
        return;
    }
    e->id      = id;
    e->data    = data;
    e->len     = len;
    e->last_ms = (uint32_t)esp_timer_get_time() / 1000u;
    *bytes += len;
    /* 总量超限：从最旧开始扔，直到装得下 */
    while (*bytes > cap_bytes) {
        mem_cache_t *old = mem_cache_slot(tab, n, bytes);
        if (!old || old == e) {
            break;
        }
    }
}

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
    size_t cap;              /* 当前**已分配**的容量（按需增长，不是上限） */
    size_t limit;            /* 单次响应的硬上限（超过就让上层报错） */
    bool   overflow;         /* 因为超限/没内存中止过：给上层一句能看懂的话 */
    char   location[512];    /* 301/302 的 Location：响应头只能从事件里拿
                              * （esp_http_client_get_header() 取的是**请求**头，
                              *   实测在 perform() 之后取 Location 一直是空的） */
} http_sink_t;

/* 下载缓冲的起步容量：LRC 只有几十字节，不该为了它占几 MB */
#define HTTP_SINK_MIN   (16 * 1024)

/*
 * 把接收缓冲扩到至少 need 字节（按需增长）。返回 false = 超上限或没内存。
 *
 * ★ 两条纪律（2026-10 修"封面 ESP_ERR_NO_MEM"）：
 *   1) **显式从 PSRAM 分配**（heap_caps_malloc + MALLOC_CAP_SPIRAM）：
 *      封面/歌单这类缓冲动辄几百 KB 到几 MB，内部 RAM 根本放不下；
 *   2) **不要一上来就 malloc(上限)**。原来的写法是 `malloc(max_len)`，
 *      封面那条路 max_len = 4 MB —— 实测直接 ESP_ERR_NO_MEM（日志里
 *      "cover ... download failed (ESP_ERR_NO_MEM, 0 bytes)"），因为 PSRAM 里
 *      已经躺着 4 MB 流式环形缓冲 + framebuffer + 封面缓存，再要一块 **4 MB 连续**
 *      空间太难，而且 200 KB 的封面白白占 4 MB。
 *      现在从 16 KB 起步、翻倍增长：300y300 的封面（~200 KB）只占 256 KB。
 */
static bool sink_reserve(http_sink_t *s, size_t need)
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

/*
 * GET 一段数据（JSON / LRC / 封面字节都走这里）。成功时 *out 为 malloc 的缓冲
 * （对文本是 NUL 结尾的字符串；对二进制则按 out_len 给长度，不保证 NUL 语义）。
 * max_len 是这一跳的单次响应上限（防止坏响应吃光内存）。
 *
 * ★ 重定向自己处理，不用 esp_http_client 的自动跳转：
 *   这个实例把 http://meting.xcnahida.cn/api?... 302 到 https 同一路径，
 *   而 IDF 的自动跳转在实测里会把查询串弄丢（服务器直接回 400，
 *   日志就是 "GET status 400: http://meting.xcnahida.cn/api...playlist..."）。
 *   我们改成 disable_auto_redirect + 自己读 Location 再发一次，
 *   顺便把每一跳打进日志，方便定位。
 */
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

/* 拼 "Bearer xxx"（没有 token 时返回空串）。只对我们自己的 API 地址加这个头 ——
 * 绝不能把 token 泄露给 meting 那种第三方代理。 */
static const char *net_auth_hdr(void)
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
static const char *url_for_log(const char *url)
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

static esp_err_t http_get_text(const char *url, char **out, size_t *out_len, size_t max_len)
{
    if (!url || !out || max_len == 0) {
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

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            free(sink.buf);
            return ESP_FAIL;
        }
        /* 访问鉴权：只对**我们自己的 API 基地址**加 Authorization 头（见 net_auth_hdr） */
        const char *auth = net_auth_hdr();
        if (auth[0] && strncmp(cur, online_netease_api_base(), strlen(online_netease_api_base())) == 0) {
            esp_http_client_set_header(cli, "Authorization", auth);
        }
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
            /* 超上限/没内存时 sink_reserve 会置 overflow，给一句能看懂的话 */
            if (sink.overflow) {
                ESP_LOGW(TAG, "GET too large (limit %u KB, got %u bytes): %s",
                         (unsigned)(max_len / 1024), (unsigned)sink.len, url_for_log(cur));
                snprintf(s_last_err, sizeof(s_last_err), "响应过大或内存不足(%u KB)",
                         (unsigned)(sink.len / 1024));
            } else {
                /* ⚠ url_for_log()：**不能**打整条 URL —— 账号接口的查询串里是登录 cookie */
                ESP_LOGW(TAG, "GET failed (%s): %s", esp_err_to_name(ret), url_for_log(cur));
                snprintf(s_last_err, sizeof(s_last_err), "网络错误 %s", esp_err_to_name(ret));
            }
            free(sink.buf);
            return ret;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "GET status %d: %s", status, url_for_log(cur));
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
            ESP_LOGW(TAG, "download status %d: %s", status, url_for_log(cur));
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
                /* 名字是可选的：没有就是空串（UI 退回显示 ID） */
                size_t nlen = sizeof(s_names[s_id_count]);
                snprintf(key, sizeof(key), NVS_KEY_NAME_FMT, i);
                if (nvs_get_str(h, key, s_names[s_id_count], &nlen) != ESP_OK) {
                    s_names[s_id_count][0] = '\0';
                }
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
        /* 名字（空串就直接删键，别留垃圾） */
        snprintf(key, sizeof(key), NVS_KEY_NAME_FMT, (int)i);
        if (i < s_id_count && s_names[i][0]) {
            nvs_set_str(h, key, s_names[i]);
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
    esp_err_t ret = http_get_text(url, &json, NULL, JSON_MAX_LEN);
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
            str_copy(s_names[i - 1], sizeof(s_names[0]), s_names[i]);   /* 名字跟着挪 */
        }
        s_id_count--;
        s_names[s_id_count][0] = '\0';
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
        if (http_get_text(url, &json, NULL, JSON_MAX_LEN) == ESP_OK) {
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

    /* 封面与歌词：进 PSRAM 缓存（失败不影响播放） */
    uint8_t *j = NULL;
    size_t   jl = 0;
    if (online_song_cover_data(song, &j, &jl) != ESP_OK) {
        ESP_LOGD(TAG, "no cover for %u", (unsigned)song->id);
    }
    free(j);
    char *l = NULL;
    if (online_song_lyrics_data(song, &l, NULL) != ESP_OK) {
        ESP_LOGD(TAG, "no lyrics for %u", (unsigned)song->id);
    }
    free(l);
    return ESP_OK;
}

/*
 * 封面字节（PSRAM 缓存，不落 SD）。
 * 命中缓存直接拷贝给调用方；没命中就下（封面 URL 是 http://p*.music.126.net，
 * 直接 http_get_text 即可），进缓存 + 拷贝。
 * *out 是调用方所有的新缓冲，用 free() 释放。
 */
/*
 * 封面格式：**只认文件头魔数**，绝不看 URL 后缀或 Content-Type。
 *
 * ★ 平台这两个都会撒谎（2026-10 实测网易云）：
 *     URL 后缀 .jpg、Content-Type: image/jpg，实际字节是 PNG。
 *   解码端 media_meta 的 image_to_rgb565() 同样是按魔数分派的（PNG 走 libpng、
 *   其余按 JPEG 走 esp_new_jpeg），所以这里判定一致，不会出现"按后缀选了错的解码器"。
 *
 * 返回可打印的格式名；不认识的（WebP / GIF / HTML 错误页…）返回 NULL 并给出名字。
 */
static const char *cover_format(const uint8_t *d, size_t n, const char **why)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return "JPEG";
    }
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) {
        return "PNG";
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        *why = "WebP（本工程没有 WebP 解码器）";
    } else if (n >= 4 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') {
        *why = "GIF";
    } else if (n >= 5 && memcmp(d, "<?xml", 5) == 0) {
        *why = "XML（多半是错误响应）";
    } else if (n >= 5 && (memcmp(d, "<!DOC", 5) == 0 || memcmp(d, "<html", 5) == 0)) {
        *why = "HTML（多半是错误页）";
    } else {
        *why = "未知格式";
    }
    return NULL;
}

esp_err_t online_song_cover_data(const online_song_t *song, uint8_t **out, size_t *out_len)
{
    if (!song || !out || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }

    mem_cache_t *hit = mem_cache_find(s_cover_cache, COVER_CACHE_MAX, song->id);
    if (hit) {
        uint8_t *copy = (uint8_t *)psram_alloc(hit->len);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, hit->data, hit->len);
        *out = copy;
        if (out_len) {
            *out_len = hit->len;
        }
        return ESP_OK;
    }

    if (!song->cover_url[0] || !online_has_network()) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * 拼上服务端缩图参数。**只在原 URL 没有查询串时加** ——
     * 万一是带签名的 URL，多一个参数会让签名失效（网易云封面链接没有查询串，
     * 这条路是安全的；检测到已有 '?' 就原样用）。
     */
    char url[ONLINE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s", song->cover_url);
    if (!strchr(url, '?')) {
        const size_t n = strlen(url);
        if (n + 24 < sizeof(url)) {
            snprintf(url + n, sizeof(url) - n, "?param=%dy%d",
                     ONLINE_COVER_PARAM, ONLINE_COVER_PARAM);
        }
    }

    char *raw = NULL;
    size_t len = 0;
    esp_err_t ret = http_get_text(url, &raw, &len, COVER_MAX_BYTES);
    if (ret != ESP_OK || len < 64) {
        /* 超上限时给一句能看懂的话（下载器只会回一个笼统的错误码） */
        if (len >= COVER_MAX_BYTES) {
            snprintf(s_last_err, sizeof(s_last_err), "封面太大(>%u MB)",
                     (unsigned)(COVER_MAX_BYTES / (1024u * 1024u)));
            ESP_LOGW(TAG, "cover %u too large (>%u bytes): %s",
                     (unsigned)song->id, (unsigned)COVER_MAX_BYTES, url);
        } else {
            ESP_LOGW(TAG, "cover %u download failed (%s, %u bytes): %s",
                     (unsigned)song->id, esp_err_to_name(ret), (unsigned)len, url);
        }
        free(raw);
        return (ret == ESP_OK) ? ESP_ERR_NOT_FOUND : ret;
    }

    /* ★ 按魔数认格式（不信后缀、不信 Content-Type），顺便把结果打进日志，
     * 以后"封面下到了却没显示"一眼就能看出是不是格式/解码的问题。 */
    const char *why = NULL;
    const char *fmt = cover_format((const uint8_t *)raw, len, &why);
    if (!fmt) {
        ESP_LOGW(TAG, "cover %u: unsupported %s (magic %02x %02x %02x %02x, %u bytes, url=%s)",
                 (unsigned)song->id, why ? why : "format",
                 (unsigned char)raw[0], (unsigned char)raw[1],
                 (unsigned char)raw[2], (unsigned char)raw[3], (unsigned)len, url);
        /* 中文全角括号一个 3 字节，GCC 证不出 64 字节缓冲放得下（-Werror=format-truncation），
         * 按契约显式限长：前缀 7 字 ≈ 21 字节 + 20 + NUL，稳在 64 内 */
        snprintf(s_last_err, sizeof(s_last_err), "封面格式不支持：%.20s",
                 why ? why : "未知");
        free(raw);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "cover %u: %s, %u bytes", (unsigned)song->id, fmt, (unsigned)len);

    mem_cache_put(s_cover_cache, COVER_CACHE_MAX, &s_cover_cache_bytes,
                  COVER_CACHE_BYTES_MAX, song->id, (uint8_t *)raw, len);
    uint8_t *copy = (uint8_t *)psram_alloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, raw, len);
    *out = copy;
    if (out_len) {
        *out_len = len;
    }
    ESP_LOGI(TAG, "cover cached in PSRAM: %u (%u bytes)", (unsigned)song->id, (unsigned)len);
    return ESP_OK;
}

/*
 * 歌词文本（PSRAM 缓存，不落 SD）。命中缓存直接拷贝；没命中就下（纯文本 LRC，
 * UTF-8 原样），进缓存 + 拷贝。*out 用 free() 释放。
 */
esp_err_t online_song_lyrics_data(const online_song_t *song, char **out, size_t *out_len)
{
    if (!song || !out || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }

    mem_cache_t *hit = mem_cache_find(s_lrc_cache, LRC_CACHE_MAX, song->id);
    if (hit) {
        char *copy = (char *)psram_alloc(hit->len + 1);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, hit->data, hit->len);
        copy[hit->len] = '\0';
        *out = copy;
        if (out_len) {
            *out_len = hit->len;
        }
        return ESP_OK;
    }

    if (!song->lrc_url[0] || !online_has_network()) {
        return ESP_ERR_NOT_FOUND;
    }
    char *text = NULL;
    size_t len = 0;
    esp_err_t ret = http_get_text(song->lrc_url, &text, &len, LRC_CACHE_BYTES_MAX);
    if (ret != ESP_OK || len < 8) {
        free(text);
        return (ret == ESP_OK) ? ESP_ERR_NOT_FOUND : ret;
    }
    mem_cache_put(s_lrc_cache, LRC_CACHE_MAX, &s_lrc_cache_bytes,
                  LRC_CACHE_BYTES_MAX, song->id, (uint8_t *)text, len);
    char *copy = (char *)psram_alloc(len + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    *out = copy;
    if (out_len) {
        *out_len = len;
    }
    ESP_LOGI(TAG, "lyrics cached in PSRAM: %u (%u bytes)", (unsigned)song->id, (unsigned)len);
    return ESP_OK;
}

/*
 * 只把重定向解析掉，**不下载**：拿到音频的最终地址（https CDN），交给播放器流式播放。
 * 为什么自己解析：meting 的 type=url 是 302 到别的 CDN 主机，而 GMF 的 HTTP IO
 * 内部用 esp_http_client 的自动跳转（实测会丢查询串 → 服务器回 400）；
 * 先把最终 URL 算出来，播放时就只剩一次直连 GET。
 */
/* MP3 帧头 → 码率(bps)，解析失败返回 0。表格照 ISO/IEC 11172-3 Layer III。 */
static int mp3_bitrate_bps(const uint8_t *p, size_t n)
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
     * ★ 账号歌（"我的歌单"/每日推荐来的）没有 meting 的 audio_url —— 直链必须带 cookie
     *   现取（online_netease_song_url）。判据就用"audio_url 为空"：不用给
     *   online_song_t 加字段，也就不会动到 SD 上定长记录（'OPL1'）的格式。
     *   顺带好处：/song/url/v1 直接给 size + 码率，**总时长是准的**（不用再靠墙钟估）。
     */
    if (!song->audio_url[0]) {
        if (song->id == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        char sid[16];
        snprintf(sid, sizeof(sid), "%u", (unsigned)song->id);
        return online_netease_song_url(sid, out, cap, out_bytes, out_dur_ms);
    }
    out[0] = '\0';
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    char cur[512];
    snprintf(cur, sizeof(cur), "%s", song->audio_url);
    s_last_err[0] = '\0';

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
            snprintf(s_last_err, sizeof(s_last_err), "网络错误 %s", esp_err_to_name(ret));
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
                snprintf(s_last_err, sizeof(s_last_err), "重定向缺少 Location");
                return ESP_ERR_INVALID_RESPONSE;
            }
            snprintf(cur, sizeof(cur), "%s", loc);
            continue;
        }
        if (status != 200) {
            esp_http_client_cleanup(cli);
            snprintf(s_last_err, sizeof(s_last_err), "服务器返回 HTTP %d（版权受限？）", status);
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* 读 4 KB 前缀：只为了拿 MP3 码率，读完就断开 */
        uint8_t *head = (uint8_t *)malloc(4096);
        int got = 0;
        if (head) {
            got = esp_http_client_read(cli, (char *)head, 4096);
            if (got < 0) {
                got = 0;
            }
            if (out_dur_ms && total > 0 && got > 0) {
                const int br = mp3_bitrate_bps(head, (size_t)got);
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
    snprintf(s_last_err, sizeof(s_last_err), "重定向次数过多");
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t online_song_stream_url(const online_song_t *song, char *out, size_t cap)
{
    if (!song || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ★ 账号歌：audio_url 为空 → 带 cookie 现取直链（说明见 online_song_stream_info） */
    if (!song->audio_url[0]) {
        if (song->id == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        char sid[16];
        snprintf(sid, sizeof(sid), "%u", (unsigned)song->id);
        return online_netease_song_url(sid, out, cap, NULL, NULL);
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
 * 流式播放前的轻量准备：**只**抓封面与歌词（几十~几百 KB，进 PSRAM 缓存），
 * 不碰音频 —— 音频由播放器边下边播。拷贝随手释放：播放端会再各取一份。
 */
esp_err_t online_song_prepare_stream(const online_song_t *song)
{
    if (!song || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *j = NULL;
    char *l = NULL;
    const esp_err_t c = online_song_cover_data(song, &j, NULL);
    const esp_err_t lr = online_song_lyrics_data(song, &l, NULL);
    free(j);
    free(l);
    ESP_LOGI(TAG, "stream meta for '%s': cover=%s lyrics=%s",
             song->title, esp_err_to_name(c), esp_err_to_name(lr));
    return ESP_OK;
}

/* 清掉 PSRAM 里的封面/歌词缓存 */
static void psram_cache_clear(void)
{
    for (size_t i = 0; i < COVER_CACHE_MAX; i++) {
        mem_cache_free_slot(&s_cover_cache[i]);
    }
    s_cover_cache_bytes = 0;
    for (size_t i = 0; i < LRC_CACHE_MAX; i++) {
        mem_cache_free_slot(&s_lrc_cache[i]);
    }
    s_lrc_cache_bytes = 0;
}

esp_err_t online_cache_clear(void)
{
    /* SD 上的音频缓存（封面/歌词早已不进 SD） */
    DIR *d = opendir(ONLINE_AUDIO_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            /* 目录名 + '/' + FATFS 最长 255 字节文件名：给够空间，别让编译器担心截断 */
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", ONLINE_AUDIO_DIR, ent->d_name);
            unlink(path);
        }
        closedir(d);
    }
    /* PSRAM 里的封面/歌词缓存 */
    psram_cache_clear();
    return ESP_OK;
}

bool online_has_network(void)
{
    return wifi_service_has_ip();
}

/*
 * 删掉一个目录里的全部内容，并尝试 rmdir（用于清理历史遗留的 cover/、lrc/ 目录）。
 */
static void wipe_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

/* ---------------------------------------------------------------- init */

/* 前向声明：定义在文件后半（搜索历史那一节），init 里要先用到 */
static void nvs_load_history(void);
/* 同上：账号昵称的 NVS 读取定义在"网易云账号登录"那一节 */
static void nvs_load_nick(void);
/* 同上：cookie 的读取也定义在后半（没有它就只能等第一次用到才恢复登录态） */
static void net_cookie_load(void);

esp_err_t online_service_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    mkdir_if_needed(ONLINE_ROOT);
    mkdir_if_needed(ONLINE_AUDIO_DIR);
    /* 封面/歌词不再落 SD：把旧固件写下来的 cover/ 目录与 *.lrc 清掉，
     * 免得它们继续占用 SD 空间、也避免下次切歌再读到旧文件。 */
    wipe_dir(ONLINE_COVER_DIR);
    wipe_dir(ONLINE_LRC_DIR);
    {
        DIR *d = opendir(ONLINE_AUDIO_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') {
                    continue;
                }
                const char *dot = strrchr(ent->d_name, '.');
                if (dot && strcasecmp(dot, ".lrc") == 0) {
                    char path[320];
                    snprintf(path, sizeof(path), "%s/%s", ONLINE_AUDIO_DIR, ent->d_name);
                    unlink(path);
                }
            }
            closedir(d);
        }
    }
    nvs_load_ids();
    nvs_load_history();
    nvs_load_nick();          /* 账号昵称：重启后不等网络就能显示"已登录：XX" */
    net_cookie_load();        /* 登录态：SD 已挂的话这里就能恢复，省得界面闪一下未登录 */
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

const char *online_playlist_name(size_t idx)
{
    if (idx >= s_id_count) {
        return NULL;
    }
    /* 没起过名就空串 —— 让 UI 自己决定退回显示 ID 还是别的，服务不做展示决策 */
    return s_names[idx];
}

esp_err_t online_playlist_rename(size_t idx, const char *name)
{
    if (idx >= s_id_count) {
        return ESP_ERR_INVALID_ARG;
    }
    /* name == NULL 或空串 = 清掉自定义名（退回显示 ID） */
    str_copy(s_names[idx], sizeof(s_names[0]), name ? name : "");
    nvs_save_ids();
    ESP_LOGI(TAG, "playlist '%s' renamed to '%s'", s_ids[idx], s_names[idx]);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 搜索历史 */

static char   s_hist[ONLINE_HISTORY_MAX][ONLINE_KW_MAX];
static size_t s_hist_n;

static void nvs_load_history(void)
{
    s_hist_n = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    if (nvs_get_u8(h, NVS_KEY_SH_CNT, &n) == ESP_OK) {
        if (n > ONLINE_HISTORY_MAX) {
            n = ONLINE_HISTORY_MAX;
        }
        for (uint8_t i = 0; i < n; i++) {
            char key[12];
            snprintf(key, sizeof(key), NVS_KEY_SH_FMT, i);
            size_t len = sizeof(s_hist[s_hist_n]);
            if (nvs_get_str(h, key, s_hist[s_hist_n], &len) == ESP_OK && s_hist[s_hist_n][0]) {
                s_hist_n++;
            }
        }
    }
    nvs_close(h);
}

static void nvs_save_history(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_SH_CNT, (uint8_t)s_hist_n);
    for (size_t i = 0; i < ONLINE_HISTORY_MAX; i++) {
        char key[12];
        snprintf(key, sizeof(key), NVS_KEY_SH_FMT, (int)i);
        if (i < s_hist_n) {
            nvs_set_str(h, key, s_hist[i]);
        } else {
            nvs_erase_key(h, key);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

/* 把第 idx 条挪到最前（其余后移），用于"再次搜同一个词"和"删单条" */
static void hist_move_to_front(size_t idx)
{
    if (idx == 0 || idx >= s_hist_n) {
        return;
    }
    char tmp[ONLINE_KW_MAX];
    str_copy(tmp, sizeof(tmp), s_hist[idx]);
    for (size_t i = idx; i > 0; i--) {
        str_copy(s_hist[i], sizeof(s_hist[0]), s_hist[i - 1]);
    }
    str_copy(s_hist[0], sizeof(s_hist[0]), tmp);
}

size_t online_history_count(void)
{
    return s_hist_n;
}

const char *online_history_at(size_t idx)
{
    return (idx < s_hist_n) ? s_hist[idx] : NULL;
}

esp_err_t online_history_add(const char *kw)
{
    if (!kw || !kw[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 已有同一个词：提到最前，不新增（这是搜索历史的常规行为） */
    for (size_t i = 0; i < s_hist_n; i++) {
        if (strcmp(s_hist[i], kw) == 0) {
            hist_move_to_front(i);
            nvs_save_history();
            return ESP_OK;
        }
    }
    const size_t n = (s_hist_n < ONLINE_HISTORY_MAX) ? (s_hist_n + 1) : ONLINE_HISTORY_MAX;
    for (size_t i = n - 1; i > 0; i--) {
        str_copy(s_hist[i], sizeof(s_hist[0]), s_hist[i - 1]);
    }
    str_copy(s_hist[0], sizeof(s_hist[0]), kw);
    s_hist_n = n;
    nvs_save_history();
    return ESP_OK;
}

esp_err_t online_history_remove(size_t idx)
{
    if (idx >= s_hist_n) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = idx + 1; i < s_hist_n; i++) {
        str_copy(s_hist[i - 1], sizeof(s_hist[0]), s_hist[i]);
    }
    s_hist_n--;
    s_hist[s_hist_n][0] = '\0';
    nvs_save_history();
    return ESP_OK;
}

esp_err_t online_history_clear(void)
{
    s_hist_n = 0;
    for (size_t i = 0; i < ONLINE_HISTORY_MAX; i++) {
        s_hist[i][0] = '\0';
    }
    nvs_save_history();
    ESP_LOGI(TAG, "search history cleared");
    return ESP_OK;
}

/* ============================================================ 网易云账号登录
 *
 * 见 online_service.h 的说明：设备端**不做 weapi 加密**，全部走自建 API 服务
 * （参考工程 NeteaseCloudMusicApi = @neteasecloudmusicapienhanced/api v4.41.1）。
 * 这里只做三件事：取 unikey → 取二维码（base64 PNG，交给上层用 libpng 解）→
 * 轮询扫码结果，成功就把 cookie 存进 SD。
 */
#define NET_NVS_NS        "online"
#define NET_NVS_KEY_BASE  "nb"
#define NET_BASE_MAX      96
/*
 * cookie 长度：服务端返回的是**把所有 Set-Cookie 拼起来**的整串（含 Max-Age/
 * Expires/Path 这些属性），实测 3875 字节 —— 里面大量是 WM_NIKE/_ga/acw_tc 之类的
 * 无关项，真正要的只有 MUSIC_U（值本身就有 ~700 字符！）/__csrf 几个。
 * 旧值 1024 会把它**截断**，而 MUSIC_U 恰好在后面 —— 结果就是"登录成功了但账号
 * 接口全是匿名"，日志特征：`cookie saved (1023 bytes)` 之后 /user/account 要登录。
 * 现在：原始串按 4 KB 收（3875 刚好放得下），存盘/发送用**压缩后**的形式。
 * ⚠ NET_COOKIE_SEND 必须 ≥ MUSIC_U 的长度 + 其它白名单项：
 *   曾用 1024 配 `pairs[24][160]`，MUSIC_U 值 ~700 字符 > 160 被整条丢掉，
 *   日志就是 `compact 101 bytes, MUSIC_U MISSING!`（2026-10 真机踩过）。
 */
#define NET_COOKIE_MAX    4096
#define NET_COOKIE_SEND   2048
/*
 * JSON 响应上限。
 * ★ 64 KB → 512 KB（2026-10 修"每日推荐加载失败"）：/recommend/songs 走的是
 *   `/api/v3/discovery/recommend/songs`，返回的**每首歌对象都巨大**（ar/al/privileges…），
 *   30 首就轻松几百 KB，64 KB 直接被判"响应过大"而失败（sink 是按需增长到 limit，
 *   所以平时不占内存）。
 */
#define NET_JSON_MAX      (512 * 1024)
#define NET_COOKIE_FILE   ONLINE_ROOT "/netease_cookie.txt"
/* 默认地址来自 Kconfig（见 main/Kconfig.projbuild，本机填的是 http://localhost:3000）。
 * ⚠ ESP32 上的 localhost 是**设备自己**：服务跑在 PC 上时必须改成 PC 的局域网 IP，
 *   例：menuconfig → "Online music account" → 网易云 API 服务地址，或直接改 sdkconfig。
 * 运行期还能用 online_netease_api_base_set()（NVS）临时覆盖，不用重烧固件。 */
#ifdef CONFIG_ONLINE_NETEASE_API_BASE
#define NET_BASE_DEFAULT  CONFIG_ONLINE_NETEASE_API_BASE
#else
#define NET_BASE_DEFAULT  "http://localhost:3000"
#endif

static char s_net_base[NET_BASE_MAX];
static char s_net_cookie[NET_COOKIE_MAX];
static bool s_net_cookie_loaded;
static bool s_net_logged_in;
/* 已登录账号昵称（NVS 持久化；空串 = 未知）。见 NVS_KEY_NICK 与
 * online_netease_nickname()：重启后不等网络往返就能显示"已登录：XX"。 */
static char s_net_nick[64];
static bool s_net_nick_loaded;

/*
 * 在 JSON 里找字段：**先看最外层，再递归进去**。
 *
 * 为什么强调"最外层优先"：实测服务端响应里内外层都可能有同名 code ——
 *   /login/qr/key → {"data":{"code":200,"unikey":"…"},"code":200}
 * 如果递归时先命中内层的 code，/login/qr/check 的扫码状态（800/801/802/803）
 * 就会被内层的 200 顶掉，状态机整条失效。所以第一遍只扫直接子节点。
 */
static const char *json_find_str(const cJSON *root, const char *key)
{
    for (const cJSON *c = root; c; c = c->next) {          /* 第一遍：直接子节点 */
        if (cJSON_IsString(c) && c->string && strcmp(c->string, key) == 0) {
            return c->valuestring;
        }
    }
    for (const cJSON *c = root; c; c = c->next) {          /* 第二遍：递归 */
        if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && c->child) {
            const char *r = json_find_str(c->child, key);
            if (r) {
                return r;
            }
        }
    }
    return NULL;
}

static bool json_find_int(const cJSON *root, const char *key, int *out)
{
    for (const cJSON *c = root; c; c = c->next) {          /* 第一遍：直接子节点 */
        if (cJSON_IsNumber(c) && c->string && strcmp(c->string, key) == 0) {
            *out = c->valueint;
            return true;
        }
    }
    for (const cJSON *c = root; c; c = c->next) {          /* 第二遍：递归 */
        if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && c->child) {
            if (json_find_int(c->child, key, out)) {
                return true;
            }
        }
    }
    return false;
}

const char *online_netease_api_base(void)
{
    if (!s_net_base[0]) {
        nvs_handle_t h;
        size_t len = sizeof(s_net_base);
        if (nvs_open(NET_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, NET_NVS_KEY_BASE, s_net_base, &len) != ESP_OK) {
                s_net_base[0] = '\0';
            }
            nvs_close(h);
        }
        if (!s_net_base[0]) {
            snprintf(s_net_base, sizeof(s_net_base), "%s", NET_BASE_DEFAULT);
        }
        ESP_LOGI(TAG, "netease api base: %s", s_net_base);
    }
    return s_net_base;
}

esp_err_t online_netease_api_base_set(const char *base)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (base && base[0]) {
        err = nvs_set_str(h, NET_NVS_KEY_BASE, base);
        if (err == ESP_OK) {
            snprintf(s_net_base, sizeof(s_net_base), "%s", base);
        }
    } else {
        err = nvs_erase_key(h, NET_NVS_KEY_BASE);
        s_net_base[0] = '\0';          /* 下次 get 时回默认值 */
    }
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "netease api base -> %s", (base && base[0]) ? base : "(default)");
    return err;
}

/* ------------------------------------------------- 内置 cookie（免扫码登录）
 *
 * 来源：CONFIG_ONLINE_NETEASE_CK_FILE（**相对工程根目录**的路径，默认
 *       assets/ncm-ck.txt），由 components/services/online_service/CMakeLists.txt
 *       在配置阶段以 EMBED(TEXT) 编进固件 —— TEXT 会在末尾补 '\0'，于是
 *       _binary_ncm_ck_builtin_start 可以直接当 C 字符串用。
 *
 * 三种情况都**不启用**：Kconfig 留空 / 文件不存在 / 文件里没有 MUSIC_U。
 * 那时 ONLINE_NCM_CK_BUILTIN 未定义，代码走的就是"没有这个功能"的老路
 * （扫码登录 + SD 上存的 cookie），不报错也不提示。
 *
 * 地位：**扫码登录的替代品，不是替代登录态**。SD 上有扫码存下来的 cookie 时
 *       以 SD 为准（那是账号真实登录出来的，比固件里烘死的更新）。
 */
#if defined(ONLINE_NCM_CK_BUILTIN)

#ifdef __cplusplus
extern "C" {
#endif
extern const char _binary_ncm_ck_builtin_start[];   /* 由 .S 提供，C 符号名不带修饰 */
#ifdef __cplusplus
}
#endif

/*
 * 从原始 cookie 串里挑出名字为 name 的那一段（`NAME=VALUE`），拷进 out。
 * 返回写入长度（0 = 没找到）。cap > 0 时 out 一定是合法的 NUL 结尾串。
 *
 * 为什么不复用 cookie_compact()：它把**所有**非属性项都塞进 pairs[12]，
 * 而浏览器导出的 cookie 串有二十多项、真正的 MUSIC_U 往往排在很后面 ——
 * 12 个槽位根本轮不到它，结果就是"内置了 cookie 却仍然匿名"。
 * 这里按名字精确提取，与项数和顺序都无关。
 * （cookie_compact() 那条路是服务端 Set-Cookie 拼串，MUSIC_U 位置靠前，
 *   实测一直正常，所以保持原样不动，避免把能用的扫码登录改坏。）
 */
static size_t cookie_pick(const char *raw, const char *name, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!raw || !name || !name[0]) {
        return 0;
    }
    const size_t nlen = strlen(name);
    const char *p = raw;
    while (*p) {
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : (p + strlen(p));
        const char *s = p;
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
            s++;
        }
        const char *e = end;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
            e--;
        }
        if ((size_t)(e - s) > nlen && s[nlen] == '=' && strncmp(s, name, nlen) == 0) {
            size_t n = (size_t)(e - s);
            if (n > cap - 1) {
                n = cap - 1;           /* 装不下就截断：能认证上总比整条丢掉好 */
            }
            memcpy(out, s, n);
            out[n] = '\0';
            return n;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

/*
 * 用内置 cookie 填 s_net_cookie。返回是否成功（= 拿到了 MUSIC_U）。
 * 只取 MUSIC_U / __csrf：服务端 util/request.js 是 `if (!processedCookie.MUSIC_U)
 * 就按游客返回`，其余统计类 cookie（_ga / sensorsdata / 一堆 _ntes_*）它不需要。
 */
static bool net_cookie_builtin_load(void)
{
    const char *raw = _binary_ncm_ck_builtin_start;
    if (!raw || !raw[0]) {
        return false;
    }
    static char music_u[1024];             /* 别放栈上：MUSIC_U 实测 ~700 字符 */
    if (cookie_pick(raw, "MUSIC_U", music_u, sizeof(music_u)) == 0) {
        ESP_LOGW(TAG, "内置 cookie 里没有 MUSIC_U：不启用（视为未登录）");
        return false;
    }
    char csrf[256];
    if (cookie_pick(raw, "__csrf", csrf, sizeof(csrf)) > 0) {
        snprintf(s_net_cookie, sizeof(s_net_cookie), "%s; %s", music_u, csrf);
    } else {
        snprintf(s_net_cookie, sizeof(s_net_cookie), "%s", music_u);
    }
    ESP_LOGI(TAG, "netease cookie loaded from built-in (%u bytes)",
             (unsigned)strlen(s_net_cookie));
    return true;
}

#else  /* !ONLINE_NCM_CK_BUILTIN */

/*
 * 编译期没嵌入内置 cookie（Kconfig 留空 / 文件不存在）：恒 false 的桩。
 * 调用点一行不改，行为与"没有这个功能"时完全一致，也不会有未使用函数的告警。
 */
static bool net_cookie_builtin_load(void)
{
    return false;
}

#endif /* ONLINE_NCM_CK_BUILTIN */

static void net_cookie_load(void)
{
    if (s_net_cookie_loaded) {
        return;
    }
    s_net_cookie_loaded = true;
    FILE *f = fopen(NET_COOKIE_FILE, "rb");
    if (!f) {
        /* 没登录过（或 SD 没挂）：还能退到编进固件的内置 cookie */
        s_net_logged_in = net_cookie_builtin_load();
        return;
    }
    size_t n = fread(s_net_cookie, 1, sizeof(s_net_cookie) - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {   /* 去掉可能被写进去的换行 */
        if (s_net_cookie[i] == '\n' || s_net_cookie[i] == '\r') {
            s_net_cookie[i] = '\0';
            n = i;
            break;
        }
    }
    s_net_cookie[n] = '\0';
    s_net_logged_in = (s_net_cookie[0] != '\0');
    /*
     * 旧版本用 1024 字节上限存 cookie，会把 MUSIC_U（排在很后面）**截断掉**，
     * 于是文件在、登录态却无效。这里做一次体检：有文件但没有 MUSIC_U 就当未登录，
     * 免得界面停在"已登录"却什么都拉不到（用户只会看到"扫码后没反应"）。
     */
    if (s_net_logged_in && !strstr(s_net_cookie, "MUSIC_U=")) {
        ESP_LOGW(TAG, "cookie 里没有 MUSIC_U（旧版截断残留）：视为未登录，请重新扫码");
        s_net_logged_in = false;
    }
    ESP_LOGI(TAG, "netease cookie %s (%u bytes)", s_net_logged_in ? "loaded" : "empty",
             (unsigned)n);
    if (!s_net_logged_in) {
        /* SD 上没有可用的扫码登录态 → 退到编进固件的内置 cookie */
        s_net_logged_in = net_cookie_builtin_load();
    }
}

/*
 * 压缩 cookie：把服务端那一大串收拾成 `NAME=VALUE; NAME=VALUE`。
 *
 * 规则：
 *   - 按 ';' 切段、两端 trim；
 *   - 只保留含 '=' 且名字**不是属性**的段（属性：Max-Age/Expires/Path/Domain/
 *     Secure/HttpOnly/SameSite/Version）；
 *   - **同名只留最后一个**（Set-Cookie 列表里后面的才是最新的；旧实现不敢去重，
 *     于是同一个名字带不同 Path 重复了七八次，白白撑爆 URL）；
 *   - 结果是可直接发送/存盘的形式，通常 200~400 字节。
 */
static void cookie_compact(const char *raw, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw || !raw[0]) {
        return;
    }

    static const char *const attrs[] = {"max-age", "expires", "path", "domain",
                                        "secure", "httponly", "samesite", "version"};
    /*
     * ★ static 而不是栈上：24×160 = 3840 B。原先直接开在工作线程（栈只有 4 KB）上，
     *   叠加 net_cookie_save 里那 1 KB 缓冲就爆栈 —— 真机现象是点「扫码登录」后
     *   Guru Meditation (Stack protection fault)，崩在 snprintf 里。
     * 调用方只有工作线程（单线程），static 安全。
     */
    static char pairs[12][1024];      /* 去重用的暂存（白名单最多 9 个名字） */
    size_t n_pairs = 0;

    const char *p = raw;
    while (*p) {
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : (p + strlen(p));
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        const char *e = end;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) {
            e--;
        }
        const char *eq = (const char *)memchr(p, '=', (size_t)(e - p));
        if (eq && eq > p) {
            const size_t nlen = (size_t)(eq - p);
            bool is_attr = false;
            for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
                if (strlen(attrs[i]) == nlen && strncasecmp(p, attrs[i], nlen) == 0) {
                    is_attr = true;
                    break;
                }
            }
            char name[64];
            if (!is_attr && nlen < sizeof(name) && (size_t)(e - p) < sizeof(pairs[0])) {
                memcpy(name, p, nlen);
                name[nlen] = '\0';
                /* 同名覆盖：找到旧的替换掉，找不到就追加 */
                size_t slot = n_pairs;
                for (size_t i = 0; i < n_pairs; i++) {
                    const char *q = strchr(pairs[i], '=');
                    if (q && (size_t)(q - pairs[i]) == nlen && strncmp(pairs[i], name, nlen) == 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot == n_pairs && n_pairs < sizeof(pairs) / sizeof(pairs[0])) {
                    n_pairs++;
                }
                if (slot < sizeof(pairs) / sizeof(pairs[0])) {
                    snprintf(pairs[slot], sizeof(pairs[slot]), "%.*s", (int)(e - p), p);
                }
            }
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }

    size_t used = 0;
    /*
     * 输出顺序：**先把 MUSIC_U / __csrf 放最前**，再放其余。
     * 万一后面还要按缓冲截断，至少认证 cookie 一定在 —— 这是"宁缺其他、不丢认证"。
     */
    static const char *const prio[] = {"MUSIC_U=", "__csrf="};
    for (int pass = 0; pass < 3 && used + 2 < cap; pass++) {
        for (size_t i = 0; i < n_pairs && used + 2 < cap; i++) {
            bool want;
            if (pass == 0) {
                want = (strncmp(pairs[i], prio[0], strlen(prio[0])) == 0);
            } else if (pass == 1) {
                want = (strncmp(pairs[i], prio[1], strlen(prio[1])) == 0);
            } else {
                want = !(strncmp(pairs[i], prio[0], strlen(prio[0])) == 0 ||
                         strncmp(pairs[i], prio[1], strlen(prio[1])) == 0);
            }
            if (!want) {
                continue;
            }
            const int w = snprintf(out + used, cap - used, "%s%s", used ? "; " : "", pairs[i]);
            if (w <= 0 || (size_t)w >= cap - used) {
                break;                    /* 装不下了：宁可少几个非关键 cookie */
            }
            used += (size_t)w;
        }
    }
    if (used == 0) {
        /* 一个都没留下（服务端换了写法？）：退回原串，至少不比以前更差 */
        snprintf(out, cap, "%.*s", (int)(cap - 1), raw);
    }
}

static void net_cookie_save(const char *cookie)
{
    if (!cookie || !cookie[0]) {
        return;
    }
    static char compact[NET_COOKIE_SEND];   /* 同上：别放在只有 4 KB 的工作线程栈上 */
    cookie_compact(cookie, compact, sizeof(compact));
    snprintf(s_net_cookie, sizeof(s_net_cookie), "%s", compact);
    s_net_logged_in = true;
    s_net_cookie_loaded = true;

    FILE *f = fopen(NET_COOKIE_FILE, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cookie save failed (SD 没挂？登录态只在内存里)");
        return;
    }
    fwrite(s_net_cookie, 1, strlen(s_net_cookie), f);
    fclose(f);
    /* 两个长度都打出来：raw 是服务端给的，compact 是我们真正存/发的大小 */
    ESP_LOGI(TAG, "netease cookie saved (raw %u -> compact %u bytes, MUSIC_U %s)",
             (unsigned)strlen(cookie), (unsigned)strlen(s_net_cookie),
             strstr(s_net_cookie, "MUSIC_U=") ? "ok" : "MISSING!");
}

bool online_netease_logged_in(void)
{
    net_cookie_load();
    return s_net_logged_in;
}

/*
 * 重读 cookie：只在"当前判定未登录"时才真去读。
 *
 * 使用场景（用户 2026-10 报"重启后进在线页，歌单什么的都没加载出来"）：
 *   开机后立刻进在线页时 SD 可能还没挂上，那次 fopen 失败被 net_cookie_load()
 *   当成了"没登录过"（它会 latch）——界面于是停在未登录态，之后 SD 挂好也不会再读。
 *   账号页每次重建都调一次本函数，挂载完成后自动恢复登录态。
 */
esp_err_t online_netease_cookie_reload(void)
{
    if (s_net_logged_in) {
        return ESP_OK;                     /* 内存里已经有了，不用再碰 SD */
    }
    s_net_cookie_loaded = false;           /* 允许 net_cookie_load() 再读一次 */
    s_net_cookie[0] = '\0';
    net_cookie_load();
    return s_net_logged_in ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------------- 昵称（NVS） */

static void nvs_load_nick(void)
{
    if (s_net_nick_loaded) {
        return;                            /* 覆盖"没存过"的情况：别每次都开一次 NVS */
    }
    s_net_nick_loaded = true;
    nvs_handle_t h;
    s_net_nick[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_net_nick);
    if (nvs_get_str(h, NVS_KEY_NICK, s_net_nick, &len) != ESP_OK) {
        s_net_nick[0] = '\0';
    }
    nvs_close(h);
}

const char *online_netease_nickname(void)
{
    nvs_load_nick();                       /* 懒加载：init 之前被问到也能拿到 */
    return s_net_nick;
}

esp_err_t online_netease_nickname_set(const char *name)
{
    /* NVS 键的语义是"最后一次见到的昵称"，所以先更新内存里的那份 */
    if (!name) {
        name = "";
    }
    if (s_net_nick_loaded && strcmp(s_net_nick, name) == 0) {
        return ESP_OK;                     /* 没变：别擦写 flash */
    }
    snprintf(s_net_nick, sizeof(s_net_nick), "%s", name);
    s_net_nick_loaded = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err;
    if (s_net_nick[0]) {
        err = nvs_set_str(h, NVS_KEY_NICK, s_net_nick);
    } else {
        err = nvs_erase_key(h, NVS_KEY_NICK);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;                  /* 本来就没有：也算成功 */
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "netease nickname %s: %s", s_net_nick[0] ? "saved" : "cleared",
             s_net_nick[0] ? s_net_nick : "-");
    return err;
}

const char *online_netease_cookie(void)
{
    net_cookie_load();
    return s_net_cookie;
}

esp_err_t online_netease_logout(void)
{
    s_net_cookie[0] = '\0';
    s_net_logged_in = false;
    s_net_cookie_loaded = true;
    remove(NET_COOKIE_FILE);
    online_netease_nickname_set(NULL);  /* 昵称跟着一起清，别让下个账号看到上个的 */
    online_cache_clear();              /* 顺手清 PSRAM 缓存，账号歌单下次重新拉 */
    ESP_LOGI(TAG, "netease logged out");
    return ESP_OK;
}

/*
 * 拼 base + path 发 GET，返回 JSON 文本（调用方 free）。
 * with_cookie = true 时追加 `cookie=<urlencoded>`（账号类接口全部要它）。
 *
 * ⚠ 缓冲区按"base(≤96) + path(≤200) + 编码后的 cookie(~600)"取 1280：
 *   netease 的 cookie 里 MUSIC_U 很长且含 % + = 等字符，**必须 URL 编码**，
 *   否则服务端只拿到半截 cookie → 表现为"随机未登录"。
 */
#define NET_URL_MAX 4096

static esp_err_t net_api_get_ex(const char *path, bool with_cookie, char **out_json)
{
    /* static 而不是栈上：账号接口的 URL 可以到 2~3 KB，而工作线程栈不大 */
    static char url[NET_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s%s", online_netease_api_base(), path);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (with_cookie) {
        /*
         * ★ 发账号类请求前再确认一次登录态：开机后SD 晚挂 / cookie 文件刚写好时，
         *   第一次读到的是"空 cookie"，服务端会**按游客返回**（不是报错）——
         *   表现就是"重启后歌单/每日推荐都是空的或者别人的"。这里补一次重读。
         */
        online_netease_cookie_reload();
        const char *ck = online_netease_cookie();
        size_t used = strlen(url);
        if (ck[0] && used + 16 < sizeof(url)) {
            url[used++] = (strchr(path, '?') != NULL) ? '&' : '?';
            const char *pre = "cookie=";
            while (*pre && used + 1 < sizeof(url)) {
                url[used++] = *pre++;
            }
            for (const char *p = ck; *p && used + 4 < sizeof(url); p++) {
                const unsigned char c = (unsigned char)*p;
                const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                  (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
                                  c == '.' || c == '~';
                if (safe) {
                    url[used++] = (char)c;
                } else {
                    static const char hx[] = "0123456789ABCDEF";
                    url[used++] = '%';
                    url[used++] = hx[c >> 4];
                    url[used++] = hx[c & 0x0F];
                }
            }
            url[used] = '\0';
        }
    }

    *out_json = NULL;
    size_t len = 0;
    esp_err_t err = http_get_text(url, out_json, &len, NET_JSON_MAX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "netease GET %s failed: %s", path, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t net_api_get(const char *path, char **out_json)
{
    return net_api_get_ex(path, false, out_json);
}

esp_err_t online_netease_qr_key(char *out_key, size_t cap)
{
    if (!out_key || cap < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    out_key[0] = '\0';
    char *js = NULL;
    /*
     * ★ 请求参数必须和官方网页端 public/qrlogin.html 完全一致，否则会"永远 801"：
     *   /login/qr/key?timestamp=<ms>
     *   /login/qr/create?key=<k>&platform=web&qrimg=true&timestamp=<ms>&ua=ESP32-S31-Korvo-1
     *   /login/qr/check?key=<k>&timestamp=<ms>&ua=ESP32-S31-Korvo-1     ← 少了 timestamp 就会拿到
     *                                                        **缓存的 801**，手机确认了也不变
     */
    char path[64];
    snprintf(path, sizeof(path), "/login/qr/key?timestamp=%lld&ua=ESP32-S31-Korvo-1",
             (long long)(esp_timer_get_time() / 1000));
    esp_err_t err = net_api_get(path, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *uk = json_find_str(root, "unikey");
    if (!uk || !uk[0]) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "qr/key: 响应里没有 unikey");
        return ESP_ERR_INVALID_RESPONSE;
    }
    snprintf(out_key, cap, "%.*s", (int)(cap - 1), uk);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "qr key: %s", out_key);
    return ESP_OK;
}

esp_err_t online_netease_qr_create(const char *key, char *out_url, size_t url_cap,
                                   uint8_t **qrimg, size_t *qrimg_len)
{
    if (!key || !key[0] || !qrimg || !qrimg_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *qrimg = NULL;
    *qrimg_len = 0;
    if (out_url && url_cap) {
        out_url[0] = '\0';
    }

    char path[256];
    int n = snprintf(path, sizeof(path),
                     "/login/qr/create?key=%s&platform=web&qrimg=true&timestamp=%lld&ua=ESP32-S31-Korvo-1",
                     key, (long long)(esp_timer_get_time() / 1000));
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *js = NULL;
    esp_err_t err = net_api_get(path, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *url = json_find_str(root, "qrurl");
    if (url && out_url && url_cap) {
        snprintf(out_url, url_cap, "%.*s", (int)(url_cap - 1), url);
    }

    const char *img = json_find_str(root, "qrimg");
    if (!img || !img[0]) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "qr/create: 没有 qrimg（服务端版本或 qrimg 参数没生效）");
        return ESP_ERR_NOT_FOUND;
    }

    /* "data:image/png;base64,XXXX" → 跳过逗号，把 base64 解成 PNG 原始字节 */
    const char *b64 = strchr(img, ',');
    b64 = b64 ? (b64 + 1) : img;
    const size_t b64_len = strlen(b64);
    const size_t dec_max = (b64_len / 4 + 1) * 3;
    uint8_t *buf = (uint8_t *)psram_alloc(dec_max);
    if (!buf) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t dec_len = 0;
    const int rc = mbedtls_base64_decode(buf, dec_max, &dec_len,
                                         (const unsigned char *)b64, b64_len);
    cJSON_Delete(root);
    if (rc != 0) {
        free(buf);
        ESP_LOGW(TAG, "qr image base64 解码失败 (-0x%04x)", (unsigned)-rc);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *qrimg = buf;
    *qrimg_len = dec_len;
    ESP_LOGI(TAG, "qr image: %u bytes (%s)", (unsigned)dec_len,
             (dec_len > 8 && buf[1] == 'P') ? "PNG" : "非 PNG?");
    return ESP_OK;
}

esp_err_t online_netease_qr_poll(const char *key, int *out_code)
{
    if (!key || !key[0] || !out_code) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * ★★ 这里**必须**带 timestamp：少了它，每次轮询请求完全相同，服务端会把第一次的
     *    801（等待扫码）**缓存着反复返回** —— 手机扫码并确认了，界面还是"等待扫码"，
     *    而且日志一切正常（这就是 2026-10 用户报的那个现象）。官方网页端也是这么发的：
     *    /login/qr/check?key=<k>&timestamp=<ms>&ua=ESP32-S31-Korvo-1
     */
    char path[192];
    int n = snprintf(path, sizeof(path), "/login/qr/check?key=%s&timestamp=%lld&ua=ESP32-S31-Korvo-1",
                     key, (long long)(esp_timer_get_time() / 1000));
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *js = NULL;
    esp_err_t err = net_api_get(path, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    if (!root) {
        free(js);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int code = 0;
    const bool got = json_find_int(root, "code", &code);
    if (got && code == NET_QR_OK) {
        const char *ck = json_find_str(root, "cookie");
        if (ck && ck[0]) {
            net_cookie_save(ck);
        } else {
            ESP_LOGW(TAG, "qr check: 803 但响应里没有 cookie");
        }
    }
    cJSON_Delete(root);
    free(js);
    if (!got) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_code = code;
    ESP_LOGI(TAG, "qr check -> %d", code);
    return ESP_OK;
}

/* ============================================================ 账号数据（P3）
 *
 * 全部带 cookie 调。cookie 失效时 /user/account 的 profile 为 null（实测），
 * 上层据此提示"重新登录"。
 */

/* JSON 数字 ID → 字符串：歌/歌单 ID 是 10 位数（>2^31），
 * cJSON 的 valueint 是 32 位会溢出，必须走 valuedouble。 */
static void json_id_str(const cJSON *item, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (cJSON_IsNumber(item)) {
        snprintf(out, cap, "%.0f", item->valuedouble);
    } else if (cJSON_IsString(item)) {
        snprintf(out, cap, "%.*s", (int)(cap - 1), item->valuestring);
    }
}

/* 找数组：primary_key 直接是数组，或藏在 data/playlist/result/body 里 */
static const cJSON *pick_array(const cJSON *root, const char *primary_key)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, primary_key);
    if (cJSON_IsArray(a)) {
        return a;
    }
    static const char *const wraps[] = {"data", "playlist", "result", "body"};
    for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, wraps[i]);
        if (!cJSON_IsObject(w)) {
            continue;
        }
        const cJSON *inner = cJSON_GetObjectItemCaseSensitive(w, primary_key);
        if (cJSON_IsArray(inner)) {
            return inner;
        }
    }
    return NULL;
}

/* 网易云歌曲对象 → online_song_t（只填元数据；audio_url 播放时按需取直链） */
static bool song_from_json(const cJSON *song, online_song_t *out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(song, "id");
    if (!cJSON_IsNumber(id)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->id = (uint32_t)id->valuedouble;

    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(song, "name");
    if (cJSON_IsString(nm)) {
        snprintf(out->title, sizeof(out->title), "%.*s", (int)sizeof(out->title) - 1,
                 nm->valuestring);
    }

    /* 歌手：新格式 ar[]，老格式 artists[]（两种接口都可能返回） */
    const cJSON *ar = cJSON_GetObjectItemCaseSensitive(song, "ar");
    if (!cJSON_IsArray(ar)) {
        ar = cJSON_GetObjectItemCaseSensitive(song, "artists");
    }
    if (cJSON_IsArray(ar)) {
        size_t used = 0;
        const cJSON *a = NULL;
        cJSON_ArrayForEach(a, ar) {
            const cJSON *an = cJSON_GetObjectItemCaseSensitive(a, "name");
            if (!cJSON_IsString(an)) {
                continue;
            }
            const int w = snprintf(out->artist + used, sizeof(out->artist) - used,
                                   "%s%s", used ? " / " : "", an->valuestring);
            if (w <= 0 || (size_t)w >= sizeof(out->artist) - used) {
                break;
            }
            used += (size_t)w;
        }
    }

    /* 封面：新格式 al.picUrl，老格式 album.picUrl */
    const cJSON *al = cJSON_GetObjectItemCaseSensitive(song, "al");
    if (!cJSON_IsObject(al)) {
        al = cJSON_GetObjectItemCaseSensitive(song, "album");
    }
    if (cJSON_IsObject(al)) {
        const cJSON *pic = cJSON_GetObjectItemCaseSensitive(al, "picUrl");
        if (cJSON_IsString(pic)) {
            snprintf(out->cover_url, sizeof(out->cover_url), "%.*s",
                     (int)sizeof(out->cover_url) - 1, pic->valuestring);
        }
    }

    /*
     * 歌词：账号歌没有 meting 生成的 lrc_url，这里按同一个 meting 接口补一条
     * （播放端 online_song_lyrics_data() 就是去取这个 URL 的**纯文本 LRC**，
     *  所以不能换成网易云 /lyric 那个返回 JSON 的接口）。
     */
    if (!out->lrc_url[0] && out->id != 0) {
        snprintf(out->lrc_url, sizeof(out->lrc_url),
                 API_BASE "?server=netease&type=lrc&id=%u", (unsigned)out->id);
    }
    return true;
}

static esp_err_t fill_songs(const cJSON *arr, online_song_t *out, size_t max, size_t *out_n)
{
    size_t cnt = 0;
    if (cJSON_IsArray(arr)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (cnt >= max) {
                break;
            }
            if (song_from_json(it, &out[cnt])) {
                cnt++;
            }
        }
    }
    *out_n = cnt;
    return cnt ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t online_netease_account(online_account_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char *js = NULL;
    esp_err_t err = net_api_get_ex("/user/account", true, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (cJSON_IsObject(profile)) {
        json_id_str(cJSON_GetObjectItemCaseSensitive(profile, "userId"), out->uid,
                    sizeof(out->uid));
        const cJSON *nick = cJSON_GetObjectItemCaseSensitive(profile, "nickname");
        if (cJSON_IsString(nick)) {
            snprintf(out->nickname, sizeof(out->nickname), "%.*s",
                     (int)sizeof(out->nickname) - 1, nick->valuestring);
        }
        if (out->uid[0]) {
            ret = ESP_OK;
        }
    }
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "account: 未登录或 cookie 已失效");
    } else {
        /* 昵称落 NVS：重启后 UI 立刻能显示，不用等这次网络往返 */
        online_netease_nickname_set(out->nickname);
        ESP_LOGI(TAG, "account: %s (%s)", out->nickname, out->uid);
    }
    return ret;
}

esp_err_t online_netease_account_playlists(const char *uid, online_acct_playlist_t *out,
                                           size_t max, size_t *out_n)
{
    if (!uid || !uid[0] || !out || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    /*
     * ★ 必须**分页**（2026-10 用户报"账号歌单列不全"）：
     *   服务端 module/user_playlist.js 是
     *     { uid, limit: query.limit || 30, offset: query.offset || 0, includeVideo: true }
     *   并且响应里带 `more` 标志。只取第一页 = 只拿到前 N 个歌单。
     *   这里按 limit=100 翻页，直到 more=false / 空页 / 塞满 max（另设 10 页上限防死循环）。
     */
    size_t cnt = 0;
    bool truncated = false;
    int offset = 0;
    const int limit = 100;
    for (int page = 0; page < 10; page++) {
        char path[160];
        int n = snprintf(path, sizeof(path), "/user/playlist?uid=%s&limit=%d&offset=%d",
                         uid, limit, offset);
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        char *js = NULL;
        esp_err_t err = net_api_get_ex(path, true, &js);
        if (err != ESP_OK) {
            return err;
        }
        cJSON *root = cJSON_Parse(js);
        free(js);
        if (!root) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "playlist");
        size_t got = 0;
        if (cJSON_IsArray(arr)) {
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, arr) {
                got++;
                if (cnt >= max) {
                    truncated = true;   /* 到上限了：日志里说清楚，不静默丢 */
                    break;
                }
                online_acct_playlist_t *d = &out[cnt];
                memset(d, 0, sizeof(*d));
                json_id_str(cJSON_GetObjectItemCaseSensitive(it, "id"), d->id, sizeof(d->id));
                if (!d->id[0]) {
                    continue;
                }
                const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
                if (cJSON_IsString(nm)) {
                    snprintf(d->name, sizeof(d->name), "%.*s", (int)sizeof(d->name) - 1,
                             nm->valuestring);
                }
                const cJSON *pic = cJSON_GetObjectItemCaseSensitive(it, "coverImgUrl");
                if (cJSON_IsString(pic)) {
                    snprintf(d->cover_url, sizeof(d->cover_url), "%.*s",
                             (int)sizeof(d->cover_url) - 1, pic->valuestring);
                }
                const cJSON *tc = cJSON_GetObjectItemCaseSensitive(it, "trackCount");
                if (cJSON_IsNumber(tc)) {
                    d->track_count = (int)tc->valuedouble;
                }
                const cJSON *cr = cJSON_GetObjectItemCaseSensitive(it, "creator");
                if (cJSON_IsObject(cr)) {
                    char cid[24];
                    json_id_str(cJSON_GetObjectItemCaseSensitive(cr, "userId"), cid, sizeof(cid));
                    d->self_created = (cid[0] && strcmp(cid, uid) == 0);
                }
                cnt++;
            }
        }
        /* more 是布尔字面量：用 cJSON 的 type 判断，避开不同版本的 API 差异 */
        const cJSON *more = cJSON_GetObjectItemCaseSensitive(root, "more");
        const bool has_more = (more && more->type == cJSON_True);
        cJSON_Delete(root);

        offset += (int)got;
        if (!has_more || got == 0 || truncated) {
            break;
        }
    }
    *out_n = cnt;
    ESP_LOGI(TAG, "account playlists: %u (uid=%s%s)", (unsigned)cnt, uid,
             truncated ? ", 已达显示上限" : "");
    return cnt ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/*
 * /playlist/detail 兜底：一次返回整个歌单对象（含 tracks）。大歌单同样会超
 * NET_JSON_MAX，所以只在"track/all 第一页就失败"时试一下 —— 能救小歌单/特殊版权单。
 */
static esp_err_t playlist_detail_fallback(const char *pl_id, online_song_t *out,
                                          size_t max, size_t *out_n)
{
    char path[128];
    const int n = snprintf(path, sizeof(path), "/playlist/detail?id=%s", pl_id);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = net_api_get_ex(path, true, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = fill_songs(pick_array(root, "tracks"), out, max, out_n);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "playlist %s (detail fallback): %u songs", pl_id, (unsigned)*out_n);
    return err;
}

esp_err_t online_netease_playlist_page(const char *pl_id, size_t offset,
                                       online_song_t *out, size_t max, size_t *out_n)
{
    if (!pl_id || !pl_id[0] || !out || !out_n || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★★ **一次只取一页**（见头文件里的体积账）：/playlist/track/all 返回的是完整
     *   song detail 对象（≈2.3 KB/首），300 首 ≈700 KB 会超 NET_JSON_MAX(512 KB)
     *   而整单失败。所以这里按 100 首一页循环，单页响应 ≈250 KB。
     *   max 由调用方给（app_online 一次最多要一两页），到 max 或歌单结束就停。
     */
    size_t got_total = 0;
    while (got_total < max) {
        const size_t want = ((max - got_total) > ONLINE_PL_PAGE_SONGS)
                                ? (size_t)ONLINE_PL_PAGE_SONGS : (max - got_total);
        char path[160];
        const int n = snprintf(path, sizeof(path),
                               "/playlist/track/all?id=%s&limit=%u&offset=%u",
                               pl_id, (unsigned)want, (unsigned)(offset + got_total));
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        char *js = NULL;
        const esp_err_t err = net_api_get_ex(path, true, &js);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "playlist %s page @%u failed: %s (%s)", pl_id,
                     (unsigned)(offset + got_total), esp_err_to_name(err), s_last_err);
            if (got_total > 0) {
                break;                     /* 已经有数据：把已取到的交出去 */
            }
            /* 第一页就失败 → 老接口兜底（只对小歌单有效） */
            if (offset == 0) {
                return playlist_detail_fallback(pl_id, out, max, out_n);
            }
            return err;
        }
        cJSON *root = cJSON_Parse(js);
        free(js);
        if (!root) {
            if (got_total > 0) {
                break;
            }
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t got = 0;
        fill_songs(pick_array(root, "songs"), out + got_total, want, &got);
        cJSON_Delete(root);
        got_total += got;
        if (got < want) {
            break;                         /* 不足一页 = 到歌单末尾 */
        }
    }
    if (got_total == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_n = got_total;
    ESP_LOGI(TAG, "playlist %s: +%u songs (offset %u)", pl_id, (unsigned)got_total,
             (unsigned)offset);
    return ESP_OK;
}

esp_err_t online_netease_daily_songs(online_song_t *out, size_t max, size_t *out_n)
{
    if (!out || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    /*
     * ★ 先体检 cookie：服务端 util/request.js 里写着
     *     if (!processedCookie.MUSIC_U) processedCookie.MUSIC_A = anonymous_token
     *   —— **没有 MUSIC_U 时不报错，直接按游客返回**。所以"每日推荐还是未登录的"
     *   这类现象，答案往往就在这一行日志里。
     */
    ESP_LOGI(TAG, "daily recommend: cookie %s",
             strstr(online_netease_cookie(), "MUSIC_U=") ? "带 MUSIC_U" :
             "**没有 MUSIC_U**（服务端会按游客返回！）");
    char *js = NULL;
    esp_err_t err = net_api_get_ex("/recommend/songs", true, &js);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "daily recommend: 请求失败 %s（%s）", esp_err_to_name(err), s_last_err);
        return err;
    }
    ESP_LOGI(TAG, "daily recommend: 响应 %u 字节", (unsigned)strlen(js));
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = fill_songs(pick_array(root, "dailySongs"), out, max, out_n);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "daily recommend: %u songs", (unsigned)*out_n);
    return err;
}

esp_err_t online_netease_song_url(const char *song_id, char *out, size_t cap,
                                  int64_t *out_bytes, uint32_t *out_dur_ms)
{
    if (!song_id || !song_id[0] || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_bytes) {
        *out_bytes = 0;
    }
    if (out_dur_ms) {
        *out_dur_ms = 0;
    }

    char path[128];
    int n = snprintf(path, sizeof(path), "/song/url/v1?id=%s&level=standard", song_id);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = net_api_get_ex(path, true, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    const cJSON *arr = pick_array(root, "data");
    const cJSON *first = cJSON_IsArray(arr) ? arr->child : NULL;
    if (first) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(first, "url");
        if (cJSON_IsString(u) && u->valuestring && u->valuestring[0]) {
            snprintf(out, cap, "%.*s", (int)(cap - 1), u->valuestring);
            const cJSON *sz = cJSON_GetObjectItemCaseSensitive(first, "size");
            const cJSON *br = cJSON_GetObjectItemCaseSensitive(first, "br");
            if (out_bytes && cJSON_IsNumber(sz)) {
                *out_bytes = (int64_t)sz->valuedouble;
            }
            if (out_dur_ms && cJSON_IsNumber(sz) && cJSON_IsNumber(br) &&
                br->valuedouble > 0) {
                /* 总时长 = 字节 × 8 ÷ 码率（这条路上时长是**准确**的，不用再靠墙钟估） */
                *out_dur_ms = (uint32_t)(sz->valuedouble * 8.0 * 1000.0 / br->valuedouble);
            }
            ret = ESP_OK;
        }
    }
    cJSON_Delete(root);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "song url %s: %lld bytes, %u ms", song_id,
                 (long long)(out_bytes ? *out_bytes : 0),
                 (unsigned)(out_dur_ms ? *out_dur_ms : 0));
    } else {
        ESP_LOGW(TAG, "song url %s: 没拿到直链（VIP / 版权 / cookie 失效）", song_id);
    }
    return ret;
}
