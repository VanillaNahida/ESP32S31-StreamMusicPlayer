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

    online_song_t *acc = (online_song_t *)os_psram_alloc(max * sizeof(online_song_t));
    if (!acc) {
        return ESP_ERR_NO_MEM;
    }

    if (online_has_network()) {
        char enc[192];
        os_url_encode(keyword, enc, sizeof(enc));
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
                ESP_LOGI(TAG, "search '%s': %u hit(s) from server", keyword, (unsigned)n);
                free(acc);
                return ESP_OK;
            }
            free(arr);
            ESP_LOGW(TAG, "server search failed, fallback to local");
        }
    }

    const esp_err_t ret = os_search_local(keyword, out, count, max, acc);
    if (ret != ESP_OK) {
        free(acc);
    }
    return ret;
}
