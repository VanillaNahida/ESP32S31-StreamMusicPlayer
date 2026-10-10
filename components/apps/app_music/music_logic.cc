/*
 * music_logic.cc —— 纯逻辑实现（无 LVGL、无 ESP 头，可 host 单测）
 *
 * 这里每个函数都对应拆分前 app_music.cc 里的一段判断/格式化，注释跟着一起搬过来了
 * （它们记录了踩过的坑）。**改阈值 = 改行为**，别顺手调。
 */
#include "music_logic.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void music_fmt_time(uint32_t ms, char *buf, size_t sz)
{
    if (ms == 0 || ms > 24u * 3600u * 1000u) {
        snprintf(buf, sz, "--:--");
        return;
    }
    uint32_t total_s = ms / 1000;
    snprintf(buf, sz, "%u:%02u", (unsigned)(total_s / 60), (unsigned)(total_s % 60));
}

const char *music_play_mode_name(music_play_mode_t mode)
{
    switch (mode) {
    case MUSIC_MODE_ONE:     return "单曲循环";
    case MUSIC_MODE_SHUFFLE: return "随机循环";
    default:                 return "列表循环";
    }
}

int32_t music_progress_permille(uint32_t elapsed_ms, uint32_t dur_ms)
{
    if (dur_ms == 0) {
        return 0;
    }
    return (int32_t)((uint64_t)elapsed_ms * 1000ULL / dur_ms);
}

uint32_t music_seek_target_ms(int32_t permille, uint32_t dur_ms)
{
    return (uint32_t)((uint64_t)permille * dur_ms / 1000ULL);
}

void music_buffer_hint_text(bool has_speed, int kbps, int pct, char *buf, size_t sz)
{
    if (has_speed && kbps > 0) {
        if (kbps >= 80) {                           /* ≥10 KB/s：整数就够 */
            snprintf(buf, sz, "缓冲中 %d KB/s", kbps / 8);
        } else {
            const int t = kbps * 10 / 8;            /* 0.1 KB/s 精度 */
            snprintf(buf, sz, "缓冲中 %d.%d KB/s", t / 10, t % 10);
        }
    } else {
        snprintf(buf, sz, "缓冲中");
    }
    if (pct >= 0) {
        size_t used = strlen(buf);
        snprintf(buf + used, sz - used, " · %d%%", pct);
    }
}

bool music_track_seek_unsupported(const char *path)
{
    if (!path) {
        return false;
    }
    /* 在线流地址带签名查询串（...mp3?vuutv=...），认扩展名时先把它排除掉；
     * 再看**文件名部分**的最后一个点，避免把目录里的点当成扩展名 */
    const char *q    = strchr(path, '?');
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = NULL;
    for (const char *p = base; *p && (!q || p < q); ++p) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot) {
        return false;
    }
    return strncasecmp(dot, ".m4a", 4) == 0 || strncasecmp(dot, ".mp4", 4) == 0 ||
           strncasecmp(dot, ".ogg", 4) == 0 || strncasecmp(dot, ".oga", 4) == 0 ||
           strncasecmp(dot, ".opus", 5) == 0;
}

int music_next_index(int cur, int dir, size_t n)
{
    return (cur + dir + (int)n) % (int)n;
}

int music_online_index(int cur, int dir, size_t n)
{
    /*
     * 在线**只做顺序**（dir=+1 下一首 / -1 上一首 / 0 重放当前这首）。
     * 随机循环在在线队列上是被禁掉的（见 on_mode_click 与
     * app_music_play_online 的兜底），所以这里不再有随机分支 ——
     * 留着反而容易让人以为"在线也支持随机"。
     */
    return (cur + dir + (int)n) % (int)n;
}

music_watchdog_action_t music_watchdog_decide(bool is_playing, bool output_active,
                                              bool seek_in_flight, bool on_mode,
                                              int stall_ticks, int stall_kicks)
{
    /*
     * ★ 在线流式播放的**卡死看门狗**（2026-10 用户报"后台播放在线音乐概率会卡住"）。
     *
     * 现象：网络侧一个读操作卡住时，HTTP IO 会一直等（既不出错也不返回），
     * 会话状态仍然是 RUNNING，但**再也没有 PCM 写出来** —— 界面上进度/歌词定住、
     * 也没人重试，用户看到的就是"卡住不动"。
     *
     * 判据：**"声称在播"但"连续 N 秒没有 PCM 输出"**。
     *   - 在线与**本地**都盯（2026-10 补：本地也会卡 —— 见下面 !on_mode 分支的说明）；
     *   - 缓冲突发（起播/切歌那几秒）本来就 output_active=false，所以阈值给 20 s；
     *   - 阈值分两种：在线流重新缓冲可能真的要十几秒（所以 20 s），
     *     而**本地文件**第一条 PCM 最多等 2~3 秒（SD 读 + 解第一帧）——
     *     8 s 没声音就是文件解不出来，早点跳过少让用户干等。
     */
    if (!is_playing || output_active || seek_in_flight) {
        return MUSIC_WD_WAIT;
    }
    const int stall_limit = on_mode ? MUSIC_STALL_TICKS : MUSIC_LOCAL_STALL_TICKS;
    if (stall_ticks < stall_limit) {
        return MUSIC_WD_WAIT;
    }
    if (!on_mode) {
        return MUSIC_WD_SKIP_LOCAL;
    }
    return (stall_kicks < MUSIC_STALL_KICK_MAX) ? MUSIC_WD_KICK_STREAM : MUSIC_WD_ESCALATE;
}

music_next_action_t music_next_action(bool repeat_one, bool on_queue, size_t count)
{
    if (repeat_one) {
        return MUSIC_NEXT_REPLAY;
    }
    if (count > 1) {
        return MUSIC_NEXT_SKIP;
    }
    /* 只有一首（或一首都没有）时**什么都不做** —— 原实现这里落进
     * `else if (s_fail_streak >= 3)` 那个分支，只清连续失败计数。
     * 本地 count==1 时**不能**重放（那就是单曲循环，而模式不是 ONE）。 */
    (void)on_queue;
    return MUSIC_NEXT_NONE;
}
