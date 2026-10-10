/*
 * os_json.h —— cJSON 取值与"网易云对象 → online_song_t"的纯逻辑映射
 *
 * 只依赖 cJSON + os_util（无 ESP-IDF / 无网络 / 无文件系统），可以 host 编译单测
 * （拆分蓝图 §6.3：内外层同名 code、ar[]/artists[] 双格式、10 位 ID 溢出）。
 *
 * 原文件里的名字：json_find_str/int（2088–2122）、json_id_str（2790–2801）、
 * pick_array（2804–2822）、song_from_json（2825–2885）、fill_songs（2887–2903）、
 * parse_songs（904–958）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 在 JSON 里找字符串字段：**先看最外层，再递归进去**。
 *
 * ★ 为什么强调"最外层优先"：实测服务端响应里内外层都可能有同名 code ——
 *   /login/qr/key → {"data":{"code":200,"unikey":"…"},"code":200}
 *   如果递归时先命中内层的 code，/login/qr/check 的扫码状态（800/801/802/803）
 *   就会被内层的 200 顶掉，状态机整条失效。所以第一遍只扫直接子节点。
 */
const char *os_json_find_str(const cJSON *root, const char *key);
bool        os_json_find_int(const cJSON *root, const char *key, int *out);

/* JSON 数字 ID → 字符串：歌/歌单 ID 是 10 位数（>2^31），
 * cJSON 的 valueint 是 32 位会溢出，必须走 valuedouble。 */
void os_json_id_str(const cJSON *item, char *out, size_t cap);

/* 找数组：primary_key 直接是数组，或藏在 data/playlist/result/body 里 */
const cJSON *os_pick_array(const cJSON *root, const char *primary_key);

/* 网易云歌曲对象 → online_song_t（只填元数据；audio_url 播放时按需取直链） */
bool os_song_from_json(const cJSON *song, online_song_t *out);

/* 数组批量转换，返回实际条数（0 → ESP_ERR_NOT_FOUND） */
esp_err_t os_fill_songs(const cJSON *arr, online_song_t *out, size_t max, size_t *out_n);

/* 解析 meting 返回的 JSON 数组 → 歌曲数组（*out 由本模块分配，调用方 free） */
esp_err_t os_parse_songs_json(const char *json, online_song_t **out, size_t *out_count);

#ifdef __cplusplus
}
#endif
