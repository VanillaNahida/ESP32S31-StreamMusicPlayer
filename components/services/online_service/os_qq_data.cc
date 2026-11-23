/*
 * os_qq_data.cc —— QQ 音乐的业务接口：扫码登录 / 账号歌单 / 歌单曲目 / 我喜欢 /
 *                  搜索 / 取流 / 歌词 / 收藏
 *
 * 端点（全部挂在服务端 /qq 下，见 docs/qq-provider.md §4；本模块只做 GET/POST + 解析）：
 *
 *   登录   GET  /qq/login/qrcode/qq                    → data.data(base64 PNG) / identifier
 *          GET  /qq/login/qrcode/qq/status?identifier=  → data.event / data.credential
 *   歌单   GET  /qq/user/{musicid}/created_songlists    → data.playlists[]（自建）
 *          GET  /qq/user/{euin}/fav/songlists?page&num  → data.playlists[]（收藏）
 *          GET  /qq/songlist/{id}/detail?page&num       → data.songs[] / total / hasmore
 *   喜欢   GET  /qq/user/{euin}/fav/songs?page&num      → data.songs[]（dirid=201）
 *          POST /qq/songlist/add_songs|del_songs?dirid&song_id&song_type
 *   搜索   GET  /qq/search/general_search?keyword&num   → data.song.items[]
 *   取流   GET  /qq/song/{mid}/play_url                 → data.url（服务端已拼好 CDN）
 *   歌词   GET  /qq/song/{mid}/lyric                   → data.lyric（纯 LRC）
 *
 * ★ 分页口径：上游一页给 song_num 首（默认只有 10，这里显式要 100）。
 *   对外的 offset **必须是 ONLINE_QQ_PAGE_SONGS(100) 的倍数** —— app_online 的懒加载
 *   游标天然满足（每次按 100 的整数倍追加）；不满足就直接报参数错，
 *   绝不在 16 KB 工作线程栈上摆一个 100 首的临时数组（那会直接爆栈）。
 *
 * ★ 与网易云一样，这里全是**同步阻塞**调用，只能跑在工作线程里。
 */
#include "internal/os_qq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "internal/os_fs.h"
#include "internal/os_json.h"
#include "internal/os_state.h"
#include "internal/os_util.h"

static const char *TAG = "qq";

/* PSRAM 优先的缓冲（歌单/搜索一次几百 KB 的元数据都放这儿） */
static void *qq_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

static cJSON *qq_parse(const char *js)
{
    cJSON *root = cJSON_Parse(js);
    if (!root) {
        ESP_LOGW(TAG, "响应不是合法 JSON");
    }
    return root;
}

/* ---------------------------------------------------------------- 扫码登录 */

esp_err_t online_qq_qr_create(char *identifier, size_t id_cap, uint8_t **png, size_t *png_len)
{
    if (identifier && id_cap) {
        identifier[0] = '\0';
    }
    if (!png || !png_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *png = NULL;
    *png_len = 0;

    char *js = NULL;
    esp_err_t err = os_qq_get("/qq/login/qrcode/qq", false, &js);
    if (err != ESP_OK) {
        return os_qq_map_err(err);
    }
    cJSON *root = qq_parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *data = os_qq_data(root);
    if (!data) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(data, "identifier");
    if (cJSON_IsString(jid) && jid->valuestring && identifier && id_cap) {
        snprintf(identifier, id_cap, "%.*s", (int)id_cap - 1, jid->valuestring);
    }

    /* data.data = base64(PNG)；服务端同时还给 img（data URL），两个都认。 */
    const cJSON *b64 = cJSON_GetObjectItemCaseSensitive(data, "data");
    if (!cJSON_IsString(b64) || !b64->valuestring || !b64->valuestring[0]) {
        b64 = cJSON_GetObjectItemCaseSensitive(data, "img");
    }
    if (!cJSON_IsString(b64) || !b64->valuestring || !b64->valuestring[0]) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "qr: 响应里没有二维码数据");
        return ESP_ERR_NOT_FOUND;
    }
    const char *p = b64->valuestring;
    const char *comma = strchr(p, ',');
    if (comma) {
        p = comma + 1;                  /* 万一给成 data URL */
    }

    /*
     * ★★ base64 必须**先拷出来，再 cJSON_Delete(root)**（2026-11 真机踩过）：
     *    valuestring 的内存归 cJSON 所有，root 一删就释放了；继续拿那个指针去解码
     *    读到的是已释放内存 —— 现场日志是
     *        W (45358) qq: qr: base64 解码失败 (-0x002c)
     *    -0x002C 就是 MBEDTLS_ERR_BASE64_INVALID_CHARACTER：解到的是垃圾字节。
     *    拷贝时顺手去掉空白（换行/空格），免得服务端哪天换了格式又挂一次。
     */
    const size_t raw_len = strlen(p);
    if (raw_len < 16 || raw_len >= QQ_QR_B64_MAX) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "qr: base64 长度异常 (%u)", (unsigned)raw_len);
        return ESP_ERR_INVALID_SIZE;
    }
    char *b64_copy = (char *)os_psram_alloc(raw_len + 1);
    if (!b64_copy) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t b64_len = 0;
    for (size_t i = 0; i < raw_len; i++) {
        const char c = p[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        b64_copy[b64_len++] = c;
    }
    b64_copy[b64_len] = '\0';
    cJSON_Delete(root);                 /* 到这里才允许释放 JSON 树 */

    const size_t dec_max = b64_len / 4 * 3 + 4;
    uint8_t *buf = (uint8_t *)os_psram_alloc(dec_max);
    if (!buf) {
        free(b64_copy);
        return ESP_ERR_NO_MEM;
    }
    size_t dec_len = 0;
    const int rc = mbedtls_base64_decode(buf, dec_max, &dec_len,
                                        (const unsigned char *)b64_copy, b64_len);
    free(b64_copy);
    if (rc != 0 || dec_len < 8) {
        free(buf);
        ESP_LOGW(TAG, "qr: base64 解码失败 (-0x%04x, %u 字符)", (unsigned)-rc,
                 (unsigned)b64_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *png = buf;
    *png_len = dec_len;
    ESP_LOGI(TAG, "qr: %u bytes PNG (identifier %.16s…)", (unsigned)dec_len,
             identifier ? identifier : "");
    return ESP_OK;
}

esp_err_t online_qq_qr_poll(const char *identifier, int *out_event)
{
    if (!identifier || !identifier[0] || !out_event) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    const int n = snprintf(path, sizeof(path),
                           "/qq/login/qrcode/qq/status?identifier=%s", identifier);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = os_qq_get(path, false, &js);
    if (err != ESP_OK) {
        return os_qq_map_err(err);
    }
    cJSON *root = qq_parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *data = os_qq_data(root);
    int event = -1;
    const cJSON *ev = data ? cJSON_GetObjectItemCaseSensitive(data, "event") : NULL;
    if (cJSON_IsNumber(ev)) {
        event = ev->valueint;
    }
    if (event == QQ_QR_DONE) {
        /* 登录成功：data.credential 是 SDK 的 Credential 对象（snake_case）。
         * ★ 先落盘 —— 掉电/重启后不用重新扫码。 */
        const cJSON *cred = cJSON_GetObjectItemCaseSensitive(data, "credential");
        if (cJSON_IsObject(cred)) {
            char *txt = cJSON_PrintUnformatted(cred);
            if (txt) {
                const esp_err_t serr = os_qq_cred_store(txt);
                cJSON_free(txt);
                if (serr != ESP_OK) {
                    ESP_LOGW(TAG, "qr: 登录成功但凭证保存失败 (%s)", esp_err_to_name(serr));
                }
            }
        } else {
            ESP_LOGW(TAG, "qr: event=0 但响应里没有 credential");
        }
        /*
         * 昵称由**服务端**查好一起返回（/user/{euin}/homepage 的 base_info.name，
         * 见 ncm-api 的 qrcodeStatus）；设备端只负责存下来显示。
         */
        const cJSON *nick = data ? cJSON_GetObjectItemCaseSensitive(data, "nickname") : NULL;
        if (cJSON_IsString(nick) && nick->valuestring && nick->valuestring[0]) {
            os_qq_nick_store(nick->valuestring);
            ESP_LOGI(TAG, "qr: 登录昵称 = %s", nick->valuestring);
        }
    }
    cJSON_Delete(root);
    if (event < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_event = event;
    ESP_LOGI(TAG, "qr check -> event=%d", event);
    return ESP_OK;
}

esp_err_t online_qq_logout(void)
{
    /*
     * 服务端没有"注销 QQ 登录"的端点（sidecar 的登录态由 Cookie 决定，天然无状态），
     * 所以退出 = 删本地凭证：下次请求不带头 = 匿名，收藏/推荐类接口会 401。
     */
    os_qq_cred_clear();
    return ESP_OK;
}

/* ---------------------------------------------------------------- 账号歌单 */

static bool fill_playlist(const cJSON *it, online_acct_playlist_t *d, bool self_created)
{
    memset(d, 0, sizeof(*d));
    char id[24] = {0};
    os_json_id_str(cJSON_GetObjectItemCaseSensitive(it, "id"), id, sizeof(id));
    if (!id[0]) {
        return false;
    }
    snprintf(d->id, sizeof(d->id), "%.*s", (int)sizeof(d->id) - 1, id);

    /* dirid：自建歌单"加歌"用的目录 ID（收藏歌单没有；"我喜欢"固定 201） */
    const cJSON *dirid = cJSON_GetObjectItemCaseSensitive(it, "dirid");
    if (cJSON_IsNumber(dirid) && dirid->valuedouble > 0) {
        snprintf(d->dirid, sizeof(d->dirid), "%.0f", dirid->valuedouble);
    }

    const cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "title");
    if (!cJSON_IsString(t) || !t->valuestring) {
        t = cJSON_GetObjectItemCaseSensitive(it, "name");
    }
    if (cJSON_IsString(t) && t->valuestring) {
        snprintf(d->name, sizeof(d->name), "%.*s", (int)sizeof(d->name) - 1, t->valuestring);
    }
    const cJSON *pic = cJSON_GetObjectItemCaseSensitive(it, "picurl");
    if (cJSON_IsString(pic) && pic->valuestring) {
        snprintf(d->cover_url, sizeof(d->cover_url), "%.*s",
                 (int)sizeof(d->cover_url) - 1, pic->valuestring);
    }
    const cJSON *num = cJSON_GetObjectItemCaseSensitive(it, "songnum");
    if (cJSON_IsNumber(num)) {
        d->track_count = num->valueint;
    }
    d->self_created = self_created;
    /*
     * ⚠ created_songlists 里**也**包含「我喜欢」（dirid=201，实测 719 首）：
     *   它在界面上由我们统一插到第一个（见本文件 online_qq_account_playlists），
     *   这里过滤掉，避免同一个收藏夹出现两行。
     */
    if (self_created && strcmp(d->dirid, QQ_LIKE_DIRID) == 0) {
        return false;
    }
    return true;
}

/* playlists[] → out[off..]；顺路捡一个昵称（QQ 没有 /user/account） */
static size_t take_playlists(const cJSON *data, online_acct_playlist_t *out, size_t max,
                             size_t off, bool self_created, char *nick_out, size_t nick_cap)
{
    const cJSON *arr = data ? cJSON_GetObjectItemCaseSensitive(data, "playlists") : NULL;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    size_t cnt = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (off + cnt >= max) {
            break;
        }
        if (!fill_playlist(it, &out[off + cnt], self_created)) {
            continue;
        }
        if (nick_out && nick_cap && !nick_out[0]) {
            const cJSON *nk = cJSON_GetObjectItemCaseSensitive(it, "nick");
            if (!cJSON_IsString(nk)) {
                nk = cJSON_GetObjectItemCaseSensitive(it, "nickname");
            }
            if (!cJSON_IsString(nk)) {
                const cJSON *cr = cJSON_GetObjectItemCaseSensitive(it, "creator");
                if (cJSON_IsObject(cr)) {
                    nk = cJSON_GetObjectItemCaseSensitive(cr, "nick");
                }
            }
            if (cJSON_IsString(nk) && nk->valuestring && nk->valuestring[0]) {
                snprintf(nick_out, nick_cap, "%.*s", (int)nick_cap - 1, nk->valuestring);
            }
        }
        cnt++;
    }
    return cnt;
}

esp_err_t online_qq_account_playlists(online_acct_playlist_t *out, size_t max, size_t *out_n)
{
    if (!out || !out_n || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!os_qq_is_logged_in()) {
        return ESP_ERR_INVALID_STATE;
    }
    os_qq_refresh_if_needed();

    size_t cnt = 0;
    esp_err_t last = ESP_ERR_NOT_FOUND;
    char nick[64] = {0};

    /*
     * 0) 「我喜欢的音乐」—— 它不是歌单（dirid=201），但界面上就该排在第一个。
     *    这里只问 1 首，为的是拿 data.total（懒加载缓冲要按总首数一次性分配）。
     *    id 用 ONLINE_QQ_FAV_KEY 这个哨兵：点进去时工作线程据此走 fav/songs。
     */
    const char *euin0 = os_qq_encrypt_uin();
    if (euin0[0] && max > 0) {
        memset(&out[0], 0, sizeof(out[0]));
        snprintf(out[0].id, sizeof(out[0].id), "%s", ONLINE_QQ_FAV_KEY);
        snprintf(out[0].name, sizeof(out[0].name), "%s", "我喜欢的音乐");
        snprintf(out[0].dirid, sizeof(out[0].dirid), "%s", QQ_LIKE_DIRID);
        out[0].self_created = false;
        char path[384];
        snprintf(path, sizeof(path), "/qq/user/%s/fav/songs?page=1&num=1", euin0);
        char *js = NULL;
        if (os_qq_get(path, true, &js) == ESP_OK) {
            cJSON *root = qq_parse(js);
            free(js);
            if (root) {
                const cJSON *data = os_qq_data(root);
                const cJSON *t = data ? cJSON_GetObjectItemCaseSensitive(data, "total") : NULL;
                if (cJSON_IsNumber(t)) {
                    out[0].track_count = t->valueint;
                }
                cJSON_Delete(root);
            }
        }
        cnt = 1;
        last = ESP_OK;
        ESP_LOGI(TAG, "我喜欢的音乐: %d 首", out[0].track_count);
    }

    /* 1) 自建歌单（用 musicid） */
    const char *mid = os_qq_music_id();
    if (mid[0]) {
        char path[160];
        snprintf(path, sizeof(path), "/qq/user/%s/created_songlists", mid);
        char *js = NULL;
        esp_err_t err = os_qq_get(path, true, &js);
        if (err == ESP_OK) {
            cJSON *root = qq_parse(js);
            free(js);
            if (root) {
                cnt += take_playlists(os_qq_data(root), out, max, cnt, true, nick, sizeof(nick));
                cJSON_Delete(root);
                last = ESP_OK;
            } else {
                last = ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            last = os_qq_map_err(err);
        }
    }

    /* 2) 收藏歌单（用 encryptUin；凭证里没有就跳过 —— 那会 401） */
    const char *euin = os_qq_encrypt_uin();
    if (euin[0] && cnt < max) {
        char path[320];
        snprintf(path, sizeof(path), "/qq/user/%s/fav/songlists?page=1&num=100", euin);
        char *js = NULL;
        esp_err_t err = os_qq_get(path, true, &js);
        if (err == ESP_OK) {
            cJSON *root = qq_parse(js);
            free(js);
            if (root) {
                cnt += take_playlists(os_qq_data(root), out, max, cnt, false, nick, sizeof(nick));
                cJSON_Delete(root);
                last = ESP_OK;
            }
        } else if (cnt == 0) {
            last = os_qq_map_err(err);
        }
    } else if (!euin[0]) {
        ESP_LOGW(TAG, "凭证里没有 encryptUin：跳过收藏歌单");
    }

    if (nick[0]) {
        os_qq_nick_store(nick);
    }
    *out_n = cnt;
    ESP_LOGI(TAG, "QQ 歌单: %u 个（自建 + 收藏）", (unsigned)cnt);
    return cnt ? ESP_OK : last;
}

/* ---------------------------------------------------------------- 歌单曲目 / 我喜欢 */

/*
 * 拉一页 songs[] 直接填进 out[got..]。
 * 返回这一页填了多少首；*out_total / *out_more 是上游的总数与"还有更多"。
 * ⚠ 不建临时数组：100 首 online_song_t 约 120 KB，栈上放不下（工作线程 16 KB）。
 */
static size_t fetch_song_page(const char *path, online_song_t *out, size_t max,
                              int *out_total, bool *out_more)
{
    char *js = NULL;
    /* 歌单详情是公开接口：登录了就带凭证（能拿到更多音源信息），没登录也要能看 */
    if (os_qq_get(path, os_qq_is_logged_in(), &js) != ESP_OK) {
        if (out_total) {
            *out_total = 0;
        }
        if (out_more) {
            *out_more = false;
        }
        return 0;
    }
    cJSON *root = qq_parse(js);
    free(js);
    if (!root) {
        return 0;
    }
    const cJSON *data = os_qq_data(root);
    const cJSON *arr = data ? cJSON_GetObjectItemCaseSensitive(data, "songs") : NULL;
    size_t n = 0;
    os_qq_fill_songs(arr, out, max, &n);
    if (out_total) {
        const cJSON *t = data ? cJSON_GetObjectItemCaseSensitive(data, "total") : NULL;
        *out_total = cJSON_IsNumber(t) ? t->valueint : 0;
    }
    if (out_more) {
        const cJSON *h = data ? cJSON_GetObjectItemCaseSensitive(data, "hasmore") : NULL;
        *out_more = cJSON_IsNumber(h) ? (h->valueint != 0) : false;
    }
    cJSON_Delete(root);
    return n;
}

esp_err_t online_qq_playlist_page(const char *pl_id, size_t offset, online_song_t *out,
                                  size_t max, size_t *out_n)
{
    if (!pl_id || !pl_id[0] || !out || !out_n || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    if (offset % ONLINE_QQ_PAGE_SONGS) {
        ESP_LOGW(TAG, "offset %u 不是 %d 的倍数：拒绝（见文件头的分页口径）",
                 (unsigned)offset, ONLINE_QQ_PAGE_SONGS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 「我喜欢的音乐」不是歌单：调用方（界面）用同一个接口翻页，这里替它转一次 */
    if (strcmp(pl_id, ONLINE_QQ_FAV_KEY) == 0) {
        return online_qq_fav_songs(offset, out, max, out_n);
    }

    size_t got = 0;
    unsigned page = (unsigned)(offset / ONLINE_QQ_PAGE_SONGS) + 1;
    for (;;) {
        char path[192];
        const int n = snprintf(path, sizeof(path), "/qq/songlist/%s/detail?page=%u&num=%u",
                               pl_id, page, (unsigned)ONLINE_QQ_PAGE_SONGS);
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        int total = 0;
        bool more = false;
        const size_t page_n = fetch_song_page(path, out + got, max - got, &total, &more);
        got += page_n;
        if (page_n == 0 || !more || got >= max) {
            break;
        }
        if (total > 0 && (size_t)page * ONLINE_QQ_PAGE_SONGS >= (size_t)total) {
            break;
        }
        page++;
    }
    if (got == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_n = got;
    ESP_LOGI(TAG, "QQ 歌单 %s: +%u 首（offset %u）", pl_id, (unsigned)got, (unsigned)offset);
    return ESP_OK;
}

esp_err_t online_qq_fav_songs(size_t offset, online_song_t *out, size_t max, size_t *out_n)
{
    if (!out || !out_n || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    if (offset % ONLINE_QQ_PAGE_SONGS) {
        ESP_LOGW(TAG, "offset %u 不是 %d 的倍数", (unsigned)offset, ONLINE_QQ_PAGE_SONGS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *euin = os_qq_encrypt_uin();
    if (!os_qq_is_logged_in() || !euin[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t got = 0;
    unsigned page = (unsigned)(offset / ONLINE_QQ_PAGE_SONGS) + 1;
    for (;;) {
        char path[384];
        const int n = snprintf(path, sizeof(path), "/qq/user/%s/fav/songs?page=%u&num=%u",
                               euin, page, (unsigned)ONLINE_QQ_PAGE_SONGS);
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        bool more = false;
        const size_t page_n = fetch_song_page(path, out + got, max - got, NULL, &more);
        got += page_n;
        if (page_n == 0 || !more || got >= max) {
            break;
        }
        page++;
    }
    if (got == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_n = got;
    ESP_LOGI(TAG, "QQ 我喜欢: +%u 首（offset %u）", (unsigned)got, (unsigned)offset);
    return ESP_OK;
}

/* 从 songs[] 里只取 id（红心状态表用；几十 KB 的 JSON 不必都转成 online_song_t） */
static size_t collect_ids(const cJSON *arr, uint64_t *dst, size_t cap)
{
    size_t k = 0;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (k >= cap) {
            break;
        }
        char buf[24] = {0};
        os_json_id_str(cJSON_GetObjectItemCaseSensitive(it, "id"), buf, sizeof(buf));
        if (buf[0]) {
            dst[k++] = strtoull(buf, NULL, 10);
        }
    }
    return k;
}

static int qq_id_cmp(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

esp_err_t online_qq_likelist(uint64_t **out_ids, size_t *out_n)
{
    if (!out_ids || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ids = NULL;
    *out_n = 0;
    if (!os_qq_is_logged_in()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *euin = os_qq_encrypt_uin();
    if (!euin[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t cap = (size_t)QQ_LIKE_MAX_PAGES * QQ_LIKE_PAGE_SONGS;
    uint64_t *arr = (uint64_t *)qq_alloc(cap * sizeof(uint64_t));
    if (!arr) {
        return ESP_ERR_NO_MEM;
    }
    size_t k = 0;
    for (int page = 1; page <= QQ_LIKE_MAX_PAGES; page++) {
        char path[384];
        snprintf(path, sizeof(path), "/qq/user/%s/fav/songs?page=%d&num=%u", euin, page,
                 (unsigned)QQ_LIKE_PAGE_SONGS);
        char *js = NULL;
        esp_err_t err = os_qq_get(path, true, &js);
        if (err != ESP_OK) {
            if (k == 0) {
                free(arr);
                return os_qq_map_err(err);
            }
            break;
        }
        cJSON *root = qq_parse(js);
        free(js);
        if (!root) {
            if (k == 0) {
                free(arr);
                return ESP_ERR_INVALID_RESPONSE;
            }
            break;
        }
        const cJSON *data = os_qq_data(root);
        const cJSON *arr_songs = data ? cJSON_GetObjectItemCaseSensitive(data, "songs") : NULL;
        k += collect_ids(arr_songs, arr + k, cap - k);
        const cJSON *h = data ? cJSON_GetObjectItemCaseSensitive(data, "hasmore") : NULL;
        const bool more = cJSON_IsNumber(h) ? (h->valueint != 0) : false;
        cJSON_Delete(root);
        if (!more) {
            break;
        }
    }
    if (k == 0) {
        free(arr);
        return ESP_ERR_NOT_FOUND;
    }
    qsort(arr, k, sizeof(uint64_t), qq_id_cmp);
    size_t w = 1;
    for (size_t r = 1; r < k; r++) {
        if (arr[r] != arr[w - 1]) {
            arr[w++] = arr[r];
        }
    }
    *out_ids = arr;
    *out_n = w;
    ESP_LOGI(TAG, "QQ 我喜欢: %u 个 id", (unsigned)w);
    return ESP_OK;
}

bool online_qq_liked_has(const uint64_t *ids, size_t n, const char *song_id)
{
    if (!ids || !n || !song_id || !song_id[0]) {
        return false;
    }
    const uint64_t v = strtoull(song_id, NULL, 10);
    size_t lo = 0;
    size_t hi = n;
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

/* ---------------------------------------------------------------- 喜欢 / 收藏到歌单 */

/*
 * 往 dirid 加（add=true）/ 删一首歌。
 *
 * ★ 加歌时服务端会先查目标歌单里有没有这首歌（"是不是已经收藏过"），命中就不写；
 *   响应里的 data.already 就是那个答案。查目标歌单要用 playlist_id（dissid），
 *   没有就跳过检查（服务端返回 checked=false，我们只能按"加成功"报）。
 *
 * ★ 红心（online_qq_like）带 check=0：播放器已经知道状态了，没必要为一次点赞
 *   多翻十几页歌单（那会让红心亮起来慢一两秒）。
 */
static esp_err_t song_edit(const char *dirid, const char *song_id, bool add,
                           const char *playlist_id, bool check, bool *out_already)
{
    if (out_already) {
        *out_already = false;
    }
    if (!dirid || !dirid[0] || !song_id || !song_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!os_qq_is_logged_in()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[192];
    int n;
    if (add) {
        /* dirid=201 走服务端的 euin 分支，"我喜欢" 也没有真正的 dissid
         * （界面传的是 ONLINE_QQ_FAV_KEY 哨兵，含控制字符，不能进 URL）。 */
        const bool has_pl = (playlist_id && playlist_id[0] &&
                             strcmp(dirid, QQ_LIKE_DIRID) != 0);
        n = snprintf(path, sizeof(path),
                     "/qq/songlist/add_songs?dirid=%s&song_id=%s&song_type=1&check=%d%s%s",
                     dirid, song_id, check ? 1 : 0,
                     has_pl ? "&playlist_id=" : "", has_pl ? playlist_id : "");
    } else {
        n = snprintf(path, sizeof(path),
                     "/qq/songlist/del_songs?dirid=%s&song_id=%s&song_type=1", dirid, song_id);
    }
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = os_qq_post(path, true, &js);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s dirid=%s song=%s 失败 (%s)", add ? "add_songs" : "del_songs",
                 dirid, song_id, esp_err_to_name(err));
        return os_qq_map_err(err);
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    int code = 0;
    os_json_find_int(root, "code", &code);
    bool already = false;
    if (code == 200) {
        const cJSON *data = os_qq_data(root);
        const cJSON *al = data ? cJSON_GetObjectItemCaseSensitive(data, "already") : NULL;
        already = cJSON_IsBool(al) ? cJSON_IsTrue(al) : false;
    }
    cJSON_Delete(root);
    if (code != 200) {
        ESP_LOGW(TAG, "%s: code=%d", add ? "add_songs" : "del_songs", code);
        return ESP_FAIL;
    }
    if (out_already) {
        *out_already = already;
    }
    ESP_LOGI(TAG, "%s: dirid=%s song=%s %s", add ? "add_songs" : "del_songs", dirid, song_id,
             already ? "（已经在歌单里）" : "ok");
    return ESP_OK;
}

esp_err_t online_qq_like(const char *song_id, bool like)
{
    /* 红心不做"是否已收藏"的检查（见 song_edit 的注释） */
    return song_edit(QQ_LIKE_DIRID, song_id, like, NULL, false, NULL);
}

esp_err_t online_qq_song_add(const char *dirid, const char *song_id, const char *playlist_id,
                             bool *out_already)
{
    return song_edit(dirid, song_id, true, playlist_id, true, out_already);
}

esp_err_t online_qq_song_del(const char *dirid, const char *song_id)
{
    return song_edit(dirid, song_id, false, NULL, false, NULL);
}

/* ---------------------------------------------------------------- 每日30首 / 私人雷达 */

/*
 * 两种推荐都是"服务端拼好的 30 首"，响应形如
 *   {"code":200,"msg":"ok","data":{"songs":[<Song>…],"source":"guess|radar","count":30}}
 * 所以这里只做一件事：把 songs[] 转成 online_song_t。
 */
static esp_err_t qq_feed(const char *path, online_song_t *out, size_t max, size_t *out_n)
{
    if (!path || !out || !out_n || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 推荐需要登录态才有"私人"效果；未登录也让它试一次（上游回匿名推荐） */
    char *js = NULL;
    esp_err_t err = os_qq_get(path, os_qq_is_logged_in(), &js);
    if (err != ESP_OK) {
        return os_qq_map_err(err);
    }
    cJSON *root = qq_parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *data = os_qq_data(root);
    const cJSON *songs = data ? cJSON_GetObjectItemCaseSensitive(data, "songs") : NULL;
    size_t got = 0;
    os_qq_fill_songs(songs, out, max, &got);
    cJSON_Delete(root);
    if (got == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_n = got;
    ESP_LOGI(TAG, "QQ 推荐 %s: %u 首", path, (unsigned)got);
    return ESP_OK;
}

esp_err_t online_qq_daily_songs(online_song_t *out, size_t max, size_t *out_n)
{
    return qq_feed("/qq/recommend/daily", out, max, out_n);
}

esp_err_t online_qq_radar_songs(online_song_t *out, size_t max, size_t *out_n)
{
    return qq_feed("/qq/recommend/radar", out, max, out_n);
}

/* ---------------------------------------------------------------- 搜索 */

esp_err_t online_qq_search(const char *keyword, online_song_t **out, size_t *count, size_t max)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *count = 0;
    if (!keyword || !keyword[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (max == 0) {
        max = 30;
    }
    if (max > 100) {
        max = 100;
    }

    char kw[192];
    os_url_encode(keyword, kw, sizeof(kw));
    char path[320];
    const int n = snprintf(path, sizeof(path), "/qq/search/general_search?keyword=%s&num=%u",
                           kw, (unsigned)max);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = os_qq_get(path, false, &js);
    if (err != ESP_OK) {
        return os_qq_map_err(err);
    }
    cJSON *root = qq_parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* 综合搜索把歌曲放在 data.song.items[]（GeneralSearchResponse） */
    const cJSON *data = os_qq_data(root);
    const cJSON *song = data ? cJSON_GetObjectItemCaseSensitive(data, "song") : NULL;
    const cJSON *items = cJSON_IsObject(song)
                             ? cJSON_GetObjectItemCaseSensitive(song, "items") : NULL;
    online_song_t *acc = (online_song_t *)qq_alloc(max * sizeof(online_song_t));
    if (!acc) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    os_qq_fill_songs(items, acc, max, &got);
    cJSON_Delete(root);
    if (got == 0) {
        free(acc);
        return ESP_ERR_NOT_FOUND;
    }
    *out = acc;
    *count = got;
    ESP_LOGI(TAG, "QQ 搜索 '%s': %u 首", keyword, (unsigned)got);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 取流 / 歌词 */

/*
 * QQ 的**试听音源**（上游 RS02，约 30~60 秒，无权限的歌只给这一档）。
 *
 * 服务端 play_url 的 file_type 是 QQMusicApi 的枚举下标；实测（2026-11 线上）：
 *   13 = MP3_128（默认）、12 = MP3_320、11 = OGG_96、15 = ACC_96、
 *   **30 = TRY（试听）** —— 对需要会员的歌，只有 30 能拿到 purl。
 * 拿不到的是 result=104003（无权限）。
 */
#define QQ_FILE_TYPE_TRY   30

esp_err_t online_qq_song_url(const online_song_t *song, char *out, size_t cap,
                             int64_t *out_bytes, uint32_t *out_dur_ms)
{
    if (!song || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_bytes) {
        *out_bytes = 0;
    }
    if (out_dur_ms) {
        *out_dur_ms = 0;
    }
    if (!song->mid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    os_qq_refresh_if_needed();

    /*
     * 两档取流：
     *   ① 正常音质（服务端默认 MP3_128）—— 免费歌 / 会员账号能直接拿到整首；
     *   ② 试听（file_type=30）—— 上游对"无权限"的歌只给这一档，
     *      拿到的是 30~60 秒片段；不试这一档的话，VIP 歌在非会员账号上就是**完全放不了**
     *      （2026-11 线上实测：13/12/11/15 全部 104003，只有 30 有 purl）。
     *
     * ⚠ 试听片段的时长不能按元数据给（那是整首长度，会把进度条和 seek 全带偏），
     *   所以 trial 时 out_dur_ms 留 0（播放器显示 --:--，按流实际长度走）。
     */
    static const int ladder[] = { -1, QQ_FILE_TYPE_TRY };   /* -1 = 用服务端默认音质 */
    char url[ONLINE_URL_MAX * 3] = {0};
    int  last_result = 0;
    bool trial = false;

    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        char path[224];
        int n;
        const long long ts = (long long)(esp_timer_get_time() / 1000);
        if (ladder[i] < 0) {
            /* 带时间戳把服务端 2 分钟的 GET 缓存穿透掉：vkey 会过期，
             * 命中缓存可能拿到已经失效的地址（表现是"重试一次就好了"）。 */
            n = snprintf(path, sizeof(path), "/qq/song/%s/play_url?quality=standard&ts=%lld",
                         song->mid, ts);
        } else {
            n = snprintf(path, sizeof(path), "/qq/song/%s/play_url?file_type=%d&ts=%lld",
                         song->mid, ladder[i], ts);
        }
        if (n <= 0 || n >= (int)sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        char *js = NULL;
        const esp_err_t err = os_qq_get(path, os_qq_is_logged_in(), &js);
        if (err != ESP_OK) {
            return os_qq_map_err(err);
        }
        cJSON *root = qq_parse(js);
        free(js);
        if (!root) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const cJSON *data = os_qq_data(root);
        url[0] = '\0';
        if (data) {
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(data, "url");
            if (cJSON_IsString(u) && u->valuestring && u->valuestring[0]) {
                snprintf(url, sizeof(url), "%.*s", (int)sizeof(url) - 1, u->valuestring);
            }
        }
        const cJSON *res = data ? cJSON_GetObjectItemCaseSensitive(data, "result") : NULL;
        last_result = cJSON_IsNumber(res) ? res->valueint : 0;
        cJSON_Delete(root);

        if (url[0]) {
            trial = (ladder[i] > 0);
            if (trial) {
                ESP_LOGW(TAG, "QQ 取流 %s: 无完整版权，退回**试听片段**（file_type=%d）",
                         song->mid, ladder[i]);
            }
            break;
        }
        /* 只有"没有权限"值得再试试听档；空 purl / 其它错误码重试也是白跑 */
        if (last_result != 104003) {
            break;
        }
    }

    if (!url[0]) {
        /* 上游的版权/权限结论（不是网络故障）：界面据此提示原因 */
        ESP_LOGW(TAG, "QQ 取流 %s: 没有地址（result=%d, logged_in=%d）", song->mid,
                 last_result, (int)os_qq_is_logged_in());
        if (!os_qq_is_logged_in()) {
            os_state_set_last_error("请先登录 QQ 音乐（匿名只能放免费歌）");
        } else {
            os_state_set_last_error(last_result == 104003
                                        ? "这首需要会员，且没有试听片段"
                                        : "服务端没有给出播放地址");
        }
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(out, cap, "%s", url);
    if (out_bytes) {
        *out_bytes = 0;                 /* play_url 不返回文件大小 */
    }
    if (out_dur_ms && song->dur_sec && !trial) {
        *out_dur_ms = (uint32_t)song->dur_sec * 1000u;
    }
    ESP_LOGI(TAG, "QQ 取流 %s: %u 字符, %s, 元数据时长 %u s", song->mid, (unsigned)strlen(out),
             trial ? "**试听片段**" : "整首", (unsigned)song->dur_sec);
    return ESP_OK;
}

esp_err_t online_qq_lyric(const online_song_t *song, char **out, size_t *out_len)
{
    if (!song || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (!song->mid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online_has_network()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[128];
    const int n = snprintf(path, sizeof(path), "/qq/song/%s/lyric", song->mid);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *js = NULL;
    esp_err_t err = os_qq_get(path, false, &js);
    if (err != ESP_OK) {
        return os_qq_map_err(err);
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *data = os_qq_data(root);
    const cJSON *ly = data ? cJSON_GetObjectItemCaseSensitive(data, "lyric") : NULL;
    if (!cJSON_IsString(ly) || !ly->valuestring || !ly->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = strlen(ly->valuestring);
    if (len > 64 * 1024) {
        len = 64 * 1024;                /* 再长就截断：LRC 不可能这么大 */
    }
    char *text = (char *)os_psram_alloc(len + 1);
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    memcpy(text, ly->valuestring, len);
    text[len] = '\0';
    cJSON_Delete(root);
    if (len < 8) {
        free(text);
        return ESP_ERR_NOT_FOUND;       /* 只有 [ti:]/[ar:] 的空壳 */
    }
    *out = text;
    if (out_len) {
        *out_len = len;
    }
    ESP_LOGI(TAG, "QQ 歌词 %s: %u 字节", song->mid, (unsigned)len);
    return ESP_OK;
}
