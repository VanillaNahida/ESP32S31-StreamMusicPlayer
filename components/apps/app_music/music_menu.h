/*
 * music_menu.h —— 播放器页的"设置"弹窗
 *
 * 需求（2026-11）：控制排左边的那个按钮（原来是"歌曲信息"ⓘ）点开改成**设置弹窗**，
 * 里面至少两项：
 *   1. 音频可视化 FFT  开 / 关（lv_switch，落 NVS）
 *   2. 歌曲信息        ›（跳转到原来的 music_info 弹窗）
 *
 * 只做"开关 + 入口"，所有实际动作都委托出去（music_spectrum / music_info），
 * 所以这个文件没有业务状态，只有弹窗句柄。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开（已经开着就直接返回，不重复弹） */
void music_menu_open(void);
/* 页面销毁时收尾：浮层挂在 lv_layer_top() 上，不会随页面自动消失，必须显式关 */
void music_menu_on_page_delete(void);

#ifdef __cplusplus
}
#endif
