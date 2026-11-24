/*
 * os_like.cc —— 红心收藏（"我喜欢的音乐"）
 *
 * 端点（都在自建 NeteaseCloudMusicApiEnhanced 上，见 os_state.h 的接口一览）：
 *   GET /like?id=<歌曲id>&like=true|false    加 / 取消喜欢（**必须登录**）
 *   GET /likelist?uid=<uid>                  "我喜欢"歌单的全部歌曲 id
 *                                            → {"ids":[186016,…],"checkPoint":…,"code":200}
 *   GET /song/like/check?ids=<id>[,…]        单曲是否已喜欢（本模块没用，留作对照）
 *
 * ★ 线上实测（2026-11，Bearer 新 key）—— 决定了下面的错误判读：
 *   - 不带 cookie：/like → HTTP 500 {"songs":[],"playlistId":…,"code":500}（服务端自己炸）；
 *     /likelist → HTTP 200 {"ids":[],…,"code":200}（游客，**空表不报错**）；
 *     /song/like/check → 200 {"code":400}；
 *   - 带一个假的 MUSIC_U：三个接口统一 **HTTP 301 {"msg":"需要登录","code":301"}**。
 *   ⇒ http_get_hops() 对非 200 **一律丢 body**（只留 ESP_ERR_INVALID_RESPONSE），
 *     所以这里分不出 301 与 500，只能靠"调用前先问 online_netease_logged_in()"把
 *     "没登录"挡在外面；cookie 过期则表现为一次普通失败（上层提示重试）。
 *
 * ★ uid 为什么要落 NVS：/likelist 必填 uid，而它只能从 /user/account 的
 *   profile.userId 拿到。昵称已经这么存了（NVS_KEY_NICK），uid 照做 —— 否则每次开机
 *   都得先打一次 /user/account 才能收藏，而"红心"是点一下就要有反应的操作。
 *
 * ★ 列表按 **uint64_t** 存：网易云的 id 已经出现 >2^32 的（10 位数），cJSON 的
 *   valueint 是 32 位会溢出，所以统一走 os_json_id_str() 转字符串再 strtoull()。
 *   排序 + 去重之后二分，1 万首收藏也只占 80 KB（PSRAM）。
 */
#include "internal/os_api.h"
#include "internal/os_json.h"
#include "internal/os_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "online";

/* ------------------------------------------------------------------ uid */

static char s_uid[24];
static bool s_uid_loaded;

const char *online_netease_uid(void)
{
    if (!s_uid_loaded) {
        s_uid_loaded = true;
        s_uid[0] = '\0';
        nvs_handle_t h;
        size_t len = sizeof(s_uid);
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, NVS_KEY_UID, s_uid, &len) != ESP_OK) {
                s_uid[0] = '\0';
            }
            nvs_close(h);
        }
    }
    return s_uid;
}

esp_err_t online_netease_uid_set(const char *uid)
{
    char v[24] = {0};
    if (uid && uid[0]) {
        snprintf(v, sizeof(v), "%s", uid);
    }
    if (s_uid_loaded && strcmp(s_uid, v) == 0) {
        return ESP_OK;                      /* 没变：别每次进账号页都写一次 NVS */
    }
    snprintf(s_uid, sizeof(s_uid), "%s", v);
    s_uid_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (s_uid[0]) {
        err = nvs_set_str(h, NVS_KEY_UID, s_uid);
    } else {
        err = nvs_erase_key(h, NVS_KEY_UID);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "netease uid %s: %s", s_uid[0] ? "saved" : "cleared",
             s_uid[0] ? s_uid : "-");
    return err;
}

/* ------------------------------------------------------------------ 红心 */

esp_err_t online_netease_like(const char *song_id, bool like)
{
    if (!song_id || !song_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[64];
    snprintf(path, sizeof(path), "/like?id=%s&like=%s", song_id, like ? "true" : "false");

    char *js = NULL;
    esp_err_t err = os_net_api_get_ex(path, true, &js);
    if (err != ESP_OK) {
        /* 301（cookie 过期）也会落到这里，见文件头 */
        ESP_LOGW(TAG, "like %s=%s failed (%s), logged_in=%d", song_id, like ? "true" : "false",
                 esp_err_to_name(err), (int)online_netease_logged_in());
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    int code = 0;
    os_json_find_int(root, "code", &code);     /* 最外层优先，见 os_json_find_int */
    cJSON_Delete(root);
    if (code == 200) {
        ESP_LOGI(TAG, "like %s -> %s ok", song_id, like ? "liked" : "unliked");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "like %s=%s: code=%d%s", song_id, like ? "true" : "false", code,
             (code == 301) ? " (需要登录)" : "");
    return (code == 301) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

static int like_id_cmp(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 读 /likelist 拿全量 id（升序去重）。*out_ids 由调用方 free()。
 *
 * uid 缺失时先打一次 /user/account 把 uid 补上（只发生在开机第一次查询，
 * 之后都在 NVS 里）。未登录 / cookie 过期 → ESP_ERR_INVALID_STATE。 */
esp_err_t online_netease_likelist(uint64_t **out_ids, size_t *out_n)
{
    if (!out_ids || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ids = NULL;
    *out_n = 0;

    const char *uid = online_netease_uid();
    if (!uid[0]) {
        online_account_t acct;
        if (online_netease_account(&acct) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;      /* 未登录 */
        }
        uid = online_netease_uid();
        if (!uid[0]) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "/likelist?uid=%s", uid);

    char *js = NULL;
    esp_err_t err = os_net_api_get_ex(path, true, &js);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "likelist failed (%s)", esp_err_to_name(err));
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "ids");
    if (!cJSON_IsArray(ids)) {
        int code = 0;
        os_json_find_int(root, "code", &code);
        cJSON_Delete(root);
        ESP_LOGW(TAG, "likelist: 没有 ids 数组 (code=%d)", code);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t n = (size_t)cJSON_GetArraySize(ids);
    /* PSRAM 优先：1 万首收藏是 80 KB，别去挤内部 RAM */
    uint64_t *arr = NULL;
    if (n) {
        arr = (uint64_t *)heap_caps_malloc(n * sizeof(uint64_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!arr) {
            arr = (uint64_t *)malloc(n * sizeof(uint64_t));
        }
        if (!arr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    size_t k = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, ids) {
        char buf[24];
        os_json_id_str(it, buf, sizeof(buf));
        if (!buf[0]) {
            continue;
        }
        arr[k++] = strtoull(buf, NULL, 10);
    }
    cJSON_Delete(root);

    /* 升序 + 去重（二分的前提） */
    if (k > 1) {
        qsort(arr, k, sizeof(uint64_t), like_id_cmp);
        size_t w = 1;
        for (size_t r = 1; r < k; r++) {
            if (arr[r] != arr[w - 1]) {
                arr[w++] = arr[r];
            }
        }
        k = w;
    }
    *out_ids = arr;
    *out_n = k;
    ESP_LOGI(TAG, "likelist: %u 首收藏", (unsigned)k);
    return ESP_OK;
}

bool online_netease_liked_has(const uint64_t *ids, size_t n, const char *song_id)
{
    if (!ids || !n || !song_id || !song_id[0]) {
        return false;
    }
    const uint64_t v = strtoull(song_id, NULL, 10);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (ids[mid] == v) {
            return true;
        }
        if (ids[mid] < v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- 最近收藏的那首
 *
 * 「我喜欢的音乐」歌单卡片的封面要显示**最新收藏**的那首歌（2026-11 用户），
 * 而歌单列表第一首通常是「最早收藏」的那一首，所以收藏成功时在这里记一份。
 * 记录落 NVS（key 'lkcov'），重启后仍能用；值是一个短字符串：
 *     <src>,<id>,<cover_url>
 * 用 %n 把前两段的长度量出来，剩下的原样当 URL（URL 里可能有逗号，不能按分隔符切）。
 */
#define NVS_KEY_LIKECOVER  "lkcov"
#define LIKECOVER_BUF      (ONLINE_URL_MAX + 32)

static online_song_t s_last_liked;
static bool          s_last_liked_valid;
static bool          s_last_liked_loaded;

static void last_liked_load(void)
{
    if (s_last_liked_loaded) {
        return;
    }
    s_last_liked_loaded = true;

    char buf[LIKECOVER_BUF];
    buf[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(buf);
    const esp_err_t err = nvs_get_str(h, NVS_KEY_LIKECOVER, buf, &len);
    nvs_close(h);
    if (err != ESP_OK || !buf[0]) {
        return;
    }

    unsigned src = 0;
    unsigned id  = 0;
    int off = 0;
    if (sscanf(buf, "%u,%u,%n", &src, &id, &off) != 2 || off <= 0 || buf[off] == 0) {
        return;
    }
    memset(&s_last_liked, 0, sizeof(s_last_liked));
    s_last_liked.id  = id;
    s_last_liked.src = (src == (unsigned)ONLINE_SRC_QQ) ? ONLINE_SRC_QQ : ONLINE_SRC_NETEASE;
    snprintf(s_last_liked.cover_url, sizeof(s_last_liked.cover_url), "%s", buf + off);
    s_last_liked_valid = true;
}

void online_last_liked_song_set(const online_song_t *song)
{
    if (!song || song->id == 0 || !song->cover_url[0]) {
        return;
    }
    s_last_liked = *song;          /* 只留 id/标题/歌手/封面/src，够当封面用了 */
    s_last_liked_valid  = true;
    s_last_liked_loaded = true;

    char buf[LIKECOVER_BUF];
    snprintf(buf, sizeof(buf), "%u,%u,%s", (unsigned)song->src, (unsigned)song->id,
             song->cover_url);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_str(h, NVS_KEY_LIKECOVER, buf) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "last liked song: src=%u id=%u", (unsigned)song->src, (unsigned)song->id);
}

bool online_last_liked_song_get(online_song_t *out)
{
    last_liked_load();
    if (!s_last_liked_valid) {
        return false;
    }
    if (out) {
        *out = s_last_liked;
    }
    return true;
}
