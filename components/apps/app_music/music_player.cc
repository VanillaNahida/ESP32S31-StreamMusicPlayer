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

    /* 封面：先退回占位（别把上一首的封面留在屏上），再交给工作线程解。
     * ★ 同一首重起播（暂停后重来 / 续播定位）时屏上那张就是对的，别动它 ——
     *   丢掉重来有可能永久拿不回来（在线封面缓冲一次性消费，见 music_cover.h）。 */
    if (music_cover_needs_reload(idx)) {
        music_view_player_cover_show(NULL, 0, 0);
        music_cover_reap();
        music_cover_request(idx);
        music_cover_kick();
    }

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

/*
 * "加载中"判据 —— 用**状态**而不是控件。
 * 视图层那个 `s_c.btn_busy` 只反映播放器页上的按钮，用户在别的页面用蓝牙媒体键
 * 或音量键按播放时它是 false，那道门就漏了；两个判据的条件本身是同一个
 * （解析在线地址/抓封面歌词 on_busy、缓冲 buf_show）。
 */
static bool music_loading(void)
{
    const music_state_t *st = music_state();
    return st->on_busy || st->buf_show;
}

void music_player_on_play_pause(void)
{
    music_state_t *st = music_state();
    if (st->index < 0) {
        ui_toast("请先在本地音乐里选一首", 1600);
        return;
    }
    /*
     * ★ 加载中不受理（按钮此刻是转圈 + 禁用，见 music_view_player_set_busy）。
     *
     * 为什么必须在这里再挡一道：加载时按播放会走 `audio_service_toggle()`，
     * 而这时会话可能刚建到一半 —— toggle 失败后落到下面的"重新起播"分支，
     * 重新起播会**释放屏上的封面再重新请求**，而那份封面数据可能已经被消费掉 →
     * 封面就"没了"（2026-10 用户报的"加载时点播放，封面丢失"）。
     * `LV_STATE_DISABLED` 只是"看起来禁用"，吃不吃点击别依赖它。
     */
    if (music_loading()) {
        ui_toast("正在加载，请稍候", 1000);
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
    st->playing  = false;
    st->buf_show = false;                     /* 停了就没有"加载中"了 */
    music_view_player_set_busy(false);
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
     * ★★ 2026-10 修（真机："这个 MP3 一拖进度条就立刻切下一曲"）：
     *
     * 现象链：探不出时长 → 用「文件大小 ÷ 码率」估出**偏大**的时长 → 用户拖到最右
     *        → 落点越过文件尾 → 解码器立即 EOF → 引擎判定"放完" → app 切下一曲。
     * 实证（同一个文件）：
     *   I media_meta: mp3 probe: 128000 bps, 0 ms, 44100 Hz, 2ch     ← 探测 0 ms
     *   I espaudiocore: MP3 Xing: duration=55.25s                    ← 解码器：55.25 s
     *   I audio_seek: local seek -> 63842 ms                         ← 却拖到 63.8 s
     *   I espaudiocore: decode finished: end of stream               ← 立刻 EOF
     * 63.8 s 的来源就是"整文件 ÷ 128 kbps"，而这个文件带了 **33972 字节的 ID3v2**
     * （封面等），把它算进音频长度就偏大了 ✗。
     *
     * 所以：**时长以解码器/引擎报的为准**（`audio_service_get_info()` 里那个是
     * `espaudiocore_get_duration_ms()`，Xing/STREAMINFO 算出来的，准确），并把落点
     * 夹到"离末尾 1 秒"以内 —— 宁可少拖一秒，也不能正好落在 EOF 上被当成播完。
     */
    {
        const bool streamed = (st->on_mode && !st->on_stream_local);
        uint32_t dur = 0;
        audio_info_t info = {};
        const bool info_ok = audio_service_get_info(&info);
        if (info_ok && info.duration_ms > 0) {
            dur = info.duration_ms;          /* 解码器/引擎给的最可信（Xing/STREAMINFO 算的） */
        } else if (st->dur_ms > 0) {
            /*
             * ★ 退回**应用已知的时长**（探测值，或"文件大小 ÷ 码率"的估算）。
             *   上一版这里只信解码器、它报 0 就直接拒绝，结果把这个文件也挡了 ✗：
             *     W app_music: 时长未知（app=87217 ms, decoder=0 ms），拒绝定位
             *   —— 而 app=87217 ms 恰恰是**对的**（PC 侧逐帧解剖真值 87.04 s ✓，
             *   这个文件是无 Xing 头的标准 CBR，估算法本来就算得准 ✓）。
             *   解码库对"无 Xing"文件不做估时（`duration` 返回 -1/0），所以**必须**
             *   用应用侧的值兜住，否则这类文件永远拖不动。
             */
            dur = st->dur_ms;
        }
        if (dur > 2000) {
            if (target > dur - 1000) {
                ESP_LOGW(TAG, "seek 目标 %u ms 超出本曲时长 %u ms，夹到 %u ms",
                         (unsigned)target, (unsigned)dur, (unsigned)(dur - 1000));
                target = dur - 1000;         /* 离末尾留 1 秒，绝不正好落在 EOF 上 */
            }
        } else if (!streamed) {
            /*
             * 连应用侧也没有时长（真未知）：才拒绝这次拖动，而不是拿瞎猜的值去赌 ✗。
             * 这类文件多是 VBR + 无 Xing 头（首帧标称码率远高于平均 ⇒ 估算成倍偏大 ✗）。
             */
            ESP_LOGW(TAG, "时长未知（app=%u ms, decoder=%d ms），拒绝定位：%s",
                     (unsigned)st->dur_ms, (int)(info_ok ? info.duration_ms : -1),
                     st->track_path);
            ui_toast("这首读不出总时长，暂不支持拖动定位", 2000);
            return false;
        }
    }
    /*
     * ★★ A 方案（2026-10，用户报"一拖进度条就立刻切下一曲"）：
     *
     *   会话**已经结束/正在收尾**时（歌刚好放完，你这时松手提交拖动 ✗），
     *   光让库挪一下读位置是没用的 —— 解码任务已经 EOF 退出，没人再解码 ⇒
     *   输出始终是空的 ⇒ 引擎照旧走"播放完成"分支 ⇒ 切下一曲 ✗。
     *   实测日志顺序（`local seek` 明明返回 OK 却表现为失败）：
     *
     *     I espaudiocore: decode finished: end of stream          ← ① 先 EOF
     *     I audio_eng: end of stream (waiting for output drain)
     *     I audio_seek: local seek -> 41864 ms                    ← ② seek 提交，返回 ESP_OK ✓
     *     I espaudiocore: decode task exiting
     *     I audio_eng: playback finished (gen 7)                  ← ③ 按"放完"处理 → 切歌 ✗
     *
     *   正确做法就是用户的意图："我要听 41.9 秒那里" ⇒ **按目标位置重新起播这首歌**。
     *   复用现成入口 `music_player_start_track_at(idx, resume_ms)`（它内部就是
     *   "起播 + 就绪后定位"，与进度记忆续播同一条路径 ✓，不新增状态 ✗）。
     *
     *   ⚠ 必须在下面的 `audio_service_seek_supported()` 那道门**之前**判断：
     *     会话已经结束 ⇒ `audio_engine_session_active()` 为假 ⇒ 那道门会给
     *     "定位失败：时长或码率未知" ✗，根本走不到这里 ✗。
     *   ⚠ 只对**本地播放**这么做：在线曲目不在本地下标里，`st->index` 指向的是别的歌 ✗。
     */
    {
        const bool local_now = (!st->on_mode || st->on_stream_local);
        if (local_now && st->index >= 0 && !music_loading() && !audio_service_is_playing()) {
            ESP_LOGW(TAG, "seek 落在已结束的会话上：按目标 %u ms 重新起播（idx=%d）",
                     (unsigned)target, st->index);
            ui_toast("重新播放并定位…", 1200);
            music_player_start_track_at(st->index, target);
            return true;
        }
    }
    /*
     * 能不能拖**只问服务**（`audio_service_seek_supported()`），不再按扩展名预拦：
     *   - **本地文件恒真** —— vendored 库里每个解码器都实现了 `seek_ms()`
     *     （WAV 按字节率、Vorbis `ov_pcm_seek`、Opus `op_pcm_seek`、M4A 走 mp4 demuxer、
     *     FLAC/MP3 各有实现），所以 **wav / ogg / opus / m4a 本地也能拖**；
     *     万一某个容器解不了，`audio_service_seek_ms()` 会返回错误，下面那句
     *     "定位失败：…" 会如实告诉用户，而不是提前把功能挡掉。
     *   - 在线流要求"容器能从流中间重开 + 已知总长与时长"（见 audio_seek.cc）。
     *
     * ⚠ 2026-10 用户问"opus/wav/ogg 为什么不能拖" —— 根因就是这里原先把
     *   `music_track_seek_unsupported()`（一张**流式**容器名单）也用来挡本地文件。
     *   那张名单现在改名为 `music_container_needs_seekable_source()`，
     *   只用于"在线歌要不要先下载再播"。
     */
    if (music_loading()) {
        ui_toast("正在加载，稍后再拖动", 1400);
        return false;
    }
    if (!audio_service_seek_supported()) {
        /* 在线 vs 本地给不同的话：在线的限制来自"容器能不能从流中间重开 / 总长与时长
         * 有没有就绪"，说成"码率未知"会让人摸不着头脑（FLAC 流就是被容器挡的）。 */
        const bool streamed = (st->on_mode && !st->on_stream_local);
        ui_toast(streamed ? "这首在线歌暂不能拖动（需先完整缓冲）"
                          : "定位失败：时长或码率未知", 1800);
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

