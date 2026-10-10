/*
 * music_online.h —— 在线曲目：队列所有权 / 异步准备 / 地址重刷 / 起播
 *
 * 搬迁自 app_music.cc 的 146–318（队列与准备状态）、1970–2374（准备线程与起播）。
 *
 * ★ 所有权规则（原样保留，别在别处 free 这两类指针）：
 *   - `s_on_songs`（现在是 music_state.on_songs）由 app_online 交过来；换队列时它调用
 *     app_music_online_detach()，本模块**接管**那块内存，等自己换队列或退回本地播放再 free；
 *   - 在线封面/歌词的内存缓冲（on_cover_data / on_lrc_data）由**工作线程**写入，
 *     只有 **LVGL 任务**释放（起播时消费掉，或换歌/退页时丢弃）。
 *
 * ★ 工作线程发布顺序（不可改）：先写数据 → 再写 ready 的字段 → **最后置 ready** →
 *   最后清 busy。反过来 tick 会读到半成品，或让两个线程同时写同一块缓冲。
 */
#pragma once

#include <stddef.h>

#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 丢掉"已准备好、还没起播"的在线结果（封面/歌词缓冲一并释放）。
 * 只在 LVGL 任务里调（缓冲由工作线程发布、LVGL 任务独占释放）。 */
void music_online_discard_ready(void);

/* 释放"托管中且已不是当前队列"的旧在线数组 */
void music_online_owned_drop(void);

/*
 * 起一个在线准备任务（LVGL 任务里调；解析 302 / 下载兜底 + HTTP+TLS 不能放在 LVGL 任务里）。
 * 内部只在"有请求（on_req >= 0）且没有线程在跑"时才会真的起飞。
 */
void music_online_prepare_kick(void);

/* 当前是否有一个准备任务在跑（决定要不要给用户弹"缓冲中，请稍后..."） */
bool music_online_busy(void);

/*
 * 在线切歌的"界面先行"：把播放器页切成目标歌曲（标题/歌手/封面占位/歌词清空/进度归零）。
 * **不碰播放器**：停旧歌由调用方负责。
 */
void music_online_switch_ui_to(int idx);

/*
 * 起播一首在线歌（LVGL 任务上下文）：uri 取 music_state.on_stream
 * （https 流地址 或 下载兜底缓存路径，audio_service_play_uri() 两种都认）。
 */
void music_online_start_track(int idx);

/*
 * 播放地址过期 → 重新解析一次地址（LVGL 任务上下文）。
 * 返回 true = 已经发起刷新（调用方**不要**跳下一首，等 ready 重放这首）。
 */
bool music_online_url_refresh(void);

/*
 * 在线音乐 app 的入口（实现对外 API app_music_play_online）。
 * **必须在 LVGL 任务上下文调用**。
 */
void music_online_play(const online_song_t *songs, size_t count, size_t idx);

/* 在线队列所有权交接（实现对外 API app_music_online_detach） */
void music_online_detach(const online_song_t *songs);

#ifdef __cplusplus
}
#endif
