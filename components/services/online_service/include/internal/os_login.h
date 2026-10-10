/*
 * os_login.h —— 网易云登录态：cookie（内置 / SD） + 扫码登录 + 昵称
 *
 * 原文件里的名字：cookie_pick（2202–2239，纯逻辑）、net_cookie_builtin_load
 * （2246–2266 与 2274–2277 两份条件编译实现）、net_cookie_load（2281–2319，
 * 含 latch s_net_cookie_loaded）、cookie_compact（2332–2432，纯逻辑）、
 * net_cookie_save（2434–2456）、online_netease_logged_in/cookie/cookie_reload/
 * logout（2458–2559）、nvs_load_nick / online_netease_nickname(_set)（2485–2541）、
 * online_netease_qr_key/create/poll（2627–2780）。
 *
 * ⚠ 登录态的语义**不能改**（蓝图 R12/R14）：
 *   - `net_cookie_load()` 一旦下结论就 latch（SD 未挂时失败也会 latch），所以需要
 *     `online_netease_cookie_reload()` 在"当前未登录"时再读一次 SD；
 *   - 服务端"没有 MUSIC_U 就按游客返回、**不报错**"，判据是
 *     `online_netease_account()` 返回 ESP_ERR_INVALID_STATE。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 用内置 cookie 填内存 cookie（返回是否成功 = 拿到了 MUSIC_U）。
 * 只取 MUSIC_U / __csrf：服务端 util/request.js 是 `if (!processedCookie.MUSIC_U)
 * 就按游客返回`，其余统计类 cookie（_ga / sensorsdata / 一堆 _ntes_*）它不需要。
 */
bool os_cookie_builtin_load(void);

/* 懒加载 cookie（latch：**失败也会置位**，见头文件说明），init 里先调一次 */
void os_cookie_load(void);

/* 压缩 cookie：`NAME=VALUE; NAME=VALUE`（去属性、同名取最后、MUSIC_U/__csrf 优先）。
 * 纯逻辑，可 host 单测。 */
void os_cookie_compact(const char *raw, char *out, size_t cap);

/* 从原始 cookie 串里挑出 `NAME=VALUE`（按名字精确提取，与项数和顺序无关）。
 * 返回写入长度（0 = 没找到）。纯逻辑，可 host 单测。 */
size_t os_cookie_pick(const char *raw, const char *name, char *out, size_t cap);

/* 存 cookie：压缩后写内存 + 落 SD（netease_cookie.txt），并打 raw→compact 字节数 */
void os_cookie_save(const char *cookie);

/* 昵称（NVS 持久化，懒加载）：读 */
void os_nick_load(void);

#ifdef __cplusplus
}
#endif
