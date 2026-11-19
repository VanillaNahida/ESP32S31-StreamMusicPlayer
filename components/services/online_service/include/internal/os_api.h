/*
 * os_api.h —— 自建网易云 API 服务：带 cookie 的 GET（+ base 常量的再导出）
 *
 * 原文件里的名字：net_api_get_ex / net_api_get（2571–2625）。
 * base 地址的常量与 online_netease_api_base(_set) 归 os_account.h / os_account.cc
 * （它们本来就是"账号体系"那一节的常量），这里 include 进来再导出，方便调用方少 include。
 *
 * ★ 2026-11 服务端策略变更（本地跑同样的代码，"随机未登录"变成"必定 400"）：
 *   以前 cookie 是以 `?cookie=<urlencoded>` **查询参数**发出去的；新版
 *   @neteasecloudmusicapienhanced/api（v4.41.x）默认 `cookie.allowQuery=false`，
 *   查询串里的 cookie 会被**直接 400 拒掉**：
 *     {"code":400,"data":null,"msg":"Passing cookies in the query string is disabled;
 *      send them in the X-Netease-Cookie header or a POST body"}
 *   现在一律走请求头 **`X-Netease-Cookie: <压缩后的 cookie>`**（明文，不用再编码），
 *   由 os_net_api_get_ex() 传给 http_get_text_hdr()，只在请求自家 base 时加
 *   （见 os_http_client.cc 的 is_own_api()，绝不给 CDN/第三方）。上限 16 KiB。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "internal/os_account.h"   /* NET_BASE_* / NET_NVS_* */
#include "internal/os_state.h"     /* ONLINE_ROOT（cookie 落盘路径用） */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cookie 长度：服务端返回的是**把所有 Set-Cookie 拼起来**的整串（含 Max-Age/
 * Expires/Path 这些属性），实测 3875 字节 —— 里面大量是 WM_NIKE/_ga/acw_tc 之类的
 * 无关项，真正要的只有 MUSIC_U（值本身就有 ~700 字符！）/__csrf 几个。
 * 旧值 1024 会把它**截断**，而 MUSIC_U 恰好在后面 —— 结果就是"登录成功了但账号
 * 接口全是匿名"，日志特征：`cookie saved (1023 bytes)` 之后 /user/account 要登录。
 * 现在：原始串按 4 KB 收（3875 刚好放得下），存盘/发送用**压缩后**的形式。
 * ⚠ NET_COOKIE_SEND 必须 ≥ MUSIC_U 的长度 + 其它白名单项：
 *   曾用 1024 配 `pairs[24][160]`，MUSIC_U 值 ~700 字符 > 160 被整条丢掉，
 *   日志就是 `compact 101 bytes, MUSIC_U MISSING!`（2026-10 真机踩过）。
 */
#define NET_COOKIE_MAX    4096
#define NET_COOKIE_SEND   2048

/*
 * JSON 响应上限。
 * ★ 64 KB → 512 KB（2026-10 修"每日推荐加载失败"）：/recommend/songs 走的是
 *   `/api/v3/discovery/recommend/songs`，返回的**每首歌对象都巨大**（ar/al/privileges…），
 *   30 首就轻松几百 KB，64 KB 直接被判"响应过大"而失败（sink 是按需增长到 limit，
 *   所以平时不占内存）。
 */
#define NET_JSON_MAX      (512 * 1024)

/* cookie 落盘路径 */
#define NET_COOKIE_FILE   ONLINE_ROOT "/netease_cookie.txt"

/*
 * 拼 base + path 发 GET，返回 JSON 文本（调用方 free）。
 * with_cookie = true 时加两个请求头（账号类接口全部要它）：
 *   Authorization: Bearer <CONFIG_ONLINE_NETEASE_API_TOKEN>
 *   X-Netease-Cookie: <online_netease_cookie() 的压缩串>
 * 401/403 = key 不对或被拒；HTTP 400 且 msg 提到 cookie → 又变回查询串了。
 */
esp_err_t os_net_api_get_ex(const char *path, bool with_cookie, char **out_json);
esp_err_t os_net_api_get(const char *path, char **out_json);

#ifdef __cplusplus
}
#endif
