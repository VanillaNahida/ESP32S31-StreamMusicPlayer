/*
 * os_qq_json.cc —— QQ 音乐 JSON → online_song_t（纯逻辑，可 host 单测）
 *
 * 服务端 /qq 把 sidecar 的响应原样放进 data（外壳统一成
 * {"code":200,"msg":"ok","data":…}），所以这里只需要认 sidecar 的模型字段。
 * 字段名来自 QQMusicApi 的 pydantic 模型（model_dump，**snake_case**）：
 *
 *   Song  { id, mid, name, title, subtitle, interval(秒), singer[{name,mid}],
 *           album{ id, mid, name, title, pmid }, pay{…}, file{ size_128mp3, … } }
 *   SongList { id, dirid, title, picurl, desc, songnum, listennum }
 *
 * ★ 封面不在响应里给 URL，要按 QQ 的 photo_new 规则自己拼：
 *     https://y.gtimg.cn/music/photo_new/T002R300x300M000<album.mid>.jpg
 *   （见 QQMusicApi 的 models/base.py 里 _build_photo_new_cover_url）。
 *   300x300 与网易云封面同档，播放器页放得下，也不浪费流量。
 */
#include "internal/os_qq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/os_json.h"

/* 响应壳里的 data 对象；不是对象就返回 NULL */
const cJSON *os_qq_data(const cJSON *root)
{
    if (!root) {
        return NULL;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(data) ? data : NULL;
}

void os_qq_cover_from_album(const cJSON *album, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!cJSON_IsObject(album)) {
        return;
    }
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(album, "mid");
    if (!cJSON_IsString(mid) || !mid->valuestring || !mid->valuestring[0]) {
        return;
    }
    /* "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + mid + ".jpg"
     * 固定部分 55 字节 + NUL，剩下的都留给 mid（超长就截断，不影响可用性）。 */
    const int room = (int)cap - 56;
    if (room <= 0) {
        return;
    }
    snprintf(out, cap, "https://y.gtimg.cn/music/photo_new/T002R300x300M000%.*s.jpg",
             room, mid->valuestring);
}

/* QQ 的 Song 对象 → online_song_t（只填元数据） */
bool os_qq_song_from_json(const cJSON *song, online_song_t *out)
{
    if (!song || !out) {
        return false;
    }
    char idbuf[24] = {0};
    os_json_id_str(cJSON_GetObjectItemCaseSensitive(song, "id"), idbuf, sizeof(idbuf));
    if (!idbuf[0]) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->src = ONLINE_SRC_QQ;
    out->id  = (uint32_t)strtoul(idbuf, NULL, 10);

    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(song, "mid");
    if (cJSON_IsString(mid) && mid->valuestring) {
        snprintf(out->mid, sizeof(out->mid), "%.*s", (int)sizeof(out->mid) - 1,
                 mid->valuestring);
    }
    if (!out->mid[0] || out->id == 0) {
        return false;                 /* 两个都没有：取不了流 */
    }

    /* 歌名：name 优先，缺了退回 title（搜索场景 title 带 <em> 高亮，name 是干净的） */
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(song, "name");
    if (!cJSON_IsString(nm) || !nm->valuestring || !nm->valuestring[0]) {
        nm = cJSON_GetObjectItemCaseSensitive(song, "title");
    }
    if (cJSON_IsString(nm) && nm->valuestring) {
        snprintf(out->title, sizeof(out->title), "%.*s", (int)sizeof(out->title) - 1,
                 nm->valuestring);
    }

    /* 歌手：singer[{name}]，多个用 " / " 连起来（与网易云的显示一致） */
    const cJSON *singers = cJSON_GetObjectItemCaseSensitive(song, "singer");
    if (cJSON_IsArray(singers)) {
        size_t used = 0;
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, singers) {
            const cJSON *sn = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (!cJSON_IsString(sn) || !sn->valuestring) {
                continue;
            }
            const int w = snprintf(out->artist + used, sizeof(out->artist) - used,
                                   "%s%s", used ? " / " : "", sn->valuestring);
            if (w <= 0 || (size_t)w >= sizeof(out->artist) - used) {
                break;
            }
            used += (size_t)w;
        }
    }

    os_qq_cover_from_album(cJSON_GetObjectItemCaseSensitive(song, "album"), out->cover_url,
                           sizeof(out->cover_url));

    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(song, "interval");
    if (cJSON_IsNumber(iv) && iv->valuedouble > 0 && iv->valuedouble < 65535) {
        out->dur_sec = (uint16_t)iv->valuedouble;
    }
    return true;
}

esp_err_t os_qq_fill_songs(const cJSON *arr, online_song_t *out, size_t max, size_t *out_n)
{
    size_t cnt = 0;
    if (out_n) {
        *out_n = 0;
    }
    if (!cJSON_IsArray(arr) || !out || max == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (cnt >= max) {
            break;
        }
        if (os_qq_song_from_json(it, &out[cnt])) {
            cnt++;
        }
    }
    if (out_n) {
        *out_n = cnt;
    }
    return cnt ? ESP_OK : ESP_ERR_NOT_FOUND;
}
