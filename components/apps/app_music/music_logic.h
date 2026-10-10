/*
 * music_logic.h —— 音乐 APP 的**纯逻辑**（拆分重构新增）
 *
 * 这里只放"给一组数字/字符串、返回一个决定或一段文本"的函数：
 *   - **不 include lvgl.h**、不 include 任何服务头、不碰全局状态（除了只读的
 *     music_state_t 快照），所以可以脱离 ESP-IDF 在 host 侧单测；
 *   - 原来这些判断散落在 app_music.cc 的 tick_cb / play_next / on_seek_* 里，
 *     与 LVGL 绘制、服务调用混在一起 —— 抽出来是为了让"阈值与判据"能被单独验证
 *     （在线 20 s / 本地 8 s、提前结束的 >30 s 且差 15 s、续播到期保护 5 s …）。
 *
 * ⚠ 这里的每个阈值都与拆分前的 app_music.cc 一字不差地对应，
 *   改任何一个都等于改行为，必须先在真机验证。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "music_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 常量 */

#define MUSIC_TICK_MS              500    /* 进度/歌词刷新周期 */
#define MUSIC_PROGRESS_SAVE_TICKS  10     /* 每 10 个 tick（5 秒）把播放进度记一次 */
/* 续播最多等这么多个 tick（12 × 500 ms = 6 秒）还拿不到码率就放弃定位 */
#define MUSIC_RESUME_WAIT_MAX_TICKS 12
/* 恢复位置落在"离结尾不到这么久"的范围内就从头放（防越界落到文件尾） */
#define MUSIC_RESUME_END_GUARD_MS  5000
/* seek 之后最多等这么久没 PCM 就交给看门狗兜底（别把标志一直挂着） */
#define MUSIC_SEEK_STALL_MAX_TICKS (20000 / MUSIC_TICK_MS)
/* 在线流式"卡死看门狗"阈值 */
#define MUSIC_STALL_TICKS          (20000 / MUSIC_TICK_MS)  /* 在线：连续 20 s 没有 PCM = 卡住 */
#define MUSIC_LOCAL_STALL_TICKS    (8000 / MUSIC_TICK_MS)   /* 本地：8 s 足够（SD 读 + 解第一帧 2~3 s） */
#define MUSIC_STALL_KICK_MAX       3    /* 在线每首最多踢 3 次 */
#define MUSIC_ONLINE_URL_RETRY_MAX 2    /* 每首最多重刷这么多次播放地址 */
#define MUSIC_EARLY_EOF_RETRY_MAX  3    /* 每首最多续播这么多次（提前结束） */
#define MUSIC_BUF_TEXT_MAX         96   /* 缓冲提示文本缓冲（含 " · 100%" 后缀） */

/* ------------------------------------------------------------------ 时间与文本 */

/* ms → "M:SS"；0 或超过 24 h → "--:--" */
void music_fmt_time(uint32_t ms, char *buf, size_t sz);

/* 枚举 → "列表循环/单曲循环/随机循环" */
const char *music_play_mode_name(music_play_mode_t mode);

/* 总时长未知（0）时返回 0，见 music_state_progress_permille */
int32_t music_progress_permille(uint32_t elapsed_ms, uint32_t dur_ms);

/* 千分比 → 毫秒（拖动进度条松手时用；用 64 位中间量防溢出） */
uint32_t music_seek_target_ms(int32_t permille, uint32_t dur_ms);

/*
 * 缓冲提示文字（需求："缓冲中 20 KB/s" + 一个转圈，别让用户以为卡死）。
 *
 * 速度来自 GMF 的下载统计（audio_service_stream_speed()，单位 **Kbps**），
 * 这里 **÷8 换成 KB/s** —— 单位千万别混：320 kbps 的流按 Kbps 印成 "320 KB/s" 会差 8 倍。
 * 10 KB/s 以下给一位小数（慢速网络下 0 与 1 KB/s 的区别看得出来）。
 * pct < 0 表示百分比拿不到（分块传输 / 未知长度），那就只显示速度。
 * has_speed=false 或 kbps<=0 时只显示"缓冲中"（**不显示 0 KB/s**）。
 */
void music_buffer_hint_text(bool has_speed, int kbps, int pct, char *buf, size_t sz);

/* ------------------------------------------------------------------ 容器 / 队列 */

/*
 * 按字节 seek 之后"解码器能不能自己重新对上帧"决定了这个容器能不能拖。
 *
 *   MP3 / AAC / TS / FLAC —— 帧头自带同步码，从中间接上就行
 *       （FLAC 的 STREAMINFO 解码器在打开时已经拿到，中途 seek 不需要重读）；
 *   MP4 / M4A —— 必须有 moov 里的 sample table 才能解释字节流，按字节跳过去就是杂音；
 *   OGG / Opus —— 每个逻辑流开头有 codec setup 头，中途接上解不出来。
 *
 * ⚠ 名单以外一律"允许拖"：错了顶多听起来对不上，而挡住一个本来能拖的格式
 *   会直接废掉功能。要改名单先在真机上试一遍。
 *
 * ⚠ 这道判断现在是**第二道关**：audio_service_seek_supported() 才是权威判据
 *   （本地文件恒真；在线只在容器是 MP3/ADTS-AAC 且已知总长+时长时为真）。
 *   名单保留是因为它同时被 online_prepare_task 复用 —— 换解码库后
 *   **不能边下边解的容器**（m4a/mp4/ogg/opus）要改走"先下载到 SD 再当本地文件播"，
 *   判据与这里同一张名单、原因也是同一个（"需要可 seek 的源"）。
 */
bool music_track_seek_unsupported(const char *path);

/* 本地队列换曲：返回目标下标。dir=+1/-1 顺序走；随机模式由调用方先掷骰子传入 rand_idx。 */
int music_next_index(int cur, int dir, size_t n);

/* 在线队列换曲：只有顺序/重放（在线不支持随机），返回目标下标。 */
int music_online_index(int cur, int dir, size_t n);

/* 当前是否在"在线队列"上（决定看门狗阈值与是否记进度） */
static inline bool music_on_queue(const music_state_t *st)
{
    return st->on_mode && st->on_songs && st->on_count > 0;
}

/* ------------------------------------------------------------------ 阈值判据 */

/*
 * ★ "提前结束" ≠ 放完（2026-10 用户报：连接被 reset 后卡住/没声音）。
 *
 * 判据：知道总时长（> 30 s 才算"一首歌"），而当前进度明显短于它（还差 > 15 s）。
 * 这时 done 到得早是因为 TCP reset 被当成整体结束，应该重刷地址 + 从断点续播，
 * 而不是跳下一首。
 */
static inline bool music_is_early_eof(uint32_t elapsed_ms, uint32_t dur_ms)
{
    return dur_ms > 30000 && (elapsed_ms + 15000) < dur_ms;
}

/* 连续多少首都放不了就停下并提示 */
static inline bool music_fail_streak_give_up(int fail_streak)
{
    return fail_streak >= 3;
}

/* ------------------------------------------------------------------ 决策枚举 */

typedef enum {
    MUSIC_WD_WAIT = 0,     /* 还没到阈值（或正在 seek、或没在播）：什么都不做 */
    MUSIC_WD_SKIP_LOCAL,   /* 本地文件：这么久没 PCM = 解不出来，跳过它 */
    MUSIC_WD_KICK_STREAM,  /* 在线：踢一下管线（pause/resume） */
    MUSIC_WD_ESCALATE,     /* 在线：踢够次数了，升级为"重刷地址 + 断点续播" */
} music_watchdog_action_t;

/*
 * 卡死看门狗决策（阈值：在线 20 s / 本地 8 s；seek 进行中有豁免）。
 *
 * 调用方把"服务侧的事实"取好传进来（is_playing / output_active / on_mode），
 * 这里只做判据 —— 原来这段在 tick_cb 里与 pause/resume/toast 混在一起。
 */
music_watchdog_action_t music_watchdog_decide(bool is_playing, bool output_active,
                                              bool seek_in_flight, bool on_mode,
                                              int stall_ticks, int stall_kicks);

typedef enum {
    MUSIC_NEXT_NONE = 0,   /* 没有别的曲目、也不是单曲循环：什么都不做 */
    MUSIC_NEXT_REPLAY,     /* 单曲循环：同一首重来 */
    MUSIC_NEXT_SKIP,       /* 顺序/列表：换下一首 */
} music_next_action_t;

/*
 * "播完了 → 接下来放哪首"的决策。
 *
 * ⚠ 这一块必须**在线/本地分开**（2026-10 用户报"在线单曲循环失效"）：
 *   以前只有"本地"那一套逻辑，于是
 *     - 在线 + 单曲循环会去调**本地**的 start_track()：那首根本不在本地库里，
 *       要么报"曲目不存在"、要么拿同下标的**本地曲目**顶上 —— 就是"单曲循环失效"；
 *     - 整块还被 `storage_service_count() > 0` 挡着：没插卡 / 本地为空时，
 *       在线歌放完**停住不续播**。
 *   现在：在线队列走 play_next()（它认识在线队列，会重新解析地址 → 起播）。
 *
 * @param repeat_one 播放模式是单曲循环且 index >= 0
 * @param on_queue   当前是在线队列
 * @param count      当前队列长度（在线 = on_count，本地 = storage_service_count()）
 *
 * ⚠ 本地/在线的**重放语义不同**（在线要重新解析地址，所以走 play_next(0)），
 *   但**判据完全一样**（count > 1 才换下一首），所以这个函数不需要区分来源 ——
 *   调用方按 on_queue 决定"Replay"具体调哪一个即可。
 */
music_next_action_t music_next_action(bool repeat_one, bool on_queue, size_t count);

#ifdef __cplusplus
}
#endif
