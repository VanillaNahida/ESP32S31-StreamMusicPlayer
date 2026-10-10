/*
 * music_state.cc —— 共享状态的实现 + 几处**粗粒度状态迁移**
 *
 * 为什么不给每个字段配一对 getter/setter：那等于把全局变量换个写法，仍然约束不了时序。
 * 这里只提供"起播/切回本地/定位受理/看门狗复位"这几个真正成套的动作，
 * 以及进度记忆的唯一出口 save_progress()。
 */
#include "music_state.h"

#include <stdio.h>
#include <string.h>

/* 固件唯一实例（声明在 music_state.h）。
 * ⚠ C++ 的指定初始化必须按**声明顺序**写，别图省事乱序（-Werror 直接报错）。 */
music_state_t g_music_state = {
    .index          = -1,
    .play_mode      = MUSIC_MODE_LIST,
    .on_req         = -1,
    .on_ready_idx   = -1,
    .cover_want     = -1,
    .cover_out_idx  = -1,
};

music_state_t *music_state(void)
{
    return &g_music_state;
}

void music_state_begin_local(music_state_t *st, int idx, const char *path, uint32_t resume_ms)
{
    st->index      = idx;
    st->resume_ms  = resume_ms;
    /* 先把进度条摆到续播位置，别让它从 0 跳一下（真正的定位见 tick） */
    st->elapsed_ms = resume_ms;
    st->dur_ms     = 0;
    st->seek_in_flight = false;
    st->save_ticks = 0;
    st->resume_wait = 0;
    snprintf(st->track_path, sizeof(st->track_path), "%s", path ? path : "");
}

void music_state_begin_online(music_state_t *st, int idx, const char *uri, uint32_t dur_ms)
{
    st->on_mode    = true;
    st->index      = idx;
    st->resume_ms  = 0;
    st->elapsed_ms = 0;
    st->dur_ms     = dur_ms;      /* 在线总时长（估算值；未知则为 0） */
    st->seek_in_flight = false;
    st->save_ticks = 0;
    st->resume_wait = 0;
    /* 新的一首：卡死看门狗计数从零开始（见 tick 里的说明） */
    music_state_reset_watchdog(st);
    snprintf(st->track_path, sizeof(st->track_path), "%s", uri ? uri : "");
}

void music_state_begin_none(music_state_t *st)
{
    st->index = -1;
}

void music_state_leave_online(music_state_t *st)
{
    st->on_mode       = false;
    st->url_retry     = 0;             /* 切回本地：在线那套计数清掉 */
    st->on_resume_ms  = 0;             /* 切回本地：不带着在线的断点走 */
    st->eof_retry     = 0;
}

void music_state_note_seek_accepted(music_state_t *st, uint32_t target_ms)
{
    /*
     * 定位期间的两个"重新计数"：
     *   - stall_kicks/stall_ticks：卡死看门狗只盯"声称在播但没有 PCM"，
     *     而重新缓冲正好就是这段特征 —— 不重置的话它会在阈值到点后去 pause/resume，
     *     把正在缓冲的 seek 又搅一次；
     *   - elapsed_ms：本来靠墙钟累计，要对齐到请求的位置，否则下一次 tick 拉回原处。
     */
    music_state_reset_watchdog(st);
    st->elapsed_ms = target_ms;
}

void music_state_reset_watchdog(music_state_t *st)
{
    st->stall_kicks = 0;
    st->stall_ticks = 0;
}

int32_t music_state_progress_permille(const music_state_t *st)
{
    if (st->dur_ms == 0) {
        return 0;
    }
    return (int32_t)((uint64_t)st->elapsed_ms * 1000ULL / st->dur_ms);
}

const char *music_state_status_text(const music_state_t *st, bool playing)
{
    if (st->index < 0) {
        return "未在播放";
    }
    return playing ? "正在播放" : "已暂停";
}
