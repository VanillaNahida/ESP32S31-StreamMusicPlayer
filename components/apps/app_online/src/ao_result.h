/*
 * ao_result.h —— 结果分发与 200 ms 轮询
 *
 * 这是"工作线程 → 界面"的唯一出口：
 *   ao_result_poll_cb（lv_timer，LVGL 任务上下文）
 *     ├─ 网络态变化 → 重建三页
 *     ├─ ao_work_take() → 按 kind 分派（含二维码状态机）
 *     └─ 登录态下昵称/歌单的退避补拉
 */
#pragma once

#include "lvgl.h"

/* 建 200 ms 轮询定时器（page_online() 末尾调） */
void ao_result_poll_start(void);
/* 删轮询定时器（online_on_delete() 开头调） */
void ao_result_poll_stop(void);
/* 定时器本体（由 ao_result_poll_start 挂上） */
void ao_result_poll_cb(lv_timer_t *t);
