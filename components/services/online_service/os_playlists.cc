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

#include "internal/os_fs.h"        /* online_has_network() / os_psram_alloc() */
#include "internal/os_pl_format.h"
#include "internal/os_qq.h"
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

/*
 * 一次抓取最多落多少首。
 *
 * meting 时代是"服务端一次给整份 JSON 数组"，条数不受控；现在改成分页
 * `/playlist/track/all`（100 首/页，见 online_netease_playlist_page），
 * 内存与磁盘都得先划个上限：
 *   - 内存：PSRAM 里先按上限预留（online_song_t ≈ 1.2 KB/首），1000 首 ≈ 1.2 MB；
 *   - 磁盘：定长记录 1252 B/首，1000 首 ≈ 1.25 MB —— 歌单文件本来就在 MB 级。
 * 超过就截断（打日志），别让一个几千首的"每日推荐"把 PSRAM/存储卡打满。
 */
#define ONLINE_PL_FETCH_MAX 1000

esp_err_t online_playlist_add(const char *id)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★ 2026-11：meting 代理已下线（`?server=netease&type=playlist` 现在 404），
     *   歌单改走自建 API 的原生分页接口 —— 与"账号歌单懒加载"复用**同一个**函数
     *   （/playlist/track/all?id=&limit=100&offset=…，首屏失败自动退 /playlist/detail）。
     */
    online_song_t *songs =
        (online_song_t *)os_psram_alloc(ONLINE_PL_FETCH_MAX * sizeof(online_song_t));
    if (!songs) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    esp_err_t ret = online_netease_playlist_page(id, 0, songs, ONLINE_PL_FETCH_MAX, &count);
    if (ret != ESP_OK) {
        free(songs);
        os_state_set_last_error("歌单抓取失败（空歌单 / 接口变了 / 歌单不存在？）");
        return ret;
    }
    if (count >= ONLINE_PL_FETCH_MAX) {
        ESP_LOGW(TAG, "playlist %s: 只取前 %u 首（分页上限）", id, (unsigned)count);
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

/*
 * 添加 QQ 音乐的歌单（与上面同一条"抓取 → 二进制落盘 → 记 NVS"路径）。
 *
 * 为什么单独一份而不是给 online_playlist_add() 加个 provider 参数：
 * 服务层不知道界面当前在看哪家（同一个歌单 ID 是纯数字，两家都可能存在），
 * 由调用方（app_online 的工作线程）按音源显式选择更清楚，也不会误抓。
 */
esp_err_t online_playlist_add_qq(const char *id)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * ★ 网易云与 QQ 的歌单 ID 都是纯数字，**同一个数字可能是两个平台的歌单**。
     *   所以 QQ 的在 NVS / SD 上统一加前缀 'q'（"q123" → pl_q123.bin），
     *   两边各自增删改查互不影响。调用方传进来的是原始数字（首次添加）或
     *   已带前缀的键（长按"更新"），这里统一剥掉再拼回去。
     */
    const char *raw = (id[0] == 'q' && id[1] >= '0' && id[1] <= '9') ? (id + 1) : id;
    char key[ONLINE_ID_MAX];
    snprintf(key, sizeof(key), "q%.*s", ONLINE_ID_MAX - 2, raw);

    online_song_t *songs =
        (online_song_t *)os_psram_alloc(ONLINE_PL_FETCH_MAX * sizeof(online_song_t));
    if (!songs) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    esp_err_t ret = online_qq_playlist_page(raw, 0, songs, ONLINE_PL_FETCH_MAX, &count);
    if (ret != ESP_OK) {
        free(songs);
        os_state_set_last_error("QQ 歌单抓取失败（空歌单 / 歌单不存在？）");
        return ret;
    }
    if (count >= ONLINE_PL_FETCH_MAX) {
        ESP_LOGW(TAG, "QQ playlist %s: 只取前 %u 首（上限）", raw, (unsigned)count);
    }
    ret = os_pl_save(key, songs, count);
    free(songs);
    if (ret != ESP_OK) {
        return ret;
    }
    if (os_state_playlist_index(key) < 0) {
        if (os_state_playlist_count() >= ONLINE_MAX_PLAYLISTS) {
            return ESP_ERR_NO_MEM;
        }
        os_state_playlist_append(key);
        os_playlists_nvs_save();
    }
    ESP_LOGI(TAG, "QQ playlist %s (key %s): %u songs", raw, key, (unsigned)count);
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
        /* 本地没有：抓一次再读（顺带落盘）。
         * "q<数字>" 是 QQ 的键（见 online_playlist_add_qq），别拿它去问网易云。 */
        const bool is_qq_key = (id[0] == 'q' && id[1] >= '0' && id[1] <= '9');
        ret = is_qq_key ? online_playlist_add_qq(id) : online_playlist_add(id);
        if (ret == ESP_OK) {
            ret = os_pl_load(id, songs, count);
        }
    }
    return ret;
}
