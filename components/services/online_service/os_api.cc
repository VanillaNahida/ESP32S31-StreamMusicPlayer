/*
 * os_api.cc —— 自建 API 服务：带 cookie 的 GET（base 地址见 os_account.cc）
 *
 * 逐字搬自原 online_service.cc 的 net_api_get_ex / net_api_get（2571–2625），
 * 只加了 os_ 前缀；`s_last_err` 的读取换成 os_state_last_error()。
 * 依赖：os_http_client（http_get_text）+ os_login（cookie 懒加载/重读）。
 */
#include "internal/os_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "internal/os_http_client.h"
#include "internal/os_login.h"
#include "internal/os_state.h"
#include "online_service.h"

static const char *TAG = "online";

/*
 * 拼 base + path 发 GET，返回 JSON 文本（调用方 free）。
 * with_cookie = true 时追加 `cookie=<urlencoded>`（账号类接口全部要它）。
 *
 * ⚠ 缓冲区按"base(≤96) + path(≤200) + 编码后的 cookie(~600)"取 1280：
 *   netease 的 cookie 里 MUSIC_U 很长且含 % + = 等字符，**必须 URL 编码**，
 *   否则服务端只拿到半截 cookie → 表现为"随机未登录"。
 */
esp_err_t os_net_api_get_ex(const char *path, bool with_cookie, char **out_json)
{
    /* static 而不是栈上：账号接口的 URL 可以到 2~3 KB，而工作线程栈不大 */
    static char url[NET_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s%s", online_netease_api_base(), path);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (with_cookie) {
        /*
         * ★ 发账号类请求前再确认一次登录态：开机后SD 晚挂 / cookie 文件刚写好时，
         *   第一次读到的是"空 cookie"，服务端会**按游客返回**（不是报错）——
         *   表现就是"重启后歌单/每日推荐都是空的或者别人的"。这里补一次重读。
         */
        online_netease_cookie_reload();
        const char *ck = online_netease_cookie();
        size_t used = strlen(url);
        if (ck[0] && used + 16 < sizeof(url)) {
            url[used++] = (strchr(path, '?') != NULL) ? '&' : '?';
            const char *pre = "cookie=";
            while (*pre && used + 1 < sizeof(url)) {
                url[used++] = *pre++;
            }
            for (const char *p = ck; *p && used + 4 < sizeof(url); p++) {
                const unsigned char c = (unsigned char)*p;
                const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                  (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
                                  c == '.' || c == '~';
                if (safe) {
                    url[used++] = (char)c;
                } else {
                    static const char hx[] = "0123456789ABCDEF";
                    url[used++] = '%';
                    url[used++] = hx[c >> 4];
                    url[used++] = hx[c & 0x0F];
                }
            }
            url[used] = '\0';
        }
    }

    *out_json = NULL;
    size_t len = 0;
    esp_err_t err = http_get_text(url, out_json, &len, NET_JSON_MAX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "netease GET %s failed: %s", path, esp_err_to_name(err));
    }
    return err;
}

esp_err_t os_net_api_get(const char *path, char **out_json)
{
    return os_net_api_get_ex(path, false, out_json);
}
