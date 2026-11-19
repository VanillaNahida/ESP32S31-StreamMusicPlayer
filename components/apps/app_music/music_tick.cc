/*
 * music_tick.cc —— 500 ms 常驻心跳：播放状态机主干
 *
 * 搬迁自 app_music.cc 的 1317–1790（原本是一个 474 行的大函数 tick_cb）。
 * 这里按原注释的段落切成 13 个 `tick_*`（外加封面搬运），本体只做调度 ——
 * 行为逐段对应、注释跟着段落一起搬。
 *
 * ★ 这个定时器**开机就建、永不销毁**（见 music_app.cc 的长注释）：
 *   页面只是"显示"，状态机跑在页面之外。所以这里所有碰 UI 的地方都必须容忍
 *   "播放器页不存在"（视图侧的空操作就是原来那一堆 if (s_p_xxx) 的等价形式）。
 */
#include "music_player.h"

#include <stdint.h>

#include "esp_log.h"

#include "audio_service.h"
#include "bsp.h"
#include "play_state.h"
#include "storage_service.h"
#include "ui_core.h"

#include "music_cover.h"
#include "music_logic.h"
#include "music_like.h"
#include "music_online.h"
#include "music_state.h"
#include "music_view_player.h"

static const char *TAG = "app_music";

static void tick_on_finished(bool done);

/* ------------------------------------------------------------------ tick 各段 */

/* #1 来源徽标兜底（本地/在线）：内部有"值没变就直接返回"的判断，每 tick 调也不花钱。
 * 放在最前面：切歌、切队列的路径很多，靠这一处兜住最稳。 */
static void tick_src_badge(void)
{
    music_view_player_apply_src_badge();
}

/* #2 在线曲目：准备好 → 起播；失败 → 提示（都在 LVGL 任务里处理） */
static void tick_online_ready(void)
{
    music_state_t *st = music_state();
    if (st->on_ready) {
        const int idx = st->on_ready_idx;
        st->on_ready = false;
        st->on_ready_idx = -1;
        if (idx >= 0 && st->on_ready_seq == st->on_seq) {
            music_online_start_track(idx);
        } else {
            /* 过期 ready（用户中间又点了别的歌，工作线程发布窗口里的旧结果）：
             * 只把它的缓冲释放掉，不起播。 */
            music_online_discard_ready();
        }
    } else if (st->on_req >= 0) {
        music_online_prepare_kick();
    } else if (st->on_failed) {
        st->on_failed = false;
        ui_toast("在线歌曲获取失败（网络/版权限制？）", 2200);
        /* 缓冲提示不能留在屏上：把歌词区恢复成 歌词 / 暂无歌词
         * （buf_show 也要清掉，否则下一个 tick 又把"缓冲中…"写回去） */
        st->buf_show = false;
        st->seek_buf_pending = false;
        music_view_player_lyric_apply_mode();
    }
    /* 注：原先这里会"顺手预取下一首的封面/歌词"，但它会占住唯一的准备槽位，
     * 用户这时点歌就得干等它跑完（实测 1~3 秒）——为了"点了就切"的及时性，去掉。 */
}

/*
 * #3 拖动定位的落点核对（见 music_player_seek_begin 里的说明）：
 * 服务在 seek 后第一块 PCM 出来时回报"按字节反推的真实时间点"。和请求值差得明显
 * （> 3 s）就把进度与歌词对齐过去 —— 否则用户听到的位置和界面显示会一直差着一截，
 * 而且拖一次差一次。
 *
 * ⚠ 现行语义：audio_service_seek_landing_ms() 现在**恒返回 false**（换
 *   libs/espaudiocore 之后落点核对机制已退休），所以这里只会一次次递减等待计数、
 *   什么都不做。代码保留是为了将来服务侧再提供落点时不需要重写这段；
 *   不要再按"服务端没按 Range 给数据"那套旧结论去理解它。
 */
static void tick_seek_landing(void)
{
    music_state_t *st = music_state();
    if (st->seek_landing_wait <= 0) {
        return;
    }
    uint32_t land = 0;
    if (audio_service_seek_landing_ms(&land)) {
        st->seek_landing_wait = 0;
        const int32_t diff = (int32_t)land - (int32_t)st->elapsed_ms;
        if (diff > 3000 || diff < -3000) {
            ESP_LOGW(TAG, "seek landing off by %d ms (asked %u, got %u), realign",
                     (int)diff, (unsigned)st->elapsed_ms, (unsigned)land);
            st->elapsed_ms = land;
            music_view_player_lyric_sync();
        }
    } else {
        st->seek_landing_wait--;    /* 还没出声/拿不到位置：下个 tick 再看 */
    }
}

/*
 * #4 在线歌缓冲中：把**下载百分比**刷到右栏说明行（需求："缓冲的时候…
 * 显示加载百分比"）。
 *
 * ★ 显示窗口 = 从 set_buffering_hint() 到**真的出声**
 *   （audio_service_output_active）。为什么不用 on_busy 判：
 *   on_busy 只覆盖"解析地址 + 抓封面/歌词"那一段，它在
 *   audio_service_play_uri() 之前就结束了 —— 那时音频的流还没开始，
 *   进度恒为 0，百分比几乎永远显示不出来。而 playing 又太早：
 *   管线一 run() 就是 playing，声音还没出来。所以用"PCM 到过 codec"
 *   这个信号（见 audio_service_output_active 的注释）。
 *   服务端分块传输（没有 Content-Length）时 stream_progress() 返回 false，
 *   那就保持 set_buffering_hint() 写的那句"缓冲中，请稍后..."。
 */
static void tick_buffer_hint(void)
{
    music_state_t *st = music_state();
    if (!st->buf_show) {
        return;
    }
    if (audio_service_output_active()) {
        st->buf_show = false;
        /* 加载结束：播放按钮从"转圈 + 禁用"恢复成播放/暂停图标 */
        music_view_player_set_busy(false);
        /*
         * 拖动定位引起的这次缓冲结束了：打一行落点，用来核对 seek 准不准
         * （百分比是"IO 已读到的位置 / 文件总长"，跟请求的时间点大致对应）。
         */
        if (st->seek_buf_pending) {
            st->seek_buf_pending = false;
            int pct = -1;
            if (audio_service_stream_progress(&pct)) {
                ESP_LOGI(TAG, "seek rebuffered, stream at %d%%", pct);
            } else {
                ESP_LOGI(TAG, "seek rebuffered");
            }
        }
        music_view_player_lyric_apply_mode();   /* 出声了：说明行交回 歌词 / 暂无歌词 */
    } else {
        music_view_player_refresh_buffer_hint(); /* 缓冲中 <速度> · <pct>%（速度是活的） */
    }
}

/*
 * #5 播放结束 / 解码出错 → 换下一首。
 *
 * 为什么不在事件回调里直接换：事件回调跑在播放器任务里，在那里重入播放器
 * 等于在它自己的回调里再起播。所以回调只置标志，真正的动作放到这里（LVGL 任务上下文）。
 */
static void tick_consume_events(void)
{
    music_state_t *st = music_state();
    bool err  = st->err_req;
    bool done = st->advance_req;
    st->err_req     = false;
    st->advance_req = false;

    if (err) {
        /*
         * 出错就退出加载态：否则"转圈 + 禁用播放按钮"会一直挂着（用户既听不到声音、
         * 又按不动播放），比原来的表现还糟。
         */
        st->buf_show = false;
        music_view_player_set_busy(false);
        /*
         * 在线流媒体：**先怀疑播放地址过期**（网易云 URL 带时间戳 + 签名，放一阵就 403）。
         * 这时重新解析一次地址接着放，而不是把这首歌当"放不了"跳过去 ——
         * 用户点回来听同一首是很自然的操作。
         * 刷新后仍然失败（或已经刷够次数）才走下面的"这首放不了，已跳过"。
         */
        if (music_online_url_refresh()) {
            err  = false;
            done = false;             /* 别顺着 advance_req 跳下一首 */
        } else {
            st->fail_streak++;
            if (music_fail_streak_give_up(st->fail_streak)) {
                ui_toast("连续几首都放不了，先停下", 2200);
                done = false;
                music_state_begin_none(st);
            } else {
                ui_toast("这首放不了，已跳过", 1600);
            }
        }
    }
    /* 真的播起来一会儿才算这首没问题，清掉失败计数与地址重刷计数
     * （否则一首坏文件会一直跳；而"重刷过"的账也要在放成功之后销掉） */
    if (st->playing && st->elapsed_ms > 3000) {
        st->fail_streak = 0;
        st->url_retry   = 0;
    }
    tick_on_finished(done);
}

/*
 * #6 播放结束 → 按模式决定下一首（由 tick_consume_events 调用）。
 *
 * ⚠ 这一步必须**在线/本地分开**（2026-10 用户报"在线单曲循环失效"），
 *   判据与理由见 music_logic.h 的 music_next_action()。
 */
static void tick_on_finished(bool done)
{
    music_state_t *st = music_state();
    if (!done) {
        return;
    }
    const bool   on_queue   = music_on_queue(st);
    const bool   repeat_one = (st->play_mode == MUSIC_MODE_ONE && st->index >= 0);
    const size_t local_n    = storage_service_count();

    /*
     * ★ "提前结束" ≠ 放完（2026-10 用户报：连接被 reset 后卡住/没声音）。
     *
     * 判据与理由见 music_logic.h 的 music_is_early_eof()：
     * 重刷地址 + 从断点续播，而不是跳下一首；单独的 eof_retry 上限防死循环，
     * 额度只在真换曲时清零。
     */
    if (on_queue && music_is_early_eof(st->elapsed_ms, st->dur_ms)) {
        if (st->eof_retry < MUSIC_EARLY_EOF_RETRY_MAX) {
            st->eof_retry++;
            ESP_LOGW(TAG, "online stream ended early (%u/%u ms), refresh + resume (try %d/%d)",
                     (unsigned)st->elapsed_ms, (unsigned)st->dur_ms,
                     st->eof_retry, MUSIC_EARLY_EOF_RETRY_MAX);
            if (music_online_url_refresh()) {   /* 内部会把 elapsed_ms 记成续播点 */
                done = false;
            }
        } else {
            ESP_LOGW(TAG, "online stream keeps ending early, give up and advance");
            st->eof_retry = 0;
        }
    }

    if (!done) {
        return;
    }
    const size_t count = on_queue ? st->on_count : local_n;
    switch (music_next_action(repeat_one, on_queue, count)) {
    case MUSIC_NEXT_REPLAY:
        if (on_queue) {
            /* 同一首重新走一遍"解析地址 → 起播"：play_next(0) 就是它。
             * 顺带把可能已经过期的签名地址换成新的（见 music_online_url_refresh）。 */
            ESP_LOGI(TAG, "online repeat-one: replay idx=%d", st->index);
            music_player_play_next(0);
        } else {
            /* 本地：同一首从头再放（坏文件由上面的 fail_streak 兜住，
             * 连续失败 3 次会把 done 置假并停下，不会死循环） */
            music_player_start_track(st->index);
        }
        break;
    case MUSIC_NEXT_SKIP:
        music_player_play_next(+1);
        break;
    default:
        if (music_fail_streak_give_up(st->fail_streak)) {
            st->fail_streak = 0;
        }
        break;
    }
}

/*
 * #7 卡死看门狗。判据与阈值见 music_logic.h 的 music_watchdog_decide()；
 * 这里只负责"按判据做事 + 打日志"。
 */
static void tick_watchdog(void)
{
    music_state_t *st = music_state();

    /*
     * ★ "seek 进行中"期间不介入：`audio_service_seek_ms()` 会清掉出声标志，
     *   看门狗看到的就是"声称在播但没 PCM" —— 本地阈值只有 8 s，一次慢一点的定位
     *   （大 FLAC 重新填缓冲）会被误判成"文件解不出来"直接跳过。
     *   出声了就清标志；真卡死超过 MUSIC_SEEK_STALL_MAX_TICKS 才把标志放开交给看门狗兜底。
     */
    if (st->seek_in_flight) {
        if (audio_service_output_active()) {
            st->seek_in_flight = false;
        } else if (++st->seek_inflight_n > MUSIC_SEEK_STALL_MAX_TICKS) {
            ESP_LOGW(TAG, "seek produced no PCM for %u s, let the watchdog act",
                     (unsigned)(MUSIC_SEEK_STALL_MAX_TICKS * MUSIC_TICK_MS / 1000));
            st->seek_in_flight = false;
        }
    }

    const bool playing = audio_service_is_playing();
    const bool flowing = audio_service_output_active();

    if (!playing || flowing || st->seek_in_flight) {
        st->stall_ticks = 0;
        return;
    }
    if (++st->stall_ticks < (st->on_mode ? MUSIC_STALL_TICKS : MUSIC_LOCAL_STALL_TICKS)) {
        return;
    }
    /* 计时器置零后再按阈值决策（阈值与判据在 music_logic.cc，两边必须一致） */
    st->stall_ticks = 0;
    switch (music_watchdog_decide(true, false, false, st->on_mode, 0, st->stall_kicks)) {
    case MUSIC_WD_SKIP_LOCAL:
        /*
         * ★ 本地文件也一样会"卡死"（2026-10 用户报：某 FLAC 反复
         *   `AUD_Dec_Parse: Not found frame in max frame size 3635`，
         *   解码器**一帧都吐不出来**，但会话状态仍是 RUNNING ——
         *   界面看着"在播"、没有任何声音、日志还一直刷屏）。
         *   SD 读很快，这么久没 PCM 只能是这个文件解不出来 → 当"放不了"跳过，
         *   别无限耗着（以前这里只听在线流，本地这种情况**永远没人管**）。
         *   注意用 play_next() 而不是 advance_req：后者在单曲循环下会把
         *   同一个坏文件再放一遍，等于死循环。
         */
        st->fail_streak++;
        ESP_LOGW(TAG, "local file no PCM for %u s (fail streak %d), skip it",
                 (unsigned)(MUSIC_LOCAL_STALL_TICKS * MUSIC_TICK_MS / 1000), st->fail_streak);
        if (music_fail_streak_give_up(st->fail_streak)) {
            st->fail_streak = 0;
            ui_toast("连续几个文件都放不了，先停下", 2200);
            audio_service_stop();
            st->playing = false;
            music_view_player_set_play_icon(false);
        } else {
            ui_toast("这个文件解不出来，已跳过", 1800);
            music_player_play_next(+1);
        }
        break;

    case MUSIC_WD_KICK_STREAM: {
        st->stall_kicks++;
        int pct = -1;
        audio_service_stream_progress(&pct);
        ESP_LOGW(TAG, "stream stall: 无输出 %u s，踢一下管线（第 %d 次，下载 %d%%）",
                 (unsigned)(MUSIC_STALL_TICKS * MUSIC_TICK_MS / 1000), st->stall_kicks, pct);
        audio_service_pause();
        if (audio_service_resume() != ESP_OK) {
            audio_service_seek_ms(st->elapsed_ms);   /* resume 不回来就重定位一次 */
        }
        /*
         * ★ 兜底：pause 成功而 resume 失败时，管线会**停在 PAUSED** ——
         *   现象就是"卡一下之后彻底没声音"，而且看门狗只盯"声称在播"的流
         *   （is_playing() 为假就不再进这个分支），于是**再也没人来救**。
         *   所以踢完不管走哪条路，最后都确认它真的在跑。
         */
        if (!audio_service_is_playing()) {
            ESP_LOGW(TAG, "stream stall: player not running after kick, force resume");
            audio_service_resume();
        }
        break;
    }

    case MUSIC_WD_ESCALATE:
        /*
         * ★ 踢了 STALL_KICK_MAX 次还是没有 PCM：pause/resume 已经救不回来了。
         *   最可能的原因是**播放地址失效 / 连接被服务端彻底掐死**（此时底层
         *   既不出错也不返回，所以 app 那套错误处理永远收不到事件 —— 这也是
         *   用户看到"彻底没声音"的原因）。
         *   这里主动升级成"重刷地址 + 断点续播"：地址过期会被顺手修掉，
         *   没过期也能靠 seek 关掉旧连接、带 Range 重开一条。
         */
        ESP_LOGW(TAG, "stream stall: 已重试 %d 次仍无输出，升级为重刷地址 + 断点续播",
                 MUSIC_STALL_KICK_MAX);
        st->stall_kicks = 0;                          /* 下一轮重新计数 */
        if (!music_online_url_refresh()) {
            /* 刷新额度也用完了：交给错误处理（换下一首 / 连续失败后停下） */
            ESP_LOGW(TAG, "stream stall: refresh gave up, report as error");
            st->err_req = true;
        }
        break;

    default:
        break;
    }
}

/*
 * #8 墙钟。
 *
 * ★ 只在**真的出声之后**才累加（2026-10 修"歌词与歌曲不同步"）：
 *   进度只能靠墙钟累加；而在线歌从 run() 到真正出声之间还有 1~3 秒的缓冲 ——
 *   以前这段时间也被算进 elapsed_ms，于是歌词**整体提前**（缓冲越久偏得越多）。
 *   这里加一道 audio_service_output_active()（PCM 真的写到 codec/蓝牙那一刻才为真），
 *   起播前不计时，误差就消失了。顺带把暂停/卡顿期间也排除了。
 */
static void tick_advance_clock(void)
{
    music_state_t *st = music_state();
    if (audio_service_is_playing() && audio_service_output_active()) {
        /* resume_ms > 0 期间先不累计：这时音源还在 0 附近走，等下面
         * 定位完成后再把 elapsed_ms 对齐到续播位置，否则显示会来回跳。 */
        if (st->resume_ms == 0) {
            st->elapsed_ms += MUSIC_TICK_MS;
        }
    }
    st->playing = audio_service_is_playing();
}

/*
 * #9 在线"断点续播"（2026-10 用户报：CDN 掐断长连接后卡住/没声音）。
 *
 * music_online_url_refresh() 只是重新解析地址，起播是从 0 开始的；真正的续播在这里：
 * 等"总长 + 时长"都就绪（audio_service_seek_supported()），再调 seek_ms() 定位回断点
 * —— 它内部会重新算字节、清 PCM 基准，所以进度条/歌词会跟着对齐。
 *
 * 与下面本地续播（resume_ms）分开写：本地那条依赖 info.bitrate > 0，
 * 而在线流的码率恒为 0，走它只会超时放弃。
 */
static void tick_resume_online(void)
{
    music_state_t *st = music_state();
    if (st->on_resume_ms > 0 && st->on_mode && st->playing) {
        if (audio_service_seek_supported()) {
            const uint32_t target = st->on_resume_ms;
            st->on_resume_ms = 0;
            if (audio_service_seek_ms(target) == ESP_OK) {
                st->elapsed_ms = target;    /* seek 成功：时间轴先按目标对齐 */
                st->seek_buf_pending = !audio_service_output_active();
                music_state_reset_watchdog(st);   /* 重新缓冲期间别让看门狗误判 */
                music_view_player_lyric_sync();
                ESP_LOGI(TAG, "online resume at %u ms after URL refresh", (unsigned)target);
            } else {
                ESP_LOGW(TAG, "online resume seek failed, continue from 0");
            }
        } else if (++st->on_resume_wait > MUSIC_RESUME_WAIT_MAX_TICKS) {
            /* 总长/时长一直没就绪（或服务端没给 Content-Length）：放弃续播，从头放 */
            st->on_resume_ms = 0;
            st->elapsed_ms   = 0;
            ESP_LOGW(TAG, "online resume: not ready after %d ticks, start from 0",
                     MUSIC_RESUME_WAIT_MAX_TICKS);
        }
    } else if (!st->on_mode) {
        st->on_resume_wait = 0;
    }
}

/*
 * #10 进度记忆（需求）：从"上次听到的位置"续播。
 *
 * 为什么放在这里而不是起播时：按字节定位需要平均码率，而码率要等服务报出信息
 * 之后才有。所以起播时先把位置记在 resume_ms，这里每 500 ms 试一次，
 * 码率一到就定位，然后清掉标志（只尝试一次，失败不反复 seek）。
 */
static void tick_resume_local(void)
{
    music_state_t *st = music_state();
    if (st->resume_ms == 0 || !st->playing || !st->track_path[0]) {
        return;
    }
    const uint32_t target = st->resume_ms;
    /*
     * ⚠ 这里**不再**按容器拦（2026-10：原先用"流式容器名单"挡掉了 m4a/ogg/opus 的
     *   续播，导致这些格式的进度记忆形同虚设）。本地文件都实现了 seek_ms()，
     *   让下面的 `audio_service_seek_ms()` 自己去判；失败就从头放（else 分支）。
     */
    audio_info_t info;
    const bool info_ok = audio_service_get_info(&info) && info.bitrate > 0;
    if (++st->resume_wait > MUSIC_RESUME_WAIT_MAX_TICKS) {
        /* 兜底：一直等不到码率（容器/格式报不出 bitrate）就放弃定位，
         * 否则 elapsed_ms 会被"续播位置"钉死，进度条与时间都不再走。
         * 真实已播 = 起播到现在经过的墙钟。 */
        st->resume_ms  = 0;
        st->elapsed_ms = (uint32_t)st->resume_wait * MUSIC_TICK_MS;
        ESP_LOGW(TAG, "no bitrate for resume, continue from wall clock");
        return;
    }
    if (!info_ok) {
        return;
    }
    st->resume_ms = 0;
    /*
     * ★ 到期保护（2026-10 真机崩溃之后加，务必保留）。
     *
     * 恢复的位置可能**超过文件总长**（进度记忆写的是"当时听到哪"，
     * 换过文件、文件被换掉、或上次是在末尾附近退出的，都会越界）。
     * 越界时按字节 seek 会正好落到文件尾 → 解码器立刻 EOF → 管线收尾关闭，
     * 而收尾逻辑还会再发一次 seek → 打到已关闭的 IO 上（历史上直接把整机打 panic）。
     * 所以：落在最后一小段里就直接从头放（听感上也更合理）。
     */
    if (info.duration_ms > MUSIC_RESUME_END_GUARD_MS * 2 &&
        target + MUSIC_RESUME_END_GUARD_MS >= info.duration_ms) {
        st->elapsed_ms = 0;
        ESP_LOGI(TAG, "resume pos %u ms is at the end of %u ms track, start from 0",
                 (unsigned)target, (unsigned)info.duration_ms);
    } else if (audio_service_seek_ms(target) == ESP_OK) {
        st->elapsed_ms = target;
        /* 续播定位之后同样要"复位上一行 + 重新居中"
         * （别清 lyric_cur，见 music_view_player_lyric_sync 的说明） */
        music_view_player_lyric_sync();
        ESP_LOGI(TAG, "resumed at %u ms", (unsigned)target);
    } else {
        st->elapsed_ms = 0;      /* 定位失败：界面别停在假进度上 */
    }
}

/*
 * #11 进度记忆：每 5 秒把"当前曲目 + 已播毫秒"同步到 play_state 的 RAM 态，
 * 真正写 flash 由 play_state 内部按 8 秒防抖合并，不会狂写 NVS。
 */
static void tick_save_progress(void)
{
    music_state_t *st = music_state();
    if (++st->save_ticks >= MUSIC_PROGRESS_SAVE_TICKS) {
        st->save_ticks = 0;
        if (st->playing && st->track_path[0]) {
            music_player_save_progress(st->elapsed_ms);
        }
    }
}

/* #12 总时长推导：优先用探测到的准确值（Xing/Info 帧数），否则按 文件大小/码率 估 */
static void tick_derive_duration(void)
{
    music_state_t *st = music_state();
    if (st->dur_ms != 0) {
        return;
    }
    audio_info_t info;
    const track_t *tr = storage_service_track((size_t)(st->index < 0 ? 0 : st->index));
    if (st->index >= 0 && tr && audio_service_get_info(&info)) {
        if (info.duration_ms >= 1000) {
            st->dur_ms = info.duration_ms;
        } else if (info.bitrate > 0) {
            st->dur_ms = (uint32_t)((uint64_t)tr->size_bytes * 8ULL * 1000ULL /
                                    (uint64_t)info.bitrate);
            if (st->dur_ms < 1000) {
                st->dur_ms = 0;
            }
        }
    }
}

/* #12b 在线流：采用服务**纠正过**的总时长。
 *
 * 起因（2026-10 用户报"在线 seek 支持还不是很好"）：在线总时长来自元数据，
 * 而网易云对 VIP 歌下发的是 **30 秒试听片段** —— 元数据说 200 s、流里只有 27 s。
 * 后果有三处，全是用户能看到的：
 *   1. 进度条按 200 s 爬，永远走不到头；
 *   2. seek 的时间→字节映射按错误码率算，拖到 50% 落点不对；
 *   3. 「提前结束」判据（`dur > 30s && elapsed + 15s < dur`）把片段正常播完
 *      当成"流被掐断" → 反复重刷地址重连。
 * 服务侧用"已下载 ÷ 已播"实测出真码率后会把 `info.duration_ms` 修正过来
 * （见 audio_engine_bitrate），这里跟它走。
 */
static void tick_adopt_stream_duration(void)
{
    music_state_t *st = music_state();
    if (!st->on_mode || st->on_stream_local || st->index < 0) {
        return;
    }
    audio_info_t info;
    if (!audio_service_get_info(&info) || info.duration_ms < 1000) {
        return;
    }
    const uint32_t d = info.duration_ms;
    /* 差 5% 以内不动（免得每 tick 微调让进度条抖） */
    if (st->dur_ms > d + d / 20 || st->dur_ms + st->dur_ms / 20 < d) {
        ESP_LOGI(TAG, "online duration: %u -> %u ms (stream-derived)",
                 (unsigned)st->dur_ms, (unsigned)d);
        st->dur_ms = d;
    }
}

/* #13 控件刷新（进度条 / 时间 / 按钮 / 输出图标 / 歌词高亮） */
static void tick_refresh_widgets(void)
{
    music_state_t *st = music_state();
    /* 进度：千分比显示，跟总时长解耦（时长未知也不会跳）。
     * "手指正在拖"的跳过逻辑在视图里（seek 标志的 owner 是视图）。 */
    music_view_player_refresh_timesliders();
    music_view_player_set_play_icon(st->playing);
    /* 输出设备可能被设置页/蓝牙事件改过：按钮图标跟着走 */
    music_view_player_out_btn_sync();
    /* 红心：先让业务侧消费工作线程的结果（含 toast），再把状态画到按钮上 */
    music_like_tick();
    music_view_player_like_apply();
    /* 主题色（跟随封面）：封面/曲目变了才真的重设，颜色没变是空操作 */
    music_view_player_accent_apply();
    if (music_view_player_lyric_expanded()) {
        music_view_player_lyric_tick();
    }
}

/* ------------------------------------------------------------------ tick 主干 */

void music_player_tick(void)
{
    tick_src_badge();
    tick_online_ready();
    tick_seek_landing();
    tick_buffer_hint();
    tick_consume_events();
    tick_watchdog();
    tick_advance_clock();
    tick_resume_online();
    tick_resume_local();
    tick_save_progress();
    tick_derive_duration();
    tick_adopt_stream_duration();
    tick_refresh_widgets();

    /* 封面：先把工作线程的产出搬上屏（下标对不上就丢掉），再按需拉起下一次解码 */
    music_view_player_cover_pump();
}

/* ------------------------------------------------------------------ 页面收尾与恢复 */

void music_player_on_view_destroyed(void)
{
    music_state_t *st = music_state();
    /* 离开播放器页就把进度落盘：用户很可能紧接着就断电/退出 */
    if (st->track_path[0]) {
        music_player_save_progress(st->elapsed_ms);
        play_state_commit();
    }
    /* 离开播放器页：没起播的在线结果直接丢弃（封面/歌词缓冲一并释放） */
    music_online_discard_ready();
}

/*
 * 打开播放器页（压栈）。给别的 APP 用（在线音乐点歌之后直接进播放器看封面/歌词），
 * 本地列表页点歌之后也走它。描述符是拷贝进路由栈的，所以这里的栈上临时量是安全的
 * （见 ui_router_push 注释）。
 */
void music_router_open_player(void)
{
    page_desc_t page = { "music", music_view_player_build, NULL, NULL, false };
    ui_router_push(&page);
}

/*
 * 开机恢复上次的曲目与进度（进度记忆）。
 *
 * 只把状态摆好、**不自动出声**：进播放器页就能看到上次那首歌和它的进度，
 * 按 ▶ 才从记忆位置继续（on_play_pause → start_track_at(index, elapsed_ms)，
 * 真正的字节定位在 tick 里等码率到手后执行）。
 * 断电前的音量则是在 audio_service_init() 里恢复的（不需要 UI）。
 */
void music_player_restore_last_session(void)
{
    music_state_t *st = music_state();
    char path[STORAGE_PATH_MAX];
    uint32_t pos = 0;
    if (play_state_track_get(path, sizeof(path), &pos) != ESP_OK) {
        return;
    }
    if (!bsp_sdcard_is_mounted()) {
        ESP_LOGW(TAG, "restore skipped: sdcard not mounted");
        return;
    }

    /* 队列按"曲目所在目录这一层"装载，与浏览页点歌时的语义一致 */
    char dir[STORAGE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), "%s", "/sdcard");
    }

    if (storage_service_load_dir(dir) <= 0) {
        ESP_LOGW(TAG, "restore: '%s' 里没有可播的曲目", dir);
        return;
    }
    const int idx = storage_service_index_of_path(path);
    if (idx < 0) {
        ESP_LOGW(TAG, "restore: 记忆的曲目已不在（%s）", path);
        return;
    }

    music_state_begin_local(st, idx, path, pos);
    music_view_player_lyric_load(path);    /* 页面建起来时 lyric_rebuild 直接用 */
    ESP_LOGI(TAG, "restored last track: %s @ %u ms", path, (unsigned)pos);
}

