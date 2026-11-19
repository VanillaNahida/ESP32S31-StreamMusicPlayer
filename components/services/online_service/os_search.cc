/*
 * os_search.cc —— 搜索：自建 API 的原生端点 + 本地歌单子串兜底
 *
 * 原文件里的名字：search_local（1065–1110）、online_search（1116–1160）。
 * 关键词的百分号编码（url_encode 1046–1062）搬到了纯逻辑的 os_util.h：os_url_encode()。
 *
 * ★ 2026-11 大改（服务端 API 换代，见 os_state.h 的"在线接口一览"）：
 *   - meting 代理整条被删（/meting/api* 一律 404），所以**不再有 meting 兜底**；
 *   - 首选改成 `/cloudsearch`：返回**新格式**（`ar[]` / `al.picUrl` / `dt`），
 *     搜索结果里天然带封面（老 `/search` 的 album 只有 picId，拼不出封面地址）；
 *   - 次选 `/search`（老格式，artists[]/album）：cloudsearch 被限流或返回空时兜底；
 *   - 最后仍是"扫本地已保存歌单"（没网也能搜）。
 *
 * ⚠ 本地兜底的扫盘循环原来自己抄了一份 pl_load 的读盘逻辑（只校验 magic），
 *   已收敛到 os_pl_format.h 的 os_pl_open_for_scan()/os_pl_read_record()
 *   —— 判据保持"只认 magic"不变（蓝图 R8 的行为面）。
 */
#include "internal/os_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "esp_log.h"

#include "internal/os_api.h"
#include "internal/os_fs.h"
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
 * 打一次搜索接口，把 songs[] 填进 acc。
 *
 * 鉴权是中间件自己的 `Authorization: Bearer <API Key>`（net_auth_hdr()，
 * 值来自 CONFIG_ONLINE_NETEASE_API_TOKEN），**与网易云 CK 无关** ——
 * 所以未登录也能搜（只是结果不带个性化）；登录了就把 CK 一起捎上（X-Netease-Cookie），
 * 服务端会按账号返回更准的结果。
 */
static esp_err_t search_one(const char *path, size_t max, online_song_t *acc, size_t *out_n)
{
    *out_n = 0;
    char *json = NULL;
    const esp_err_t err = os_net_api_get_ex(path, true, &json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "search GET %s failed: %s", path, esp_err_to_name(err));
        return err;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGW(TAG, "search: bad JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* /cloudsearch 与 /search 都是 {"result":{"songs":[…]}}，os_pick_array 都认 */
    const cJSON *arr = os_pick_array(root, "songs");
    const esp_err_t ret = os_fill_songs(arr, acc, max, out_n);
    cJSON_Delete(root);
    return ret;
}

/*
 * 搜索歌曲：① `/cloudsearch`（新格式，带封面）→ ② `/search`（老格式兜底）
 *          → ③ 扫本地已保存歌单。
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
        const unsigned limit = (max > 50) ? 50U : (unsigned)max;   /* 50 条足够 UI 翻 */

        /* ① /cloudsearch：新格式（ar[] / al.picUrl / dt），搜索结果自带封面 */
        char path[320];
        int n = snprintf(path, sizeof(path), "/cloudsearch?keywords=%s&type=1&limit=%u",
                         enc, limit);
        if (n > 0 && n < (int)sizeof(path)) {
            size_t got = 0;
            if (search_one(path, max, acc, &got) == ESP_OK && got > 0) {
                *out = acc;
                *count = got;
                ESP_LOGI(TAG, "search '%s': %u hit(s) from cloudsearch", keyword,
                         (unsigned)got);
                return ESP_OK;
            }
        }

        /* ② /search：老格式（artists[]，album 里没有 picUrl → 封面要靠 /song/detail 补） */
        n = snprintf(path, sizeof(path), "/search?keywords=%s&type=1&limit=%u", enc, limit);
        if (n > 0 && n < (int)sizeof(path)) {
            size_t got = 0;
            if (search_one(path, max, acc, &got) == ESP_OK && got > 0) {
                *out = acc;
                *count = got;
                ESP_LOGI(TAG, "search '%s': %u hit(s) from search", keyword, (unsigned)got);
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "server search '%s' returned nothing, fallback to local", keyword);
    }

    const esp_err_t ret = os_search_local(keyword, out, count, max, acc);
    if (ret != ESP_OK) {
        free(acc);
    }
    return ret;
}
