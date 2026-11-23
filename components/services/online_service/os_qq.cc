/*
 * os_qq.cc —— QQ 音乐：凭证（NVS + base64 头）+ 临期刷新 + 带凭证的请求
 *
 * 与 os_login.cc（网易云 cookie）是**并列**的两套登录态：
 *   网易云：X-Netease-Cookie: MUSIC_U=…; __csrf=…       （浏览器 cookie 串）
 *   QQ    ：X-QQMusic-Credential: base64(JSON)          （SDK 的 Credential 对象）
 * 两者都只对自家 API base 发（见 os_http_client.cc 的 is_own_api）。
 *
 * 实现要点：
 *   1) 凭证**整份 JSON** 存 NVS（单条字符串）。解析出来的字段只用于"要不要刷新"、
 *      拼路径（musicid / encryptUin）和显示，不复制到别处；
 *   2) 刷新是**串行**的（本模块所有调用都在工作线程里，没有并发）；
 *      临期门槛 = 到期前 10%，且距上次尝试至少 30 分钟（服务端也按账号限频 30 s）；
 *   3) 401/403 只置"已失效"标志，不删凭证（见 os_qq.h 文件头）。
 */
#include "internal/os_qq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "internal/os_http_client.h"
#include "internal/os_json.h"
#include "internal/os_state.h"

static const char *TAG = "qq";

/* 刷新尝试的最小间隔：30 分钟（真到了"临期"才会进来，这里只是防抖） */
#define QQ_REFRESH_MIN_GAP_MS   (30 * 60 * 1000)
/* 到期前多少比例开始刷新：10% */
#define QQ_REFRESH_MARGIN_PCT   10

static char    s_cred[QQ_CRED_MAX];
static bool    s_loaded;
static bool    s_expired;
static char    s_hdr[QQ_HDR_MAX];
static char    s_musicid[24];
static char    s_euin[192];
static char    s_nick[64];
static bool    s_nick_loaded;
static int64_t s_create_time;
static int64_t s_expires_in;
static int64_t s_last_refresh_ms;

/* ---------------------------------------------------------------- 内部 */

/*
 * 把一份凭证 JSON 解析进内存态（不落盘）。
 * 只要求 musicid > 0 且 musickey 非空 —— 与服务端 normalizeCredential 的判据一致。
 */
static esp_err_t cred_apply(const char *json)
{
    if (!json || !json[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "credential: JSON 解析失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    char mid[24] = {0};
    os_json_id_str(cJSON_GetObjectItemCaseSensitive(root, "musicid"), mid, sizeof(mid));
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "musickey");
    const bool has_key = cJSON_IsString(key) && key->valuestring && key->valuestring[0];
    if (!mid[0] || strcmp(mid, "0") == 0 || !has_key) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "credential: 缺 musicid/musickey，拒绝保存");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(root, "musickey_create_time");
    const cJSON *ei = cJSON_GetObjectItemCaseSensitive(root, "key_expires_in");
    const cJSON *eu = cJSON_GetObjectItemCaseSensitive(root, "encrypt_uin");
    s_create_time = cJSON_IsNumber(ct) ? (int64_t)ct->valuedouble : 0;
    s_expires_in  = cJSON_IsNumber(ei) ? (int64_t)ei->valuedouble : 0;
    snprintf(s_musicid, sizeof(s_musicid), "%s", mid);
    if (cJSON_IsString(eu) && eu->valuestring) {
        snprintf(s_euin, sizeof(s_euin), "%.*s", (int)sizeof(s_euin) - 1, eu->valuestring);
    } else {
        s_euin[0] = '\0';
    }
    cJSON_Delete(root);

    /* ⚠ json 可能就是 s_cred 自己（os_qq_cred_load 直接解析 NVS 读进的那块）：
     *   snprintf 的源与目标重叠是未定义行为，这种情况就别再拷一遍。 */
    if (json != s_cred) {
        snprintf(s_cred, sizeof(s_cred), "%.*s", (int)sizeof(s_cred) - 1, json);
    }
    /* 请求头：base64(JSON)。失败就清空 —— 宁可报"未登录"也不要发一个坏头。 */
    size_t olen = 0;
    const int rc = mbedtls_base64_encode((unsigned char *)s_hdr, sizeof(s_hdr) - 1, &olen,
                                        (const unsigned char *)s_cred, strlen(s_cred));
    if (rc != 0) {
        ESP_LOGW(TAG, "credential: base64 编码失败 (-0x%04x)", (unsigned)-rc);
        s_hdr[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    s_hdr[olen] = '\0';
    s_expired = false;
    ESP_LOGI(TAG, "credential loaded: musicid=%s, expires_in=%lld s, encryptUin %s",
             s_musicid, (long long)s_expires_in, s_euin[0] ? "ok" : "MISSING!");
    return ESP_OK;
}

static void cred_clear_mem(void)
{
    s_cred[0] = '\0';
    s_hdr[0] = '\0';
    s_musicid[0] = '\0';
    s_euin[0] = '\0';
    s_nick[0] = '\0';
    s_create_time = 0;
    s_expires_in  = 0;
    s_expired     = false;
}

/* ---------------------------------------------------------------- 凭证 */

bool os_qq_cred_load(void)
{
    if (s_loaded) {
        return s_cred[0] != '\0' && s_hdr[0] != '\0';
    }
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(QQ_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_cred);
    esp_err_t err = nvs_get_str(h, QQ_NVS_KEY_CRED, s_cred, &len);
    nvs_close(h);
    if (err != ESP_OK || !s_cred[0]) {
        s_cred[0] = '\0';
        return false;
    }
    if (cred_apply(s_cred) != ESP_OK) {
        s_cred[0] = '\0';
        return false;
    }
    return true;
}

bool os_qq_is_logged_in(void)
{
    return os_qq_cred_load();
}

const char *os_qq_cred_json(void)
{
    os_qq_cred_load();
    return s_cred;
}

const char *os_qq_hdr(void)
{
    os_qq_cred_load();
    return s_hdr;
}

const char *os_qq_music_id(void)
{
    os_qq_cred_load();
    return s_musicid;
}

const char *os_qq_encrypt_uin(void)
{
    os_qq_cred_load();
    return s_euin;
}

bool os_qq_cred_expired(void)
{
    return s_expired;
}

esp_err_t os_qq_cred_store(const char *json)
{
    if (!json || !json[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = cred_apply(json);
    if (err != ESP_OK) {
        return err;
    }
    s_loaded = true;

    nvs_handle_t h;
    esp_err_t nerr = nvs_open(QQ_NVS_NS, NVS_READWRITE, &h);
    if (nerr == ESP_OK) {
        nerr = nvs_set_str(h, QQ_NVS_KEY_CRED, s_cred);
        if (nerr == ESP_OK) {
            nerr = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (nerr != ESP_OK) {
        /* 落盘失败不影响本次使用（内存里已经有了），但重启要重新扫码 —— 说清楚 */
        ESP_LOGW(TAG, "credential save to NVS failed: %s", esp_err_to_name(nerr));
    }
    ESP_LOGI(TAG, "QQ 登录成功：musicid=%s", s_musicid);
    return ESP_OK;
}

const char *os_qq_nick(void)
{
    if (!s_nick_loaded) {
        s_nick_loaded = true;
        s_nick[0] = '\0';
        nvs_handle_t h;
        if (nvs_open(QQ_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_nick);
            if (nvs_get_str(h, QQ_NVS_KEY_NICK, s_nick, &len) != ESP_OK) {
                s_nick[0] = '\0';
            }
            nvs_close(h);
        }
    }
    return s_nick;
}

esp_err_t os_qq_nick_store(const char *nick)
{
    const char *v = nick ? nick : "";
    if (s_nick_loaded && strcmp(s_nick, v) == 0) {
        return ESP_OK;                  /* 没变：别每次进页面都写 flash */
    }
    snprintf(s_nick, sizeof(s_nick), "%s", v);
    s_nick_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(QQ_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (s_nick[0]) {
        err = nvs_set_str(h, QQ_NVS_KEY_NICK, s_nick);
    } else {
        err = nvs_erase_key(h, QQ_NVS_KEY_NICK);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void os_qq_cred_clear(void)
{
    cred_clear_mem();
    s_loaded = true;
    s_nick_loaded = true;
    nvs_handle_t h;
    if (nvs_open(QQ_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, QQ_NVS_KEY_CRED);
        nvs_erase_key(h, QQ_NVS_KEY_NICK);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "QQ credential cleared");
}

/*
 * 临期刷新。
 *
 * 判据（与 docs/qq-provider.md §3.2 一致）：
 *   - 凭证里同时有 musickey_create_time 与 key_expires_in 才谈得上"临期"；
 *   - 现在时刻 >= 到期时刻 - 10% 寿命 → 刷；
 *   - 距上次尝试不足 30 分钟 → 不刷（服务端还有 30 s 的按账号限频）。
 *
 * 设备没有 RTC 时 time(NULL) 是 1970：那时 now < create，判据自然不成立，
 * 行为退化成"不刷新"—— 凭证还能用就不该动它，这是安全的一侧。
 */
esp_err_t os_qq_refresh_if_needed(void)
{
    if (!os_qq_cred_load()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_create_time <= 0 || s_expires_in <= 0) {
        return ESP_ERR_NOT_FOUND;              /* 上游没给寿命：不猜 */
    }
    const int64_t now = (int64_t)time(NULL);
    const int64_t expiry = s_create_time + s_expires_in;
    const int64_t margin = s_expires_in * QQ_REFRESH_MARGIN_PCT / 100;
    if (now * 1000 < (s_create_time + s_expires_in - margin) * 1000) {
        return ESP_ERR_NOT_FOUND;              /* 还早：不刷 */
    }
    /* 播放线程（起播取流）与工作线程（歌单/收藏）都可能走到这里：
     * 用一个很轻的"在刷"标志把并发压成一次 —— 丢一次刷新没关系，下次到点还会来。 */
    static volatile bool refreshing;
    if (refreshing) {
        return ESP_ERR_NOT_FOUND;
    }
    const int64_t t_ms = esp_timer_get_time() / 1000;
    if (s_last_refresh_ms && (t_ms - s_last_refresh_ms) < QQ_REFRESH_MIN_GAP_MS) {
        return ESP_ERR_NOT_FOUND;              /* 刚试过：别反复打 */
    }
    s_last_refresh_ms = t_ms;
    refreshing = true;

    ESP_LOGI(TAG, "credential 临期（%lld/%lld），尝试刷新", (long long)now, (long long)expiry);
    char *js = NULL;
    const esp_err_t err = os_qq_post("/qq/login/refresh", true, &js);
    if (err != ESP_OK) {
        refreshing = false;
        ESP_LOGW(TAG, "credential refresh failed: %s (%s)", esp_err_to_name(err),
                 os_state_last_error() ? os_state_last_error() : "");
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *data = os_qq_data(root);
    const cJSON *cred = data ? cJSON_GetObjectItemCaseSensitive(data, "credential") : NULL;
    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsObject(cred)) {
        /* ★ 先落盘再用：刷新已经发生，丢了响应就只能重新扫码（服务端有托管兜底，
         *   但设备能自己保住的就自己保住）。 */
        char *txt = cJSON_PrintUnformatted(cred);
        if (txt) {
            ret = os_qq_cred_store(txt);
            cJSON_free(txt);
        }
    }
    cJSON_Delete(root);
    refreshing = false;
    ESP_LOGI(TAG, "credential refresh %s", ret == ESP_OK ? "ok" : "failed");
    return ret;
}

/* ---------------------------------------------------------------- 请求 */

/*
 * 发一次带凭证的 QQ 请求。
 *
 * ★ 头必须成对发：
 *     X-QQMusic-Credential: <base64>
 *     X-QQMusic-Credential-Encoding: base64
 *   服务端 parser（ncm-api 的 qq-credential.js）只在第二个头说 base64 时才解码，
 *   少了它会把 base64 当原始 JSON 解析 → 400。
 */
static esp_err_t qq_request(http_verb_t verb, const char *path, bool with_cred, char **out_json)
{
    if (!path || !path[0] || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    const char *hdr = (with_cred && os_qq_is_logged_in()) ? os_qq_hdr() : NULL;
    if (with_cred && (!hdr || !hdr[0])) {
        return ESP_ERR_INVALID_STATE;          /* 需要登录但没登录 */
    }

    /* static 而不是栈上：工作线程栈只有 16 KB，这里 base(96)+path(≤512) */
    static char url[1024];
    const int n = snprintf(url, sizeof(url), "%s%s", online_netease_api_base(), path);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t len = 0;
    const esp_err_t err = http_request_ext(verb, url,
                                           hdr ? "X-QQMusic-Credential" : NULL, hdr,
                                           hdr ? "X-QQMusic-Credential-Encoding" : NULL,
                                           hdr ? "base64" : NULL,
                                           out_json, &len, QQ_JSON_MAX);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    /*
     * http_get_hops() 对非 200 只留一句"服务器返回 HTTP %d"（body 丢掉）。
     * 401/403 就是凭证失效 —— 记下来，让调用方映射成"请重新扫码"，
     * 但**别在这里删凭证**：也可能是服务端自己 502 或网络抖动。
     */
    const char *why = os_state_last_error();
    if (why && (strstr(why, "HTTP 401") || strstr(why, "HTTP 403"))) {
        s_expired = true;
        ESP_LOGW(TAG, "QQ 凭证失效（%s）：需要重新扫码", why);
    } else if (why && strstr(why, "HTTP 400")) {
        ESP_LOGW(TAG, "QQ 请求被拒（%s）：检查凭证头与参数", why);
    }
    ESP_LOGW(TAG, "QQ %s failed: %s (%s)",
             (verb == HTTP_VERB_POST) ? "POST" : "GET", esp_err_to_name(err),
             why ? why : "-");
    return err;
}

esp_err_t os_qq_get(const char *path, bool with_cred, char **out_json)
{
    return qq_request(HTTP_VERB_GET, path, with_cred, out_json);
}

esp_err_t os_qq_post(const char *path, bool with_cred, char **out_json)
{
    return qq_request(HTTP_VERB_POST, path, with_cred, out_json);
}

esp_err_t os_qq_map_err(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_RESPONSE && s_expired) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

/* ================================================================ 对外封装
 *
 * online_service.h 里那一组 `online_qq_*` 是 C 接口（界面/播放器只 include 那个头），
 * 内部实现用 os_qq_ 前缀。这一层就是"改名"，没有逻辑 —— 别在这里加判断。
 */

bool online_qq_logged_in(void)
{
    return os_qq_is_logged_in();
}

const char *online_qq_nickname(void)
{
    return os_qq_nick();
}

esp_err_t online_qq_nickname_set(const char *name)
{
    return os_qq_nick_store(name);
}

const char *online_qq_musicid(void)
{
    return os_qq_music_id();
}

esp_err_t online_qq_cred_reload(void)
{
    /* NVS 不需要"等挂载"，但界面在开机/退出登录后重建时统一调它即可 */
    s_loaded = false;
    return os_qq_cred_load() ? ESP_OK : ESP_ERR_NOT_FOUND;
}
