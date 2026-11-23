/*
 * os_qq.h —— QQ 音乐的内部实现共用件：凭证（NVS + base64 头）+ 带凭证的请求
 *
 * 对外契约在 online_service.h 的「QQ 音乐」一节；这里只放"两个 .cc 都要看"的东西。
 *
 * ★ 凭证怎么放（安全性 / 可恢复性）：
 *   - 整份 JSON（服务端 /qq/login/qrcode/qq/status 里 data.credential）**原样**存 NVS
 *     的单条字符串（QQ_CRED_MAX 上限），开机即可用，不需要联网校验；
 *   - 请求时 base64 成 "X-QQMusic-Credential"（并带 "-Encoding: base64"，
 *     服务端 parser 只在这个头说 base64 时才解码，见 ncm-api 的 qq-credential.js）；
 *   - 刷新策略（docs/qq-provider.md §3.2）：**临期 10% 才刷**、串行、成功先落盘。
 *     每次开机都刷等于拿网络 + 风控赌一次，没必要。
 *
 * ★ 401 的处理：http_get_hops() 对非 200 一律丢 body，只留 "服务器返回 HTTP %d"
 *   这句话（见 os_http_client.cc）。所以这里从 last_error 里认 401/403，置一个
 *   "凭证已失效"标志交给调用方映射成 ESP_ERR_INVALID_STATE（UI 提示重新扫码），
 *   但**不擅自删除凭证** —— 一次网络抖动或服务端 502 不该让用户重新扫码。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 凭证 JSON 上限（实测 ~700 字节；留足后续字段） */
#define QQ_CRED_MAX        2048
/* base64(QQ_CRED_MAX) + 余量 */
#define QQ_HDR_MAX         3072
/* 单次响应上限：搜索结果 30 首 ≈ 84 KB、歌单一页 100 首 ≈ 100 KB */
#define QQ_JSON_MAX        (512 * 1024)
/* 二维码 PNG 的 base64 上限（实测 584 字符，PNG ~430 字节） */
#define QQ_QR_B64_MAX      8192

/* NVS（与网易云共用 'online' 命名空间，键名不同） */
#define QQ_NVS_NS          "online"
#define QQ_NVS_KEY_CRED    "qqcred"
#define QQ_NVS_KEY_NICK    "qqnick"

/* ---------------------------------------------------------------- 凭证 */

/* 懒加载 NVS 里的凭证。返回是否已登录（有 musicid + musickey）。 */
bool os_qq_cred_load(void);
bool os_qq_is_logged_in(void);
/* 原始凭证 JSON（未登录 = 空串） */
const char *os_qq_cred_json(void);
/* 已 base64 的请求头值（未登录 = 空串） */
const char *os_qq_hdr(void);
const char *os_qq_music_id(void);      /* 空串 = 未登录 */
const char *os_qq_encrypt_uin(void);   /* 空串 = 凭证里没有（收藏类接口会 401） */
const char *os_qq_nick(void);

/* 校验并保存一份凭证 JSON（落 NVS）。返回 ESP_OK / ESP_ERR_INVALID_RESPONSE */
esp_err_t os_qq_cred_store(const char *json);
/* 昵称（NVS 持久化；NULL/空串 = 清掉） */
esp_err_t os_qq_nick_store(const char *nick);
/* 清内存 + NVS（退出登录） */
void os_qq_cred_clear(void);
/* 业务请求 401 后置位；重新登录或换凭证时清掉 */
bool os_qq_cred_expired(void);
/* 临期则刷新（成功会落盘）。返回 ESP_OK / ESP_ERR_NOT_FOUND（不需要）/ 失败原因 */
esp_err_t os_qq_refresh_if_needed(void);

/* ---------------------------------------------------------------- 请求 */

/*
 * 带凭证的 GET / POST（path 以 /qq 开头，例如 "/qq/search/general_search?keyword=…"）。
 * 成功时 *out_json 是完整的响应壳 {"code":200,"msg":"ok","data":…}（调用方 free）。
 * 401/403 时返回 ESP_ERR_INVALID_RESPONSE 并置 os_qq_cred_expired()。
 */
esp_err_t os_qq_get(const char *path, bool with_cred, char **out_json);
esp_err_t os_qq_post(const char *path, bool with_cred, char **out_json);

/*
 * 把上面的错误码映射成对外的语义：凭证失效 → ESP_ERR_INVALID_STATE，
 * 其余原样返回（网络 / 服务端故障）。
 */
esp_err_t os_qq_map_err(esp_err_t err);

/* ---------------------------------------------------------------- JSON 纯逻辑 */

/* 响应壳里的 data 对象（没有就返回 NULL） */
const cJSON *os_qq_data(const cJSON *root);
/* QQ 的 Song 对象 → online_song_t（只填元数据；src=QQ） */
bool os_qq_song_from_json(const cJSON *song, online_song_t *out);
/* 数组批量转换；*out_n = 实际条数 */
esp_err_t os_qq_fill_songs(const cJSON *arr, online_song_t *out, size_t max, size_t *out_n);
/* 专辑对象 → 封面地址（T002R300x300，QQ 的 photo_new 规则） */
void os_qq_cover_from_album(const cJSON *album, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
