/*
 * app_music.h —— 音乐 APP（播放器页 + 本地音乐列表页）
 */
#pragma once

#include <stddef.h>
#include "lvgl.h"
#include "online_service.h"   /* online_song_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 注册/替换 "music" 与 "local" 两个 APP 描述符（需在 ui_core_init 之前调用） */
void app_music_register_apps(void);

/*
 * 把一份**在线歌单**交给播放器并从第 idx 首开始播（在线音乐 app 用）。
 *
 * - songs 数组由调用方持有：在把下一份数组交给本函数之前必须一直有效
 *   （app_music 只保存指针，用于上下曲与标题/歌手显示）；
 * - 音频由后台任务先下载到 SD 再播放（见 online_service.h），所以这之后
 *   封面/歌词/进度/拖动定位都走本地那一套；
 * - **必须在 LVGL 任务上下文调用**（会更新播放器页控件）。
 */
void app_music_play_online(const online_song_t *songs, size_t count, size_t idx);

/* 打开播放器页（压栈）。给别的 APP 用：在线音乐点歌之后就进播放器看封面/歌词。
 * **必须在 LVGL 任务上下文调用**。 */
void app_music_open_player(void);

/*
 * 蓝牙耳机上的媒体按键（AVRCP Target 收到后由 main 转进来）。
 * **可在任意任务上下文调用**：内部只置一个标志，真正的动作由 LVGL 任务里的
 * 定时器执行（LVGL 不是线程安全的，见 AGENTS.md 第 6 节）。
 * cmd 取值同 esp_bt_audio_media_ctrl_cmd_t：1=PLAY 2=PAUSE 3=STOP 4=NEXT 5=PREV
 */
void app_music_on_media_ctrl(int cmd);

#ifdef __cplusplus
}
#endif