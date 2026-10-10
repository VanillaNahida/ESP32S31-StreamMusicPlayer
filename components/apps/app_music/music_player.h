/*
 * music_player.h —— 播放控制状态机（业务层）
 *
 * 搬迁自 app_music.cc 的 196–262（进度记忆）、496–698（起播/换曲/播放暂停）、
 * 1140–1315（播放事件与蓝牙媒体键）、1317–1790（tick 主干）、3527–3577（开机恢复）。
 *
 * ★ 这一层的纪律（拆开后更要守住）：
 *   - 不认识 LVGL：所有绘制都经 music_view_player.h 的接口；
 *   - 事件回调（audio_service 的播放事件）运行在播放器任务上下文，**只置标志**；
 *   - tick 是**开机常驻**的（页面不在也跑），控件指针的空操作由视图负责。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_service.h"   /* audio_evt_t：事件回调的签名 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 进度记忆/起播 */

/*
 * 进度记忆的唯一出口：**在线流式不记**（实现里判 on_mode && !on_stream_local）。
 */
void music_player_save_progress(uint32_t pos_ms);

/*
 * 起播一首**本地**曲目：resume_ms > 0 表示"从这里的进度继续"（进度记忆 / 开机续播）。
 * 换曲一律传 0，只有 on_play_pause 重新起播当前曲目、以及开机恢复时才带位置。
 */
void music_player_start_track_at(int idx, uint32_t resume_ms);
void music_player_start_track(int idx);

/* 换曲总入口（dir=+1/-1/0）：在线走"重新解析地址"链路，本地走随机/顺序 */
void music_player_play_next(int dir);

/* 播放/暂停键（含耳机上的 PLAY）。管线 STOPPED 时从记忆进度重起播。 */
void music_player_on_play_pause(void);

/* 停止播放（拔卡等）：状态与按钮图标一起收尾 */
void music_player_stop(void);

/* 蓝牙媒体键的消费点（100 ms 的常驻定时器）；任意任务上下文只经它置标志 */
void music_player_media_cmd_tick(void);
void music_player_on_media_ctrl(int cmd);

/* 500 ms 的常驻心跳：整个播放状态机的主干 */
void music_player_tick(void);

/* 播放服务事件（运行在播放器任务上下文）：只置标志，别碰 LVGL */
void music_player_on_audio_evt(audio_evt_t evt, void *user);

/*
 * 拖动定位受理（由播放器页松手时调用）。
 * 两道关都不给用户解释的话，用户只会以为界面卡了 —— 所以内部按需 ui_toast。
 * 返回 true = 已经受理（调用方接着更新进度条/时间/歌词）；false = 被挡下。
 */
bool music_player_seek_begin(uint32_t target_ms);

/* 播放器页销毁时调（落盘进度 / 丢弃没起播的在线结果） */
void music_player_on_view_destroyed(void);

/* 开机恢复上次的曲目与进度（只摆状态、不出声） */
void music_player_restore_last_session(void);

/* 打开播放器页（压栈）。**必须在 LVGL 任务上下文调用**。 */
void music_router_open_player(void);

#ifdef __cplusplus
}
#endif
