/*
 * music_player_bg.h —— 播放器页背景图（assets/player_background.jpg）
 *
 * 与首页背景（ui_core/ui_home_bg.cc）同一套做法：JPEG 编译期嵌进固件，
 * 后台任务软解成 RGB565（PSRAM），再当 screen 的 bg_image 贴上。
 *
 * 为什么要单独一个文件：播放器页的 screen 是**每次进页面新建**的（页面销毁时
 * 整个 screen 跟着没了），所以图要在页面建好后再贴一次 —— 见 attach()。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起一次后台解码任务（幂等）。开机时调，进播放器页就是现成的，不用等几百毫秒 */
void music_player_bg_start(void);

/* 把（已经解好的）背景图贴到这个 screen 上；图还没好就只记下来，
 * 解好那一刻解码任务会自己补贴（LVGL 操作都在锁内做）。 */
void music_player_bg_attach(lv_obj_t *scr);

#ifdef __cplusplus
}
#endif
