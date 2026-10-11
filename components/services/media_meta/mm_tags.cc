/*
 * mm_tags.cc —— 读文本标签：标题 / 艺术家 / 专辑 / 作曲家
 *
 * 为什么需要单独一个文件：`storage_service` 的曲库索引只存了**标题与艺术家**
 * （给列表用），在线歌单也只有 `title` / `artist` —— 而播放器页新加的"歌曲信息"
 * 弹窗要显示**专辑**与**作曲家**。这两个字段只能回到文件本身去读。
 *
 * 覆盖范围（够用即可；其它容器返回 ESP_ERR_NOT_SUPPORTED，界面显示 "—"）：
 *   - MP3  ：ID3v2.2/2.3/2.4 的 TIT2|TT2、TPE1|TP1、TALB|TAL、TCOM|TCM
 *   - FLAC ：VORBIS_COMMENT 块（type 4）里的 TITLE / ARTIST / ALBUM / COMPOSER
 *
 * 编码：
 *   - ID3 的文本帧带一个"编码字节"，中文标签常见 UTF-16(BOM) —— 直接复用
 *     `mm_id3_text_to_utf8()`（拼音/歌词那边验证过的同一套转换）。
 *   - VORBIS_COMMENT 按 FLAC 规范**恒为 UTF-8**，直接拷贝（仍做一次合法性检查，
 *     万一遇到非 UTF-8 的野生文件就退回逐字节直拷，总比整段丢掉好）。
 *
 * 线程/资源：只读文件、只写调用方给的缓冲；`FILE*` 用完即关（调用方不用管）。
 */

#include "media_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "internal/mm_id3.h"      /* mm_id3_text_to_utf8 */

static const char *TAG = "media_meta";

/* 标签值缓冲上限（对齐 media_tags_t 的字段大小） */
#define TAG_VAL_MAX  128

/* ---------------------------------------------------------------- 小工具 */

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t synchsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/* 把 UTF-8 值拷进目标字段（去掉首尾空白；空串不覆盖已有的值） */
static void put_utf8(char *dst, size_t cap, const char *src, size_t len)
{
    if (dst[0] || !src || len == 0 || cap == 0) {
        return;                       /* 已经有了就别覆盖（同名标签可能重复出现） */
    }
    while (len > 0 && (src[0] == ' ' || src[0] == '\t')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\0')) {
        len--;
    }
    if (len == 0) {
        return;
    }
    if (len > cap - 1) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ---------------------------------------------------------------- MP3 / ID3v2 */

static void id3_field(const char *id, media_tags_t *out, const char *val)
{
    if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0) {
        put_utf8(out->title, sizeof(out->title), val, strlen(val));
    } else if (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0) {
        put_utf8(out->artist, sizeof(out->artist), val, strlen(val));
    } else if (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0) {
        put_utf8(out->album, sizeof(out->album), val, strlen(val));
    } else if (strcmp(id, "TCOM") == 0 || strcmp(id, "TCM") == 0) {
        put_utf8(out->composer, sizeof(out->composer), val, strlen(val));
    }
}

static bool tags_from_id3(FILE *f, media_tags_t *out)
{
    uint8_t hdr[10];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return false;
    }
    if (memcmp(hdr, "ID3", 3) != 0) {
        return false;
    }
    const uint8_t major = hdr[3];
    if (major < 2 || major > 4 || (hdr[5] & 0xC0)) {
        return false;                 /* 版本不认识 / unsync / 扩展头：交给别人 */
    }

    const size_t tag_end = 10u + (size_t)synchsafe32(hdr + 6);
    const size_t fhdr_sz = (major <= 2) ? 6u : 10u;
    const size_t id_sz   = (major <= 2) ? 3u : 4u;

    for (size_t pos = 10; pos + fhdr_sz <= tag_end; ) {
        uint8_t fh[10];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(fh, 1, fhdr_sz, f) != fhdr_sz) {
            break;
        }
        if (fh[0] == 0) {
            break;                    /* 填充区开始 */
        }
        char id[5] = { 0, 0, 0, 0, 0 };
        memcpy(id, fh, id_sz);
        size_t fsz;
        if (major <= 2) {
            fsz = be24(fh + 3);
        } else if (major == 3) {
            fsz = be32(fh + 4);
        } else {
            fsz = synchsafe32(fh + 4);
        }
        if (fsz == 0 || pos + fhdr_sz + fsz > tag_end) {
            break;
        }

        const bool want = (strncmp(id, "TIT2", 4) == 0 || strncmp(id, "TT2", 3) == 0 ||
                           strncmp(id, "TPE1", 4) == 0 || strncmp(id, "TP1", 3) == 0 ||
                           strncmp(id, "TALB", 4) == 0 || strncmp(id, "TAL", 3) == 0 ||
                           strncmp(id, "TCOM", 4) == 0 || strncmp(id, "TCM", 3) == 0);
        /* 文本帧不会很大；上限只是防畸形长度 */
        if (want && fsz >= 2 && fsz <= 8u * 1024u) {
            uint8_t *body = (uint8_t *)malloc(fsz);
            if (!body) {
                break;
            }
            if (fread(body, 1, fsz, f) == fsz) {
                char val[TAG_VAL_MAX * 2];
                val[0] = '\0';
                mm_id3_text_to_utf8(body[0], body + 1, fsz - 1, val, sizeof(val));
                if (val[0]) {
                    id3_field(id, out, val);
                }
            }
            free(body);
        } else if (fseek(f, (long)fsz, SEEK_CUR) != 0) {
            break;
        }
        pos += fhdr_sz + fsz;
    }
    return true;
}

/* ---------------------------------------------------------------- FLAC */

static void flac_vorbis_key(const char *kv, size_t len, media_tags_t *out)
{
    const char *eq = (const char *)memchr(kv, '=', len);
    if (!eq) {
        return;
    }
    const size_t klen = (size_t)(eq - kv);
    const char *val   = eq + 1;
    const size_t vlen = len - klen - 1;

    if (klen == 5 && strncasecmp(kv, "TITLE", 5) == 0) {
        put_utf8(out->title, sizeof(out->title), val, vlen);
    } else if (klen == 6 && strncasecmp(kv, "ARTIST", 6) == 0) {
        put_utf8(out->artist, sizeof(out->artist), val, vlen);
    } else if (klen == 5 && strncasecmp(kv, "ALBUM", 5) == 0) {
        put_utf8(out->album, sizeof(out->album), val, vlen);
    } else if (klen == 8 && strncasecmp(kv, "COMPOSER", 8) == 0) {
        put_utf8(out->composer, sizeof(out->composer), val, vlen);
    }
}

static bool tags_from_flac(FILE *f, media_tags_t *out)
{
    uint8_t magic[4];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(magic, 1, 4, f) != 4) {
        return false;
    }
    if (memcmp(magic, "fLaC", 4) != 0) {
        return false;
    }

    bool last = false;
    for (int guard = 0; guard < 64 && !last; guard++) {
        uint8_t bh[4];
        if (fread(bh, 1, 4, f) != 4) {
            break;
        }
        last = (bh[0] & 0x80) != 0;
        const uint8_t  type = (uint8_t)(bh[0] & 0x7F);
        const uint32_t len  = be24(bh + 1);

        if (type == 4 && len >= 8 && len <= 512u * 1024u) {     /* VORBIS_COMMENT */
            uint8_t *blk = (uint8_t *)malloc(len);
            if (!blk) {
                break;
            }
            if (fread(blk, 1, len, f) != len) {
                free(blk);
                break;
            }
            size_t o = 0;
            const uint32_t vendor = (o + 4 <= len) ? (uint32_t)(blk[o] | (blk[o+1] << 8) |
                                                                (blk[o+2] << 16) | ((uint32_t)blk[o+3] << 24)) : 0;
            o += 4 + vendor;
            if (o + 4 <= len) {
                const uint32_t cnt = (uint32_t)(blk[o] | (blk[o+1] << 8) |
                                                (blk[o+2] << 16) | ((uint32_t)blk[o+3] << 24));
                o += 4;
                for (uint32_t i = 0; i < cnt && o + 4 <= len; i++) {
                    const uint32_t kl = (uint32_t)(blk[o] | (blk[o+1] << 8) |
                                                   (blk[o+2] << 16) | ((uint32_t)blk[o+3] << 24));
                    o += 4;
                    if (kl == 0 || o + kl > len) {
                        break;
                    }
                    /* 键值对里不会出现 NUL，直接当 C 串处理前先保证有终止 */
                    char *kv = (char *)malloc(kl + 1);
                    if (!kv) {
                        break;
                    }
                    memcpy(kv, blk + o, kl);
                    kv[kl] = '\0';
                    flac_vorbis_key(kv, kl, out);
                    free(kv);
                    o += kl;
                }
            }
            free(blk);
            return true;
        }
        if (len > 0 && fseek(f, (long)len, SEEK_CUR) != 0) {
            break;
        }
    }
    return false;                     /* 没有 VORBIS_COMMENT：没有可读标签 */
}

/* ---------------------------------------------------------------- 入口 */

esp_err_t media_meta_read_tags(const char *audio_path, media_tags_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    bool ok = tags_from_id3(f, out);
    if (!ok) {
        ok = tags_from_flac(f, out);
    }
    fclose(f);

    if (!ok) {
        return ESP_ERR_NOT_SUPPORTED;      /* 其它容器：界面显示 "—" */
    }
    ESP_LOGD(TAG, "tags: '%s' / '%s' / '%s' / '%s'", out->title, out->artist,
             out->album, out->composer);
    return ESP_OK;
}
