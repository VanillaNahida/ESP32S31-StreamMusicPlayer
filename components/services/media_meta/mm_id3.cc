/*
 * mm_id3.cc —— ID3v2（MP3）帧解析：封面帧 + 歌词帧 + ID3 文本编码
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 * 原 381–478（id3_cover）、1319–1576（内嵌歌词 / 文本编码 / SYLT / id3_lyrics）。
 * **纯搬迁**，注释随函数一起搬（其中"unsync / 扩展头直接放弃"那条是真机踩过的坑）。
 *
 * 只解析需要的帧，不整块读 ID3（封面可能上百 KB）——帧头 fseek 过去，
 * 只读目标帧的 body。
 *
 * 依赖：mm_bits（字节序/synchsafe/MIME 判定）、mm_cover（mm_copy_image，
 * 与 FLAC 封面共用的"把图片字节拷给调用方"）。
 */
#include "internal/mm_id3.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "internal/mm_bits.h"
#include "internal/mm_cover.h"

static const char *TAG = "media_meta";

/* ---------------------------------------------------------- ID3v2 (MP3) */

esp_err_t mm_id3_cover(FILE *f, uint8_t **out, size_t *out_len, bool *is_png)
{
    uint8_t hdr[10];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (memcmp(hdr, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint8_t major = hdr[3];
    if (major < 2 || major > 4) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 位 7 = unsynchronisation、位 6 = extended header：都会打乱帧布局，
     * 少见，直接放弃（宁可不显示封面，也不要解析出垃圾） */
    if (hdr[5] & 0xC0) {
        ESP_LOGD(TAG, "id3 flags 0x%02x not handled", (unsigned)hdr[5]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t tag_end = 10u + (size_t)mm_synchsafe32(hdr + 6);
    const size_t fhdr_sz = (major <= 2) ? 6u : 10u;
    const size_t id_sz   = (major <= 2) ? 3u : 4u;

    for (size_t pos = 10; pos + fhdr_sz <= tag_end; ) {
        uint8_t fh[10];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(fh, 1, fhdr_sz, f) != fhdr_sz) {
            break;
        }
        if (fh[0] == 0) {
            break;                      /* 填充区，后面没有帧了 */
        }
        char id[5] = { 0, 0, 0, 0, 0 };
        memcpy(id, fh, id_sz);

        size_t fsz;
        if (major <= 2) {
            fsz = mm_be24(fh + 3);
        } else if (major == 3) {
            fsz = mm_be32(fh + 4);
        } else {
            fsz = mm_synchsafe32(fh + 4);
        }
        if (fsz == 0 || pos + fhdr_sz + fsz > tag_end) {
            break;
        }

        if (strcmp(id, "APIC") == 0 || strcmp(id, "PIC") == 0) {
            uint8_t *body = (uint8_t *)malloc(fsz);
            if (!body) {
                return ESP_ERR_NO_MEM;
            }
            size_t got = fread(body, 1, fsz, f);
            if (got != fsz) {
                free(body);
                break;
            }

            size_t o   = 1;             /* 0: 文本编码 */
            bool   png = false;
            if (major <= 2) {
                /* PIC: 3 字节图片格式 "JPG" / "PNG" */
                png = (got >= 4) && (memcmp(body + o, "PNG", 3) == 0);
                o += 3;
            } else {
                /* APIC: MIME 是 latin1、以 0 结尾 */
                const char *mime = (const char *)(body + o);
                size_t mlen = 0;
                while (o + mlen < got && mime[mlen] != '\0') {
                    mlen++;
                }
                png = mm_mime_is_png(mime);
                o += mlen + 1;
            }
            o += 1;                     /* 图片类型（3 = 封面） */

            /* 描述串：编码 1/2 是 UTF-16（双字节 0 结尾），其余单字节 */
            if (body[0] == 1 || body[0] == 2) {
                while (o + 1 < got && !(body[o] == 0 && body[o + 1] == 0)) {
                    o += 2;
                }
                o += 2;
            } else {
                while (o < got && body[o] != 0) {
                    o++;
                }
                o += 1;
            }

            esp_err_t ret = (o < got) ? mm_copy_image(out, out_len, is_png, body + o, got - o, png)
                                      : ESP_ERR_NOT_FOUND;
            free(body);
            return ret;
        }
        pos += fhdr_sz + fsz;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ================================================================== 内嵌歌词
 *
 * 需求：从 ID3 标签读歌词 → 自动保存到 SD 卡的 /lrc 目录；播放时按
 *   1) 歌曲同目录同名 .lrc
 *   2) <挂载点>/lrc/<同名>.lrc（本工程落盘的位置）
 *   3) 内嵌歌词（ID3v2 的 USLT；没有 USLT 时把 SYLT 时间轴转成 LRC）
 * 的顺序找。
 *
 * 只解析这一帧的内容，不整块读 ID3（封面可能上百 KB）——帧头 fseek 过去，
 * 只读 USLT/SYLT 这一帧的 body。
 */

/* ---------------------------------------------------------- 文本编码转换 */

/* ID3 文本 → UTF-8，返回写入字节数（保证 dst NUL 结尾）。
 * enc: 0=Latin1, 1=UTF-16(带 BOM), 2=UTF-16BE, 3=UTF-8 */
size_t mm_id3_text_to_utf8(uint8_t enc, const uint8_t *src, size_t src_len,
                           char *dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0) {
        return 0;
    }
    size_t o = 0;

    if (enc == 0 || enc == 3) {
        for (size_t i = 0; i < src_len && o + 1 < dst_cap; i++) {
            const uint8_t c = src[i];
            if (c == 0) {                       /* 文本靠 NUL 结束 */
                break;
            }
            if (enc == 3 || c < 0x80) {
                dst[o++] = (char)c;
            } else {
                if (o + 2 >= dst_cap) {
                    break;
                }
                dst[o++] = (char)(0xC0 | (c >> 6));
                dst[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        dst[o] = '\0';
        return o;
    }

    /* UTF-16（1 = 带 BOM，2 = 大端） */
    bool le = (enc == 2) ? false : true;
    size_t i = 0;
    if (enc == 1 && src_len >= 2) {
        if (src[0] == 0xFF && src[1] == 0xFE) {
            le = true;  i = 2;
        } else if (src[0] == 0xFE && src[1] == 0xFF) {
            le = false; i = 2;
        }
    }
    while (i + 1 < src_len) {
        uint32_t cp = le ? (uint32_t)(src[i] | ((uint32_t)src[i + 1] << 8))
                         : (uint32_t)(((uint32_t)src[i] << 8) | src[i + 1]);
        i += 2;
        if (cp == 0) {
            break;
        }
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < src_len) {   /* 代理对 */
            const uint32_t lo = le ? (uint32_t)(src[i] | ((uint32_t)src[i + 1] << 8))
                                   : (uint32_t)(((uint32_t)src[i] << 8) | src[i + 1]);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 2;
            }
        }
        if (o + 4 >= dst_cap) {
            break;
        }
        if (cp < 0x80u) {
            dst[o++] = (char)cp;
        } else if (cp < 0x800u) {
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000u) {
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[o++] = (char)(0xF0 | (cp >> 18));
            dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[o] = '\0';
    return o;
}

/* 跳过一个"编码相关"的 NUL 结尾串（1/2 是双字节 0 结尾），返回新下标 */
static size_t skip_encoded_string(uint8_t enc, const uint8_t *b, size_t len, size_t o)
{
    if (enc == 1 || enc == 2) {
        while (o + 1 < len && !(b[o] == 0 && b[o + 1] == 0)) {
            o += 2;
        }
        return (o + 2 <= len) ? o + 2 : len;
    }
    while (o < len && b[o] != 0) {
        o++;
    }
    return (o < len) ? o + 1 : len;
}

/* SYLT（同步歌词）→ LRC 文本；失败返回 NULL */
char *mm_id3_sylt_to_lrc(const uint8_t *b, size_t len)
{
    if (len < 8) {
        return NULL;
    }
    const uint8_t enc  = b[0];
    const int     tsf  = b[4];              /* 1 = MPEG 帧，2 = 毫秒 */
    size_t        o    = skip_encoded_string(enc, b, len, 6);

    char  *out = (char *)malloc(len * 2 + 128);
    if (!out) {
        return NULL;
    }
    const size_t cap = len * 2 + 128;
    size_t w = 0;

    while (o + 1 < len) {
        size_t tlen = 0;
        if (enc == 1 || enc == 2) {
            while (o + tlen + 1 < len && !(b[o + tlen] == 0 && b[o + tlen + 1] == 0)) {
                tlen += 2;
            }
        } else {
            while (o + tlen < len && b[o + tlen] != 0) {
                tlen++;
            }
        }
        if (o + tlen >= len) {
            break;
        }
        char txt[240];
        mm_id3_text_to_utf8(enc, b + o, tlen, txt, sizeof(txt));
        o += tlen + ((enc == 1 || enc == 2) ? 2u : 1u);
        if (o + 4 > len) {
            break;
        }
        const uint32_t raw = mm_be32(b + o);
        o += 4;
        /* 帧数 → 毫秒：MPEG1 Layer3 @44.1k 一帧 1152/44100 ≈ 26.12 ms；
         * 只是个近似（SYLT 用帧数时多半是 MPEG1），够 LRC 用 */
        const uint32_t ms = (tsf == 2) ? raw : (uint32_t)(((uint64_t)raw * 2612ULL) / 100ULL);

        char line[300];
        int n = snprintf(line, sizeof(line), "[%02u:%02u.%02u]%s\n",
                         (unsigned)(ms / 60000u), (unsigned)((ms / 1000u) % 60u),
                         (unsigned)((ms % 1000u) / 10u), txt);
        if (n <= 0 || w + (size_t)n + 1 >= cap) {
            break;
        }
        memcpy(out + w, line, (size_t)n);
        w += (size_t)n;
    }

    if (w == 0) {
        free(out);
        return NULL;
    }
    out[w] = '\0';
    return out;
}

/* 在 ID3v2 里找歌词帧；USLT 优先，没有就用 SYLT 转的 LRC */
esp_err_t mm_id3_lyrics(FILE *f, char **out)
{
    uint8_t hdr[10];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (memcmp(hdr, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint8_t major = hdr[3];
    if (major < 2 || major > 4 || (hdr[5] & 0xC0)) {
        return ESP_ERR_NOT_SUPPORTED;       /* 版本不认识 / unsync / 扩展头 */
    }

    const size_t tag_end = 10u + (size_t)mm_synchsafe32(hdr + 6);
    const size_t fhdr_sz = (major <= 2) ? 6u : 10u;
    const size_t id_sz   = (major <= 2) ? 3u : 4u;
    char        *sylt    = NULL;

    for (size_t pos = 10; pos + fhdr_sz <= tag_end; ) {
        uint8_t fh[10];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(fh, 1, fhdr_sz, f) != fhdr_sz) {
            break;
        }
        if (fh[0] == 0) {
            break;
        }
        char id[5] = { 0, 0, 0, 0, 0 };
        memcpy(id, fh, id_sz);
        size_t fsz;
        if (major <= 2) {
            fsz = mm_be24(fh + 3);
        } else if (major == 3) {
            fsz = mm_be32(fh + 4);
        } else {
            fsz = mm_synchsafe32(fh + 4);
        }
        if (fsz == 0 || pos + fhdr_sz + fsz > tag_end) {
            break;
        }

        const bool is_uslt = (strcmp(id, "USLT") == 0);
        const bool is_sylt = (strcmp(id, "SYLT") == 0);
        if ((is_uslt || is_sylt) && fsz >= 6 && fsz <= 128u * 1024u) {
            uint8_t *body = (uint8_t *)malloc(fsz);
            if (!body) {
                free(sylt);
                return ESP_ERR_NO_MEM;
            }
            if (fread(body, 1, fsz, f) != fsz) {
                free(body);
                break;
            }
            if (is_uslt) {
                /* enc(1) lang(3) 描述串 正文 */
                size_t o = skip_encoded_string(body[0], body, fsz, 4);
                char  *txt = (char *)malloc((fsz - o) * 2 + 8);
                if (txt) {
                    size_t n = mm_id3_text_to_utf8(body[0], body + o, fsz - o, txt, (fsz - o) * 2 + 8);
                    free(body);
                    if (n > 0) {
                        free(sylt);
                        *out = txt;
                        return ESP_OK;
                    }
                    free(txt);
                } else {
                    free(body);
                }
            } else {
                char *txt = mm_id3_sylt_to_lrc(body, fsz);
                free(body);
                if (txt && !sylt) {
                    sylt = txt;             /* 先留着，USLT 才是首选 */
                } else {
                    free(txt);
                }
            }
        }
        pos += fhdr_sz + fsz;
    }

    if (sylt) {
        *out = sylt;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
