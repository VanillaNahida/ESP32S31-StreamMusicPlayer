/*
 * os_json.cc —— cJSON 取值与"网易云对象 → online_song_t"的实现
 *
 * 逐字搬自原 online_service.cc，只加 os_ 前缀：
 *   json_find_str   2088–2104
 *   json_find_int   2106–2122
 *   json_id_str     2790–2801
 *   pick_array      2804–2822
 *   song_from_json  2825–2885
 *   fill_songs      2887–2903
 *   parse_songs      904–958
 * 依赖只有 cJSON + os_util + os_fs（psram_alloc），注释一律跟着搬。
 */
#include "internal/os_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "internal/os_fs.h"
#include "internal/os_state.h"   /* API_BASE：song_from_json 合成的 lrc_url 用它 */
#include "internal/os_util.h"

static const char *TAG = "online";

/*
 * 在 JSON 里找字段：**先看最外层，再递归进去**。
 *
 * 为什么强调"最外层优先"：实测服务端响应里内外层都可能有同名 code ——
 *   /login/qr/key → {"data":{"code":200,"unikey":"…"},"code":200}
 * 如果递归时先命中内层的 code，/login/qr/check 的扫码状态（800/801/802/803）
 * 就会被内层的 200 顶掉，状态机整条失效。所以第一遍只扫直接子节点。
 */
const char *os_json_find_str(const cJSON *root, const char *key)
{
    for (const cJSON *c = root; c; c = c->next) {          /* 第一遍：直接子节点 */
        if (cJSON_IsString(c) && c->string && strcmp(c->string, key) == 0) {
            return c->valuestring;
        }
    }
    for (const cJSON *c = root; c; c = c->next) {          /* 第二遍：递归 */
        if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && c->child) {
            const char *r = os_json_find_str(c->child, key);
            if (r) {
                return r;
            }
        }
    }
    return NULL;
}

bool os_json_find_int(const cJSON *root, const char *key, int *out)
{
    for (const cJSON *c = root; c; c = c->next) {          /* 第一遍：直接子节点 */
        if (cJSON_IsNumber(c) && c->string && strcmp(c->string, key) == 0) {
            *out = c->valueint;
            return true;
        }
    }
    for (const cJSON *c = root; c; c = c->next) {          /* 第二遍：递归 */
        if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && c->child) {
            if (os_json_find_int(c->child, key, out)) {
                return true;
            }
        }
    }
    return false;
}

/* JSON 数字 ID → 字符串：歌/歌单 ID 是 10 位数（>2^31），
 * cJSON 的 valueint 是 32 位会溢出，必须走 valuedouble。 */
void os_json_id_str(const cJSON *item, char *out, size_t cap)
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
const cJSON *os_pick_array(const cJSON *root, const char *primary_key)
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
bool os_song_from_json(const cJSON *song, online_song_t *out)
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

esp_err_t os_fill_songs(const cJSON *arr, online_song_t *out, size_t max, size_t *out_n)
{
    size_t cnt = 0;
    if (cJSON_IsArray(arr)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (cnt >= max) {
                break;
            }
            if (os_song_from_json(it, &out[cnt])) {
                cnt++;
            }
        }
    }
    *out_n = cnt;
    return cnt ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* 解析 meting 返回的 JSON 数组 → 歌曲数组 */
esp_err_t os_parse_songs_json(const char *json, online_song_t **out, size_t *out_count)
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
    online_song_t *arr = (online_song_t *)os_psram_alloc((size_t)n * sizeof(online_song_t));
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
        os_str_copy(s->title, sizeof(s->title), jt->valuestring);
        os_str_copy(s->artist, sizeof(s->artist), cJSON_IsString(ja) ? ja->valuestring : "");
        os_str_copy(s->cover_url, sizeof(s->cover_url), cJSON_IsString(jp) ? jp->valuestring : "");
        os_str_copy(s->audio_url, sizeof(s->audio_url), ju->valuestring);
        os_str_copy(s->lrc_url, sizeof(s->lrc_url), cJSON_IsString(jl) ? jl->valuestring : "");
        s->id = os_id_from_url(s->audio_url);
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
