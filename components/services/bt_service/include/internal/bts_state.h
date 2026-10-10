/*
 * bts_state.h —— bt_service 内部：跨文件共享的状态 + 查询访问器
 *
 * 原来是 bt_service.cc 里的一堆 static 全局，现在集中到 bts_state.cc 定义、
 * 由本头文件 extern 出去。**只改名（s_xxx → bts_xxx）+ 搬家**：
 * 类型、volatile 限定、初值都与拆分前一致，访问它的语句一字未动。
 *
 * 并发模型（未变）：这些状态由蓝牙栈回调任务（BTC）写，被 LVGL 任务 / feed 任务 /
 * esp_timer 任务读；volatile 只覆盖其中一部分 —— 拆分没有改变任何一处共享语义。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bt_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 对外状态（BT 回调任务写、LVGL 任务读） ---- */
extern volatile bt_state_t bts_state;
extern volatile bool       bts_inited;
extern volatile bool       bts_discovering;
extern volatile bool       bts_connected;
extern volatile bool       bts_streaming;

/* ---- 发现列表与配对列表（写：BT 回调任务 / LVGL 任务；读：LVGL 任务） ---- */
extern bt_dev_t        bts_dev[BT_DEV_MAX];
extern volatile size_t bts_dev_count;
extern bt_dev_t        bts_bonded[BT_DEV_MAX];
extern volatile size_t bts_bonded_count;

/* ---- 对端名字/地址（对外的 bt_service_peer_* 返回的就是这里的内部指针） ---- */
extern char    bts_peer_name[BT_NAME_MAX];
extern char    bts_peer_addr[18];
extern uint8_t bts_peer_bda[6];

/* ---- 协商到的码流参数 + 累计丢弃字节 ---- */
extern volatile int      bts_rate;
extern volatile int      bts_channels;
extern volatile uint32_t bts_dropped;

/* 6 字节 BDA 比较 */
bool bts_same_bda(const uint8_t *a, const uint8_t *b);

/* 在发现列表里按 BDA 找一条（create = 没有就新建；表满/不建时返回 NULL） */
bt_dev_t *bts_dev_find(const uint8_t *bda, bool create);

#ifdef __cplusplus
}
#endif
