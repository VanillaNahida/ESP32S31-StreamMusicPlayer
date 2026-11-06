/*
 * os_search.cc —— 搜索：服务端 type=search + 本地歌单子串兜底
 *
 * 逐字搬自原 online_service.cc：
 *   search_local    1065–1110（读盘循环换成 os_pl_format 的宽松版，判据不变）
 *   online_search   1116–1160
 */
#include "internal/os_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "esp_log.h"

#include "internal/os_api.h"
#include "internal/os_fs.h"
#include "internal/os_http_client.h"
#include "internal/os_json.h"
#include "internal/os_pl_format.h"
#include "internal/os_state.h"
#include "internal/os_util.h"

static const char *TAG = "online";

/* 本地兜底：在所有已保存歌单里按子串找（不需要网络） */
esp_err_t os_search_local(const char *keyword, online_song_t **out, size_t *count, size_t max,
                          online_song_t *acc)
{
    size_t n = 0;
    for (size_t p = 0; p < os_state_playlist_count() && n < max; p++) {
        const char *pid = os_state_playlist_id(p);
        if (!pid) {
            continue;
        }
        uint32_t hdr_count = 0;
        FILE *f = os_pl_open_for_scan(pid, &hdr_count);
        if (!f) {
            continue;
        }
        for (uint32_t i = 0; i < hdr_count && n < max; i++) {
            online_rec_t rec = {};
            if (!os_pl_read_record(f, &rec)) {
                break;
            }
            if (!rec.title[0]) {
                continue;
            }
            if (strcasestr(rec.title, keyword) || strcasestr(rec.artist, keyword)) {
                online_song_t *s = &acc[n];
                os_pl_rec_to_song(&rec, s);
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
 * 网易云官方搜索：走自建中间件 `GET /search?keywords=<关键词>`（用户 2026-11 指定）。
 *
 * 完整地址就是 `CONFIG_ONLINE_NETEASE_API_BASE` + 这里的 path，即
 *   https://ncmapi.ysnb.com.cn/search?keywords=xxx&type=1&limit=N
 * （`os_net_api_get_ex()` 自己拼基地址并按需追加 `cookie=`）。
 *
 * ★ 为什么它排第一位、meting 退到兜底：meting 的 `type=search` 响应在 TLS 层
 *   **读不出来**（真机稳定 3/3 次 `Incomlete data received, ret=-135, 0/8967 bytes`，
 *   根因见 AGENTS.md §11.4.1），而搜索**没有"换明文 http"那条退路**（meting 只认
 *   https，`http://` 会 301 过去）—— 于是那头 100% 失败 = 搜索整个不能用。
 *   中间件这条通道是**每次取播放直链都在用**的，实测通。
 *
 * ★ 不需要网易云登录：访问鉴权是中间件自己的 `Authorization: Bearer <token>`
 *   （`net_auth_hdr()`，来自 CONFIG_ONLINE_NETEASE_API_TOKEN），与 CK 无关；
 *   没登录也能搜（只是结果不带个性化/会员信息）。
 *
 * 返回形状（**老版 /search 格式**，与 /cloudsearch 的 `al`/`ar` 不同）：
 *   {"result":{"songs":[{"id":…,"name":…,"duration":…,"artists":[…],
 *                        "album":{"id":…,"picId":…,"name":…}}]}}
 *   → `os_song_from_json()` 的 `artists[]` 分支能吃；但 `album` 里**没有 picUrl**
 *     （只有 picId，没法拼封面地址），所以搜出来的歌 `cover_url` 为空 —— 搜索结果
 *     列表本身不显示封面（无影响），要封面就得改用 `/cloudsearch`。
 *   失败/未授权时是 401 或 200 + `{"code":…}` 无 songs，由 os_pick_array 返回 NULL
 *   → os_fill_songs 报 NOT_FOUND → 调用方继续退到 meting。
 */
static esp_err_t search_via_netease(const char *enc_keyword, size_t max, online_song_t *acc,
                                    size_t *out_n)
{
    *out_n = 0;
    const unsigned limit = (max > 50) ? 50U : (unsigned)max;   /* 老接口上限 100，50 条足够 UI */
    char path[320];
    const int n = snprintf(path, sizeof(path), "/search?keywords=%s&type=1&limit=%u",
                           enc_keyword, limit);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *json = NULL;
    const esp_err_t err = os_net_api_get_ex(path, true, &json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "netease search failed: %s", esp_err_to_name(err));
        return err;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGW(TAG, "netease search: bad JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *arr = os_pick_array(root, "songs");
    const esp_err_t ret = os_fill_songs(arr, acc, max, out_n);
    cJSON_Delete(root);
    return ret;
}

/*
 * 搜索歌曲：① 中间件 `/search?keywords=`（首选，不需要登录）→ ② Meting 的
 * type=search（关键词放 id=）→ ③ 扫本地已保存歌单。
 */
esp_err_t online_search(const char *keyword, online_song_t **out, size_t *count, size_t max)
{
    if (!keyword || !out || !count || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *count = 0;

    online_song_t *acc = (online_song_t *)os_psram_alloc(max * sizeof(online_song_t));
    if (!acc) {
        return ESP_ERR_NO_MEM;
    }

    if (online_has_network()) {
        char enc[192];
        os_url_encode(keyword, enc, sizeof(enc));

        /* ① 中间件官方搜索：结果最全，且绕开 meting 那条读不出来的 TLS 响应。
         *    鉴权走 Bearer token（不是 CK），所以**不判断是否登录**。 */
        size_t n = 0;
        if (search_via_netease(enc, max, acc, &n) == ESP_OK && n > 0) {
            *out = acc;
            *count = n;
            ESP_LOGI(TAG, "search '%s': %u hit(s) from netease api", keyword, (unsigned)n);
            return ESP_OK;
        }

        /* ② meting 兜底（未登录 / 中间件不可用） */
        char url[320];
        snprintf(url, sizeof(url), API_SEARCH_BASE "?server=netease&type=search&id=%s", enc);
        char *json = NULL;
        if (http_get_text(url, &json, NULL, JSON_MAX_LEN) == ESP_OK) {
            online_song_t *arr = NULL;
            size_t n = 0;
            const esp_err_t ret = os_parse_songs_json(json, &arr, &n);
            free(json);
            if (ret == ESP_OK && n > 0) {
                if (n > max) {
                    n = max;                 /* 只要前 max 条 */
                }
                *out = arr;
                *count = n;
                ESP_LOGI(TAG, "search '%s': %u hit(s) from meting", keyword, (unsigned)n);
                free(acc);
                return ESP_OK;
            }
            free(arr);
            ESP_LOGW(TAG, "meting search failed, fallback to local");
        }
    }

    const esp_err_t ret = os_search_local(keyword, out, count, max, acc);
    if (ret != ESP_OK) {
        free(acc);
    }
    return ret;
}
