/*
 * os_api.cc —— 自建 API 服务：带 cookie 的 GET（base 地址见 os_account.cc）
 *
 * 逐字搬自原 online_service.cc 的 net_api_get_ex / net_api_get（2571–2625），
 * 只加了 os_ 前缀；`s_last_err` 的读取换成 os_state_last_error()。
 * 依赖：os_http_client（http_get_text_hdr）+ os_login（cookie 懒加载/重读）。
 *
 * ★ 2026-11 服务端策略变更（参考工程 src/platform/http/netease-cookie.js）：
 *   cookie **不再接受查询串** `?cookie=…`（会被 400 拒绝），只认请求头
 *   `X-Netease-Cookie: NAME=VALUE; NAME=VALUE`。所以这里不再做百分号编码、
 *   也不再往 URL 后面拼参数，整串交给 http_get_text_hdr()（它只在自家 base 上加头）。
 *
 *   cookie 仍然只发"压缩后"的那串（MUSIC_U / __csrf 等白名单项，~1 KB，
 *   见 os_login.cc 的 os_cookie_compact），不把浏览器那 3.8 KB 全发出去。
 */
#include "internal/os_api.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "internal/os_http_client.h"
#include "internal/os_login.h"
#include "internal/os_state.h"
#include "online_service.h"

static const char *TAG = "online";

esp_err_t os_net_api_get_ex(const char *path, bool with_cookie, char **out_json)
{
    if (!path || !path[0] || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    /* static 而不是栈上：base(≤96) + path(≤200) 再加上别处拼的参数，工作线程栈不大 */
    static char url[NET_URL_MAX];
    const int n = snprintf(url, sizeof(url), "%s%s", online_netease_api_base(), path);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *ck = NULL;
    if (with_cookie) {
        /*
         * ★ 发账号类请求前再确认一次登录态：开机后 SD 晚挂 / cookie 文件刚写好时，
         *   第一次读到的是"空 cookie"，服务端会**按游客返回**（不是报错）——
         *   表现就是"重启后歌单/每日推荐都是空的或者别人的"。这里补一次重读。
         */
        online_netease_cookie_reload();
        ck = online_netease_cookie();
        if (!ck[0]) {
            ck = NULL;                 /* 未登录：不发这个头（服务端按游客处理） */
        }
    }

    size_t len = 0;
    const esp_err_t err = http_get_text_hdr(url, ck, out_json, &len, NET_JSON_MAX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "netease GET %s failed: %s", path, esp_err_to_name(err));
    }
    return err;
}

esp_err_t os_net_api_get(const char *path, char **out_json)
{
    return os_net_api_get_ex(path, false, out_json);
}
