/*
 * os_account.cc —— 账号 API base + 账号信息 / 歌单分页 / 每日推荐 / 直链
 *
 * 逐字搬自原 online_service.cc：
 *   online_netease_api_base       2124–2141
 *   online_netease_api_base_set   2143–2165
 *   online_netease_account        2905–2944
 *   online_netease_account_playlists 2946–3035
 *   playlist_detail_fallback      3041–3063
 *   online_netease_playlist_page  3065–3130
 *   online_netease_daily_songs    3132–3166
 *   online_netease_song_url       3168–3227
 * 只加 os_ 前缀 / 换成访问器：
 *   json_id_str → os_json_id_str、pick_array → os_pick_array、
 *   song_from_json → os_song_from_json、fill_songs → os_fill_songs、
 *   net_api_get_ex → os_net_api_get_ex、s_last_err 读取 → os_state_last_error()。
 * URL、查询串、"每日推荐 cookie 体检"那一行日志文案全部原样。
 */
#include "internal/os_account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "internal/os_api.h"
#include "internal/os_fs.h"
#include "internal/os_json.h"
#include "internal/os_login.h"     /* online_netease_cookie / _nickname_set */
#include "internal/os_state.h"

static const char *TAG = "online";

static char s_net_base[NET_BASE_MAX];

const char *online_netease_api_base(void)
{
    if (!s_net_base[0]) {
        nvs_handle_t h;
        size_t len = sizeof(s_net_base);
        if (nvs_open(NET_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, NET_NVS_KEY_BASE, s_net_base, &len) != ESP_OK) {
                s_net_base[0] = '\0';
            }
            nvs_close(h);
        }
        if (!s_net_base[0]) {
            snprintf(s_net_base, sizeof(s_net_base), "%s", NET_BASE_DEFAULT);
        }
        ESP_LOGI(TAG, "netease api base: %s", s_net_base);
    }
    return s_net_base;
}

esp_err_t online_netease_api_base_set(const char *base)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (base && base[0]) {
        err = nvs_set_str(h, NET_NVS_KEY_BASE, base);
        if (err == ESP_OK) {
            snprintf(s_net_base, sizeof(s_net_base), "%s", base);
        }
    } else {
        err = nvs_erase_key(h, NET_NVS_KEY_BASE);
        s_net_base[0] = '\0';          /* 下次 get 时回默认值 */
    }
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "netease api base -> %s", (base && base[0]) ? base : "(default)");
    return err;
}

esp_err_t online_netease_account(online_account_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char *js = NULL;
    esp_err_t err = os_net_api_get_ex("/user/account", true, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (cJSON_IsObject(profile)) {
        os_json_id_str(cJSON_GetObjectItemCaseSensitive(profile, "userId"), out->uid,
                       sizeof(out->uid));
        const cJSON *nick = cJSON_GetObjectItemCaseSensitive(profile, "nickname");
        if (cJSON_IsString(nick)) {
            snprintf(out->nickname, sizeof(out->nickname), "%.*s",
                     (int)sizeof(out->nickname) - 1, nick->valuestring);
        }
        if (out->uid[0]) {
            ret = ESP_OK;
        }
    }
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "account: 未登录或 cookie 已失效");
    } else {
        /* 昵称落 NVS：重启后 UI 立刻能显示，不用等这次网络往返 */
        online_netease_nickname_set(out->nickname);
        ESP_LOGI(TAG, "account: %s (%s)", out->nickname, out->uid);
    }
    return ret;
}

esp_err_t online_netease_account_playlists(const char *uid, online_acct_playlist_t *out,
                                           size_t max, size_t *out_n)
{
    if (!uid || !uid[0] || !out || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    /*
     * ★ 必须**分页**（2026-10 用户报"账号歌单列不全"）：
     *   服务端 module/user_playlist.js 是
     *     { uid, limit: query.limit || 30, offset: query.offset || 0, includeVideo: true }
     *   并且响应里带 `more` 标志。只取第一页 = 只拿到前 N 个歌单。
     *   这里按 limit=100 翻页，直到 more=false / 空页 / 塞满 max（另设 10 页上限防死循环）。
     */
    size_t cnt = 0;
    bool truncated = false;
    int offset = 0;
    const int limit = 100;
    for (int page = 0; page < 10; page++) {
        char path[160];
        int n = snprintf(path, sizeof(path), "/user/playlist?uid=%s&limit=%d&offset=%d",
                         uid, limit, offset);
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        char *js = NULL;
        esp_err_t err = os_net_api_get_ex(path, true, &js);
        if (err != ESP_OK) {
            return err;
        }
        cJSON *root = cJSON_Parse(js);
        free(js);
        if (!root) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "playlist");
        size_t got = 0;
        if (cJSON_IsArray(arr)) {
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, arr) {
                got++;
                if (cnt >= max) {
                    truncated = true;   /* 到上限了：日志里说清楚，不静默丢 */
                    break;
                }
                online_acct_playlist_t *d = &out[cnt];
                memset(d, 0, sizeof(*d));
                os_json_id_str(cJSON_GetObjectItemCaseSensitive(it, "id"), d->id, sizeof(d->id));
                if (!d->id[0]) {
                    continue;
                }
                const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
                if (cJSON_IsString(nm)) {
                    snprintf(d->name, sizeof(d->name), "%.*s", (int)sizeof(d->name) - 1,
                             nm->valuestring);
                }
                const cJSON *pic = cJSON_GetObjectItemCaseSensitive(it, "coverImgUrl");
                if (cJSON_IsString(pic)) {
                    snprintf(d->cover_url, sizeof(d->cover_url), "%.*s",
                             (int)sizeof(d->cover_url) - 1, pic->valuestring);
                }
                const cJSON *tc = cJSON_GetObjectItemCaseSensitive(it, "trackCount");
                if (cJSON_IsNumber(tc)) {
                    d->track_count = (int)tc->valuedouble;
                }
                const cJSON *cr = cJSON_GetObjectItemCaseSensitive(it, "creator");
                if (cJSON_IsObject(cr)) {
                    char cid[24];
                    os_json_id_str(cJSON_GetObjectItemCaseSensitive(cr, "userId"), cid, sizeof(cid));
                    d->self_created = (cid[0] && strcmp(cid, uid) == 0);
                }
                cnt++;
            }
        }
        /* more 是布尔字面量：用 cJSON 的 type 判断，避开不同版本的 API 差异 */
        const cJSON *more = cJSON_GetObjectItemCaseSensitive(root, "more");
        const bool has_more = (more && more->type == cJSON_True);
        cJSON_Delete(root);

        offset += (int)got;
        if (!has_more || got == 0 || truncated) {
            break;
        }
    }
    *out_n = cnt;
    ESP_LOGI(TAG, "account playlists: %u (uid=%s%s)", (unsigned)cnt, uid,
             truncated ? ", 已达显示上限" : "");
    return cnt ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/*
 * /playlist/detail 兜底：一次返回整个歌单对象（含 tracks）。大歌单同样会超
 * NET_JSON_MAX，所以只在"track/all 第一页就失败"时试一下 —— 能救小歌单/特殊版权单。
 */
static esp_err_t playlist_detail_fallback(const char *pl_id, online_song_t *out,
                                          size_t max, size_t *out_n)
{
    char path[128];
    const int n = snprintf(path, sizeof(path), "/playlist/detail?id=%s", pl_id);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = os_net_api_get_ex(path, true, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = os_fill_songs(os_pick_array(root, "tracks"), out, max, out_n);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "playlist %s (detail fallback): %u songs", pl_id, (unsigned)*out_n);
    return err;
}

esp_err_t online_netease_playlist_page(const char *pl_id, size_t offset,
                                       online_song_t *out, size_t max, size_t *out_n)
{
    if (!pl_id || !pl_id[0] || !out || !out_n || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★★ **一次只取一页**（见头文件里的体积账）：/playlist/track/all 返回的是完整
     *   song detail 对象（≈2.3 KB/首），300 首 ≈700 KB 会超 NET_JSON_MAX(512 KB)
     *   而整单失败。所以这里按 100 首一页循环，单页响应 ≈250 KB。
     *   max 由调用方给（app_online 一次最多要一两页），到 max 或歌单结束就停。
     */
    size_t got_total = 0;
    while (got_total < max) {
        const size_t want = ((max - got_total) > ONLINE_PL_PAGE_SONGS)
                                ? (size_t)ONLINE_PL_PAGE_SONGS : (max - got_total);
        char path[160];
        const int n = snprintf(path, sizeof(path),
                               "/playlist/track/all?id=%s&limit=%u&offset=%u",
                               pl_id, (unsigned)want, (unsigned)(offset + got_total));
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        char *js = NULL;
        const esp_err_t err = os_net_api_get_ex(path, true, &js);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "playlist %s page @%u failed: %s (%s)", pl_id,
                     (unsigned)(offset + got_total), esp_err_to_name(err),
                     os_state_last_error() ? os_state_last_error() : "");
            if (got_total > 0) {
                break;                     /* 已经有数据：把已取到的交出去 */
            }
            /* 第一页就失败 → 老接口兜底（只对小歌单有效） */
            if (offset == 0) {
                return playlist_detail_fallback(pl_id, out, max, out_n);
            }
            return err;
        }
        cJSON *root = cJSON_Parse(js);
        free(js);
        if (!root) {
            if (got_total > 0) {
                break;
            }
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t got = 0;
        os_fill_songs(os_pick_array(root, "songs"), out + got_total, want, &got);
        cJSON_Delete(root);
        got_total += got;
        if (got < want) {
            break;                         /* 不足一页 = 到歌单末尾 */
        }
    }
    if (got_total == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_n = got_total;
    ESP_LOGI(TAG, "playlist %s: +%u songs (offset %u)", pl_id, (unsigned)got_total,
             (unsigned)offset);
    return ESP_OK;
}

esp_err_t online_netease_daily_songs(online_song_t *out, size_t max, size_t *out_n)
{
    if (!out || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    /*
     * ★ 先体检 cookie：服务端 util/request.js 里写着
     *     if (!processedCookie.MUSIC_U) processedCookie.MUSIC_A = anonymous_token
     *   —— **没有 MUSIC_U 时不报错，直接按游客返回**。所以"每日推荐还是未登录的"
     *   这类现象，答案往往就在这一行日志里。
     */
    ESP_LOGI(TAG, "daily recommend: cookie %s",
             strstr(online_netease_cookie(), "MUSIC_U=") ? "带 MUSIC_U" :
             "**没有 MUSIC_U**（服务端会按游客返回！）");
    char *js = NULL;
    esp_err_t err = os_net_api_get_ex("/recommend/songs", true, &js);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "daily recommend: 请求失败 %s（%s）", esp_err_to_name(err),
                 os_state_last_error() ? os_state_last_error() : "");
        return err;
    }
    ESP_LOGI(TAG, "daily recommend: 响应 %u 字节", (unsigned)strlen(js));
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = os_fill_songs(os_pick_array(root, "dailySongs"), out, max, out_n);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "daily recommend: %u songs", (unsigned)*out_n);
    return err;
}

/*
 * 播放直链：**带当前登录 CK 请求自建中间件**（NeteaseCloudMusicApi 的 /song/url/v1）。
 *
 * ★ 这是本工程取"播放地址"的**唯一正路**（2026-10 用户要求）：
 *   meting 的 `type=url` 是**匿名**请求 —— 会员 / 数字专辑歌曲只会下发
 *   **30 秒试听片段**（真机实测 Content-Length 439 KB ≈ 128 kbps 的 27 秒，
 *   元数据时长却是整首 → 曾经把 seek 映射和进度条一起带偏，见 audio_engine_bitrate
 *   里那段说明）。带上账号 CK 之后，中间件返回的才是**完整**直链，会员歌能整首听。
 *   meting 退居"辅助"：歌词 / 封面 / 歌单列表照旧走它。
 *
 * 等级：先 `standard`（128 kbps，任何账号都拿得到），拿不到 URL 再试一次
 *   `exhigh`（320 kbps，VIP 有效）—— 有些歌只在更高等级下才有直链。
 *   ⚠ 每次尝试都是一次 HTTP 往返，所以只在**第一次失败**时才多花这一次。
 */
#define NETEASE_URL_LEVEL_1  "standard"
#define NETEASE_URL_LEVEL_2  "exhigh"

/* 试一次 /song/url/v1。成功填 out/out_bytes/out_dur_ms 并返回 ESP_OK。 */
static esp_err_t song_url_try(const char *song_id, const char *level,
                              char *out, size_t cap, int64_t *out_bytes,
                              uint32_t *out_dur_ms, bool *out_trial)
{
    *out_trial = false;

    char path[128];
    int n = snprintf(path, sizeof(path), "/song/url/v1?id=%s&level=%s", song_id, level);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = os_net_api_get_ex(path, true, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    const cJSON *arr = os_pick_array(root, "data");
    const cJSON *first = cJSON_IsArray(arr) ? arr->child : NULL;
    if (first) {
        /*
         * ★ 试听片段识别（免费账号 / 无版权时服务端就是这样回的）：
         *   `freeTrialInfo` 非空 = 这首只给了片段（哪怕 url 是有的）。
         *   把这个事实打出来，用户就知道"该开会员"而不是"播放器坏了"。
         */
        const cJSON *trial = cJSON_GetObjectItemCaseSensitive(first, "freeTrialInfo");
        if (trial && !cJSON_IsNull(trial)) {
            *out_trial = true;
        }
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(first, "url");
        if (cJSON_IsString(u) && u->valuestring && u->valuestring[0]) {
            snprintf(out, cap, "%.*s", (int)(cap - 1), u->valuestring);
            const cJSON *sz = cJSON_GetObjectItemCaseSensitive(first, "size");
            const cJSON *br = cJSON_GetObjectItemCaseSensitive(first, "br");
            if (out_bytes && cJSON_IsNumber(sz)) {
                *out_bytes = (int64_t)sz->valuedouble;
            }
            if (out_dur_ms && cJSON_IsNumber(sz) && cJSON_IsNumber(br) &&
                br->valuedouble > 0) {
                /* 总时长 = 字节 × 8 ÷ 码率（这条路上时长是**准确**的，不用再靠墙钟估） */
                *out_dur_ms = (uint32_t)(sz->valuedouble * 8.0 * 1000.0 / br->valuedouble);
            }
            ret = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t online_netease_song_url(const char *song_id, char *out, size_t cap,
                                  int64_t *out_bytes, uint32_t *out_dur_ms)
{
    if (!song_id || !song_id[0] || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_bytes) {
        *out_bytes = 0;
    }
    if (out_dur_ms) {
        *out_dur_ms = 0;
    }

    bool trial = false;
    esp_err_t ret = song_url_try(song_id, NETEASE_URL_LEVEL_1, out, cap,
                                 out_bytes, out_dur_ms, &trial);
    if (ret != ESP_OK) {
        /* standard 拿不到：再试一次 exhigh（VIP 等级），失败就如实返回 */
        ESP_LOGD(TAG, "song url %s: level=%s 无直链，重试 %s",
                 song_id, NETEASE_URL_LEVEL_1, NETEASE_URL_LEVEL_2);
        ret = song_url_try(song_id, NETEASE_URL_LEVEL_2, out, cap,
                           out_bytes, out_dur_ms, &trial);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "song url %s: %lld bytes, %u ms%s", song_id,
                 (long long)(out_bytes ? *out_bytes : 0),
                 (unsigned)(out_dur_ms ? *out_dur_ms : 0),
                 trial ? " **试听片段(freeTrialInfo)**：该账号对这首没有完整版权" : "");
    } else {
        ESP_LOGW(TAG, "song url %s: 没拿到直链（cookie 失效 / 无版权 / 中间件不可达）", song_id);
    }
    return ret;
}
