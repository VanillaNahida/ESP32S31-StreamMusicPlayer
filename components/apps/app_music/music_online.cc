/*
 * music_online.cc —— 在线曲目：队列所有权 / 异步准备 / 地址重刷 / 起播
 *
 * 搬迁自 app_music.cc 的 146–318、1970–2374。注释原样保留 ——
 * 那些"为什么不能在这里 free""为什么要序号校验"的记录拆开后更容易被忽略。
 */
#include "music_online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_service.h"
#include "media_meta.h"
#include "play_state.h"
#include "storage_service.h"
#include "ui_core.h"

#include "music_cover.h"
#include "music_logic.h"
#include "music_player.h"
#include "music_state.h"
#include "music_view_player.h"

static const char *TAG = "app_music";

/* 在线曲目准备任务的栈：HTTP + TLS 不浅。
 * 实测 6 KB 会在 TLS/证书校验里爆 "Stack protection fault"
 * （Guru 定位到 heap_caps_aligned_alloc ← mbedtls 握手），所以给 12 KB；
 * 任务只在准备期间存在，跑完就 vTaskDelete。 */
#define ONLINE_TASK_STACK    (12 * 1024)

/* 当前的在线队列（songs 数组本体由本模块托管，见 music_online.h 的产权说明） */
static const online_song_t *on_songs(void)
{
    return (const online_song_t *)music_state()->on_songs;
}

/* 当前曲目是不是在线曲目；是则把歌曲 id 写进 out（红心用）。
 * 只读 music_state + 队列数组，任何任务都能调。 */
bool music_online_current_id(char *out, size_t cap)
{
    if (!out || cap < 2) {
        return false;
    }
    out[0] = '\0';
    const music_state_t *st = music_state();
    if (!st->on_mode) {
        return false;                       /* 本地队列：没有网易云 id */
    }
    const online_song_t *songs = on_songs();
    if (!songs || st->index < 0 || (size_t)st->index >= st->on_count) {
        return false;
    }
    if (songs[st->index].id == 0) {
        return false;
    }
    snprintf(out, cap, "%u", (unsigned)songs[st->index].id);
    return true;
}

/* 本首已经"重刷过几次播放地址"。
 *
 * 网易云给的播放地址是**带时间戳 + 签名**的（`.../20261010132238/...mp3?vuutv=...`），
 * 过一阵就失效：打开流时拿到 403。这种失败**不该跳歌**，重新解析一次地址就能接着放。
 * 计数在 music_state.url_retry（起播成功或换了歌就清零；每首最多
 * MUSIC_ONLINE_URL_RETRY_MAX 次，防止死循环）。 */

/*
 * 丢弃"已准备好、还没起播"的在线结果（封面/歌词缓冲也一并释放）。
 * 只在 LVGL 任务里调：缓冲由工作线程发布、LVGL 任务独占释放，
 * 发布点在工作线程最后几条指令里，最坏情况（发布窗口内恰好切歌）由
 * on_ready_seq 的序号校验兜住 —— 过期的 ready 在 tick 里只会被丢弃释放，
 * 不会再被消费。
 */
void music_online_discard_ready(void)
{
    music_state_t *st = music_state();
    st->on_ready     = false;
    st->on_ready_idx = -1;
    if (st->on_cover_data) {
        free(st->on_cover_data);
        st->on_cover_data = NULL;
        st->on_cover_len  = 0;
    }
    if (st->on_lrc_data) {
        free(st->on_lrc_data);
        st->on_lrc_data = NULL;
        st->on_lrc_len  = 0;
    }
}

/* ------------------------------------------------- 在线队列的所有权（2026-10）
 *
 * on_songs 由 app_online 交过来。**换队列时不能由 app_online 直接 free**：
 * 用户"点歌 → 又连着打开两个别的歌单"之后，播放器还存着旧数组的指针，
 * 上/下一首与自动续播就会读已释放内存（曲名乱码/串到别的歌、甚至崩溃）。
 * 所以 app_online 换队列时调 app_music_online_detach()：本模块接管那块内存，
 * 等**自己换队列或退回本地播放**时再释放。
 */
void music_online_owned_drop(void)
{
    music_state_t *st = music_state();
    if (st->on_owned && st->on_owned != st->on_songs) {
        free((void *)st->on_owned);
        st->on_owned = NULL;
    }
}

void music_online_detach(const online_song_t *songs)
{
    music_state_t *st = music_state();
    if (!songs) {
        return;
    }
    if (st->on_mode && st->on_songs == (const void *)songs) {
        music_online_owned_drop();    /* 上一份托管的先放掉（此时它已不是当前队列） */
        st->on_owned = songs;
        ESP_LOGD(TAG, "online queue handed over (still in use)");
        return;
    }
    ESP_LOGD(TAG, "online queue released (unused)");
    free((void *)songs);
}

/* ------------------------------------------------------------- 在线曲目准备 */

typedef struct {
    online_song_t song;      /* 拷贝一份，工作线程不再读共享数组 */
    int           idx;
    bool          prefetch;  /* 只是提前缓存，不下发起播 */
    uint32_t      seq;       /* 发起时的请求序号 */
} online_req_t;

static void online_prepare_task(void *arg);

bool music_online_busy(void)
{
    return music_state()->on_busy;
}

/* 起一个准备任务（LVGL 任务里调；HTTP+TLS 不能放在 LVGL 任务里） */
void music_online_prepare_kick(void)
{
    music_state_t *st = music_state();
    if (st->on_busy || st->on_req < 0 || !st->on_songs) {
        return;
    }
    const int idx = st->on_req;
    st->on_req = -1;
    if (idx < 0 || (size_t)idx >= st->on_count) {
        return;
    }
    online_req_t *req = (online_req_t *)malloc(sizeof(online_req_t));
    if (!req) {
        st->on_failed = true;
        return;
    }
    req->song = on_songs()[idx];
    req->idx  = idx;
    req->prefetch = st->on_prefetch;
    req->seq      = st->on_seq;
    st->on_prefetch = false;
    st->on_busy = true;
    if (xTaskCreate(online_prepare_task, "on_fetch", ONLINE_TASK_STACK, req, 2, NULL) != pdPASS) {
        st->on_busy   = false;
        st->on_failed = true;
        free(req);
        ESP_LOGW(TAG, "online fetch task create failed");
    }
}

static void online_prepare_task(void *arg)
{
    music_state_t *st = music_state();
    online_req_t *req = (online_req_t *)arg;
    char stream[MUSIC_STREAM_MAX] = {0};
    char path[STORAGE_PATH_MAX] = {0};

    /* 预取只抓"下一首"的封面/歌词（几十 KB），音频等到真要播时再解析 */
    if (req->prefetch) {
        online_song_prepare_stream(&req->song);
        ESP_LOGI(TAG, "prefetched meta for '%s'", req->song.title);
        st->on_busy = false;
        free(req);
        vTaskDelete(NULL);
        return;
    }

    /* 1) 解析 302 拿最终音频地址（不下载）→ 交给播放器流式播放 */
    st->on_dur_ms = 0;
    esp_err_t ret = online_song_stream_info(&req->song, stream, sizeof(stream),
                                             NULL, &st->on_dur_ms);
    uint8_t local_only = 0;
    /*
     * ★ 换解码库后新增的一道门（2026-10）：**有些容器根本不能"边下边解"**。
     *
     * libs/espaudiocore 的 ringbuf 输入是纯顺序流，而 M4A/MP4 要 moov 索引、
     * OGG/Opus 要每个逻辑流开头的 codec setup 头 —— 从流中间接不上，库会直接
     * 返回"不支持"。这类歌必须先整首下载到 SD 再当本地文件播（本地文件源可 seek，
     * 那三种容器就都能解）。
     *
     * 判据是 `music_container_needs_seekable_source()`（那张"顺序流接不上"的名单）。
     * ⚠ 它**只**服务于这个决定（在线要不要先下载）。"能不能拖进度条"是另一件事，
     *   由 `audio_service_seek_supported()` 判 —— 本地文件全都能拖（各解码器都实现了
     *   seek_ms），别拿这张名单去挡拖动（2026-10 就是这么挡错的）。
     */
    if (ret == ESP_OK && music_container_needs_seekable_source(stream)) {
        ESP_LOGI(TAG, "容器不支持流式解码，改走先下载再播: %s", stream);
        ret = online_song_prepare(&req->song, path, sizeof(path));
        local_only = 1;
    } else if (ret != ESP_OK) {
        /* 2) 流式地址拿不到（版权/网络）→ 退回"先下载再播" */
        ESP_LOGW(TAG, "stream url failed (%s), fallback to download", esp_err_to_name(ret));
        ret = online_song_prepare(&req->song, path, sizeof(path));
        local_only = 1;
    }
    /*
     * ⚠ 原来这里还有一个 "流式地址拿得到" 的 else 分支，只调一句
     *   online_song_prepare_stream(&req->song);
     * 那是**纯冗余**：紧跟着的 207/208 行已经在取封面 + 歌词了，而
     * online_song_prepare_stream() 干的就是"取封面、取歌词、再各 free 掉"。
     * 缓存命中时它不花钱，但**失败时会真的再下一次** —— 真机上就表现为
     * 同一张封面 5 秒内下两遍、两遍都失败（`read error :-0x0087`），
     * 白白多占十几秒 Wi-Fi。删掉，让封面只有"取一份给播放器"这一条路。
     */
    /* 封面/歌词各取一份拷贝（PSRAM），起播时直接给解码/解析用 */
    uint8_t *jpg = NULL;
    size_t   jpg_len = 0;
    char    *lrc = NULL;
    size_t   lrc_len = 0;
    online_song_cover_data(&req->song, &jpg, &jpg_len);
    online_song_lyrics_data(&req->song, &lrc, &lrc_len);

    /* 期间用户又点了别的歌：立刻收工，把"正在准备"让出去，
     * 让新的那首尽快（下一个 tick）开始解析 */
    if (req->seq != st->on_seq) {
        ESP_LOGI(TAG, "abort stale online fetch (idx=%d)", req->idx);
        free(jpg);
        free(lrc);
        st->on_busy = false;
        free(req);
        vTaskDelete(NULL);
        return;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "online prepare failed: %s", esp_err_to_name(ret));
        free(jpg);
        free(lrc);
        st->on_failed = true;
    } else if (req->seq != st->on_seq) {
        ESP_LOGI(TAG, "drop stale online result (idx=%d)", req->idx);
        free(jpg);
        free(lrc);
    } else {
        snprintf(st->on_stream, sizeof(st->on_stream), "%s", local_only ? path : stream);
        st->on_stream_local = (local_only != 0);
        /* 封面/歌词缓冲：**所有权转给 LVGL 任务**（只有它在消费时释放，
         * 见 on_cover_data 的注释与 music_online_discard_ready） */
        st->on_cover_data = jpg;
        st->on_cover_len  = jpg_len;
        st->on_lrc_data   = lrc;
        st->on_lrc_len    = lrc_len;
        st->on_ready_idx  = req->idx;
        st->on_ready_seq  = req->seq;
        st->on_ready      = true;      /* 最后置位：上面的字段先写好 */
    }
    st->on_busy = false;
    free(req);
    vTaskDelete(NULL);
}

/*
 * 播放地址过期 → 重新解析一次地址（LVGL 任务上下文，从 tick 里调）。
 *
 * 网易云给的流地址带时间戳和签名（`.../20261010132238/....mp3?vuutv=...`），
 * 放一阵或过一会儿再点回来就 403。到 app 这一层只剩一个 AUDIO_EVT_ERROR
 * （**拿不到状态码**），所以策略是：在线流媒体只要出错，先重刷一次地址再放 ——
 * 刷新后仍然失败才按"这首放不了"跳过。
 *
 * 复用点歌那条准备链路（解析 302 → 封面/歌词 → ready → tick 起播）：
 * 封面/歌词在 online_service 里本来就按歌缓存，重刷基本只花解析 URL 的时间。
 */
bool music_online_url_refresh(void)
{
    music_state_t *st = music_state();
    if (!st->on_mode || !st->on_songs || st->index < 0 || (size_t)st->index >= st->on_count) {
        return false;                       /* 本地曲目/没有在线队列：维持原行为 */
    }
    if (st->on_busy) {
        /*
         * 已经有一次解析在跑（多半是用户刚点了别的歌、失败的那首是旧的）：
         * 等它的结果就行，既不叠一个请求、也**别把当前这首当成"放不了"跳掉**。
         * 返回 true 只是告诉调用方"不要 advance"，不重复弹提示。
         */
        ESP_LOGI(TAG, "online player error while preparing: keep waiting");
        return true;
    }
    if (st->url_retry >= MUSIC_ONLINE_URL_RETRY_MAX) {
        ESP_LOGW(TAG, "online url refresh gave up (tried %d)", st->url_retry);
        return false;
    }
    st->url_retry++;
    /*
     * ★ 记住"从哪儿断的"：刷新成功后由 tick 定位回去（on_resume_ms），
     *   别让用户听了 40 秒又被扔回 0 秒。
     *   门槛 5 秒：刚起播就出错的话，从头放更干净（也不值得开一次 Range 重连）。
     *   注意 elapsed_ms 只在**真的出声**期间累加（见 tick 里的墙钟），
     *   卡住/无声那段时间不会让它虚涨 —— 所以它天然就是"断点"。
     */
    if (st->elapsed_ms > 5000) {
        st->on_resume_ms   = st->elapsed_ms;
        st->on_resume_wait = 0;
    } else {
        st->on_resume_ms = 0;
    }
    ESP_LOGI(TAG, "online url stale/expired (403?), refreshing (try %d/%d), resume at %u ms",
             st->url_retry, MUSIC_ONLINE_URL_RETRY_MAX, (unsigned)st->on_resume_ms);
    ui_toast("音乐链接已过期，正在刷新链接...", 2000);
    music_view_player_set_buffering_hint();

    /* 与点歌同一条路：旧结果作废 → 重新下单 → 工作线程解析 → ready → tick 起播 */
    st->on_seq = st->on_seq + 1;
    music_online_discard_ready();
    st->on_failed = false;
    st->on_req    = st->index;
    music_online_prepare_kick();
    return true;
}

/*
 * 起播一首在线歌（LVGL 任务上下文）。
 * uri 可能是 https 流地址（on_stream）或本地缓存路径（下载兜底）：
 * audio_service_play_uri() 两种都认。
 * 封面/歌词走内存缓冲（on_cover_data / on_lrc_data，PSRAM，不落 SD）：
 *   歌词在这里解析完就释放；封面转交给 cover 的解码请求（请求独占）。
 */
void music_online_start_track(int idx)
{
    music_state_t *st = music_state();
    const char *uri = st->on_stream;
    if (!st->on_songs || idx < 0 || (size_t)idx >= st->on_count || !uri || !uri[0]) {
        return;
    }
    const online_song_t *song = &on_songs()[idx];

    music_state_begin_online(st, idx, uri, st->on_dur_ms);
    st->seek_in_flight = false;

    /* 歌词：直接从内存里的 LRC 文本解析（不再读 SD 上的 .lrc 文件）。
     * 解析会把每一行拷进歌词缓冲，所以这里解析完就可以释放缓冲。 */
    bool lrc_ok = false;
    if (st->on_lrc_data) {
        lrc_ok = music_view_player_lyric_parse(st->on_lrc_data);
        free(st->on_lrc_data);
        st->on_lrc_data = NULL;
        st->on_lrc_len  = 0;
    }
    /* 下载兜底（本地音频文件）：音频文件旁/全局 lrc 目录若还有边车歌词，兜底找一次 */
    if (!lrc_ok && st->on_stream_local) {
        lrc_ok = music_view_player_lyric_load(uri);
    }
    if (!lrc_ok) {
        music_view_player_lyric_reset();
    }
    music_view_player_set_now_playing(song->title, song->artist);
    music_view_player_lyric_rebuild();

    /*
     * ★ 重新挂上"缓冲中"提示：上面的 lyric_rebuild()/lyric_apply_mode() 会把说明行
     * 交回歌词，但这时音频的流**还没开始下**（audio_service_play_uri 就在下面几行）
     * —— 而"缓冲中 xx%"最有意义的就是接下来这一段。tick 会把它刷成百分比，
     * 等真的出声（audio_service_output_active）再交回 lyric_apply_mode()。
     */
    music_view_player_set_buffering_hint();

    /*
     * 封面：**只有真的换了一首才丢掉重来**（music_cover_needs_reload）。
     * 同一首重起播（地址过期重连 / 加载中点了播放）时屏上那张就是对的，
     * 丢掉它反而可能永久拿不回来 —— 在线封面的内存缓冲是一次性消费的。
     */
    if (music_cover_needs_reload(idx)) {
        music_view_player_cover_show(NULL, 0, 0);
        music_cover_reap();
        music_cover_request(idx);
        music_cover_kick();
    }

    music_view_player_reset_progress();
    music_view_player_set_elapsed_text(0);

    /* 进度记忆只对下载兜底（本地文件）做：流地址是带时效签名的，存下来没意义 */
    if (st->on_stream_local) {
        music_player_save_progress(0);
        play_state_commit();
    }

    if (audio_service_play_uri(uri) != ESP_OK) {
        ui_toast("在线歌曲播放失败（网络或格式不支持）", 2200);
        return;
    }
    /*
     * ★ 告诉服务"这首多长"（在线拖动定位的前提）。
     *
     * 服务要靠**平均码率**把时间换成字节，而流上没有文件可探测 —— 只能用
     * "总字节（HTTP Content-Length）× 8000 ÷ 时长"。时长只能从这里给。
     * **必须在 play_uri() 之后**：换曲会把上一首清掉。
     */
    audio_service_set_duration_ms((uint32_t)st->dur_ms);
    st->playing = true;
    music_view_player_set_play_icon(true);
    ESP_LOGI(TAG, "online track %d/%u: %s - %s (%s)",
             idx + 1, (unsigned)st->on_count, song->title, song->artist,
             st->on_stream_local ? "cached" : "streaming");
}

/*
 * ★ 在线切歌的"界面先行"：**先把播放器页切成目标歌曲，再显示缓冲信息**。
 *
 * 为什么要这样（用户 2026-10 提的）：点歌到真正起播之间有 1~3 秒
 * （解析 302 地址 + 抓封面 + 取歌词，都在工作线程里），这段时间如果界面上
 * 还留着**上一首**的标题/歌手/封面/歌词，看起来就是"点了没反应、切歌没生效"。
 * 现在点下去就先把目标歌曲的信息铺上去，随后再由 tick 在右栏说明行上
 * 打"缓冲中… xx%"。
 *
 * 只动 UI 与几个状态量，**不碰播放器**：停旧歌由调用方负责（先停再调本函数），
 * 封面/歌词等音频就绪后由 music_online_start_track() 再写一遍同一套字段（幂等）。
 */
void music_online_switch_ui_to(int idx)
{
    music_state_t *st = music_state();
    if (!st->on_songs || idx < 0 || (size_t)idx >= st->on_count) {
        return;
    }
    const online_song_t *song = &on_songs()[idx];

    music_view_player_set_now_playing(song->title, song->artist);
    /* 总时长/进度都还不知道：给占位值（tick 里对 dur_ms == 0 处处有保护） */
    music_view_player_set_total_text_placeholder();
    music_view_player_set_elapsed_text(0);
    music_view_player_reset_progress();
    st->elapsed_ms = 0;
    st->dur_ms     = 0;
    st->buf_show   = false;      /* 让 set_buffering_hint() 重新置位（下面调用方会调） */
    st->seek_buf_pending = false;
    st->seek_in_flight   = false;

    /* 封面：先退回占位（别把上一首的封面留在屏上），就绪后再解新的 */
    music_view_player_cover_show(NULL, 0, 0);
    music_cover_reap();
    music_cover_request(idx);

    /* 歌词：先清空并重画（此时新歌词还没抓到），别继续显示上一首的歌词；
     * lyric_rebuild() 会顺带把右栏说明行切成"暂无歌词"，紧接着调用方的
     * set_buffering_hint() 会把它改成"缓冲中…"。 */
    music_view_player_lyric_reset();
    music_view_player_lyric_rebuild();
}

/*
 * 在线音乐 app 的入口：把一份在线歌单交给播放器并从第 idx 首开始播。
 * **必须在 LVGL 任务上下文调用**（会更新播放器页控件）。
 */
void music_online_play(const online_song_t *songs, size_t count, size_t idx)
{
    music_state_t *st = music_state();
    if (!songs || count == 0 || idx >= count) {
        return;
    }
    st->on_songs = songs;
    st->on_count = count;
    st->on_mode  = true;
    /* 队列已经换成 songs：托管中的旧队列（若有）现在可以放了。
     * 顺序很重要 —— 先让 on_songs 指向新数组，再 free 旧的。 */
    music_online_owned_drop();
    st->index    = (int)idx;
    st->on_seq   = st->on_seq + 1;
    st->url_retry = 0;             /* 点了新歌：地址重刷次数从头算 */
    st->on_resume_ms = 0;          /* 用户点了这一首：从头放（不续播断点） */
    st->eof_retry    = 0;
    /*
     * ★ 在线队列只支持 列表循环 / 单曲循环：本地开着"随机"时切到在线要先落回列表，
     *   否则随机在在线队列上不会生效（用户在本地设了随机、进在线却发现顺序播放，
     *   只会以为是 bug）。这里不弹 Toast —— 紧接着的"缓冲中"Toast 会把它顶掉。
     *   播放器页的模式图标与来源徽标会立刻反映出来，看得见。
     */
    if (st->play_mode == MUSIC_MODE_SHUFFLE) {
        st->play_mode = MUSIC_MODE_LIST;
        ESP_LOGI(TAG, "shuffle -> list (switch to online queue)");
    }
    music_view_player_apply_play_mode();
    music_view_player_apply_src_badge();
    music_online_discard_ready();      /* 点歌即弃：旧结果（若有）不再起播 */
    st->on_failed = false;
    st->on_req   = (int)idx;

    /*
     * ★ 点击必须**立刻有反馈**：
     *   1) 先把正在播的那首停掉（否则旧歌会一直唱到新歌的"地址解析 + 封面 +
     *      歌词"全都就绪，实测这张网要 1~3 秒，听感就是"点了没反应"）；
     *   2) **再把播放器页切成目标歌曲**（标题/歌手/封面占位/歌词清空/进度归零）；
     *   3) 最后才显示缓冲信息（说明行"缓冲中… xx%"，tick 里刷百分比）。
     * 顺序很关键：用户要的是"先看到切到了哪首，再看到它在缓冲"。
     */
    audio_service_stop();
    st->playing = false;
    music_view_player_set_play_icon(false);
    music_online_switch_ui_to((int)idx);
    ui_toast("缓冲中，请稍后...", 1500);
    music_view_player_set_buffering_hint();
    music_online_prepare_kick();
}
