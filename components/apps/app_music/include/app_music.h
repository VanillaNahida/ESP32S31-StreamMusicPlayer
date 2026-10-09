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
 * - songs 数组由调用方持有：**播放器接手（s_on_songs = songs）之后，这块内存就归
 *   本模块托管**（见 app_music_online_detach 与"队列所有权"的说明）；调用方换队列时
 *   先用 app_music_online_detach() 把旧数组交接过来，绝不要自己 free；
 * - 音频由后台任务先下载到 SD 再播放（见 online_service.h），所以这之后
 *   封面/歌词/进度/拖动定位都走本地那一套；
 * - **必须在 LVGL 任务上下文调用**（会更新播放器页控件）。
 */
void app_music_play_online(const online_song_t *songs, size_t count, size_t idx);

/*
 * 在线队列**所有权交接**（2026-10 修 use-after-free）：
 * 在线音乐 app 换掉当前歌单时，把"被替换下来、但播放器可能还在引用"的数组交给本模块。
 *
 * - 播放器正在用它 → 本模块接管，等到换队列/退回本地播放时再 free；
 * - 播放器没在用它 → 立刻 free。
 *
 * 为什么不能在 app_online 里直接 free：用户只要"点歌 → 连着打开两个别的歌单"，
 * 旧数组就会被放掉，而播放器还存着它的指针 —— 之后按上/下一首或自动续播就是
 * 读已释放内存（表现：曲名变成乱码/别的歌，甚至整机崩溃）。
 */
void app_music_online_detach(const online_song_t *songs);

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