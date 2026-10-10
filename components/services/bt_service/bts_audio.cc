/*
 * bts_audio.cc —— bt_service 内部：SBC 编码器 + 软件 ASRC + 重采样暂存
 *
 * 从 bt_service.cc 445–671 原样搬来（bt_enc_close / bt_asrc_open / bt_enc_open /
 * bt_stage_append，注释全部跟着走）。**没有改任何参数**：bitpool 封顶 40、
 * ASRC 走软件路径、缓冲分配方式、日志文案都与拆分前逐字一致。
 *
 * 状态定义在本文件（原来是 bt_service.cc 的 static），通过 bts_audio.h 给 feed 任务用；
 * s_enc_registered 只在 bts_enc_open 里用，保持文件内 static。
 */

#include "bts_audio.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_types.h"
#include "esp_asrc.h"
#include "esp_heap_caps.h"

#include "bts_state.h"      /* bts_rate / bts_channels / bts_dropped */

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

/* ---- SBC 编码 + 重采样（只有 feed 任务碰，不需要锁） ---- */
esp_audio_enc_handle_t bts_enc;          /* 编码器句柄，随流起停开关 */
int                    bts_enc_in_bytes;  /* 一个 SBC 帧对应的 PCM 字节数 */
int                    bts_enc_out_bytes; /* 编码后一帧的建议缓冲大小 */
uint8_t               *bts_enc_frame;     /* 编码输出缓冲 */
static bool            s_enc_registered;/* esp_audio_enc_register_default 只调一次 */

esp_asrc_handle_t bts_asrc;          /* 非 NULL = 需要 48k → 协商采样率 */
uint16_t          bts_asrc_in_smp;   /* ASRC 一个"采样帧"的输入字节数 */
uint16_t          bts_asrc_out_smp;  /* ASRC 一个"采样帧"的输出字节数 */
uint32_t          bts_asrc_in_bytes; /* 每次喂给 ASRC 的字节数 */
uint8_t          *bts_asrc_in;
uint8_t          *bts_asrc_out;
uint32_t          bts_asrc_out_cap_samples;
uint8_t          *bts_stage;         /* 重采样后的 PCM 暂存（凑够一帧再编码） */
uint32_t          bts_stage_len;

/* 当前 A2DP 流句柄（写：BT 回调任务 / deinit；读：feed 任务） */
esp_bt_audio_stream_handle_t bts_stream;

/* ---------------------------------------------------------------- PCM → A2DP */

/*
 * ⚠ A2DP 源要的是**编码后的帧**，不是 PCM —— 这是"推给蓝牙没声音"的根因。
 *
 * esp_bt_audio 的源头音流通路只是把调用方写进来的字节原样打包发出去
 * （bt_audio_classic_stream.c: release_write → xQueueSend(data_q)，
 *  bt_audio_a2dp_src.c: 从 data_q 收包 → esp_a2d_source_audio_data_send()），
 * **组件内部不做任何编码**。它按"一个包 = 一个编码帧"来算批次：
 *     max_batch_size = (MTU - 头) / sbc_frame_size
 *     data_size      = Σ 每个包的 data_len      // 超过 MTU 直接整批丢掉
 *     rtp_ts        += samples_per_frame × 包数
 * 我们要做的正是官方示例里 pipeline 的 `aud_enc` 那一级
 * （examples/bt_audio/main/stream_proc.c: esp_gmf_audio_enc_reconfig(...)
 *  用的就是 esp_bt_audio_stream_get_codec_info() 给的 codec_cfg），
 * 也就是：PCM → SBC 编码 → 一包一帧 → acquire_write/release_write。
 *
 * 如果直接把 2 KB 的 PCM 当一个包写进去，不但内容不是 SBC 帧（耳机解出来是噪音），
 * 每批的 data_size 还会超过 MTU 被整批丢弃 —— 现象就是**完全没声音**。
 *
 * codec_cfg 是组件按**协商结果**生成好的 esp_sbc_enc_config_t（采样率/声道/
 * block/subband/bitpool 都是对端定的），所以这里不能自己拼参数，
 * 必须拿 codec_cfg 去开编码器，否则耳机解不了。
 */

void bts_audio_close(void)
{
    if (bts_enc) {
        esp_audio_enc_close(bts_enc);
        bts_enc = NULL;
    }
    if (bts_asrc) {
        esp_asrc_close(bts_asrc);
        bts_asrc = NULL;
    }
    free(bts_asrc_in);    bts_asrc_in    = NULL;
    free(bts_asrc_out);   bts_asrc_out   = NULL;
    free(bts_enc_frame);  bts_enc_frame  = NULL;
    free(bts_stage);      bts_stage      = NULL;
    bts_stage_len         = 0;
    bts_enc_in_bytes      = 0;
    bts_enc_out_bytes     = 0;
    bts_asrc_in_bytes     = 0;
    bts_asrc_out_cap_samples = 0;
}

/*
 * 打开 48 kHz → dest_rate 的重采样器。
 *
 * ⚠ 这里刻意用**软件** ASRC，不用硬件（ESP_ASRC_PERF_TYPE_AUTO/HW_ONLY）：
 *   S31 的硬件 ASRC 走 GDMA，输入/输出缓冲要满足 cache-line 对齐与 PSRAM 一致性，
 *   组件把这类缓冲的分配交给调用方（asrc_mem.c 用 MALLOC_CAP_SPIRAM 问对齐），
 *   一旦缓冲的归属/同步没对齐，CPU 读到的就是没写完/过期的重采样数据 ——
 *   现场表现正是"先慢放、几秒后全是噪音"。
 *   48k→44.1k 立体声的软件重采样在 320 MHz 上有的是余量（每秒不到 200 KB），
 *   而且完全绕开 DMA/cache，先用可靠的软件路径；要上硬件 ASRC 就应该整体
 *   走官方 GMF 管线（io_file → aud_dec → aud_rate_cvt → aud_enc → io_bt），
 *   让 GMF 管缓冲，而不是在这里手工拼。
 */
bool bts_asrc_open(uint32_t dest_rate)
{
    esp_asrc_cfg_t cfg = {};
    cfg.src_info.sample_rate     = A2DP_PCM_RATE;
    cfg.src_info.channel         = A2DP_PCM_CH;
    cfg.src_info.bits_per_sample = A2DP_PCM_BITS;
    cfg.dest_info                = cfg.src_info;
    cfg.dest_info.sample_rate    = dest_rate;
    cfg.perf_type                = ESP_ASRC_PERF_TYPE_SW_SPEED;
    cfg.complexity               = 3;
    cfg.timeout_ms               = 1000;

    if (esp_asrc_open(&cfg, &bts_asrc) != ESP_ASRC_ERR_OK || !bts_asrc) {
        bts_asrc = NULL;
        return false;
    }
    if (esp_asrc_get_bytes_per_sample(bts_asrc, &bts_asrc_in_smp, &bts_asrc_out_smp) != ESP_ASRC_ERR_OK ||
        bts_asrc_in_smp == 0 || bts_asrc_out_smp == 0) {
        bts_audio_close();
        return false;
    }
    bts_asrc_in_bytes = (uint32_t)ASRC_IN_FRAMES * bts_asrc_in_smp;

    uint32_t out_samples = 0;
    if (esp_asrc_get_out_sample_num(bts_asrc, ASRC_IN_FRAMES, &out_samples) != ESP_ASRC_ERR_OK ||
        out_samples == 0) {
        bts_audio_close();
        return false;
    }
    bts_asrc_out_cap_samples = out_samples + 32;      /* 余量：非整数比时输出数会浮动 */

    /* 软件路径不要求 cache 对齐，但放内部 RAM 更稳（热路径、无 PSRAM 延迟） */
    const size_t in_sz  = bts_asrc_in_bytes;
    const size_t out_sz = bts_asrc_out_cap_samples * bts_asrc_out_smp;
    bts_asrc_in  = (uint8_t *)heap_caps_aligned_alloc(64, in_sz,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bts_asrc_out = (uint8_t *)heap_caps_aligned_alloc(64, out_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bts_asrc_in) {
        bts_asrc_in = (uint8_t *)malloc(in_sz);
    }
    if (!bts_asrc_out) {
        bts_asrc_out = (uint8_t *)malloc(out_sz);
    }
    if (!bts_asrc_in || !bts_asrc_out) {
        ESP_LOGE(TAG, "asrc buffer alloc failed");
        bts_audio_close();
        return false;
    }
    ESP_LOGI(TAG, "asrc ready (sw): %d Hz -> %u Hz, %u frames/call, out cap %u frames",
             A2DP_PCM_RATE, (unsigned)dest_rate, (unsigned)ASRC_IN_FRAMES,
             (unsigned)bts_asrc_out_cap_samples);
    return true;
}

/* 按协商到的 codec_cfg 打开编码器；成功返回 true */
bool bts_enc_open(esp_bt_audio_stream_handle_t h)
{
    esp_bt_audio_stream_codec_info_t info = {};
    if (esp_bt_audio_stream_get_codec_info(h, &info) != ESP_OK || !info.codec_cfg) {
        ESP_LOGW(TAG, "no codec info for stream, retry later");
        return false;
    }
    if (info.codec_type != ESP_BT_AUDIO_STREAM_CODEC_SBC) {
        /* 本工程只注册了 SBC（CONFIG_BT_A2DP_CODEC_AAC_ENABLED 未开），
         * 真协商到别的编解码器就只能放弃了，别硬编出错音 */
        ESP_LOGE(TAG, "unsupported A2DP codec type %d (only SBC)", (int)info.codec_type);
        return false;
    }

    if (!s_enc_registered) {
        esp_audio_err_t r = esp_audio_enc_register_default();
        if (r != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "register default encoders failed: %d", (int)r);
            return false;
        }
        s_enc_registered = true;
    }

    esp_audio_enc_config_t cfg = {};
    cfg.type   = ESP_AUDIO_TYPE_SBC;
    /*
     * ⚠ 这里必须用**拷贝**：`info.codec_cfg` 是组件内部协商结果的内存，
     *   直接改它会污染协议栈自己的记录（也说不清谁拥有它）。
     */
    esp_sbc_enc_config_t sbc = {};
    if (!info.codec_cfg || info.cfg_size < sizeof(sbc)) {
        /* 协商结果拿不到就别硬编：零配置编出来的是噪音（旧代码直接把 cfg 指过去，
         * 一样会失败，这里只是把话说明白） */
        ESP_LOGE(TAG, "no negotiated SBC cfg (%u B)", (unsigned)info.cfg_size);
        return false;
    }
    memcpy(&sbc, info.codec_cfg, sizeof(sbc));
    const uint16_t bp_neg = sbc.bitpool;
    if (sbc.bitpool > A2DP_SBC_BITPOOL_MAX) {
        sbc.bitpool = A2DP_SBC_BITPOOL_MAX;
    }
    cfg.cfg    = &sbc;
    cfg.cfg_sz = sizeof(sbc);

    esp_audio_err_t err = esp_audio_enc_open(&cfg, &bts_enc);
    if (err != ESP_AUDIO_ERR_OK || !bts_enc) {
        ESP_LOGE(TAG, "sbc enc open failed: %d", (int)err);
        bts_enc = NULL;
        return false;
    }
    if (esp_audio_enc_get_frame_size(bts_enc, &bts_enc_in_bytes, &bts_enc_out_bytes) != ESP_AUDIO_ERR_OK ||
        bts_enc_in_bytes <= 0 || bts_enc_out_bytes <= 0) {
        ESP_LOGE(TAG, "sbc enc frame size query failed");
        bts_audio_close();
        return false;
    }

    bts_enc_frame = (uint8_t *)malloc((size_t)bts_enc_out_bytes);
    bts_stage     = (uint8_t *)malloc(STAGE_BYTES);
    if (!bts_enc_frame || !bts_stage) {
        ESP_LOGE(TAG, "sbc enc buffer alloc failed");
        bts_audio_close();
        return false;
    }
    bts_stage_len = 0;

    /* 协商参数以 codec_info 为准（channels 是声道**位掩码**，要 popcount 才是声道数） */
    bts_rate     = (int)info.sample_rate;
    bts_channels = __builtin_popcount((unsigned)info.channels);

    ESP_LOGI(TAG, "sbc enc ready: %d Hz, %d ch, PCM/frame %d B, SBC/frame %d B, "
             "bitpool %u->%u (max %u), blk %u, sub %u",
             (int)bts_rate, (int)bts_channels, bts_enc_in_bytes, bts_enc_out_bytes,
             (unsigned)bp_neg, (unsigned)sbc.bitpool, (unsigned)A2DP_SBC_BITPOOL_MAX,
             (unsigned)sbc.block_length, (unsigned)sbc.sub_bands_num);
    /* 期望码率 = 帧字节 × 每秒帧数（帧率 = 采样率 ÷ 每帧采样数）：链路吃不吃的下看这个数 */
    {
        const uint32_t per_frame = (uint32_t)(bts_enc_in_bytes /
                                    (bts_channels > 0 ? bts_channels * 2 : 2));
        if (per_frame > 0) {
            const uint32_t fps = (uint32_t)bts_rate / per_frame;
            ESP_LOGI(TAG, "a2dp budget: %u f/s x %d B = %u kbps (link must carry this)",
                     (unsigned)fps, bts_enc_out_bytes,
                     (unsigned)((uint64_t)fps * (uint64_t)bts_enc_out_bytes * 8ULL / 1000ULL));
        }
    }

    /*
     * 本地管线固定 48 kHz。对端采样率不是 48k 时必须重采样：
     *   1) 不重采样 → 变调（44.1k 会被当 44.1k 播放，音调高 8.8%）；
     *   2) 而且按 48k 攒出来的帧数比链路能吃下的多 8.8%，
     *      队列很快满 → 每帧都要等/丢，实测"发 1000 帧丢 200 KB"。
     * 重采样失败时只记警告：宁可变调也要出声。
     */
    if (bts_rate != A2DP_PCM_RATE && !bts_asrc_open((uint32_t)bts_rate)) {
        ESP_LOGW(TAG, "ASRC 打开失败，退回不重采样：会有约 %.1f%% 的音调偏差",
                 ((float)A2DP_PCM_RATE / (float)bts_rate - 1.0f) * 100.0f);
    }
    return true;
}

/* 把重采样后的 PCM 追加到暂存区（凑够一个编码帧才编码） */
void bts_stage_append(const uint8_t *data, uint32_t len)
{
    if (!data || len == 0 || !bts_stage) {
        return;
    }
    if (bts_stage_len + len > STAGE_BYTES) {
        bts_dropped = bts_dropped + len;  /* 理论上到不了：一次输出 < 一个编码帧 */
        return;
    }
    memcpy(bts_stage + bts_stage_len, data, len);
    bts_stage_len += len;
}
