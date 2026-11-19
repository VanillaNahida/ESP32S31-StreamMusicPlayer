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
 *
 * 依赖只有 cJSON（2026-11：meting 的 parse_songs 删了，psram_alloc / os_util 也就
 * 不需要了 —— 旧的 audio_url/lrc_url 合成逻辑一并移除）。
 */
#include "internal/os_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
     * 歌词 / 播放地址都**不在这里填**（2026-11 改）：
     *   - meting 代理已下线，以前合成的 `?type=lrc` 地址会 404；
     *   - 真歌词要 `GET /lyric?id=<id>` 再从 JSON 的 lrc.lyric 里取（见 os_media_cache.cc），
     *     播放直链则按需 `GET /song/url/v1`（见 os_account.cc）。
     * 所以 online_song_t 里的 audio_url / lrc_url 只是**历史字段**（兼容旧歌单文件），
     * 新抓到的歌一律留空，由 id 现场换。
     */
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

/* ⚠ 2026-11：原 os_parse_songs_json()（解析 meting 的 JSON 数组）已随 meting 下线删除。
 * 歌单/搜索现在都用网易云原生对象（ar/al/songs[]），走 os_song_from_json()。 */
