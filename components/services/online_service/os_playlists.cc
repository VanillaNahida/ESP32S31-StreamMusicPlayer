/*
 * os_playlists.cc —— 歌单：NVS 列表 / 抓取 / 二进制落盘 / 对外三个 API
 *
 * 逐字搬自原 online_service.cc：
 *   nvs_load_ids          732–760
 *   nvs_save_ids          762–787
 *   id_index              789–797
 *   online_playlist_add   960–1003
 *   online_playlist_remove 1005–1024
 *   online_playlist_load  1026–1040
 * 只有两处结构性替换（行为不变）：
 *   - id_index → os_state_playlist_index()（同一个遍历，只是变量搬到了 os_state.cc）；
 *   - nvs_load_ids 的顺序追加 → os_state_playlist_load_one()
 *     （原来靠"直接写 s_ids[s_id_count]"实现，等价）。
 */
#include "internal/os_playlists.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "internal/os_fs.h"        /* online_has_network() */
#include "internal/os_http_client.h"
#include "internal/os_json.h"
#include "internal/os_pl_format.h"
#include "internal/os_state.h"
#include "internal/os_util.h"

static const char *TAG = "online";

void os_playlists_nvs_load(void)
{
    nvs_handle_t h;
    os_state_playlist_reset();
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
            char id[ONLINE_ID_MAX] = {0};
            char name[ONLINE_NAME_MAX] = {0};
            snprintf(key, sizeof(key), NVS_KEY_ID_FMT, i);
            size_t len = sizeof(id);
            if (nvs_get_str(h, key, id, &len) == ESP_OK && id[0]) {
                /* 名字是可选的：没有就是空串（UI 退回显示 ID） */
                size_t nlen = sizeof(name);
                snprintf(key, sizeof(key), NVS_KEY_NAME_FMT, i);
                if (nvs_get_str(h, key, name, &nlen) != ESP_OK) {
                    name[0] = '\0';
                }
                os_state_playlist_load_one(id, name);
            }
        }
    }
    nvs_close(h);
}

void os_playlists_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_CNT, (uint8_t)os_state_playlist_count());
    for (size_t i = 0; i < ONLINE_MAX_PLAYLISTS; i++) {
        char key[12];
        const char *id = os_state_playlist_id(i);
        const char *nm = os_state_playlist_name(i);
        snprintf(key, sizeof(key), NVS_KEY_ID_FMT, (int)i);
        if (id) {
            nvs_set_str(h, key, id);
        } else {
            nvs_erase_key(h, key);
        }
        /* 名字（空串就直接删键，别留垃圾） */
        snprintf(key, sizeof(key), NVS_KEY_NAME_FMT, (int)i);
        if (nm && nm[0]) {
            nvs_set_str(h, key, nm);
        } else {
            nvs_erase_key(h, key);
        }
    }
    nvs_commit(h);
    nvs_close(h);
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
    ret = os_parse_songs_json(json, &songs, &count);
    free(json);
    if (ret != ESP_OK) {
        os_state_set_last_error("歌单解析失败（空歌单或格式变了？）");
        return ret;
    }

    ret = os_pl_save(id, songs, count);
    free(songs);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 记进 NVS（已存在就只算刷新） */
    if (os_state_playlist_index(id) < 0) {
        if (os_state_playlist_count() >= ONLINE_MAX_PLAYLISTS) {
            return ESP_ERR_NO_MEM;
        }
        os_state_playlist_append(id);
        os_playlists_nvs_save();
    }
    ESP_LOGI(TAG, "playlist %s: %u songs", id, (unsigned)count);
    return ESP_OK;
}

esp_err_t online_playlist_remove(const char *id)
{
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }
    const int idx = os_state_playlist_index(id);
    if (idx >= 0) {
        os_state_playlist_remove_at((size_t)idx);
        os_playlists_nvs_save();
    }
    char path[128];
    os_pl_path(id, path, sizeof(path));
    unlink(path);
    return ESP_OK;
}

esp_err_t online_playlist_load(const char *id, online_song_t **songs, size_t *count)
{
    if (!id || !songs || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = os_pl_load(id, songs, count);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* 本地没有：抓一次再读（顺带落盘） */
        ret = online_playlist_add(id);
        if (ret == ESP_OK) {
            ret = os_pl_load(id, songs, count);
        }
    }
    return ret;
}
