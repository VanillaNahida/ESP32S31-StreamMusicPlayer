/*
 * os_account.h —— 账号数据：账号信息 / 账号歌单分页 / 歌单曲目分页 / 每日推荐 / 直链
 *
 * 原文件里的名字：online_netease_account（2905–2944）、
 * online_netease_account_playlists（2946–3035）、playlist_detail_fallback（3041–3063）、
 * online_netease_playlist_page（3065–3130）、online_netease_daily_songs（3132–3166）、
 * online_netease_song_url（3168–3227），以及账号 API base 的常量
 * （NET_NVS_NS / NET_BASE_MAX / NET_BASE_DEFAULT，2036–2069）。
 *
 * ============================================================ 账号数据
 *
 * 全部带 cookie 调。cookie 失效时 /user/account 的 profile 为 null（实测），
 * 上层据此提示"重新登录"。
 *
 * ⚠ 两条硬约束（别"顺手简化"）：
 *   - /user/playlist 必须翻页（limit=100，最多 10 页，`more` 判续）；
 *   - /playlist/track/all 返回**完整 song detail 对象**（≈2.3 KB/首），
 *     一次要 300 首 ≈700 KB 会超 NET_JSON_MAX(512 KB) 整单失败，
 *     所以按 ONLINE_PL_PAGE_SONGS(100) 首一页循环。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- API base
 * ⚠ base 是**可配置**的（NVS 'online'/'nb'）：服务可能跑在 PC 上（局域网 IP:3000）
 *   或部署到公网，固件不硬编码。ESP32 上的 localhost 是**设备自己**。 */
#define NET_NVS_NS        "online"
#define NET_NVS_KEY_BASE  "nb"
#define NET_BASE_MAX      96

/* 默认地址来自 Kconfig（见 main/Kconfig.projbuild，本机填的是 http://localhost:3000）。
 * ⚠ ESP32 上的 localhost 是**设备自己**：服务跑在 PC 上时必须改成 PC 的局域网 IP，
 *   例：menuconfig → "Online music account" → 网易云 API 服务地址，或直接改 sdkconfig。
 * 运行期还能用 online_netease_api_base_set()（NVS）临时覆盖，不用重烧固件。 */
#ifdef CONFIG_ONLINE_NETEASE_API_BASE
#define NET_BASE_DEFAULT  CONFIG_ONLINE_NETEASE_API_BASE
#else
#define NET_BASE_DEFAULT  "http://localhost:3000"
#endif

#ifdef __cplusplus
}
#endif
