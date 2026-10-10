/*
 * os_login.cc —— 登录态：cookie（内置 / SD / 压缩 / 存盘）+ 昵称 + 扫码三接口 + 登出
 *
 * 逐字搬自原 online_service.cc：
 *   cookie_pick               2202–2239（内置 cookie 的条件编译块内）
 *   net_cookie_builtin_load   2246–2266 / 2274–2277（两份 #if/#else 实现）
 *   net_cookie_load           2281–2319
 *   cookie_compact            2332–2432
 *   net_cookie_save           2434–2456
 *   online_netease_logged_in  2458–2462
 *   online_netease_cookie_reload 2472–2481
 *   nvs_load_nick             2485–2501
 *   online_netease_nickname(_set) 2503–2541
 *   online_netease_cookie     2543–2547
 *   online_netease_logout     2549–2559
 *   online_netease_qr_key     2627–2663
 *   online_netease_qr_create  2665–2732
 *   online_netease_qr_poll    2734–2780
 *
 * ⚠ 三处"别搬错"的地方：
 *   1) cookie_pick 从"内置 cookie"的 #if 块里**提出来**成公共静态函数：它本身是
 *      纯逻辑，与 ONLINE_NCM_CK_BUILTIN 无关；提出来后 `#else` 桩里那份不退化的
 *      行为与原来完全一致（原桩不使用它）。同时它现在能被 host 单测直接调到。
 *   2) cookie_compact 的 `static char pairs[12][1024]`（~12 KB）与 net_cookie_save
 *      的 `static char compact[2048]` **必须留在 static**：注释 2345–2349 记录了
 *      放栈上导致 `Stack protection fault` 的真机事故。
 *   3) 扫码轮询路径**必须带 timestamp**，否则服务端缓存第一次的 801（见函数注释）。
 */
#include "internal/os_login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "internal/os_api.h"
#include "internal/os_fs.h"
#include "internal/os_json.h"
#include "internal/os_media_cache.h"
#include "internal/os_state.h"
#include "internal/os_util.h"
#include "online_service.h"

static const char *TAG = "online";

static char s_net_cookie[NET_COOKIE_MAX];
static bool s_net_cookie_loaded;
static bool s_net_logged_in;
/* 已登录账号昵称（NVS 持久化；空串 = 未知）。见 NVS_KEY_NICK 与
 * online_netease_nickname()：重启后不等网络往返就能显示"已登录：XX"。 */
static char s_net_nick[64];
static bool s_net_nick_loaded;

/* ------------------------------------------------- 内置 cookie（免扫码登录）
 *
 * 来源：CONFIG_ONLINE_NETEASE_CK_FILE（**相对工程根目录**的路径，默认
 *       assets/ncm-ck.txt），由 components/services/online_service/CMakeLists.txt
 *       在配置阶段以 EMBED(TEXT) 编进固件 —— TEXT 会在末尾补 '\0'，于是
 *       _binary_ncm_ck_builtin_start 可以直接当 C 字符串用。
 *
 * 三种情况都**不启用**：Kconfig 留空 / 文件不存在 / 文件里没有 MUSIC_U。
 * 那时 ONLINE_NCM_CK_BUILTIN 未定义，代码走的就是"没有这个功能"的老路
 * （扫码登录 + SD 上存的 cookie），不报错也不提示。
 *
 * 地位：**扫码登录的替代品，不是替代登录态**。SD 上有扫码存下来的 cookie 时
 *       以 SD 为准（那是账号真实登录出来的，比固件里烘死的更新）。
 */
#if defined(ONLINE_NCM_CK_BUILTIN)

#ifdef __cplusplus
extern "C" {
#endif
extern const char _binary_ncm_ck_builtin_start[];   /* 由 .S 提供，C 符号名不带修饰 */
#ifdef __cplusplus
}
#endif

#endif /* ONLINE_NCM_CK_BUILTIN */

/*
 * 从原始 cookie 串里挑出名字为 name 的那一段（`NAME=VALUE`），拷进 out。
 * 返回写入长度（0 = 没找到）。cap > 0 时 out 一定是合法的 NUL 结尾串。
 *
 * 为什么不复用 cookie_compact()：它把**所有**非属性项都塞进 pairs[12]，
 * 而浏览器导出的 cookie 串有二十多项、真正的 MUSIC_U 往往排在很后面 ——
 * 12 个槽位根本轮不到它，结果就是"内置了 cookie 却仍然匿名"。
 * 这里按名字精确提取，与项数和顺序都无关。
 * （cookie_compact() 那条路是服务端 Set-Cookie 拼串，MUSIC_U 位置靠前，
 *   实测一直正常，所以保持原样不动，避免把能用的扫码登录改坏。）
 */
size_t os_cookie_pick(const char *raw, const char *name, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!raw || !name || !name[0]) {
        return 0;
    }
    const size_t nlen = strlen(name);
    const char *p = raw;
    while (*p) {
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : (p + strlen(p));
        const char *s = p;
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
            s++;
        }
        const char *e = end;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
            e--;
        }
        if ((size_t)(e - s) > nlen && s[nlen] == '=' && strncmp(s, name, nlen) == 0) {
            size_t n = (size_t)(e - s);
            if (n > cap - 1) {
                n = cap - 1;           /* 装不下就截断：能认证上总比整条丢掉好 */
            }
            memcpy(out, s, n);
            out[n] = '\0';
            return n;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

/*
 * 用内置 cookie 填 s_net_cookie。返回是否成功（= 拿到了 MUSIC_U）。
 * 只取 MUSIC_U / __csrf：服务端 util/request.js 是 `if (!processedCookie.MUSIC_U)
 * 就按游客返回`，其余统计类 cookie（_ga / sensorsdata / 一堆 _ntes_*）它不需要。
 */
#if defined(ONLINE_NCM_CK_BUILTIN)
bool os_cookie_builtin_load(void)
{
    const char *raw = _binary_ncm_ck_builtin_start;
    if (!raw || !raw[0]) {
        return false;
    }
    static char music_u[1024];             /* 别放栈上：MUSIC_U 实测 ~700 字符 */
    if (os_cookie_pick(raw, "MUSIC_U", music_u, sizeof(music_u)) == 0) {
        ESP_LOGW(TAG, "内置 cookie 里没有 MUSIC_U：不启用（视为未登录）");
        return false;
    }
    char csrf[256];
    if (os_cookie_pick(raw, "__csrf", csrf, sizeof(csrf)) > 0) {
        snprintf(s_net_cookie, sizeof(s_net_cookie), "%s; %s", music_u, csrf);
    } else {
        snprintf(s_net_cookie, sizeof(s_net_cookie), "%s", music_u);
    }
    ESP_LOGI(TAG, "netease cookie loaded from built-in (%u bytes)",
             (unsigned)strlen(s_net_cookie));
    return true;
}

#else  /* !ONLINE_NCM_CK_BUILTIN */

/*
 * 编译期没嵌入内置 cookie（Kconfig 留空 / 文件不存在）：恒 false 的桩。
 * 调用点一行不改，行为与"没有这个功能"时完全一致，也不会有未使用函数的告警。
 */
bool os_cookie_builtin_load(void)
{
    return false;
}

#endif /* ONLINE_NCM_CK_BUILTIN */

void os_cookie_load(void)
{
    if (s_net_cookie_loaded) {
        return;
    }
    s_net_cookie_loaded = true;
    FILE *f = fopen(NET_COOKIE_FILE, "rb");
    if (!f) {
        /* 没登录过（或 SD 没挂）：还能退到编进固件的内置 cookie */
        s_net_logged_in = os_cookie_builtin_load();
        return;
    }
    size_t n = fread(s_net_cookie, 1, sizeof(s_net_cookie) - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {   /* 去掉可能被写进去的换行 */
        if (s_net_cookie[i] == '\n' || s_net_cookie[i] == '\r') {
            s_net_cookie[i] = '\0';
            n = i;
            break;
        }
    }
    s_net_cookie[n] = '\0';
    s_net_logged_in = (s_net_cookie[0] != '\0');
    /*
     * 旧版本用 1024 字节上限存 cookie，会把 MUSIC_U（排在很后面）**截断掉**，
     * 于是文件在、登录态却无效。这里做一次体检：有文件但没有 MUSIC_U 就当未登录，
     * 免得界面停在"已登录"却什么都拉不到（用户只会看到"扫码后没反应"）。
     */
    if (s_net_logged_in && !strstr(s_net_cookie, "MUSIC_U=")) {
        ESP_LOGW(TAG, "cookie 里没有 MUSIC_U（旧版截断残留）：视为未登录，请重新扫码");
        s_net_logged_in = false;
    }
    ESP_LOGI(TAG, "netease cookie %s (%u bytes)", s_net_logged_in ? "loaded" : "empty",
             (unsigned)n);
    if (!s_net_logged_in) {
        /* SD 上没有可用的扫码登录态 → 退到编进固件的内置 cookie */
        s_net_logged_in = os_cookie_builtin_load();
    }
}

/*
 * 压缩 cookie：把服务端那一大串收拾成 `NAME=VALUE; NAME=VALUE`。
 *
 * 规则：
 *   - 按 ';' 切段、两端 trim；
 *   - 只保留含 '=' 且名字**不是属性**的段（属性：Max-Age/Expires/Path/Domain/
 *     Secure/HttpOnly/SameSite/Version）；
 *   - **同名只留最后一个**（Set-Cookie 列表里后面的才是最新的；旧实现不敢去重，
 *     于是同一个名字带不同 Path 重复了七八次，白白撑爆 URL）；
 *   - 结果是可直接发送/存盘的形式，通常 200~400 字节。
 */
void os_cookie_compact(const char *raw, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw || !raw[0]) {
        return;
    }

    static const char *const attrs[] = {"max-age", "expires", "path", "domain",
                                        "secure", "httponly", "samesite", "version"};
    /*
     * ★ static 而不是栈上：24×160 = 3840 B。原先直接开在工作线程（栈只有 4 KB）上，
     *   叠加 net_cookie_save 里那 1 KB 缓冲就爆栈 —— 真机现象是点「扫码登录」后
     *   Guru Meditation (Stack protection fault)，崩在 snprintf 里。
     * 调用方只有工作线程（单线程），static 安全。
     */
    static char pairs[12][1024];      /* 去重用的暂存（白名单最多 9 个名字） */
    size_t n_pairs = 0;

    const char *p = raw;
    while (*p) {
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : (p + strlen(p));
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        const char *e = end;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) {
            e--;
        }
        const char *eq = (const char *)memchr(p, '=', (size_t)(e - p));
        if (eq && eq > p) {
            const size_t nlen = (size_t)(eq - p);
            bool is_attr = false;
            for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
                if (strlen(attrs[i]) == nlen && strncasecmp(p, attrs[i], nlen) == 0) {
                    is_attr = true;
                    break;
                }
            }
            char name[64];
            if (!is_attr && nlen < sizeof(name) && (size_t)(e - p) < sizeof(pairs[0])) {
                memcpy(name, p, nlen);
                name[nlen] = '\0';
                /* 同名覆盖：找到旧的替换掉，找不到就追加 */
                size_t slot = n_pairs;
                for (size_t i = 0; i < n_pairs; i++) {
                    const char *q = strchr(pairs[i], '=');
                    if (q && (size_t)(q - pairs[i]) == nlen && strncmp(pairs[i], name, nlen) == 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot == n_pairs && n_pairs < sizeof(pairs) / sizeof(pairs[0])) {
                    n_pairs++;
                }
                if (slot < sizeof(pairs) / sizeof(pairs[0])) {
                    snprintf(pairs[slot], sizeof(pairs[slot]), "%.*s", (int)(e - p), p);
                }
            }
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }

    size_t used = 0;
    /*
     * 输出顺序：**先把 MUSIC_U / __csrf 放最前**，再放其余。
     * 万一后面还要按缓冲截断，至少认证 cookie 一定在 —— 这是"宁缺其他、不丢认证"。
     */
    static const char *const prio[] = {"MUSIC_U=", "__csrf="};
    for (int pass = 0; pass < 3 && used + 2 < cap; pass++) {
        for (size_t i = 0; i < n_pairs && used + 2 < cap; i++) {
            bool want;
            if (pass == 0) {
                want = (strncmp(pairs[i], prio[0], strlen(prio[0])) == 0);
            } else if (pass == 1) {
                want = (strncmp(pairs[i], prio[1], strlen(prio[1])) == 0);
            } else {
                want = !(strncmp(pairs[i], prio[0], strlen(prio[0])) == 0 ||
                         strncmp(pairs[i], prio[1], strlen(prio[1])) == 0);
            }
            if (!want) {
                continue;
            }
            const int w = snprintf(out + used, cap - used, "%s%s", used ? "; " : "", pairs[i]);
            if (w <= 0 || (size_t)w >= cap - used) {
                break;                    /* 装不下了：宁可少几个非关键 cookie */
            }
            used += (size_t)w;
        }
    }
    if (used == 0) {
        /* 一个都没留下（服务端换了写法？）：退回原串，至少不比以前更差 */
        snprintf(out, cap, "%.*s", (int)(cap - 1), raw);
    }
}

void os_cookie_save(const char *cookie)
{
    if (!cookie || !cookie[0]) {
        return;
    }
    static char compact[NET_COOKIE_SEND];   /* 同上：别放在只有 4 KB 的工作线程栈上 */
    os_cookie_compact(cookie, compact, sizeof(compact));
    snprintf(s_net_cookie, sizeof(s_net_cookie), "%s", compact);
    s_net_logged_in = true;
    s_net_cookie_loaded = true;

    FILE *f = fopen(NET_COOKIE_FILE, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cookie save failed (SD 没挂？登录态只在内存里)");
        return;
    }
    fwrite(s_net_cookie, 1, strlen(s_net_cookie), f);
    fclose(f);
    /* 两个长度都打出来：raw 是服务端给的，compact 是我们真正存/发的大小 */
    ESP_LOGI(TAG, "netease cookie saved (raw %u -> compact %u bytes, MUSIC_U %s)",
             (unsigned)strlen(cookie), (unsigned)strlen(s_net_cookie),
             strstr(s_net_cookie, "MUSIC_U=") ? "ok" : "MISSING!");
}

bool online_netease_logged_in(void)
{
    os_cookie_load();
    return s_net_logged_in;
}

/*
 * 重读 cookie：只在"当前判定未登录"时才真去读。
 *
 * 使用场景（用户 2026-10 报"重启后进在线页，歌单什么的都没加载出来"）：
 *   开机后立刻进在线页时 SD 可能还没挂上，那次 fopen 失败被 net_cookie_load()
 *   当成了"没登录过"（它会 latch）——界面于是停在未登录态，之后 SD 挂好也不会再读。
 *   账号页每次重建都调一次本函数，挂载完成后自动恢复登录态。
 */
esp_err_t online_netease_cookie_reload(void)
{
    if (s_net_logged_in) {
        return ESP_OK;                     /* 内存里已经有了，不用再碰 SD */
    }
    s_net_cookie_loaded = false;           /* 允许 os_cookie_load() 再读一次 */
    s_net_cookie[0] = '\0';
    os_cookie_load();
    return s_net_logged_in ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------------- 昵称（NVS） */

void os_nick_load(void)
{
    if (s_net_nick_loaded) {
        return;                            /* 覆盖"没存过"的情况：别每次都开一次 NVS */
    }
    s_net_nick_loaded = true;
    nvs_handle_t h;
    s_net_nick[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_net_nick);
    if (nvs_get_str(h, NVS_KEY_NICK, s_net_nick, &len) != ESP_OK) {
        s_net_nick[0] = '\0';
    }
    nvs_close(h);
}

const char *online_netease_nickname(void)
{
    os_nick_load();                        /* 懒加载：init 之前被问到也能拿到 */
    return s_net_nick;
}

esp_err_t online_netease_nickname_set(const char *name)
{
    /* NVS 键的语义是"最后一次见到的昵称"，所以先更新内存里的那份 */
    if (!name) {
        name = "";
    }
    if (s_net_nick_loaded && strcmp(s_net_nick, name) == 0) {
        return ESP_OK;                     /* 没变：别擦写 flash */
    }
    snprintf(s_net_nick, sizeof(s_net_nick), "%s", name);
    s_net_nick_loaded = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err;
    if (s_net_nick[0]) {
        err = nvs_set_str(h, NVS_KEY_NICK, s_net_nick);
    } else {
        err = nvs_erase_key(h, NVS_KEY_NICK);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;                  /* 本来就没有：也算成功 */
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "netease nickname %s: %s", s_net_nick[0] ? "saved" : "cleared",
             s_net_nick[0] ? s_net_nick : "-");
    return err;
}

const char *online_netease_cookie(void)
{
    os_cookie_load();
    return s_net_cookie;
}

esp_err_t online_netease_logout(void)
{
    s_net_cookie[0] = '\0';
    s_net_logged_in = false;
    s_net_cookie_loaded = true;
    remove(NET_COOKIE_FILE);
    online_netease_nickname_set(NULL);  /* 昵称跟着一起清，别让下个账号看到上个的 */
    online_cache_clear();              /* 顺手清 PSRAM 缓存，账号歌单下次重新拉 */
    ESP_LOGI(TAG, "netease logged out");
    return ESP_OK;
}

esp_err_t online_netease_qr_key(char *out_key, size_t cap)
{
    if (!out_key || cap < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    out_key[0] = '\0';
    char *js = NULL;
    /*
     * ★ 请求参数必须和官方网页端 public/qrlogin.html 完全一致，否则会"永远 801"：
     *   /login/qr/key?timestamp=<ms>
     *   /login/qr/create?key=<k>&platform=web&qrimg=true&timestamp=<ms>&ua=ESP32-S31-Korvo-1
     *   /login/qr/check?key=<k>&timestamp=<ms>&ua=ESP32-S31-Korvo-1     ← 少了 timestamp 就会拿到
     *                                                        **缓存的 801**，手机确认了也不变
     */
    char path[64];
    snprintf(path, sizeof(path), "/login/qr/key?timestamp=%lld&ua=ESP32-S31-Korvo-1",
             (long long)(esp_timer_get_time() / 1000));
    esp_err_t err = os_net_api_get(path, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *uk = os_json_find_str(root, "unikey");
    if (!uk || !uk[0]) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "qr/key: 响应里没有 unikey");
        return ESP_ERR_INVALID_RESPONSE;
    }
    snprintf(out_key, cap, "%.*s", (int)(cap - 1), uk);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "qr key: %s", out_key);
    return ESP_OK;
}

esp_err_t online_netease_qr_create(const char *key, char *out_url, size_t url_cap,
                                   uint8_t **qrimg, size_t *qrimg_len)
{
    if (!key || !key[0] || !qrimg || !qrimg_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *qrimg = NULL;
    *qrimg_len = 0;
    if (out_url && url_cap) {
        out_url[0] = '\0';
    }

    char path[256];
    int n = snprintf(path, sizeof(path),
                     "/login/qr/create?key=%s&platform=web&qrimg=true&timestamp=%lld&ua=ESP32-S31-Korvo-1",
                     key, (long long)(esp_timer_get_time() / 1000));
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *js = NULL;
    esp_err_t err = os_net_api_get(path, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *url = os_json_find_str(root, "qrurl");
    if (url && out_url && url_cap) {
        snprintf(out_url, url_cap, "%.*s", (int)(url_cap - 1), url);
    }

    const char *img = os_json_find_str(root, "qrimg");
    if (!img || !img[0]) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "qr/create: 没有 qrimg（服务端版本或 qrimg 参数没生效）");
        return ESP_ERR_NOT_FOUND;
    }

    /* "data:image/png;base64,XXXX" → 跳过逗号，把 base64 解成 PNG 原始字节 */
    const char *b64 = strchr(img, ',');
    b64 = b64 ? (b64 + 1) : img;
    const size_t b64_len = strlen(b64);
    const size_t dec_max = (b64_len / 4 + 1) * 3;
    uint8_t *buf = (uint8_t *)os_psram_alloc(dec_max);
    if (!buf) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t dec_len = 0;
    const int rc = mbedtls_base64_decode(buf, dec_max, &dec_len,
                                         (const unsigned char *)b64, b64_len);
    cJSON_Delete(root);
    if (rc != 0) {
        free(buf);
        ESP_LOGW(TAG, "qr image base64 解码失败 (-0x%04x)", (unsigned)-rc);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *qrimg = buf;
    *qrimg_len = dec_len;
    ESP_LOGI(TAG, "qr image: %u bytes (%s)", (unsigned)dec_len,
             (dec_len > 8 && buf[1] == 'P') ? "PNG" : "非 PNG?");
    return ESP_OK;
}

esp_err_t online_netease_qr_poll(const char *key, int *out_code)
{
    if (!key || !key[0] || !out_code) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * ★★ 这里**必须**带 timestamp：少了它，每次轮询请求完全相同，服务端会把第一次的
     *    801（等待扫码）**缓存着反复返回** —— 手机扫码并确认了，界面还是"等待扫码"，
     *    而且日志一切正常（这就是 2026-10 用户报的那个现象）。官方网页端也是这么发的：
     *    /login/qr/check?key=<k>&timestamp=<ms>&ua=ESP32-S31-Korvo-1
     */
    char path[192];
    int n = snprintf(path, sizeof(path), "/login/qr/check?key=%s&timestamp=%lld&ua=ESP32-S31-Korvo-1",
                     key, (long long)(esp_timer_get_time() / 1000));
    if (n <= 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *js = NULL;
    esp_err_t err = os_net_api_get(path, &js);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(js);
    if (!root) {
        free(js);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int code = 0;
    const bool got = os_json_find_int(root, "code", &code);
    if (got && code == NET_QR_OK) {
        const char *ck = os_json_find_str(root, "cookie");
        if (ck && ck[0]) {
            os_cookie_save(ck);
        } else {
            ESP_LOGW(TAG, "qr check: 803 但响应里没有 cookie");
        }
    }
    cJSON_Delete(root);
    free(js);
    if (!got) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_code = code;
    ESP_LOGI(TAG, "qr check -> %d", code);
    return ESP_OK;
}
