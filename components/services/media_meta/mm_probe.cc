/*
 * mm_probe.cc —— 容器参数探测：MP3 帧头 / FLAC / WAV / ADTS-AAC + 魔数分派
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 * 原 346–362（MP3 数据偏移）、780–1317 行。**纯搬迁**，注释随函数一起搬 ——
 * 其中 probe_flac() 里"改偏移必须配真 fseek"那段是两个 FLAC 把偏移算到
 * 94 MB / 138 MB 之后留下的现场记录（蓝图 R1），一个字都不能省。
 *
 * ⚠ FLAC 探测**按块长度跳、绝不逐字节扫**：内嵌封面（PICTURE 块）动辄几 MB，
 *   逐字节扫又慢又正是 ESP 那个解析器刷 `Not found frame in max frame size` 的来源。
 *
 * 纯解析部分（mm_mp3_parse_header / mm_probe_fill_bitrate）无 IO，可 host 单测。
 */
#include "internal/mm_container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#include "internal/mm_bits.h"

static const char *TAG = "media_meta";

size_t mm_audio_data_offset(const char *audio_path)
{
    if (!audio_path) {
        return 0;
    }
    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return 0;
    }
    uint8_t h[10];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') {
        return 0;
    }
    return 10u + (size_t)mm_synchsafe32(h + 6);
}

/* ================================================================== MP3 帧头探测
 *
 * 目标：在 GMF 只报 "48000 Hz 2ch 16bit **0 bps**" 的情况下，自己把码率/时长
 * 补出来，否则：
 *   - 总时长 = 文件大小 × 8 / 码率 算不出来 → 进度条永远 0、时间显示 --:--
 *   - 拖动定位没有"毫秒 → 字节"的换算系数
 *
 * 做法：跳过 ID3v2，扫第一个合法 MPEG 音频帧头（并用"下一帧也应落在
 * frame_size 之后"复核，避免把随机数据当帧头），然后：
 *   - 无 Xing/Info：用帧头码率（CBR 文件即真实码率）
 *   - 有 Xing/Info：用帧数 × 每帧样本数 / 采样率 算**准确时长**，再用
 *     音频字节数 / 时长 得平均码率（VBR 文件也准）
 */

/* 码率表（kbps），下标 = 帧头的 bitrate_index */
static const uint16_t k_mp3_br_v1_l1[16]  = { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 };
static const uint16_t k_mp3_br_v1_l2[16]  = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
static const uint16_t k_mp3_br_v1_l3[16]  = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const uint16_t k_mp3_br_v2_l1[16]  = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 };
static const uint16_t k_mp3_br_v2_l23[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };

/* 采样率表：[MPEG1 / MPEG2 / MPEG2.5][sample_rate_index] */
static const uint32_t k_mp3_sr[3][4] = {
    { 44100, 48000, 32000, 0 },
    { 22050, 24000, 16000, 0 },
    { 11025, 12000, 8000,  0 },
};

bool mm_mp3_parse_header(const uint8_t *p, mm_mp3_frame_t *f)
{
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }
    const int ver_code = (p[1] >> 3) & 0x03;   /* 0=2.5 1=保留 2=2 3=1 */
    const int layer_id = (p[1] >> 1) & 0x03;   /* 0=保留 1=III 2=II 3=I */
    if (ver_code == 1 || layer_id == 0) {
        return false;
    }

    const int br_idx = (p[2] >> 4) & 0x0F;
    const int sr_idx = (p[2] >> 2) & 0x03;
    const int pad    = (p[2] >> 1) & 0x01;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return false;
    }

    const bool is_v1 = (ver_code == 3);
    const int  layer = 4 - layer_id;           /* 1 / 2 / 3 */
    uint16_t   kbps;
    if (is_v1) {
        kbps = (layer == 1) ? k_mp3_br_v1_l1[br_idx]
                            : (layer == 2 ? k_mp3_br_v1_l2[br_idx] : k_mp3_br_v1_l3[br_idx]);
    } else {
        kbps = (layer == 1) ? k_mp3_br_v2_l1[br_idx] : k_mp3_br_v2_l23[br_idx];
    }
    if (kbps == 0) {
        return false;
    }

    const uint32_t sr = k_mp3_sr[is_v1 ? 0 : (ver_code == 2 ? 1 : 2)][sr_idx];
    if (sr == 0) {
        return false;
    }

    const uint32_t bitrate = (uint32_t)kbps * 1000u;
    uint32_t samples, frame_size;
    if (layer == 1) {                       /* Layer I */
        samples    = 384;
        frame_size = (12u * bitrate / sr + (uint32_t)pad) * 4u;
    } else if (layer == 2) {                /* Layer II */
        samples    = 1152;
        frame_size = 144u * bitrate / sr + (uint32_t)pad;
    } else {                                /* Layer III：V1 每帧 1152，V2/2.5 是 576 */
        samples    = is_v1 ? 1152u : 576u;
        frame_size = (samples / 8u) * bitrate / sr + (uint32_t)pad;
    }
    if (frame_size < 8) {
        return false;
    }

    f->mpeg_ver    = ver_code;
    f->layer       = layer;
    f->sample_rate = sr;
    f->bitrate     = bitrate;
    f->frame_size  = frame_size;
    f->samples     = samples;
    f->channels    = (((p[3] >> 6) & 0x03) == 3) ? 1 : 2;
    return true;
}

esp_err_t mm_probe_mp3(const char *audio_path, audio_probe_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const char *dot = strrchr(audio_path, '.');
    if (!dot || strcasecmp(dot, ".mp3") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t data_off = (uint32_t)mm_audio_data_offset(audio_path);
    fseek(f, 0, SEEK_END);
    const long file_size = ftell(f);
    if (file_size <= (long)data_off) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, (long)data_off, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    enum { PROBE_BYTES = 32 * 1024 };
    uint8_t *buf = (uint8_t *)malloc(PROBE_BYTES);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    const size_t got = fread(buf, 1, PROBE_BYTES, f);
    fclose(f);

    size_t        pos   = 0;
    mm_mp3_frame_t fr   = {};
    bool          found = false;
    for (; pos + 4 <= got; pos++) {
        if (!mm_mp3_parse_header(buf + pos, &fr)) {
            continue;
        }
        /* 复核：下一帧也应落在 frame_size 之后；剩余数据不够就采信首帧 */
        if (pos + fr.frame_size + 4 > got) {
            found = true;
            break;
        }
        mm_mp3_frame_t nxt = {};
        if (mm_mp3_parse_header(buf + pos + fr.frame_size, &nxt)) {
            found = true;
            break;
        }
    }
    if (!found) {
        free(buf);
        ESP_LOGW(TAG, "no valid mp3 frame header in first %d bytes", (int)PROBE_BYTES);
        return ESP_ERR_NOT_FOUND;
    }

    out->bitrate     = fr.bitrate;
    out->sample_rate = fr.sample_rate;
    out->channels    = (uint8_t)fr.channels;

    /* Xing / Info 头：帧头 4 字节（+2 CRC）+ side info 之后 */
    const size_t side = (fr.mpeg_ver == 3) ? (fr.channels == 1 ? 17u : 32u)
                                           : (fr.channels == 1 ? 9u : 17u);
    const size_t xing = pos + 4 + ((buf[pos + 1] & 0x01) ? 2u : 0u) + side;
    if (xing + 8 <= got &&
        (memcmp(buf + xing, "Xing", 4) == 0 || memcmp(buf + xing, "Info", 4) == 0)) {
        const uint32_t flags = mm_be32(buf + xing + 4);
        const size_t   p     = xing + 8;
        if ((flags & 0x0001u) && p + 4 <= got) {
            const uint32_t frames = mm_be32(buf + p);
            if (frames > 0 && fr.sample_rate > 0) {
                const uint64_t dur_ms =
                    (uint64_t)frames * (uint64_t)fr.samples * 1000ULL / (uint64_t)fr.sample_rate;
                if (dur_ms > 0) {
                    out->duration_ms = (uint32_t)dur_ms;
                    /* VBR：帧头码率没有意义，用"音频字节数 / 时长"当平均码率，
                     * 拖动定位按它换算才准 */
                    out->bitrate = (uint32_t)(((uint64_t)(file_size - (long)data_off) * 8ULL * 1000ULL) / dur_ms);
                }
            }
        }
    }

    ESP_LOGI(TAG, "mp3 probe: %u bps, %u ms, %u Hz, %uch (frame %u B)",
             (unsigned)out->bitrate, (unsigned)out->duration_ms,
             (unsigned)out->sample_rate, (unsigned)out->channels, (unsigned)fr.frame_size);
    free(buf);
    return ESP_OK;
}

/* ================================================================== 通用探测
 *
 * 需求（2026-10 用户报"部分音频文件播放卡顿、无法拖动、总时长无法显示"）：
 * 以前只有 media_meta_probe_mp3()，**只认 .mp3** —— 于是 FLAC / WAV / AAC 这些
 * 本地文件拿不到时长与平均码率：
 *   - GMF 的 MUSIC_INFO 对这些文件报 `0 bps, 0 ms`（真机日志：
 *     `audio_svc: music info: 48000 Hz 2ch 16bit 0 bps, 0 ms`）；
 *   - 没有时长 → 界面上总时长一直是 `--:--`、进度条不动；
 *   - 没有码率 → seek 的"时间→字节"算不出来 → 拖动直接报"定位失败：码率未知"。
 *
 * 这里按**魔数**（不是扩展名：用户的文件扩展名经常撒谎）分派，只读头部若干字节，
 * 同步小 IO，别在 LVGL 任务里调（调用方 audio_service 在播放器事件里调）。
 *
 * ⚠ 走"按块长度跳"而不是"逐字节扫"：FLAC 的内嵌封面（PICTURE 块）动辄几百 KB~几 MB，
 *   逐字节扫既慢又正是 ESP 那个解析器在真机上刷 `Not found frame in max frame size` 的原因。
 */

/* 码率兜底：容器里没有"码率"字段时（FLAC/WAV…）用 音频字节数 × 8 ÷ 时长 当平均码率。
 * 这正是 seek 需要的那个"平均"值（VBR 也成立）。 */
void mm_probe_fill_bitrate(uint32_t audio_bytes, audio_probe_t *out)
{
    if (out->bitrate == 0 && out->duration_ms > 0 && audio_bytes > 0) {
        out->bitrate = (uint32_t)(((uint64_t)audio_bytes * 8ULL * 1000ULL) / out->duration_ms);
    }
}

/*
 * FLAC：`fLaC` 之后是一串元数据块，**规范要求 STREAMINFO（类型 0）必须是第一块**。
 * 块头 4 字节：[1B: last<<7 | type][3B 大端: 数据长度]，数据紧跟其后。
 * STREAMINFO 34 字节（大端）：
 *   [0..1] 最小块 [2..3] 最大块 [4..6] 最小帧 [7..9] 最大帧
 *   [10..17] 20bit 采样率 | 3bit(声道-1) | 5bit(位深-1) | **36bit 总采样数**
 * 拿到总采样数就能算准时长；FLAC 没有码率字段，按文件大小估（见 mm_probe_fill_bitrate）。
 * 音频起始偏移 = 元数据链走完的位置（**不去读封面**，只按长度跳过）。
 */
/*
 * FLAC 元数据块链的"留证"（2026-10）：见 probe_flac 里 data_off 算飞时的告警。
 * 真机上两个 FLAC 的偏移分别飞到 94 MB / 138 MB（文件只有 38 / 36 MB），
 * 需要知道是哪一块的长度字段异常，才能判断是文件本身不标准还是我们解析错了。
 *
 * ⚠ 拆分时**保持原样**（没有收进 probe_flac 的局部结构）：蓝图 R5 建议收进去，
 *   但它们只用于诊断日志，改了就是行为变化；这里只做纯搬迁。
 */
#define FLAC_BLK_KEEP 16
static struct { uint8_t type; long len; long pos; } s_flac_blk[FLAC_BLK_KEEP];
static int s_flac_blk_seen;

static esp_err_t probe_flac(FILE *f, long file_size, audio_probe_t *out)
{
    uint8_t hdr[10];                    /* 够放 ID3v2 头（10 字节），也复用于 `fLaC`(4) */
    long    pos = ftell(f);
    /* 有些工具会在 FLAC 前面塞 ID3v2（10 字节头 + synchsafe 长度）——顺手跳过去，
     * 这一条比 ESP 自带的解析器还宽容 */
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        const uint32_t tag = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                             ((uint32_t)(hdr[8] & 0x7F) << 7) | (uint32_t)(hdr[9] & 0x7F);
        pos = 10 + (long)tag;
        if (fseek(f, pos, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
    } else if (fseek(f, pos, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "fLaC", 4) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    for (int guard = 0; guard < 64; guard++) {          /* 元数据块最多几十个，给个上限防跑飞 */
        uint8_t bh[4];
        if (fread(bh, 1, 4, f) != 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        const bool     last = (bh[0] & 0x80) != 0;
        const uint8_t  type = bh[0] & 0x7F;
        const uint32_t len  = mm_be24(bh + 1);
        if (type == 0) {                                /* STREAMINFO */
            uint8_t si[34];
            if (len < sizeof(si) || fread(si, 1, sizeof(si), f) != sizeof(si)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const uint32_t sr    = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) |
                                   ((uint32_t)si[12] >> 4);
            const uint8_t  ch    = (uint8_t)(((si[12] >> 1) & 0x07) + 1);
            const uint8_t  bits  = (uint8_t)((((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
            const uint64_t total = (((uint64_t)(si[13] & 0x0F) << 32) | ((uint64_t)si[14] << 24) |
                                    ((uint64_t)si[15] << 16) | ((uint64_t)si[16] << 8) |
                                    (uint64_t)si[17]);
            if (sr == 0) {
                return ESP_ERR_INVALID_SIZE;
            }
            out->sample_rate = sr;
            out->channels    = ch;
            if (bits) {
                /* 位深不进 audio_probe_t（结构里没有），但可以据此提示调用方 */
                ESP_LOGI(TAG, "flac: %u Hz %uch %ubit total %llu samples", (unsigned)sr,
                         (unsigned)ch, (unsigned)bits, (unsigned long long)total);
            }
            if (total > 0) {
                out->duration_ms = (uint32_t)((total * 1000ULL) / sr);
            }
            /* 音频数据的起点 = 当前位置（STREAMINFO 之后继续走完剩余块） */
            long data_off = ftell(f);
            /*
             * ★ 走完剩余的元数据块，得到音频数据的起始偏移。
             *
             * ⚠⚠ 这里踩过一个坑（2026-10，靠用户给的文件离线解剖才定位到）：
             *   原来"跳过一个块"只做了 data_off += nlen，**没有真的 fseek** ——
             *   于是下一轮的 fread(nb,1,4,f) 是从**上一块的数据中间**读的，
             *   读到的"长度"是垃圾 → 偏移越滚越大（真机飞到 94 MB / 138 MB，
             *   而文件只有 38 MB），码率只能靠兜底估算。
             *   那个文件的真实结构是：STREAMINFO(34) → VORBIS_COMMENT(111) →
             *   PICTURE(423169) → PADDING(4096) → 音频在 427430 处，块链完全标准
             *   —— 是**我们**读错了，不是文件不正常。
             *   所以每一步都必须"改偏移 + 真的移文件位置"，两者缺一不可。
             */
            const int blk_cap = (int)(sizeof(s_flac_blk) / sizeof(s_flac_blk[0]));
            s_flac_blk_seen = 0;
            uint32_t rest = len - (uint32_t)sizeof(si);   /* STREAMINFO 里已经读掉的部分之外 */
            if (rest > 0 && fseek(f, (long)rest, SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
            data_off += (long)rest;
            for (;;) {
                if (last) {
                    break;
                }
                uint8_t nb[4];
                if (fread(nb, 1, 4, f) != 4) {
                    break;
                }
                const long blk_pos = data_off;
                data_off += 4;                          /* 块头 */
                const uint32_t nlen = mm_be24(nb + 1);
                if (s_flac_blk_seen < blk_cap) {
                    s_flac_blk[s_flac_blk_seen].type = (uint8_t)(nb[0] & 0x7F);
                    s_flac_blk[s_flac_blk_seen].len  = (long)nlen;
                    s_flac_blk[s_flac_blk_seen].pos  = blk_pos;
                    s_flac_blk_seen++;
                }
                /* 长度离谱就直接停：继续走只会让偏移越飞越远 */
                if (nlen > (uint32_t)file_size) {
                    ESP_LOGW(TAG, "flac: block type %u len %lu exceeds file %ld, stop walking",
                             (unsigned)(nb[0] & 0x7F), (unsigned long)nlen, file_size);
                    break;
                }
                /* ★ 真的把文件位置移过去（这一步以前漏了，就是上面那个坑） */
                if (nlen > 0 && fseek(f, (long)nlen, SEEK_CUR) != 0) {
                    break;
                }
                data_off += (long)nlen;
                if ((nb[0] & 0x80) != 0) {              /* 最后一块 */
                    break;
                }
            }
            /*
             * 音频字节数：优先用"走完元数据块之后的位置"。
             * ⚠ 真机踩过：这个位置偶尔会算飞（异常元数据块链），于是
             *   `file_size - data_off` 不合法 → 码率一直是 0 → 这些 FLAC
             *   **拖不动进度条**（报"定位失败：码率未知"），日志里是
             *   `media_meta: audio probe: 0 bps, 194931 ms, ...`。
             *   所以这里加兜底 + 一行诊断，别再让"算不出来"悄悄变成 0。
             */
            uint32_t audio_bytes = 0;
            if (data_off > 0 && data_off < file_size) {
                audio_bytes = (uint32_t)(file_size - data_off);
            } else {
                audio_bytes = (uint32_t)(file_size - (long)sizeof(si) - 8);   /* fLaC + 块头 */
                /*
                 * ⚠ 把**块链本身**打出来：真机上这两个 FLAC 的数据偏移分别算到 94 MB /
                 *   138 MB，而文件只有 38 MB / 36 MB —— 说明某个块的"长度"字段被读成了
                 *   巨大的值（或块结构与常规不同）。同一个异常结构极可能就是 ESP 解析器
                 *   一直刷 `Not found frame in max frame size N` 的原因，所以这里要把
                 *   每一步的 type/len 留证：块头 = [1B last<<7|type][3B 大端长度]。
                 */
                ESP_LOGW(TAG, "flac: bad data offset %ld (file %ld, %u blocks seen:",
                         data_off, file_size, (unsigned)s_flac_blk_seen);
                for (int k = 0; k < s_flac_blk_seen; k++) {
                    ESP_LOGW(TAG, "flac:   block[%d] type %u, len %ld, pos %ld",
                             k, (unsigned)s_flac_blk[k].type, s_flac_blk[k].len,
                             s_flac_blk[k].pos);
                }
                ESP_LOGW(TAG, "flac: fall back to %u B", (unsigned)audio_bytes);
            }
            mm_probe_fill_bitrate(audio_bytes, out);
            ESP_LOGI(TAG, "flac: data_off %ld, audio %u B, bitrate %u bps (avg)",
                     data_off, (unsigned)audio_bytes, (unsigned)out->bitrate);
            return ESP_OK;
        }
        if (fseek(f, (long)len, SEEK_CUR) != 0) {       /* 只按长度跳，不读封面 */
            return ESP_FAIL;
        }
        if (last) {
            break;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;                       /* 没找到 STREAMINFO：按未知处理 */
}

/*
 * WAV：`RIFF` + 大小 + `WAVE`，之后是块链：`fmt `（采样率/声道/位深/字节率）
 * 与 `data`（音频字节数）。时长 = data 大小 ÷ 字节率。都是小端。
 */
static esp_err_t probe_wav(FILE *f, long file_size, audio_probe_t *out)
{
    uint8_t h[12];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "WAVE", 4) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t byte_rate = 0;
    uint32_t data_size = 0;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
            break;
        }
        const uint32_t sz = mm_le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            const size_t want = (sz < sizeof(fmt)) ? sz : sizeof(fmt);
            if (fread(fmt, 1, want, f) != want) {
                break;
            }
            out->channels    = (uint8_t)mm_le16(fmt + 2);
            out->sample_rate = mm_le32(fmt + 4);
            byte_rate        = mm_le32(fmt + 8);
            if (sz > want && fseek(f, (long)(sz - want), SEEK_CUR) != 0) {
                break;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            data_size = sz;
            break;                                      /* 音频数据起点 = 当前位置 */
        } else {
            if (fseek(f, (long)sz + (long)(sz & 1u), SEEK_CUR) != 0) {
                break;                                  /* 块按偶数字节对齐 */
            }
        }
    }
    if (out->sample_rate == 0 || byte_rate == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (data_size == 0) {                               /* 有些文件 data 长度写 0：按剩余文件算 */
        const long here = ftell(f);
        if (here > 0 && file_size > here) {
            data_size = (uint32_t)(file_size - here);
        }
    }
    if (data_size > 0) {
        out->duration_ms = (uint32_t)(((uint64_t)data_size * 1000ULL) / byte_rate);
        out->bitrate     = byte_rate * 8u;
    }
    return ESP_OK;
}

/*
 * ADTS 封装的 AAC（.aac / 部分 .m4a 的裸流）：头 7 字节里带
 *   profile / 采样率索引 / 声道 / **帧长度(13bit)** / 每帧 1024 采样。
 * 头信息足够算出"每帧字节数 ÷ 每帧时长"= 平均码率；时长用文件大小估
 * （AAC 没有全局头，逐帧扫太慢也不值得 —— 拖动定位要的是平均码率）。
 */
static const uint32_t k_aac_rate_tab[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                             22050, 16000, 12000, 11025, 8000, 7350 };

static esp_err_t probe_adts_aac(FILE *f, long file_size, audio_probe_t *out)
{
    uint8_t h[7];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) {
        return ESP_ERR_NOT_SUPPORTED;                   /* 不是 ADTS 同步头 */
    }
    const uint32_t sfi = (uint32_t)((h[2] >> 2) & 0x0F);
    if (sfi >= 13) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t sr        = k_aac_rate_tab[sfi];
    const uint32_t frame_len = (((uint32_t)h[3] & 0x03u) << 11) | ((uint32_t)h[4] << 3) |
                               ((uint32_t)h[5] >> 5);
    const uint32_t ch        = (uint32_t)(((h[2] & 0x01u) << 2) | (h[3] >> 6));
    if (sr == 0 || frame_len <= 7) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    out->sample_rate = sr;
    out->channels    = (uint8_t)((ch == 0) ? 2 : ch);   /* ADTS 里 0 表示"按 PCE"，通常 2ch */
    /* 每帧 1024 采样：帧字节数 ÷ (1024/采样率) = 瞬时码率；用它当平均码率够 seek 用 */
    out->bitrate     = (uint32_t)(((uint64_t)frame_len * 8ULL * sr) / 1024ULL);
    if (out->bitrate > 0) {
        const long here = 7;
        if (file_size > here) {
            out->duration_ms =
                (uint32_t)(((uint64_t)(file_size - here) * 8ULL * 1000ULL) / out->bitrate);
        }
    }
    return ESP_OK;
}

esp_err_t mm_probe_audio(const char *audio_path, audio_probe_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* MP3 走原有那条（它认得 ID3/Xing/Info，比这里重造一遍靠谱） */
    const char *dot = strrchr(audio_path, '.');
    if (dot && strcasecmp(dot, ".mp3") == 0) {
        return mm_probe_mp3(audio_path, out);
    }

    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    const long file_size = ftell(f);
    if (file_size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = probe_flac(f, file_size, out);
    if (ret != ESP_OK) {
        rewind(f);
        ret = probe_wav(f, file_size, out);
    }
    if (ret != ESP_OK) {
        rewind(f);
        ret = probe_adts_aac(f, file_size, out);
    }
    fclose(f);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "audio probe: %u bps, %u ms, %u Hz, %uch (%s)",
                 (unsigned)out->bitrate, (unsigned)out->duration_ms,
                 (unsigned)out->sample_rate, (unsigned)out->channels, audio_path);
    }
    return ret;
}
