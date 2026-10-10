/*
 * audio_service.cc —— 播放服务实现（ESP-GMF / esp_audio_simple_player）
 *
 * 管线：file reader → decoder → (resample 48k / ch / bit) → out 回调
 *       → esp_codec_dev_write() → I2S/TDM → ES8389 → NS4150B ×2 → 左右扬声器
 *
 * 关键事实（见 AGENTS.md）：
 *   - 本板 I2S 固定 48 kHz / 16 bit / 2ch TDM，所以把 GMF 重采样目标设成 48k
 *   - GMF file IO 的 URI 解析会剥掉 "scheme://"，所以必须写成 "file:///sdcard/..."
 *   - 播放与录音共用同一组 I2S/TDM，播放时不能录音
 */
#include "audio_service.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"   /* get_pipeline / register_io */
#include "esp_gmf_io_http.h"                   /* 自己注册 http IO：放大流缓冲 */
#include "esp_gmf_io_file.h"                   /* 自己注册 file IO：本地加预读缓冲 */
#include "esp_timer.h"                         /* 播放健康度日志的时间戳 */
#include "esp_http_client.h"                   /* 事件钩子里读 Content-Length */
#include "esp_crt_bundle.h"                    /* 音频流也做证书校验 */
#include "esp_codec_dev.h"
#include "bsp.h"
#include "media_meta.h"
#include "bt_service.h"
#include "play_state.h"    /* 音量记忆（NVS） */

static const char *TAG = "audio_svc";

/* 板级固定输出格式 */
#define OUT_RATE_HZ   (48000)
#define OUT_CHANNELS  (2)
#define OUT_BITS      (16)

static esp_asp_handle_t  s_player;
static SemaphoreHandle_t s_lock;
static audio_event_cb_t  s_evt_cb;
static void             *s_evt_user;
static volatile bool     s_running;
/* 当前音量：开机时取 NVS 里的记忆值（没有记录才用默认 55） */
static int               s_volume = PLAY_STATE_VOL_DEFAULT;
static audio_info_t      s_info;
static volatile bool     s_info_valid;
static size_t            s_data_offset;   /* 音频数据起始偏移（MP3 = ID3v2 标签长度） */
static audio_out_t       s_out = AUDIO_OUT_SPEAKER;   /* 当前输出通道（out 回调用它分流） */
/* 当前播放的 VFS 路径：MUSIC_INFO 里若带不出码率，用它回文件里探测帧头 */
#define AUDIO_PATH_MAX   (600)
static char              s_cur_path[AUDIO_PATH_MAX];
/* 本首是否已经真的出声（PCM 写到过 codec/蓝牙）。out_write_cb 里置位、
 * 每次换曲清零；UI 用它把"缓冲中 xx%"换回歌词。 */
static volatile bool     s_out_flowing;
/* 在线流式的缓冲进度统计（见 stream_total_bytes / audio_service_stream_progress）：
 *   s_stream_total —— 事件钩子里的 Content-Length。⚠ 读方向（流式播放）下
 *                     HTTP_STREAM_FINISH_REQUEST 根本不来，所以它**恒为 0**，
 *                     总长度一律走 stream_total_bytes()（问 IO 要）；
 *   s_stream_got   —— ON_RESPONSE 累加的"想要多少字节"，只当兜底；
 *   s_stream_io    —— 我们注册进 ASP 池子的那个 http IO：位置/总长度都问它最准
 *                     （esp_gmf_io_get_pos 是"解码器已消费到哪"，esp_gmf_io_get_size 是总长）。 */
static volatile uint32_t s_stream_total;
static volatile uint32_t s_stream_got;
static esp_gmf_io_handle_t s_stream_io;
/*
 * 上层（app_music）告知的本曲时长，毫秒，0 = 未知。
 *
 * GMF 的 MUSIC_INFO 里 duration 拿不到（本地文件另外用 media_meta 探测），所以
 * **在线流只能靠这个值**：seek 的"时间→字节"要用平均码率，而流上没有文件可探测，
 * 只能算"总字节（Content-Length）× 8000 ÷ 时长"。换曲时清零（见 play_uri_locked）。
 */
static volatile uint32_t s_dur_hint_ms;

/* --------------------------------------------------- 播放时间轴 / 实测码率 */
/*
 * 输出 PCM 的固定格式：管线最后是 rate_cvt(48k) → ch_cvt → bit_cvt → out，
 * 也就是**永远是 48 kHz / 16 bit / 2ch** —— 所以"写了多少字节"能直接折算成时长：
 *   48000 × 2ch × 2B / 1000ms = 192 B/ms
 * （改 sdkconfig / 板级播放参数时要跟着改这里。）
 */
#define OUT_PCM_BYTES_PER_MS  (192)

/*
 * 时间轴位置 = s_tl_base_ms + 自基准点以来写出的 PCM 时长。
 * 基准点：换曲时归零；seek 成功后设成目标时刻。
 * 用"累计字节"而不是"每块 += 时长"是为了避免整数除法逐块截断（一块 4096 B = 21.33 ms，
 * 逐块取整会攒出百分之几的漂移）。
 */
static volatile uint64_t s_pcm_bytes;
static volatile uint32_t s_tl_base_ms;
static volatile uint32_t s_stats_calls;    /* PCM 写入次数（播放健康度日志用） */
/* 输出侧诊断（见 out_write_cb）：写 codec 的累计耗时、两次回调的最大间隔 */
static volatile uint64_t s_stat_codec_us;
static volatile uint32_t s_stat_last_ms;
static volatile uint32_t s_stat_gap_max_ms;

/*
 * 实测码率（bit/s）：把"解码器消费掉的字节"除以"这段时间真正放出来的音频"。
 *
 * ⚠ `esp_gmf_io_get_pos()` 的口径（看源码确认过，别再猜）：它在
 *   `esp_gmf_io_release_read()` 里推进（→ esp_gmf_info_file_update_pos），也就是
 *   **消费者（解码器）release 掉一块数据**时才 +valid_size —— 是"解码器吃到哪了"，
 *   **不是"网络读到哪了"**。所以 Δpos/Δ时长 就是这段音频的真实码率，VBR 也成立。
 *
 * 用途：seek 的**相对定位**（向前拖时从当前位置往前推，见 seek_byte_for）。
 * 采样由 out_write_cb 里的 audio_stream_measure() 每次写 PCM 时顺带做：
 * 窗口 SEEK_MEAS_WINDOW_MS、跳过起播的 SEEK_MEAS_WARMUP_MS（那会儿解码器会抢读一大段，
 * 测出来会飙到几 Mbps），结果做 3:1 的 IIR 平滑（一次坏窗口不至于带偏）。
 */
#define SEEK_MEAS_WINDOW_MS   (1000)   /* 采样窗口 */
#define SEEK_MEAS_WARMUP_MS   (2000)   /* 跳过的起播抢读期 */
#define SEEK_REL_MIN_MS       (2000)   /* 小于这个的向前拖动不值得走相对定位 */

static uint32_t s_meas_bps;            /* 实测平均码率（0 = 还没测出来） */
static uint32_t s_meas_anchor_ms;      /* 上一个采样点的时间轴位置 */
static uint64_t s_meas_anchor_pos;     /* 上一个采样点的 IO（解码器）位置 */
static bool     s_meas_anchor_ok;      /* false = 下个采样点只做锚定、不算码率 */

/* 本曲的平均码率（定义在下面的"定位换算"一段）：
 * 采样时要拿它当"合理性上限"用，所以这里先声明。 */
static uint32_t seek_bitrate(void);

/* 取"管道真正在用的输入 IO"（定义在 ensure_stream_io 之后，起播时调用）：
 * ⚠ 不能用手上那个注册进池子的句柄 —— 池子会 esp_gmf_obj_dupl() 复制一份给管道。 */
static void stream_io_update(void);

/* seek 落点核对（见 audio_service_seek_landing_ms）：请求值 + 一次性标志 */
static volatile uint32_t s_seek_req_ms;      /* 用户要的时间点 */
static volatile uint32_t s_seek_req_bps;     /* 换算用的码率（核对要用同一个） */
static volatile bool     s_seek_landing_pending;  /* 等"seek 之后第一块 PCM" */
static volatile bool     s_seek_landing_ready;    /* 落点已经算好，等上层取走 */
static volatile uint32_t s_seek_landing_ms;       /* 按字节位置反推出来的时间点 */

/*
 * 每 5 秒打一行"播放健康度"（2026-10 加，用来回答"卡顿时解码器到底在不在解码"）。
 *
 * 判读方法（本地文件卡顿、CPU 却很低时看这一行就够）：
 *   - `PCM xx KB/s`：真正写到 codec 的**音频吞吐**。48 kHz/16bit/2ch 满速是 192 KB/s；
 *     断断续续时这个值会明显低于 192（例如 30~80），说明"没在出声"的时间占了大头；
 *   - `io=…`：IO 已消费到的字节（本地是文件偏移，在线是流位置）——
 *     它**几乎不动**但 PCM 也上不去 ⇒ 任务卡在**读 SD/网络**上（不是解码慢）；
 *     它一直在涨、PCM 却上不去 ⇒ 解码/转换环节的问题。
 *   - `flow=0`：当前这一刻没在出 PCM（缓冲/卡住）。
 * 只打 INFO，5 s 一行，不会刷屏。
 */
static void audio_stats_log(void)
{
    static uint32_t last_ms;
    static uint64_t last_pcm;
    static uint32_t last_calls;

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (last_ms == 0) {
        last_ms    = now;
        last_pcm   = s_pcm_bytes;
        last_calls = s_stats_calls;
        return;
    }
    if (now - last_ms < 5000) {
        return;
    }
    const uint32_t dt_ms  = now - last_ms;
    const uint64_t dpcm   = s_pcm_bytes - last_pcm;
    const uint32_t dcalls = s_stats_calls - last_calls;
    const uint32_t kbps   = (uint32_t)((dpcm * 1000ULL) / dt_ms / 1024ULL);   /* KB/s */

    uint64_t io_pos = 0;
    if (s_stream_io) {
        esp_gmf_io_get_pos(s_stream_io, &io_pos);
    }
    ESP_LOGI(TAG, "play stats: PCM %u KB/s (%llu B in %u ms), io=%llu B, calls=%u, flow=%d, br=%d/%u | "
             "out %llu ms in codec, gap max %u ms, audio %u ms",
             (unsigned)kbps, (unsigned long long)dpcm, (unsigned)dt_ms,
             (unsigned long long)io_pos, (unsigned)dcalls,
             (int)s_out_flowing, s_info.bitrate, (unsigned)s_meas_bps,
             (unsigned long long)(s_stat_codec_us / 1000ULL), (unsigned)s_stat_gap_max_ms,
             (unsigned)(dpcm / 192ULL));
    s_stat_codec_us  = 0;
    s_stat_gap_max_ms = 0;
    last_ms    = now;
    last_pcm   = s_pcm_bytes;
    last_calls = s_stats_calls;
}

/* ---------------------------------------------------------------- out 回调 */

static uint32_t timeline_now(void)
{
    return s_tl_base_ms + (uint32_t)(s_pcm_bytes / OUT_PCM_BYTES_PER_MS);
}

/*
 * 每写一块 PCM 调一次（out_write_cb 里）：累计时长 + 定期采一次实测码率。
 * 必须便宜：全是整数运算，且仅在跨过窗口时才会去读 IO 位置。
 */
static void audio_stream_measure(int data_size)
{
    if (data_size <= 0) {
        return;
    }
    s_pcm_bytes += (uint64_t)data_size;
    s_stats_calls = s_stats_calls + 1;   /* volatile 不能 ++（-Werror=volatile） */
    audio_stats_log();

    /* 落点核对：seek 之后的第一块 PCM —— 这时 IO 位置就是"我们实际落到哪了"。
     * 用**同一个码率**把字节反推回时间，和请求值一比就知道落点准不准
     * （服务端悄悄忽略了 Range、或者按别的范围给数据时，这里能看出来）。 */
    if (s_seek_landing_pending) {
        s_seek_landing_pending = false;
        uint64_t pos = 0;
        if (s_stream_io && esp_gmf_io_get_pos(s_stream_io, &pos) == ESP_GMF_ERR_OK && pos > 0 &&
            s_seek_req_bps > 0) {
            const uint64_t body = (pos > s_data_offset) ? (pos - s_data_offset) : 0;
            s_seek_landing_ms   = (uint32_t)((body * 8000ULL) / s_seek_req_bps);
            s_seek_landing_ready = true;
            ESP_LOGI(TAG, "seek landed: byte %llu -> %u ms (asked %u ms, diff %d ms)",
                     (unsigned long long)pos, (unsigned)s_seek_landing_ms,
                     (unsigned)s_seek_req_ms,
                     (int)((int32_t)s_seek_landing_ms - (int32_t)s_seek_req_ms));
        }
    }

    if (!s_stream_io) {
        return;                                     /* 本地文件用探测出来的平均码率 */
    }
    const uint32_t played = timeline_now();
    if (played < SEEK_MEAS_WARMUP_MS) {
        s_meas_anchor_ok = false;                   /* 起播抢读期：只等着，不测 */
        return;
    }
    if (played - s_meas_anchor_ms < SEEK_MEAS_WINDOW_MS) {
        return;
    }
    uint64_t pos = 0;
    if (esp_gmf_io_get_pos(s_stream_io, &pos) != ESP_GMF_ERR_OK) {
        return;
    }
    if (s_meas_anchor_ok && pos > s_meas_anchor_pos && played > s_meas_anchor_ms) {
        const uint64_t k = ((pos - s_meas_anchor_pos) * 8000ULL) / (played - s_meas_anchor_ms);
        /*
         * 两道合理性检查，缺一不可：
         *   - 绝对值 8 kbps ~ 2 Mbps；
         *   - 与"全曲平均码率"相差不超过 4 倍 —— 这道是防**跨 seek 的脏采样**：
         *     seek 会把 IO 位置一次性跳很远，万一那一瞬间 anchor 标志还没被清掉
         *     （LVGL 任务与 out 回调是并发写的），算出来的 k 会大得离谱，
         *     不拦的话会把后面几次定位带偏。
         */
        const uint32_t avg = seek_bitrate();
        const bool sane_abs = (k >= 8000ULL && k <= 2000000ULL);
        const bool sane_rel = (avg == 0) || (k <= (uint64_t)avg * 4ULL && k * 4ULL >= avg);
        if (sane_abs && sane_rel) {
            s_meas_bps = s_meas_bps ? (uint32_t)((s_meas_bps * 3ULL + k) / 4ULL) : (uint32_t)k;
        }
    }
    s_meas_anchor_ms  = played;
    s_meas_anchor_pos = pos;
    s_meas_anchor_ok  = true;
}

/*
 * 16-bit PCM 就地音量缩放（够用就好，不用浮点：S31 是 RISC-V，整数乘除更快）。
 * 带削波保护 —— 超过 ±32767 不夹的话会"炸音"（回绕成反相尖峰）。
 */
static void pcm16_scale(int16_t *p, int n, int vol)
{
    for (int i = 0; i < n; i++) {
        int32_t v = ((int32_t)p[i] * vol) / 100;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        p[i] = (int16_t)v;
    }
}

/*
 * GMF 把解码（并重采样）后的 PCM 交给我们。按当前输出通道分两条路：
 *   扬声器 → esp_codec_dev_write（ES8389）
 *   蓝牙   → bt_service_write_pcm（A2DP 源，非阻塞，满了丢帧）
 * 蓝牙这条路**必须**返回 data_size：返回短了 GMF 会当成写失败/结束，
 * 宁可丢掉一帧也不能让解码管线停。
 */
static int out_write_cb(uint8_t *data, int data_size, void *ctx)
{
    /* 有 PCM 真的送到 codec 了 —— 这就是"缓冲结束、开始出声"的信号，
     * UI 用它把"缓冲中 xx%"换回歌词（见 audio_service_output_active） */
    s_out_flowing = true;
    /* 时间轴与实测码率（见 s_meas_bps 的注释）——很便宜，就几次整数运算 */
    audio_stream_measure(data_size);
    /*
     * ★ 输出侧耗时与"块间隔"统计（2026-10，治"本地音频一卡一卡、CPU 却很低"）。
     *
     * 解码/重采样实测只占实时时间的 7~13% / 20%，整条链却只跑到 73% —— 时间花在**等**
     * 上。本地播放能等的地方只有两处：读文件（SD）与写输出（codec DMA）。
     *   - `codec_ms`：这一窗口里写 codec 累计花掉多少毫秒（它按 DMA 节拍阻塞，正常
     *     应该 ≈ 音频时长本身；明显更大就是 codec/I2S 慢）；
     *   - `gap_max`：两次回调之间的**最大间隔**（正常 ≈ 一个块的时长 ≈ 46 ms；
     *     出现几百毫秒的空档就是**被饿住了**——解码没跟上或任务被抢占）。
     * 与 ESP_GMF_FILE 的 `file read:` 那行一对，就能定位到底在等谁。
     */
    const int64_t t_out0 = esp_timer_get_time();
    if (s_stat_last_ms != 0) {
        const uint32_t gap = (uint32_t)(t_out0 / 1000) - s_stat_last_ms;
        if (gap > s_stat_gap_max_ms) {
            s_stat_gap_max_ms = gap;
        }
    }
    s_stat_last_ms = (uint32_t)(t_out0 / 1000);
    if (s_out == AUDIO_OUT_BLUETOOTH) {
        /*
         * ★ 蓝牙的音量必须在这里做**软件缩放**（2026-10 修"蓝牙设备音量调不动"）：
         *   扬声器路的音量作用在 ES8389（bsp_audio_set_volume），而蓝牙路是
         *   out_write_cb → bt_service_write_pcm → A2DP 编码 → 耳机，
         *   **整条链上没人看 s_volume** —— 所以以前按音量键对蓝牙完全没反应
         *   （界面数字在变、耳机音量不动）。这里是唯一同时覆盖两条通路的点。
         */
        const int vol = (s_volume < 0) ? 0 : (s_volume > 100 ? 100 : s_volume);
        if (vol <= 0) {
            return data_size;          /* 静音：不往 A2DP 推数据，但照旧返回全长 */
        }
        if (vol < 100) {
            /* 16 bit 样本数 = 字节数/2（单声道/立体声交错都一样）；100% 时走原路，零开销 */
            pcm16_scale((int16_t *)(void *)data, data_size / 2, vol);
        }
        (void)bt_service_write_pcm(data, (size_t)data_size);
        s_stat_codec_us = s_stat_codec_us + (uint64_t)(esp_timer_get_time() - t_out0);
        return data_size;
    }
    esp_codec_dev_handle_t dev = (esp_codec_dev_handle_t)ctx;
    if (!dev) {
        return -1;
    }
    const int ret = esp_codec_dev_write(dev, data, data_size);
    s_stat_codec_us = s_stat_codec_us + (uint64_t)(esp_timer_get_time() - t_out0);
    return ret;
}

/* ---------------------------------------------------------------- 事件 */

static void notify(audio_evt_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt, s_evt_user);
    }
}

static int player_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (!pkt || !pkt->payload) {
        return 0;
    }

    if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
        esp_asp_music_info_t info = {0};
        if (pkt->payload_size >= (int)sizeof(info)) {
            memcpy(&info, pkt->payload, sizeof(info));
            s_info.sample_rate = info.sample_rate;
            s_info.bitrate     = info.bitrate;
            s_info.channels    = (uint8_t)info.channels;
            s_info.bits        = (uint8_t)info.bits;
            /* 时长：GMF 报不出来（一直是 0）；本地文件下面用探测值补，在线用上层告知的值 */
            s_info.duration_ms = s_dur_hint_ms;
            /*
             * ★ 本文件的码率**一律优先用自己探测的平均码率**（media_meta_probe_mp3
             *   读 Xing/Info 帧算平均，没有 Xing 时按帧头平均），只有当"没有文件可探测"
             *   （在线流式）时才用 GMF 报的值。
             *
             * 为什么必须这样（2026-10 用户报"多次拖动进度条后歌词高亮异常"）：
             *   `audio_service_seek_ms()` 把"时间"换成"字节"用的是**平均码率**
             *   （byte = offset + ms×bitrate/8000）。而 GMF 的 MUSIC_INFO 里给的是
             *   **解码器读到的第一帧**的码率 —— VBR 文件上它和平均码率能差 30% 以上，
             *   于是"拖到 3:00"实际落到了别的时刻；上层却把 s_elapsed_ms 直接对齐到
             *   **请求**的时刻，歌词高亮（和进度条）就与实际听到的位置对不上，
             *   越拖越歪。换成平均码率后，定位落点与时间轴一致，歌词自然对齐。
             *   CBR 文件两者相等，切成"总是优先探测"不会变差。
             */
            if (s_cur_path[0]) {
                audio_probe_t probe = {};
                /*
                 * ★ 用**通用**探测（2026-10 修"部分文件没时长 / 不能拖动"）：
                 *   以前只调 media_meta_probe_mp3()，它**只认 .mp3**，于是 FLAC/WAV/AAC
                 *   这类本地文件的 s_info 一直是 `0 bps, 0 ms` ——
                 *   界面总时长 `--:--`、进度条不动、拖动报"定位失败：码率未知"。
                 *   通用版按魔数分派（FLAC 走 STREAMINFO、WAV 走 fmt/data、ADTS 走帧头），
                 *   拿不到时仍返回错误，保持原逻辑。
                 */
                if (media_meta_probe_audio(s_cur_path, &probe) == ESP_OK) {
                    if (probe.bitrate > 0) {
                        if (info.bitrate > 0 && (int)probe.bitrate != info.bitrate) {
                            ESP_LOGI(TAG, "bitrate: GMF %d -> probe(avg) %u bps",
                                     info.bitrate, (unsigned)probe.bitrate);
                        }
                        s_info.bitrate = (int)probe.bitrate;
                    }
                    if (probe.duration_ms > 0) {
                        s_info.duration_ms = probe.duration_ms;
                    }
                    if (probe.sample_rate > 0) {
                        s_info.sample_rate = (int)probe.sample_rate;   /* GMF 漏报时补上 */
                    }
                }
            }
            s_info_valid       = true;
            ESP_LOGI(TAG, "music info: %d Hz %uch %ubit %d bps, %u ms",
                     info.sample_rate, info.channels, info.bits, s_info.bitrate,
                     (unsigned)s_info.duration_ms);
        }
        return 0;
    }

    if (pkt->type != ESP_ASP_EVENT_TYPE_STATE) {
        return 0;
    }

    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    memcpy(&st, pkt->payload,
           pkt->payload_size < (int)sizeof(st) ? pkt->payload_size : (int)sizeof(st));

    switch (st) {
    case ESP_ASP_STATE_RUNNING:
        s_running = true;
        bsp_audio_pa_enable(true);      /* 有声音了才开功放，避免静音期底噪 */
        notify(AUDIO_EVT_STARTED);
        break;
    case ESP_ASP_STATE_PAUSED:
        notify(AUDIO_EVT_PAUSED);
        break;
    case ESP_ASP_STATE_FINISHED:
        s_running = false;
        bsp_audio_pa_enable(false);
        notify(AUDIO_EVT_FINISHED);
        break;
    case ESP_ASP_STATE_STOPPED:
        s_running = false;
        bsp_audio_pa_enable(false);
        break;
    case ESP_ASP_STATE_ERROR:
        s_running = false;
        bsp_audio_pa_enable(false);
        ESP_LOGE(TAG, "player error state");
        notify(AUDIO_EVT_ERROR);
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------- 生命周期 */

esp_err_t audio_service_init(void)
{
    if (s_player) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* 音量记忆：必须在开 codec 之前就取出来，否则开机先是默认音量、
     * 再"跳"一下变到记忆值（听感上就是开机爆一下） */
    s_volume = play_state_volume_get(PLAY_STATE_VOL_DEFAULT);

    esp_err_t ret = bsp_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 播放前先把 codec 以板级格式打开（GMF 重采样目标与之一致） */
    ret = bsp_audio_open_output(OUT_RATE_HZ, OUT_CHANNELS, OUT_BITS);
    if (ret != ESP_OK) {
        return ret;
    }
    bsp_audio_set_volume(s_volume);
    bsp_audio_set_mute(true);          /* 未播放先静音，防爆音 */
    bsp_audio_pa_enable(false);

    /* 注意：C++ 不支持嵌套的指定初始化列表（.in.cb = ...），必须逐字段赋值 */
    esp_asp_cfg_t cfg = {};
    cfg.in.cb        = NULL;
    cfg.in.user_ctx  = NULL;
    cfg.out.cb       = out_write_cb;
    cfg.out.user_ctx = bsp_audio_codec();
    cfg.task_prio    = 5;
    cfg.task_stack   = 6 * 1024;
    /*
     * 双核分工：播放管线（读文件 → 解码 → 重采样 → 写 codec）放 **core 0**，
     * LVGL 任务在 main.cc 里固定 core 1。两者各占一核，播放时 UI 不会被
     * 解码任务抢占。
     *
     * 注意这里**不能**写 1：esp_gmf_task.c 里是
     *     if (cfg->thread.core) { 用它 } else { 用 DEFAULT_ESP_GMF_TASK_CORE }
     * 而 DEFAULT_ESP_GMF_TASK_CORE 正好是 0 —— 传 0 会走 else 分支落到 core 0，
     * 传 1 则显式钉在 core 1（和 LVGL 抢核）。
     */
    cfg.task_core    = 0;

    esp_gmf_err_t err = esp_audio_simple_player_new(&cfg, &s_player);
    if (err != ESP_GMF_ERR_OK || !s_player) {
        ESP_LOGE(TAG, "player new failed: %d", err);
        s_player = NULL;
        return ESP_FAIL;
    }

    err = esp_audio_simple_player_set_event(s_player, player_event_cb, NULL);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "set event failed: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "audio service ready (%d Hz / %uch / %ubit)",
             OUT_RATE_HZ, OUT_CHANNELS, OUT_BITS);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 播放控制 */

/*
 * 内部的统一入口：uri 是给 GMF 的（file:///... 或 http(s)://...），
 * local_path 只在本地文件时非空（用来探测容器头/记进度路径）。
 */
static esp_err_t play_uri_locked(const char *uri, const char *local_path)
{
    /* 先停掉上一首，避免复用旧管线（会崩） */
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    if (st == ESP_ASP_STATE_RUNNING || st == ESP_ASP_STATE_PAUSED) {
        esp_audio_simple_player_stop(s_player);
    }

    /* 记下当前"路径"：本地是文件路径；在线流式记 URI（MUSIC_INFO 兜底探测用） */
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", local_path ? local_path : uri);

    /* 换曲：流缓冲的进度计数清零（钩子会重新填 total/got），出声标志也清掉 */
    s_stream_total = 0;
    s_stream_got   = 0;
    s_out_flowing  = false;
    /* 时长是"上一首的"，作废；上层起播后会用 audio_service_set_duration_ms() 补上
     * （在线必须有它才算得出平均码率 → 能拖动定位） */
    s_dur_hint_ms  = 0;
    /* 时间轴与实测码率也从头开始（见 s_meas_bps 的注释） */
    s_pcm_bytes          = 0;
    s_tl_base_ms         = 0;
    s_meas_bps           = 0;
    s_meas_anchor_ms     = 0;
    s_meas_anchor_pos    = 0;
    s_meas_anchor_ok     = false;
    s_seek_landing_ready = false;
    s_seek_landing_pending = false;

    /* 播放前解除静音并开功放由事件回调负责 */
    s_info_valid = false;   /* 旧曲目的流信息作废 */
    /* 容器头部长度：MP3 要跳过 ID3v2 标签，拖动定位的字节换算才不会整体偏后。
     * 在线流式没有本地文件，只能给 0（代价见 audio_service_seek_ms 的注释）。 */
    s_data_offset = local_path ? media_meta_audio_data_offset(local_path) : 0;
    bsp_audio_set_mute(false);
    bsp_audio_set_volume(s_volume);

    /* 前一首的输入 IO 句柄作废（管道里那个实例可能已经重建，见 stream_io_update） */
    s_stream_io = NULL;

    esp_gmf_err_t err = esp_audio_simple_player_run(s_player, uri, NULL);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "run '%s' failed: %d", uri, err);
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        notify(AUDIO_EVT_ERROR);
        return ESP_FAIL;
    }
    /*
     * 管道已经跑起来了，这时候才拿得到"它真正在用的那个输入 IO"。
     * 本地播放拿到的是 io_file（无害：下面所有查询都还要求 s_stream_total/时长非零，
     * 本地那两样都是 0，所以走的还是探测出来的平均码率）。
     */
    stream_io_update();
    ESP_LOGI(TAG, "playing %s", uri);
    return ESP_OK;
}

esp_err_t audio_service_play_path(const char *vfs_abs_path)
{
    if (!s_player || !vfs_abs_path || vfs_abs_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    /* GMF 会剥掉 "scheme://"，这里补成 "file:///sdcard/..." 才是绝对路径。
     * 缓冲必须放得下整条 UTF-8 长路径：截断出来的"短路径"会让 GMF 打不开文件 */
    char uri[600];
    int n = snprintf(uri, sizeof(uri), "file://%s", vfs_abs_path);
    if (n <= 0 || n >= (int)sizeof(uri)) {
        ESP_LOGE(TAG, "path too long for URI (%d bytes)", n);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t ret = play_uri_locked(uri, vfs_abs_path);
    xSemaphoreGive(s_lock);
    return ret;
}

/*
 * 在线流式播放：把 http(s):// 直接交给简单播放器（工程里
 * CONFIG_ESP_AUDIO_SIMPLE_PLAYER_HTTP_EN=y，池子里注册了 GMF 的 HTTP reader IO，
 * 它按 URI 的 scheme 选 IO）。
 *
 * 与 audio_service_play_path() 的区别只有两点：
 *   - 没有本地文件，所以容器头偏移给 0；
 *   - s_cur_path 记的是 URI（进度记忆/日志用；在线曲目的"续播"由上层按歌曲 ID 处理）。
 * 传进来是绝对路径时自动走本地那条路，调用方不用判断。
 */
/*
 * 流式播放的缓冲：GMF 的 HTTP IO 默认只有 20 KB 环形缓冲（头文件里写死的宏，
 * 没有 Kconfig 可调），Wi-Fi 一抖就容易 `Run timeout`。这里注册一个自己的
 * http IO 覆盖池里那个默认的：
 *   - 环形缓冲 **4 MB**：GMF 的分配器在 CONFIG_SPIRAM_BOOT_INIT=y 时优先用
 *     PSRAM（esp_gmf_oal_mem.c 里就是 heap_caps_malloc(MALLOC_CAP_SPIRAM)），
 *     所以这块缓冲落在 PSRAM，不占内部 RAM。4 MB ≈ 320 kbps 的 100 秒音频 ——
 *     相当于"一次缓冲一大段"，网络抖动时靠这段顶着，不再几秒一卡。
 *   - 每次读 8 KB（默认 3 KB），减少 TLS 记录边界上的往返；
 *   - 挂上事件钩子：**统计已收字节 / 总字节**，给 UI 显示"缓冲中 xx%"；
 *   - 顺手挂上证书包：音频流以前是不校验的。
 * 只在第一次流式播放前做一次；失败就退回 ASP 默认的 IO（不影响播放）。
 */
#define STREAM_RING_BYTES   (4 * 1024 * 1024)

/*
 * HTTP IO 的事件钩子。返回值语义（见 esp_gmf_io_http.h）：
 *   0 = 不干预，继续正常收发；-1 = 让 HTTP 客户端停下。
 * 我们只**旁观**，永远返回 0。
 *
 *   HTTP_STREAM_FINISH_REQUEST：响应头已到手，此时能从 http_client 拿到
 *                               Content-Length（分块传输时是 -1，那就显示不了百分比）；
 *   HTTP_STREAM_ON_RESPONSE   ：收到一段数据（buffer_len 是本段字节数）。
 */
static int stream_evt_hook(http_stream_event_msg_t *msg)
{
    if (!msg) {
        return 0;
    }
    switch (msg->event_id) {
    case HTTP_STREAM_FINISH_REQUEST:
        /*
         * ⚠ 这个事件**在流式播放（READER 方向）下不会来**：io_http 只在
         *   `base.dir == ESP_GMF_IO_DIR_WRITER` 的分支里 dispatch 它
         *   （见 esp_gmf_io_http.c 的 _http_open）。所以 s_stream_total 一直是 0，
         *   总字节数要走 stream_total_bytes()（问 IO 要）——别只信这里。
         *   留着这个分支只是为了将来 GMF 改成读侧也派发时能自动生效。
         */
        s_stream_total = 0;
        break;
    case HTTP_STREAM_ON_RESPONSE:
        if (msg->buffer_len > 0) {
            s_stream_got += (uint32_t)msg->buffer_len;
        }
        break;
    default:
        break;
    }
    return 0;
}

/*
 * 流的总字节数（HTTP Content-Length）。
 *
 * ⚠ **别只信事件钩子里的 `s_stream_total`**：`HTTP_STREAM_FINISH_REQUEST` 在 io_http 里
 *   只对 **writer** 方向派发（`_http_open()` 里那句
 *   `if (http->is_open && (http->base.dir == ESP_GMF_IO_DIR_WRITER))`），而流式播放是
 *   READER —— 那个事件永远不来，钩子里的值恒为 0。真机踩过：在线拖动定位报
 *   "定位失败：码率未知"，就是这条链断了（总长为 0 → 算不出平均码率 → 而这条流
 *   GMF 报的 MUSIC_INFO 码率又是 0 bps，连兜底都没有）。
 *
 * 可靠来源是 **IO 自己的 size**：`_http_open()` 里 `esp_gmf_io_set_size(self, info.size)`
 * （info.size 就是 Content-Length，读侧也照设，日志 "The total size is N bytes"）。
 * "缓冲中 xx%" 一直能显示，靠的也正是这条兜底。
 */
static uint32_t stream_total_bytes(void)
{
    uint32_t total = s_stream_total;
    if (total == 0 && s_stream_io) {
        uint64_t size = 0;
        if (esp_gmf_io_get_size(s_stream_io, &size) == ESP_GMF_ERR_OK && size > 0) {
            total = (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size;
        }
    }
    return total;
}

static void ensure_stream_io(void)
{
    static bool done;
    if (done || !s_player) {
        return;
    }
    done = true;

    http_io_cfg_t cfg = HTTP_STREAM_CFG_DEFAULT();
    cfg.dir               = ESP_GMF_IO_DIR_READER;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handle      = stream_evt_hook;
    cfg.io_cfg.buffer_cfg.io_size     = 8 * 1024;
    cfg.io_cfg.buffer_cfg.buffer_size = STREAM_RING_BYTES;
    cfg.io_cfg.thread.stack_in_ext    = true;

    esp_gmf_io_handle_t io = NULL;
    if (esp_gmf_io_http_init(&cfg, &io) != ESP_GMF_ERR_OK || !io) {
        ESP_LOGW(TAG, "custom http io init failed, keep default");
        return;
    }
    if (esp_audio_simple_player_register_io(s_player, io) != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "register http io failed, keep default");
        return;
    }
    /*
     * 注意：这里**不**记 s_stream_io —— 注册进池子的这个句柄只是一个"模板"，
     * 管道用的是池子复制出来的副本（见 stream_io_update 的注释）。
     * s_stream_io 由起播后的 stream_io_update() 从 pipeline 取。
     */
    ESP_LOGI(TAG, "stream io ready: ring %d KB (PSRAM), read %d KB",
             (int)(cfg.io_cfg.buffer_cfg.buffer_size / 1024),
             (int)(cfg.io_cfg.buffer_cfg.io_size / 1024));
}

/*
 * 本地文件给一份**带预读缓冲**的 io_file（2026-10，真机数据定案后重做）。
 *
 * 证据（板载输出、24-bit FLAC，`play stats` 每 5 秒一行，稳态六个窗口一致）：
 *   - CPU 侧全都很小：解码 7.9% + 重采样 19.7% + 24→16bit 0.6% + 声道 0.0% ≈ 28% of realtime；
 *   - 却只送出 3529 ms / 5000 ms 音频（71%）；
 *   - `file read: 1394 calls, 713728 B in 5046 ms -> 138 KB/s, 24% of the window spent reading`
 *     ⇒ **平均每次只读 512 字节**、每秒约 275 次，其中 1.2 s 花在等 SD。
 *   文件平均码率 199 KB/s，而读卡只给到 138 KB/s ⇒ 整条链被读卡限流在 71%。
 *
 * 根因：ASP 池子里默认的 `io_file` 是 `FILE_IO_CFG_DEFAULT()` —— `buffer_size = 0` /
 * `io_size = 0`，而 GMF 的 IO 基类在 `buffer_size == 0` 时**根本不建 data bus**
 * （`esp_gmf_io.c`：`if (io->data_bus == NULL && buffer_size > 0)`），于是解码器每要一小块
 * 就**同步 fread 一次**，per-call 开销吃掉三成时间。给它一个预读环形缓冲即可。
 *
 * ⚠⚠ **上次就是在这一步翻车的**：照 HTTP 那份抄，只设了 `buffer_size`、漏了 `thread.stack`
 *   —— 结果是"建了 data bus 却没有 IO 任务"（`io->type` 也不是 BLOCK），
 *   而 `esp_gmf_io_seek()` 的异步分支是"abort data bus → 由 IO 任务执行 seek"，
 *   **没有任务就永远没人执行** ⇒ 本地拖动进度条直接失效，只能撤回。
 *   所以这里 `thread.stack` 与 `buffer_size` **必须一起给**（见 AGENTS §12.4.4）。
 *
 * ❌ **2026-10 第二次尝试：真机崩溃，已回退（别再直接抄这一段）**。
 *   加上之后立刻复现了另一个更隐蔽的坑：
 *
 *     播放到文件末尾（EOF）→ 管线走完 → `ESP_GMF_FILE: CLose` → `file_io->file = NULL`
 *     → 上层（进度记忆恢复 / EOF 收尾）又发了一次 seek
 *     → `_file_seek()` 里 **`fseek(file_io->file, …)` 没判空** → `fseek(NULL)`
 *     → `Load access fault`（整机 panic）。
 *
 *   即：**预读不是问题，暴露的是 GMF `_file_seek` 缺空指针检查**（已在托管组件里补上，
 *   见 AGENTS §11.5.14）。而且这次崩溃的**触发条件**本身也是我们的 bug：
 *   恢复的进度 197 s **超过了文件总长 194.9 s** → seek 直接落到文件尾 → 立刻 EOF。
 *
 *   所以顺序是：① 补 GMF 判空 + ② 恢复进度按时长夹取（都在本轮做了）
 *   → 之后才谈得上重新打开预读。**恢复预读前必须先放一个"位置超过总长"的曲子验证**。
 */

/*
 * 把 s_stream_io 指向**管道真正在用的那个输入 IO**。
 *
 * ⚠⚠ 这里有个坑（2026-10 真机踩了两轮才定位到）：
 *   `esp_audio_simple_player_register_io()` 只是把我们的 IO 句柄**登记**进池子，
 *   建管道时走的是 `esp_gmf_pool_new_io()` → **`esp_gmf_obj_dupl()`**
 *   （`tmp->new_obj(tmp->cfg, new_obj)`）—— 池子会按 cfg **新建一份实例**给管道用。
 *   所以我们手里那个"模板"永远不会被 open，它的 `esp_gmf_io_get_pos/get_size` 恒为 0。
 *   症状：在线拖动定位报"定位失败：码率未知"（总长=0 → 算不出平均码率），
 *   而且"缓冲中 xx%"也一直显示不出来（同一个原因）。
 *   （注意 cfg 是**共享**的，所以 4 MB 环形缓冲、事件钩子这些照旧生效 ——
 *     只有"句柄身份"是错的。）
 *
 * 正确来源是 pipeline 的 in：`esp_gmf_pipeline_get_in()`（就是个赋值，无锁，随便调）。
 */
static void stream_io_update(void)
{
    s_stream_io = NULL;
    if (!s_player) {
        return;
    }
    esp_gmf_pipeline_handle_t pipe = NULL;
    if (esp_audio_simple_player_get_pipeline(s_player, &pipe) != ESP_GMF_ERR_OK || !pipe) {
        return;
    }
    esp_gmf_io_handle_t io = NULL;
    if (esp_gmf_pipeline_get_in(pipe, &io) == ESP_GMF_ERR_OK) {
        s_stream_io = io;
        /*
         * 顺手打开 GMF 的**速度统计**（需求：缓冲时显示"缓冲中 xx KB/s"）。
         *
         *   - IO 任务每次真读到数据都会 esp_gmf_io_update_speed_stats(io, 实际字节, 耗时)，
         *     所以它算的是**网络下载速率**（不是"解码器吃到哪"，那个是 get_pos）；
         *   - 单位是 **Kbps（千比特/秒）**：源码里 `interval_bytes * 8 / interval_time_ms`；
         *   - 必须在这个**管道真正在用的 IO 实例**上开（池子里那个模板开了也没用 ——
         *     同 pos/size 那个坑，见上面的注释）；
         *   - enable(true) 只在没分配时分配，重复调用只是 memset 清零，不会泄漏。
         */
        if (esp_gmf_io_enable_speed_monitor(io, true) != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "enable speed monitor failed (speed hint will be hidden)");
        }
    }
}

bool audio_service_stream_progress(int *percent)
{
    if (!s_stream_io) {
        return false;
    }
    /* 总长度见 stream_total_bytes()（钩子那条路在读方向根本不来） */
    const uint32_t total = stream_total_bytes();
    if (total == 0) {
        return false;                 /* 分块传输/未知长度：显示不了百分比 */
    }

    /*
     * 已收字节：**优先问 IO 的读位置** —— 它按"实际读到多少字节"推进，是准的。
     *
     * 为什么不用钩子里的 s_stream_got 当主：ON_RESPONSE 是在**读之前**派发的，
     * msg->buffer_len 是"这次想读多少"（我们的 io_size = 8 KB），不是真读到的字节数。
     * 网络一慢（一次 TLS 记录只回来 1~2 KB），按 8 KB 累加就会冲到 100% 而实际远没下完。
     * 所以那个计数只当兜底。
     */
    uint32_t got = 0;
    uint64_t pos = 0;
    if (esp_gmf_io_get_pos(s_stream_io, &pos) == ESP_GMF_ERR_OK && pos > 0) {
        got = (pos > total) ? total : (uint32_t)pos;
    } else {
        got = s_stream_got;
        if (got > total) {
            got = total;
        }
    }
    if (percent) {
        *percent = (int)((uint64_t)got * 100 / total);
    }
    return true;
}

/*
 * 流式播放的**网络下载速率**（需求："缓冲中 20 KB/s"）。
 *
 * 数据源是 GMF 的速度统计（`esp_gmf_io_get_speed_stats`，由 IO 任务在每次真读到数据后
 * 更新并做 1:9 平滑），它统计的是**从网络读进来的字节**，和"解码器消费到哪"（get_pos）
 * 是两回事 —— 起播缓冲期间后者几乎不动，用它算速度只会显示 0。
 *
 * ⚠ 单位：GMF 的 `current_speed_kbps` 是 **Kbps（千比特每秒）**，不是 KB/s。
 *   这里**不做单位换算**，原样交出去；显示层要 KB/s 自己 ÷8（app_music 就是这么做的，
 *   字符串也写成 KB/s，别把 Kbps 当 KB/s 印出去 —— 320 kbps 的流会被写成 320 KB/s）。
 *
 * 返回 false = 没有可用的速度（本地文件 / 统计没开 / 还没读到任何数据）。
 */
bool audio_service_stream_speed(int *kbps)
{
    if (!s_stream_io || !kbps) {
        return false;
    }
    esp_gmf_io_speed_stats_t st;
    if (esp_gmf_io_get_speed_stats(s_stream_io, &st) != ESP_GMF_ERR_OK) {
        return false;
    }
    if (st.current_speed_kbps == 0 && st.average_speed_kbps == 0) {
        return false;                  /* 还没开始读（统计刚清零）：别显示 0 KB/s */
    }
    /* 当前值还没平滑出来（起播头 100 ms）就用平均值兜一下 */
    *kbps = (int)(st.current_speed_kbps ? st.current_speed_kbps : st.average_speed_kbps);
    return true;
}

/* ---------------------------------------------------------------- 定位换算 */

/*
 * 把"时间"换算成"要 seek 的字节位置"。算不出来返回 false。
 *
 * 两条路，各有各的适用面：
 *
 * ① 绝对映射：byte = offset + 时间 × 平均码率 / 8000
 *    平均码率见 seek_bitrate()（本地=media_meta 探测值；在线=总字节÷时长；兜底第一帧码率）。
 *    对 CBR 是精确的；**向后拖只能用这条**（相对映射没法往回推）。
 *
 * ② 相对映射：byte = 解码器当前位置 + (目标时间 - 当前时间轴位置) × 实测码率 / 8000
 *    只在**向前拖**、且"实测码率"和"全曲平均码率"差了 20% 以上（说明是 VBR，
 *    或者压根没有全曲平均）时用。用的是脚下这一段真实的码率，比全曲平均贴近实际。
 *    时间轴位置见 timeline_now()，实测码率见 s_meas_bps 的注释。
 */
static bool seek_byte_for(uint32_t pos_ms, uint64_t *byte_out, uint32_t *bps_out)
{
    const uint32_t k_avg = seek_bitrate();
    uint64_t cur = 0;
    const bool have_pos = (s_stream_io &&
                           esp_gmf_io_get_pos(s_stream_io, &cur) == ESP_GMF_ERR_OK && cur > 0);
    const uint32_t tl = timeline_now();

    if (have_pos && s_meas_bps > 0 && pos_ms > tl + SEEK_REL_MIN_MS) {
        const bool vbr = (k_avg == 0) || (s_meas_bps > k_avg + k_avg / 5) ||
                         (s_meas_bps + s_meas_bps / 5 < k_avg);
        if (vbr) {
            *byte_out = cur + ((uint64_t)(pos_ms - tl) * s_meas_bps) / 8000ULL;
            *bps_out  = s_meas_bps;
            return true;
        }
    }
    if (k_avg > 0) {
        *byte_out = (uint64_t)s_data_offset + ((uint64_t)pos_ms * k_avg) / 8000ULL;
        *bps_out  = k_avg;
        return true;
    }
    return false;
}

bool audio_service_output_active(void)
{
    return s_out_flowing;
}

esp_err_t audio_service_play_uri(const char *uri)
{
    if (!s_player || !uri || !uri[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uri[0] != '/') {
        ensure_stream_io();          /* 流式播放：换成大缓冲（PSRAM）的 http IO */
    }
    if (uri[0] == '/') {
        return audio_service_play_path(uri);
    }
    if (strncmp(uri, "http://", 7) != 0 && strncmp(uri, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t ret = play_uri_locked(uri, NULL);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t audio_service_pause(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gmf_err_t err = esp_audio_simple_player_pause(s_player);
    return (err == ESP_GMF_ERR_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_service_resume(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gmf_err_t err = esp_audio_simple_player_resume(s_player);
    if (err == ESP_GMF_ERR_OK) {
        notify(AUDIO_EVT_RESUMED);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/*
 * 本曲用于"时间 → 字节"换算的**平均码率**（bit/s），取不到返回 0。
 *
 * 优先级（2026-10 加：在线拖动定位）：
 *   1) **在线流：总字节 × 8000 ÷ 时长**。
 *      总字节来自 HTTP Content-Length（stream_total_bytes() 问 IO 要；**别用钩子那个**，
 *      读方向的事件不会来），时长由上层通过 audio_service_set_duration_ms() 告知
 *      （GMF 的 MUSIC_INFO 里 duration 恒为 0）。
 *      这就是"(目标时间/总时长)×总字节数"那条公式，只是先化成码率再用：
 *      对 CBR 是精确值，对 VBR 是**真平均**——比"解码器读到的第一帧码率"准得多。
 *      ⚠ 分块传输（无 Content-Length）或时长未知时这条走不通，退回 2)。
 *   2) s_info.bitrate：本地文件在 MUSIC_INFO 处理里已经换成 media_meta 探测到的
 *      平均码率（见 player_event_cb 的说明）；在线流则是第一帧码率（VBR 上会偏）。
 *
 * ⚠ 流上不扣 ID3v2 头（s_data_offset = 0），而"总长/总时长"把标签也当成了音频：
 *   中段定位会**早到几秒**（标签越大越明显）。网易云下发的流标签很小（封面走单独的
 *   图片接口），实测可忽略；本地文件是同名参数精确扣除的。
 */
static uint32_t seek_bitrate(void)
{
    const uint32_t total = stream_total_bytes();
    if (total > 0 && s_dur_hint_ms > 0) {
        const uint64_t br = ((uint64_t)total * 8000ULL) / (uint64_t)s_dur_hint_ms;
        /* 8 kbps ~ 2 Mbps：两头都不像真码率时说明 total/时长 有一个是脏数据，
         * 宁可退回第一帧码率，也别拿它去算出一个离谱的字节位置 */
        if (br >= 8000ULL && br <= 2000000ULL) {
            return (uint32_t)br;
        }
    }
    return (s_info.bitrate > 0) ? (uint32_t)s_info.bitrate : 0;
}

void audio_service_set_duration_ms(uint32_t dur_ms)
{
    s_dur_hint_ms = dur_ms;
    if (dur_ms > 0) {
        s_info.duration_ms = dur_ms;
    }
}

bool audio_service_seek_supported(void)
{
    if (!s_player) {
        return false;
    }
    const bool ok = s_info_valid && (seek_bitrate() > 0 || s_meas_bps > 0);
    if (!ok) {
        /*
         * ★ 拒绝的原因必须能从日志里直接看出来（这条链上任何一环为 0 都会走到这里，
         *   而"定位失败：码率未知"这句 Toast 完全看不出是哪一环 —— 2026-10 为此
         *   来回排查了两轮：先是总长取错句柄、再是池子复制实例）。
         */
        ESP_LOGW(TAG, "seek not ready: info=%d io=%p total=%u dur=%u avg=%u meas=%u gmf_br=%d",
                 (int)s_info_valid, s_stream_io, (unsigned)stream_total_bytes(),
                 (unsigned)s_dur_hint_ms, (unsigned)seek_bitrate(), (unsigned)s_meas_bps,
                 s_info.bitrate);
    }
    return ok;
}

bool audio_service_seek_landing_ms(uint32_t *out_ms)
{
    if (!s_seek_landing_ready) {
        return false;
    }
    s_seek_landing_ready = false;
    if (out_ms) {
        *out_ms = s_seek_landing_ms;
    }
    return true;
}

/*
 * 按时间定位（拖动进度条）。
 *
 * 实现（都是 GMF 已有的能力，我们只是把"时间"换算成"字节"）：
 *   - esp_audio_simple_player 没有 seek 门面，只能拿到 pipeline 再调
 *     esp_gmf_pipeline_seek()，而它**只接受字节位置**，且要求 pipeline 处于
 *     PAUSED/STOPPED/FINISHED。所以这里"暂停 → 定位 → 恢复"。
 *   - 字节位置用**平均码率**估算（byte = offset + ms*bitrate/8000，见 seek_bitrate）。
 *   - GMF 那一侧：目标落在**已预读但还没喂给解码器**的数据里时，`seek_in_cache()`
 *     直接就地丢字节（零网络、瞬时，日志 "Seek within buffer"）；否则关掉 HTTP 连接
 *     再带 `Range: bytes=N-` 重开（`_http_seek` → `_http_open`）。服务端要是忽略
 *     Range 回 200，GMF 会退化成"本地丢掉前 N 字节"（慢，但不会失败）。
 *   - 解码器不重开：MP3/AAC/TS 这些帧自带同步字的格式能自己重新对齐；
 *     MP4/M4A、OGG 不行（那道判断在 app_music 里）。
 *   - 码率未知（MUSIC_INFO 还没来 / 分块传输且没有时长）→ ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t audio_service_seek_ms(uint32_t pos_ms)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_info_valid) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint64_t byte = 0;
    uint32_t bitrate = 0;
    if (!seek_byte_for(pos_ms, &byte, &bitrate)) {
        return ESP_ERR_NOT_SUPPORTED;   /* 既没有平均码率也没有实测码率 */
    }

    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    const bool was_running = (st == ESP_ASP_STATE_RUNNING);
    if (was_running) {
        if (audio_service_pause() != ESP_OK) {
            return ESP_FAIL;
        }
    }

    esp_gmf_pipeline_handle_t pipe = NULL;
    esp_gmf_err_t gerr = esp_audio_simple_player_get_pipeline(s_player, &pipe);
    if (gerr != ESP_GMF_ERR_OK || !pipe) {
        if (was_running) {
            audio_service_resume();
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * ★ 出声标志清零（在线拖动定位的关键一步）。
     *
     * seek 之后**不保证马上有声**：命中环形缓冲是瞬移，但长距离拖动会关掉 HTTP
     * 连接、带 Range 头重开，要重新缓冲几百毫秒到几秒。UI 判"缓冲结束、可以撤掉
     * '缓冲中…'提示"用的就是 audio_service_output_active()（= s_out_flowing），
     * 所以这里必须重新计 —— 否则它一直说"已经出声了"，提示刚挂上就被撤掉，
     * 用户看到的就是"进度条跳过去了，可没声音也没提示"。
     */
    s_out_flowing = false;

    gerr = esp_gmf_pipeline_seek(pipe, byte);
    if (was_running) {
        audio_service_resume();
    }
    if (gerr != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "seek %ums (byte %llu) failed: %d", (unsigned)pos_ms,
                 (unsigned long long)byte, (int)gerr);
        return ESP_FAIL;
    }

    /*
     * 定位成功后的"重新计数"（时间轴上的三件事）：
     *   - 时间轴基准挪到目标时刻：timeline_now() 从这一刻起等于"目标 + 之后放出来的时长"；
     *   - PCM 计数清零（它只用来算"自基准点以来"的增量）；
     *   - 实测码率的锚点作废：IO 位置刚跳变，这一轮差分会算出天文数字。
     * 同时挂上"落点核对"：seek 之后第一块 PCM 出来时反推真实时间，见 audio_stream_measure。
     */
    s_pcm_bytes            = 0;
    s_tl_base_ms           = pos_ms;
    s_meas_anchor_ok       = false;
    s_seek_req_ms          = pos_ms;
    s_seek_req_bps         = bitrate;
    s_seek_landing_ready   = false;
    s_seek_landing_pending = true;

    ESP_LOGI(TAG, "seek -> %ums (byte %llu, %u bps, %s)", (unsigned)pos_ms,
             (unsigned long long)byte, (unsigned)bitrate,
             (s_meas_bps > 0 && seek_bitrate() != s_meas_bps) ? "measured(relative)" : "average");
    return ESP_OK;
}

esp_err_t audio_service_toggle(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    if (st == ESP_ASP_STATE_RUNNING) {
        return audio_service_pause();
    }
    if (st == ESP_ASP_STATE_PAUSED) {
        return audio_service_resume();
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t audio_service_stop(void)
{
    if (!s_player) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gmf_err_t err = esp_audio_simple_player_stop(s_player);
    bsp_audio_set_mute(true);
    bsp_audio_pa_enable(false);
    s_running = false;
    return (err == ESP_GMF_ERR_OK) ? ESP_OK : ESP_FAIL;
}

bool audio_service_is_playing(void)
{
    if (!s_player) {
        return false;
    }
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &st);
    return st == ESP_ASP_STATE_RUNNING;
}

/* ---------------------------------------------------------------- 输出通道 */

esp_err_t audio_service_set_output(audio_out_t out)
{
    switch (out) {
    case AUDIO_OUT_NONE:
        s_out = out;
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        ESP_LOGI(TAG, "output -> none (静音 + 关功放)");
        return ESP_OK;
    case AUDIO_OUT_SPEAKER:
        /* 从蓝牙切回来：先停掉 A2DP 源流，再把本地功放接回来 */
        if (s_out == AUDIO_OUT_BLUETOOTH) {
            bt_service_media_stop();
        }
        s_out = out;
        bsp_audio_set_volume(s_volume);
        bsp_audio_set_mute(s_volume == 0);
        if (s_running) {
            bsp_audio_pa_enable(true);
        }
        ESP_LOGI(TAG, "output -> speaker (%d%%)", s_volume);
        return ESP_OK;
    case AUDIO_OUT_BLUETOOTH:
        if (!bt_service_is_connected()) {
            ESP_LOGW(TAG, "no bluetooth audio device connected");
            return ESP_ERR_INVALID_STATE;
        }
        /*
         * 开发指南 5.12：切到蓝牙要**先静音 + 拉低 PA_CTRL**，
         * 否则本地喇叭和耳机同时出声。
         */
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        if (!bt_service_is_streaming()) {
            bt_service_media_start();       /* 异步；流起来之前的 PCM 会被丢掉 */
        }
        s_out = out;
        ESP_LOGI(TAG, "output -> bluetooth (%s)", bt_service_peer_name());
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

audio_out_t audio_service_get_output(void)
{
    return s_out;
}

esp_err_t audio_service_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    const bool changed = (percent != s_volume);
    s_volume = percent;
    /* 主音量走 ES8389 数字音量寄存器 */
    esp_err_t ret = bsp_audio_set_volume(percent);
    /*
     * 音量归零自动静音；反过来，从 0 调大时必须自动解除静音，
     * 否则用 VOL+ 按键把音量从 0 加上去会"没声音"，看着像按键没生效。
     * （开机时的静音由 audio_service_init / play_path 自己管理，不走这里。）
     */
    if (percent == 0) {
        bsp_audio_set_mute(true);
    } else {
        bsp_audio_set_mute(false);
    }
    /*
     * 音量记忆（需求）：这里是**唯一**的音量写入点，所有改音量的路径
     * （播放器页音量条、硬件 VOL±、控制中心滑条）都会经过它。
     * 落盘由 play_state 合并防抖，不会因为长按 VOL+ 而狂写 flash。
     */
    if (changed) {
        play_state_volume_set(percent);
    }
    return ret;
}

int audio_service_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_service_set_mute(bool mute)
{
    return bsp_audio_set_mute(mute);
}

void audio_service_set_event_cb(audio_event_cb_t cb, void *user)
{
    s_evt_cb = cb;
    s_evt_user = user;
}

bool audio_service_get_info(audio_info_t *out)
{
    if (!out || !s_info_valid) {
        return false;
    }
    *out = s_info;
    return true;
}