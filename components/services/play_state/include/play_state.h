/*
 * play_state.h —— 播放状态记忆（NVS）
 *
 * 记三样东西（需求："添加音量记忆，歌曲播放进度记忆功能。记住当前音量，
 * 当前播放的歌曲路径，歌曲进度"）：
 *   1. 当前音量（0~100）
 *   2. 当前播放的曲目 VFS 路径（如 /sdcard/Music/xxx.mp3）
 *   3. 该曲目的已播进度（毫秒）
 *
 * 为什么要单独一个服务：
 *   - 写 NVS 是**慢操作 + 有寿命**的（一次写几毫秒、擦写次数有限），
 *     而音量会被硬件 VOL± 按成"每秒 8 次"，进度每 500 ms 就刷新一次。
 *     所以这个模块把"状态"保存在 RAM 里立即生效，落盘交给内部任务**合并防抖**
 *     （停止改动 PLAY_STATE_DEBOUNCE_MS 之后才写一次），并提供
 *     play_state_commit() 给"暂停/停止/换曲"这种必须马上落盘的时机。
 *   - 统一收口后，音量只有 audio_service_set_volume() 一个写入点，
 *     曲目/进度只有 app_music 一个写入点，不会有第二份真相。
 *
 * 线程安全：所有 API 可在任意任务上下文调用（内部互斥锁 + 通知任务）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 路径缓冲：与 storage_service.h 的 STORAGE_PATH_MAX 一致（FATFS 长文件名上界） */
#define PLAY_STATE_PATH_MAX   512

/* 没有记忆时用的默认音量 */
#define PLAY_STATE_VOL_DEFAULT  55

/* 初始化（幂等；内部会按需 nvs_flash_init）。必须在 audio_service_init() 之前调，
 * 音量才能一开机就是记忆值。 */
esp_err_t play_state_init(void);

/* ---------------------------------------------------------------- 音量 */

/* 取记忆音量；没有记录时返回 fallback */
int  play_state_volume_get(int fallback);
/* 记音量（RAM 立即生效，落盘防抖） */
void play_state_volume_set(int percent);

/* ---------------------------------------------------------------- 曲目 / 进度 */

/*
 * 取上次记忆的曲目与进度。返回 ESP_OK 表示有记录（path 已写入、pos_ms 已填）；
 * 没有记录返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t play_state_track_get(char *path, size_t cap, uint32_t *pos_ms);

/* 记"当前曲目 + 已播进度"（RAM 立即生效，落盘防抖） */
void play_state_track_set(const char *path, uint32_t pos_ms);

/* 清掉曲目记忆（曲目被删/队列已空时用） */
void play_state_track_clear(void);

/* 立刻把待写状态落盘（暂停 / 停止 / 换曲 / 退出播放页等时机调） */
void play_state_commit(void);

#ifdef __cplusplus
}
#endif
