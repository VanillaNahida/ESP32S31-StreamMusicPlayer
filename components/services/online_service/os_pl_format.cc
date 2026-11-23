/*
 * os_pl_format.cc —— 歌单二进制文件：路径 / 写盘 / 读回 / 记录编解码
 *
 * 逐字搬自原 online_service.cc（pl_path 801–808、pl_save 811–848、pl_load 851–899），
 * 外加把 search_local（1076–1101）里那份**重复的**读盘循环收敛成 os_pl_open_for_scan()
 * + os_pl_read_record()（老代码只校验 magic，这里照旧，见头文件说明）。
 *
 * 读写行为（拆分蓝图的实测记录，别"顺手优化"）：
 *   - pl_save **非原子**（无 .tmp + rename）；
 *   - pl_load 不校验文件长度，fread 失败就 break（截断文件不报错），
 *     标题为空串的记录被跳过并压紧，got == 0 才返回 ESP_ERR_INVALID_RESPONSE。
 */
#include "internal/os_pl_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"

#include "internal/os_fs.h"
#include "internal/os_state.h"
#include "internal/os_util.h"

static const char *TAG = "online";

void os_pl_path(const char *id, char *out, size_t cap)
{
    char safe[ONLINE_ID_MAX];
    os_str_copy(safe, sizeof(safe), id);
    /* 用 %.*s 明确给出长度上限：这样编译器能证明不会截断
     * （直接传任意长度字符串会触发 -Werror=format-truncation） */
    snprintf(out, cap, ONLINE_ROOT "/pl_%.*s.bin", (int)(sizeof(safe) - 1), safe);
}

/* 把内存里的歌曲数组写成二进制文件 */
esp_err_t os_pl_save(const char *id, const online_song_t *songs, size_t count)
{
    char path[128];
    os_pl_path(id, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "create %s failed", path);
        return ESP_FAIL;
    }
    online_pl_hdr_t hdr = {};
    hdr.magic   = ONLINE_PL_MAGIC;
    hdr.version = ONLINE_PL_VERSION;
    hdr.count   = (uint32_t)count;
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < count; i++) {
        online_rec_t rec = {};
        rec.id = songs[i].id;
        os_str_copy(rec.title, sizeof(rec.title), songs[i].title);
        os_str_copy(rec.artist, sizeof(rec.artist), songs[i].artist);
        os_str_copy(rec.cover_url, sizeof(rec.cover_url), songs[i].cover_url);
        os_str_copy(rec.audio_url, sizeof(rec.audio_url), songs[i].audio_url);
        os_str_copy(rec.lrc_url, sizeof(rec.lrc_url), songs[i].lrc_url);
        /*
         * 音源信息塞进 rsv 扩展区（**不动磁盘格式、不升 version**）：
         *   rsv[0]    = src（0 网易云 / 1 QQ）
         *   rsv[1..2] = dur_sec（小端 uint16，秒）
         *   rsv[4..]  = mid（QQ 的歌曲 mid，NUL 结尾）
         * 旧文件里这一段全是 0 → src=NETEASE、无 mid，行为与以前完全一致。
         */
        rec.rsv[0] = songs[i].src;
        rec.rsv[1] = (uint8_t)(songs[i].dur_sec & 0xFF);
        rec.rsv[2] = (uint8_t)(songs[i].dur_sec >> 8);
        os_str_copy((char *)&rec.rsv[4], sizeof(rec.rsv) - 4, songs[i].mid);
        if (fwrite(&rec, sizeof(rec), 1, f) != 1) {
            fclose(f);
            unlink(path);
            return ESP_FAIL;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "playlist %s saved: %u songs, %u bytes",
             path, (unsigned)count, (unsigned)(sizeof(hdr) + count * sizeof(online_rec_t)));
    return ESP_OK;
}

void os_pl_rec_to_song(const online_rec_t *rec, online_song_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = rec->id;
    os_str_copy(out->title, sizeof(out->title), rec->title);
    os_str_copy(out->artist, sizeof(out->artist), rec->artist);
    os_str_copy(out->cover_url, sizeof(out->cover_url), rec->cover_url);
    os_str_copy(out->audio_url, sizeof(out->audio_url), rec->audio_url);
    os_str_copy(out->lrc_url, sizeof(out->lrc_url), rec->lrc_url);
    /* rsv 扩展区（见 os_pl_save 里的说明）：旧记录的 src=0 即网易云 */
    out->src     = (rec->rsv[0] == (uint8_t)ONLINE_SRC_QQ) ? ONLINE_SRC_QQ : ONLINE_SRC_NETEASE;
    out->dur_sec = (uint16_t)(rec->rsv[1] | ((uint16_t)rec->rsv[2] << 8));
    os_str_copy(out->mid, sizeof(out->mid), (const char *)&rec->rsv[4]);
}

bool os_pl_read_record(FILE *f, online_rec_t *rec)
{
    online_rec_t tmp = {};
    if (fread(&tmp, sizeof(tmp), 1, f) != 1) {
        return false;
    }
    *rec = tmp;
    return true;
}

/* 读头 + 校验（严格版：magic/version/count 都要对）。返回 false 表示不能用。 */
static bool pl_read_header(FILE *f, const char *path, online_pl_hdr_t *hdr)
{
    if (fread(hdr, sizeof(*hdr), 1, f) != 1 || hdr->magic != ONLINE_PL_MAGIC ||
        hdr->version != ONLINE_PL_VERSION || hdr->count == 0 || hdr->count > 5000) {
        ESP_LOGW(TAG, "%s: bad header", path);
        return false;
    }
    return true;
}

FILE *os_pl_open(const char *id)
{
    char path[128];
    os_pl_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    online_pl_hdr_t hdr = {};
    if (!pl_read_header(f, path, &hdr)) {
        fclose(f);
        return NULL;
    }
    return f;
}

FILE *os_pl_open_for_scan(const char *id, uint32_t *out_count)
{
    char path[128];
    os_pl_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    online_pl_hdr_t hdr = {};
    /* 宽松版：只认 magic（与老 search_local 的判据一致，别在这里加严） */
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != ONLINE_PL_MAGIC) {
        fclose(f);
        return NULL;
    }
    if (out_count) {
        *out_count = hdr.count;
    }
    return f;
}

/* 读二进制文件到内存数组 */
esp_err_t os_pl_load(const char *id, online_song_t **out, size_t *out_count)
{
    char path[128];
    os_pl_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    online_pl_hdr_t hdr = {};
    if (!pl_read_header(f, path, &hdr)) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    online_song_t *arr = (online_song_t *)os_psram_alloc((size_t)hdr.count * sizeof(online_song_t));
    if (!arr) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        online_rec_t rec = {};
        if (!os_pl_read_record(f, &rec)) {
            break;
        }
        online_song_t *s = &arr[got];
        os_pl_rec_to_song(&rec, s);
        if (s->title[0]) {
            got++;
        }
    }
    fclose(f);
    if (got == 0) {
        free(arr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = arr;
    *out_count = got;
    return ESP_OK;
}
