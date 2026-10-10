/*
 * os_media_cache.cc —— 封面 / 歌词（PSRAM 缓存，不落 SD）+ 播放前的轻量准备
 *
 * 逐字搬自原 online_service.cc：
 *   cover_format           1325–1346
 *   online_song_cover_data 1348–1440
 *   online_song_lyrics_data 1446–1495
 *   psram_cache_clear      1764–1774
 *   online_song_prepare_stream 1747–1761
 *   online_song_prepare    1259–1307  ← 见下面的"搬动理由"
 *
 * ★ 与蓝图的差异（唯一一处成规模的调整，其余按 §6.2 执行）：
 *   蓝图把 online_song_prepare() 放在 os_audio_cache.cc、online_song_prepare_stream()
 *   放在 os_stream_url.cc。但这两个函数都是"调用封面/歌词 API + free 掉拷贝"的
 *   编排代码，实现必须 include 本文件的接口；而本文件（封面/歌词）又要
 *   find_cached_audio / audio_cache_trim / http_download。
 *   实测依赖是单向的：编排 → {封面/歌词, 音频缓存}，所以把三个"准备/预热"函数
 *   统一放在本文件，os_audio_cache.h 与 os_stream_url.h 都不必反向依赖媒体缓存，
 *   头文件图无环、职责也更纯（本文件 = "缓存与预热"，不掺 302 解析）。
 *
 * ⚠ 封面/歌词**只进 PSRAM、不落 SD** 的所有权约定见 os_media_cache.h 的表：
 *   online_song_cover_data() 返回的是**调用方所有的 malloc 拷贝**，缓存里那份
 *   永不外发（所以 LRU 淘汰不会踩到正在播的封面）。
 */
#include "internal/os_media_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#include "internal/os_audio_cache.h"
#include "internal/os_cache.h"
#include "internal/os_fs.h"
#include "internal/os_http_client.h"
#include "internal/os_state.h"
#include "internal/os_util.h"

static const char *TAG = "online";

static os_cache_t s_cover_cache[COVER_CACHE_MAX];
static size_t     s_cover_cache_bytes;
static os_cache_t s_lrc_cache[LRC_CACHE_MAX];
static size_t     s_lrc_cache_bytes;

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
const char *os_cover_format(const uint8_t *d, size_t n, const char **why)
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

/*
 * 封面字节（PSRAM 缓存，不落 SD）。
 * 命中缓存直接拷贝给调用方；没命中就下（封面 URL 是 http://p*.music.126.net，
 * 直接 http_get_text 即可），进缓存 + 拷贝。
 * *out 是调用方所有的新缓冲，用 free() 释放。
 */
esp_err_t online_song_cover_data(const online_song_t *song, uint8_t **out, size_t *out_len)
{
    if (!song || !out || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }

    os_cache_t *hit = os_cache_find(s_cover_cache, COVER_CACHE_MAX, song->id);
    if (hit) {
        uint8_t *copy = (uint8_t *)os_psram_alloc(hit->len);
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
            char msg[64];
            snprintf(msg, sizeof(msg), "封面太大(>%u MB)",
                     (unsigned)(COVER_MAX_BYTES / (1024u * 1024u)));
            os_state_set_last_error(msg);
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
    const char *fmt = os_cover_format((const uint8_t *)raw, len, &why);
    if (!fmt) {
        ESP_LOGW(TAG, "cover %u: unsupported %s (magic %02x %02x %02x %02x, %u bytes, url=%s)",
                 (unsigned)song->id, why ? why : "format",
                 (unsigned char)raw[0], (unsigned char)raw[1],
                 (unsigned char)raw[2], (unsigned char)raw[3], (unsigned)len, url);
        /* 中文全角括号一个 3 字节，GCC 证不出 64 字节缓冲放得下（-Werror=format-truncation），
         * 按契约显式限长：前缀 7 字 ≈ 21 字节 + 20 + NUL，稳在 64 内 */
        char msg[64];
        snprintf(msg, sizeof(msg), "封面格式不支持：%.20s", why ? why : "未知");
        os_state_set_last_error(msg);
        free(raw);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "cover %u: %s, %u bytes", (unsigned)song->id, fmt, (unsigned)len);

    os_cache_put(s_cover_cache, COVER_CACHE_MAX, &s_cover_cache_bytes,
                 COVER_CACHE_BYTES_MAX, song->id, (uint8_t *)raw, len);
    uint8_t *copy = (uint8_t *)os_psram_alloc(len);
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

    os_cache_t *hit = os_cache_find(s_lrc_cache, LRC_CACHE_MAX, song->id);
    if (hit) {
        char *copy = (char *)os_psram_alloc(hit->len + 1);
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
    os_cache_put(s_lrc_cache, LRC_CACHE_MAX, &s_lrc_cache_bytes,
                 LRC_CACHE_BYTES_MAX, song->id, (uint8_t *)text, len);
    char *copy = (char *)os_psram_alloc(len + 1);
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

/* 清掉 PSRAM 里的封面/歌词缓存 */
void os_media_cache_clear(void)
{
    os_cache_reset(s_cover_cache, COVER_CACHE_MAX, &s_cover_cache_bytes);
    os_cache_reset(s_lrc_cache, LRC_CACHE_MAX, &s_lrc_cache_bytes);
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

/*
 * 兜底路径：把整首歌下载到 SD（命中缓存直接返回），再当本地文件播放。
 * 流式失败时回退用它；也可以用来做离线缓存。
 */
esp_err_t online_song_prepare(const online_song_t *song, char *audio_path, size_t cap)
{
    if (!song || !audio_path || cap == 0 || song->id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_path[0] = '\0';

    if (os_find_cached_audio(song->id, audio_path, cap)) {
        /*
         * ★ 缓存体检：老缓存可能是**匿名时代下到的 30 秒试听片段**
         *   （2026-10 换"CK 直链优先"之前，会员歌就是这么被下下来的）。
         *   拿服务端这次报的完整大小比一比：明显偏小就作废重下 ——
         *   否则用户会一边"已经登录了"、一边永远听片段。
         */
        char probe[1024] = {0};
        int64_t full = 0;
        if (online_song_stream_info(song, probe, sizeof(probe), &full, NULL) == ESP_OK &&
            full > 0) {
            struct stat st = {};
            if (stat(audio_path, &st) == 0 && st.st_size > 0 &&
                (int64_t)st.st_size + (int64_t)st.st_size / 10 < full) {
                ESP_LOGW(TAG, "cache %s is %ld B but stream is %lld B: 疑似试听片段缓存，重下",
                         audio_path, (long)st.st_size, (long long)full);
                unlink(audio_path);
                audio_path[0] = '\0';
            }
        }
        if (audio_path[0]) {
            ESP_LOGI(TAG, "audio cache hit: %s", audio_path);
            return ESP_OK;
        }
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 先下到临时文件，嗅探格式后再改名（扩展名决定用哪个解码器） */
    char tmp[192];
    snprintf(tmp, sizeof(tmp), ONLINE_AUDIO_DIR "/%u.part", (unsigned)song->id);
    uint8_t sniff[16] = {};

    /*
     * ★ 下载也要走"登录 CK + 中间件"那条正路（online_song_stream_url 内部已经
     *   优先取 CK 直链、失败才退回 meting）。直接用 `song->audio_url` 的话，
     *   会员歌下到的是**30 秒试听片段** —— "下载兜底"的 m4a/ogg/opus 与手动缓存
     *   都会中招。
     *   缓冲取 1024：网易 CDN 的签名直链（含 `?vuutv=…`）实测 200~400 字符，
     *   `online_song_t.audio_url` 只有 320，装不下。
     */
    char dl_url[1024] = {0};
    if (online_song_stream_url(song, dl_url, sizeof(dl_url)) != ESP_OK || !dl_url[0]) {
        ESP_LOGW(TAG, "no stream url for id %u (account CK + meting 都没拿到)",
                 (unsigned)song->id);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = http_download(dl_url, tmp, 64 * 1024, sniff);
    if (ret != ESP_OK) {
        return ret;
    }
    char ext[8];
    os_ext_from_sniff(sniff, ext, sizeof(ext));
    snprintf(audio_path, cap, ONLINE_AUDIO_DIR "/%u%s", (unsigned)song->id, ext);
    unlink(audio_path);
    if (rename(tmp, audio_path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloaded '%s' -> %s", song->title, audio_path);

    os_audio_cache_trim();

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
