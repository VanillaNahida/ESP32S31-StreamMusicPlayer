/*
 * music_like.h —— 播放器页的红心（"我喜欢的音乐"）
 *
 * 对外只有两个入口：
 *   music_like_tick()   —— LVGL 任务，500 ms 心跳里调：消费工作线程的结果、弹 toast、
 *                          发现换了在线曲目就重新同步"是否已收藏"
 *   music_like_toggle() —— 点击回调：加/取消喜欢（未登录直接提示，不起线程）
 *
 * 线程纪律与 music_online 完全一致：**网络请求只在工作线程里**（LVGL 任务做 HTTP
 * 会把界面卡住几百毫秒，TLS 握手更长），工作线程只写 music_state 的 volatile
 * 字段，LVGL 任务消费。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL 任务：每个 tick 调一次（幂等） */
void music_like_tick(void);

/* 点击红心：加/取消喜欢（当前曲目不是在线曲目时什么也不做） */
void music_like_toggle(void);

#ifdef __cplusplus
}
#endif
