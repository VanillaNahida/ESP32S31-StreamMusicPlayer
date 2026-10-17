/*
 * bts_conn.h —— bt_service 内部：A2DP 连接状态机（实现在 bts_conn.cc）
 *
 * 只有本组件内部用（别在别的组件 include）：对外那一层是 bt_service.h 的
 * `bt_service_connect()` / `bt_service_is_connecting()` / `bt_service_connect_target()`
 * / `bt_service_connect_error()`。
 *
 * 分工：
 *   bts_scan_pair.cc  bt_service_connect() → bts_conn_request()
 *   bts_events.cc     连接事件           → bts_conn_on_connected() / bts_conn_on_disconnected()
 *   bt_service.cc     deinit / 关开关    → bts_conn_cancel()
 *   （锁与定时器都是**首次使用时**懒创建，没有单独的 init 钩子）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 受理一次连接请求：内部会"停查询 / 断旧对端 / 延时 → 发起 / 超时 / 重试"。
 * 返回 ESP_OK             = 已受理（可能还在准备阶段，UI 立刻可以画"连接中"）
 * 返回 ESP_ERR_INVALID_STATE = 未就绪，或**正在连同一台**（UI 提示"正在连接，请稍后再试"） */
esp_err_t bts_conn_request(const uint8_t bda[6]);

/* 连接成功：清目标、停定时器（bts_events 在 connected=true 时调） */
void bts_conn_on_connected(const uint8_t bda[6]);

/* 断开事件：返回 true = 已由状态机处理（调用方**不要**再把 bts_state 改成 READY），
 * false = 普通掉线/主动断开，按原逻辑走（bts_events 在 connected=false 时调） */
bool bts_conn_on_disconnected(const uint8_t bda[6]);

/* 关开关 / deinit：作废在途的超时与重试，别让定时器去戳已经 deinit 的协议栈 */
void bts_conn_cancel(void);

/* 查询：是否在"连接中"（含准备/重试等待） */
bool bts_conn_is_busy(void);
/* 查询：正在连（或刚失败）的目标 BDA，没有则 NULL */
const uint8_t *bts_conn_target(void);
/* 查询：失败原因短句（"连接超时" / "连接失败"…），非失败态为空串 */
const char *bts_conn_error_str(void);

#ifdef __cplusplus
}
#endif
