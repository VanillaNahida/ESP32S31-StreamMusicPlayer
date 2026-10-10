/*
 * music_player.cc —— 播放控制（业务层）
 *
 * 搬迁自 app_music.cc 的 196–262（进度记忆）、496–698（起播/换曲/播放暂停）、
 * 1140–1315（播放事件与蓝牙媒体键）、3527–3577（开机恢复）与 seek 受理那一段。
 *
 * tick（500 ms 常驻心跳）在 music_tick.cc：它是原 474 行的大函数 tick_cb，
 * 按原注释的段落切成 13 个 `tick_*`，本体只做调度。
 */
#include "music_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "bsp.h"
#include "media_meta.h"
#include "play_state.h"
#include "storage_service.h"
#include "ui_core.h"

#include "music_cover.h"
#include "music_logic.h"
#include "music_online.h"
#include "music_state.h"
#include "music_view_player.h"

static const char *TAG = "app_music";

/* ------------------------------------------------------------------ 进度记忆 */

/*
 * 进度记忆的唯一出口：**在线流式不记**。
 * 流地址是 CDN 带时效签名的 URL（几分钟后失效），存下来下次也用不了，
 * 反而会把"上次听到哪首本地歌"覆盖掉。下载兜底（本地文件）照常记。
 */
void music_player_save_progress(uint32_t pos_ms)
{
    music_state_t *st = music_state();
    if (st->on_mode && !st->on_stream_local) {
        return;
    }
    if (!st->track_path[0]) {
        return;
    }
    play_state_track_set(st->track_path, pos_ms);
}

/* ------------------------------------------------------------------ 起播与换曲 */

/*
 * 起播一首：resume_ms > 0 表示"从这里的进度继续"（进度记忆 / 开机续播）。
 * 换曲一律传 0，只有 on_play_pause 重新起播当前曲目、以及开机恢复时才带位置。
 */
void music_player_start_track_at(int idx, uint32_t resume_ms)
{
    music_state_t *st = music_state();
    const track_t *t = storage_service_track((size_t)idx);
    if (!t) {
        ui_toast("曲目不存在", 1200);
        return;
    }

    /* 切回本地队列：清掉"在线队列"的任何残留（封面/歌词缓冲也要释放）。
     * on_songs 置空 + 把托管的旧队列放掉：本地播放不会再引用它，
     * 而 app_online 那边已经不再持有这块内存（所有权在本模块）。 */
    music_state_leave_online(st);
    music_view_player_apply_src_badge();   /* 徽标切回"本地播放"（此刻 on_mode 已是 false） */
    music_view_player_apply_play_mode();   /* 本地才可能用到随机循环，图标跟着模式走 */
    music_online_discard_ready();
    st->on_songs    = NULL;
    st->on_count    = 0;
    st->on_req      = -1;            /* 本地播放：丢掉还没发出去的在线请求 */
    music_online_owned_drop();

    music_state_begin_local(st, idx, t->path, resume_ms);
    music_view_player_lyric_load(t->path);
    music_view_player_set_now_playing(t->title, t->artist);

    /* 歌词：换曲要重建（页面可能已经存在，例如播完自动续播） */
    music_view_player_lyric_rebuild();
    if (music_view_player_lyric_expanded()) {
        music_view_player_lyric_tick();
    }
    st->buf_show = false;          /* 本地文件：立刻出声，不要"缓冲中"提示 */
    st->seek_buf_pending = false;

    /* 封面：先退回占位（别把上一首的封面留在屏上），再交给工作线程解 */
    music_view_player_cover_show(NULL, 0, 0);
    music_cover_reap();
    music_cover_request(idx);
    music_cover_kick();

    /*
     * 进度条归零：时长这时还不知道（要等服务报出信息），进度条先按 0 摆好。
     * 续播位置在定位成功后随 elapsed_ms 一起由 tick 换算成比例显示。
     */
    music_view_player_reset_progress();
    music_view_player_set_elapsed_text(st->elapsed_ms);

    /* 进度记忆：换曲就是要记住"现在放的是哪首、放到哪了"，立刻落盘 */
    music_player_save_progress(resume_ms);
    play_state_commit();

    if (audio_service_play_path(t->path) != ESP_OK) {
        ui_toast("播放失败：格式不支持或文件损坏", 2000);
        return;
    }
    st->playing = true;
    music_view_player_set_play_icon(true);
}

void music_player_start_track(int idx)
{
    music_player_start_track_at(idx, 0);
}

void music_player_play_next(int dir)
{
    music_state_t *st = music_state();
    /* 在线队列：换曲要先去把音频下下来（工作线程），不能在这里阻塞 LVGL */
    if (st->on_mode && st->on_songs && st->on_count > 0) {
        int idx = st->index;
        if (idx < 0) {
            idx = 0;
        } else {
            idx = music_online_index(idx, dir, st->on_count);
        }
        st->index = idx;             /* 目标歌先记上：界面/状态立刻指向它 */
        st->on_seq = st->on_seq + 1;
        st->url_retry = 0;           /* 换了一首：地址重刷次数从头算 */
        st->on_resume_ms = 0;        /* 换曲不续播（只有"同一首被掐断"才用断点） */
        st->eof_retry    = 0;
        music_online_discard_ready();/* 清掉还没起播的旧结果（缓冲一并释放） */
        st->on_failed = false;
        st->on_req = idx;
        /*
         * 和点歌走同一套顺序（见 music_online_play）：
         * 停旧歌 → 界面切到目标歌 → 再显示缓冲信息。
         * 自动续播时旧歌本来就播完了，多调一次 stop 无害。
         */
        audio_service_stop();
        st->playing = false;
        music_view_player_set_play_icon(false);
        music_online_switch_ui_to(idx);
        music_online_prepare_kick();
        if (music_online_busy()) {
            ui_toast("缓冲中，请稍后...", 1200);
            music_view_player_set_buffering_hint();
        }
        return;
    }

    size_t n = storage_service_count();
    if (n == 0) {
        return;
    }
    int idx = st->index;
    if (idx < 0) {
        idx = 0;
    } else if (st->play_mode == MUSIC_MODE_SHUFFLE && n > 1) {
        /* 随机循环：挑一首和当前不同的（n == 1 时退化为原曲） */
        int r = idx;
        while (r == idx) {
            r = (int)(esp_random() % (uint32_t)n);
        }
        idx = r;
    } else {
        idx = music_next_index(idx, dir, n);
    }
    music_player_start_track(idx);
}

void music_player_on_play_pause(void)
{
    music_state_t *st = music_state();
    if (st->index < 0) {
        ui_toast("请先在本地音乐里选一首", 1600);
        return;
    }
    /*
     * audio_service_toggle() 只在 RUNNING / PAUSED 之间切；管线处于 STOPPED
     * （开机恢复的上次曲目、或手动停止过）时它返回错误 —— 这时要**从记忆的
     * 进度重新起播**，否则"开机后按播放没反应"。
     */
    if (audio_service_toggle() != ESP_OK) {
        /* 在线模式：地址已经拿到了，直接重起播即可（不用再解析一遍） */
        if (st->on_mode && st->on_stream[0]) {
            music_online_start_track(st->index);
        } else {
            music_player_start_track_at(st->index, st->elapsed_ms);
        }
        return;
    }
    st->playing = audio_service_is_playing();
    music_view_player_set_play_icon(st->playing);
    /* 暂停就立刻把进度落盘（用户可能马上断电） */
    if (!st->playing && st->track_path[0]) {
        music_player_save_progress(st->elapsed_ms);
        play_state_commit();
    }
}

void music_player_stop(void)
{
    music_state_t *st = music_state();
    audio_service_stop();
    st->playing = false;
    music_view_player_set_play_icon(false);
}

/* ------------------------------------------------------------------ 事件与媒体键 */

/*
 * 播放事件 → LVGL 任务的中转标志。
 * 事件回调运行在播放器任务上下文（audio_service.h 明确要求别做重活），
 * 所以那里只置标志，UI 更新与换曲都放到 tick（LVGL 任务）里做。
 */
void music_player_on_audio_evt(audio_evt_t evt, void *user)
{
    (void)user;
    music_state_t *st = music_state();
    switch (evt) {
    case AUDIO_EVT_FINISHED:
        st->advance_req = true;
        break;
    case AUDIO_EVT_ERROR:
        /* 解码失败 / 格式不支持：先报错再跳过，不然界面看着像卡死 */
        st->err_req     = true;
        st->advance_req = true;
        break;
    default:
        break;
    }
}

void music_player_on_media_ctrl(int cmd)
{
    /* 只置标志：这里可能是蓝牙栈的任务上下文 */
    music_state()->media_cmd = cmd;
}

/* 蓝牙媒体命令：跑在 LVGL 任务里（由 100 ms 的常驻定时器取一次） */
void music_player_media_cmd_tick(void)
{
    music_state_t *st = music_state();
    const int cmd = st->media_cmd;
    if (cmd == 0) {
        return;
    }
    st->media_cmd = 0;
    switch (cmd) {
    case 1:      /* ESP_BT_AUDIO_MEDIA_CTRL_CMD_PLAY */
        if (st->index >= 0) {
            /* 管线是 STOPPED（开机恢复的曲目）时 resume 无效，要从记忆进度起播 */
            if (audio_service_resume() != ESP_OK) {
                if (st->on_mode && st->on_stream[0]) {
                    music_online_start_track(st->index);
                } else {
                    music_player_start_track_at(st->index, st->elapsed_ms);
                }
            }
            st->playing = true;
        }
        break;
    case 2:      /* PAUSE */
        audio_service_pause();
        st->playing = false;
        break;
    case 3:      /* STOP */
        audio_service_stop();
        st->playing = false;
        break;
    case 4:      /* NEXT */
        music_player_play_next(+1);
        break;
    case 5:      /* PREV */
        music_player_play_next(-1);
        break;
    default:
        break;
    }
    /* 暂停 / 停止（含耳机上按的）：立刻把进度落盘 */
    if ((cmd == 2 || cmd == 3) && st->track_path[0]) {
        music_player_save_progress(st->elapsed_ms);
        play_state_commit();
    }
    music_view_player_set_play_icon(st->playing);
    ESP_LOGI(TAG, "media ctrl %d applied", cmd);
}

/* ------------------------------------------------------------------ seek 受理 */

bool music_player_seek_begin(uint32_t target)
{
    music_state_t *st = music_state();
    /*
     * 两道关都要给用户一句解释 —— 进度条是"活的"（拇指跟手），要是拖完什么都不发生，
     * 用户只会以为界面卡了（2026-10 用户报的原话就是"点了没反应"）。
     *   1) 容器：MP4/M4A/OGG 这类要索引/setup 头的格式，按字节跳过去解码器对不上帧；
     *   2) 位置算不算得出来：本地要有探测到的平均码率，在线要有"总字节 + 时长"。
     */
    if (music_track_seek_unsupported(st->track_path)) {
        ui_toast("这个格式不支持拖动定位", 2000);
        return false;
    }
    if (!audio_service_seek_supported()) {
        ui_toast("定位失败：码率未知", 1800);
        return false;
    }

    /*
     * 在线流式：长距离拖动会关掉连接、带 Range 重开，得重新缓冲几百毫秒起。
     * 先挂上"缓冲中"提示（tick 按 audio_service_output_active() 撤掉 ——
     * audio_service_seek_ms() 内部已经把出声标志清零，所以它表示"seek 之后又出声了"）。
     * 暂停状态下不挂：那时不会有 PCM 出来，提示会永远留在屏上。
     */
    const bool streamed = (st->on_mode && !st->on_stream_local);
    if (streamed && audio_service_is_playing()) {
        music_view_player_set_buffering_hint();
        st->seek_buf_pending = true;    /* 出声后打一行落点，便于对日志核对 */
    }

    if (audio_service_seek_ms(target) != ESP_OK) {
        ui_toast("定位失败：码率未知或格式不支持", 1800);
        st->seek_buf_pending = false;
        return false;
    }

    /*
     * 落点核对：等"seek 之后第一块 PCM"出来时服务回报的真实落点
     * （最多 5 秒 ≈ 10 个 tick，超时就不管了）。本地文件也开着 ——
     * 文件 IO 同样能按字节反推，所以不是 streamed ? 10 : 0。
     */
    st->seek_landing_wait = 10;

    /*
     * ★ 标一个"seek 进行中"（2026-10 加，防止看门狗误伤）：
     *   `audio_service_seek_ms()` 会把出声标志清零，于是 tick 看到
     *   "声称在播但没 PCM" —— 而本地那条看门狗阈值只有 8 s，一次慢一点的本地
     *   seek（大 FLAC 重新填缓冲）就会被判成"这个文件解不出来"直接跳过。
     *   这个标志让看门狗在定位期间不介入；真卡死超过 MUSIC_SEEK_STALL_MAX_TICKS 才放手。
     */
    st->seek_in_flight  = true;
    st->seek_inflight_n = 0;

    music_state_note_seek_accepted(st, target);
    /* 进度记忆跟着用户的手动定位走（落盘由 play_state 防抖合并）。
     * 在线流地址是带时效签名的，存下来没意义（和起播路径的判断保持一致）。 */
    if (st->track_path[0] && !streamed) {
        music_player_save_progress(target);
    }
    return true;
}

