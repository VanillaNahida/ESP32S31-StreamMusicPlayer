/*
 * mm_cover.cc —— 封面提取编排（内嵌封面 / 独立图文件 / 内存缓冲）
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 * 原 298–312（分区注释）、365–377、482–549、641–778 行。**纯搬迁**，
 * 注释随函数一起搬。
 *
 * 像素缓冲的所有权契约一个字都没变：分配在 mm_image.cc 里用
 * **jpeg_calloc_align**（16 字节对齐），释放走 mm_cover_free() 的
 * **jpeg_free_align** —— 这一对必须成对，换掉任何一边就是堆损坏（蓝图 R2）。
 */
#include "internal/mm_cover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_jpeg_common.h"    /* jpeg_free_align：与 mm_image.cc 的分配配对 */

#include "internal/mm_bits.h"
#include "internal/mm_id3.h"
#include "internal/mm_image.h"

static const char *TAG = "media_meta";

/* ================================================================== 内嵌封面
 *
 * 支持的容器 / 位置：
 *   MP3  ID3v2.2 的 PIC 帧、ID3v2.3/2.4 的 APIC 帧
 *   FLAC 元数据块里的 PICTURE(type 6)
 * 其它容器（M4A 的 covr atom、OGG 的 METADATA_BLOCK_PICTURE）留到后续里程碑。
 *
 * 编码：JPEG 走 esp_new_jpeg，PNG 走官方 libpng 组件（见 mm_image.cc）。
 * 实际按**文件魔数**分派，不看 MIME —— 不少 MP3 的 APIC 把 MIME 写成 image/jpeg
 * 却塞的是 PNG（或反过来），信 MIME 会解不出来。
 * 其它编码返回 ESP_ERR_NOT_SUPPORTED。
 */

/* 把整个文件里的图片字节拷出来；成功返回 ESP_OK 并填 out/out_len/is_png */
esp_err_t mm_copy_image(uint8_t **out, size_t *out_len, bool *is_png,
                        const uint8_t *src, size_t len, bool png)
{
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, src, len);
    *out     = buf;
    *out_len = len;
    *is_png  = png;
    return ESP_OK;
}

/* ---------------------------------------------------------- FLAC */

static esp_err_t flac_cover(FILE *f, uint8_t **out, size_t *out_len, bool *is_png)
{
    uint8_t magic[4];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (memcmp(magic, "fLaC", 4) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t pos = 4;
    for (int i = 0; i < 64; i++) {      /* 元数据块数量有限，防跑飞 */
        uint8_t bh[4];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(bh, 1, sizeof(bh), f) != sizeof(bh)) {
            break;
        }
        const bool  last = (bh[0] & 0x80) != 0;
        const int   type = bh[0] & 0x7F;
        const size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | (size_t)bh[3];

        if (type == 6 && len > 32) {    /* PICTURE */
            uint8_t *body = (uint8_t *)malloc(len);
            if (!body) {
                return ESP_ERR_NO_MEM;
            }
            if (fread(body, 1, len, f) != len) {
                free(body);
                break;
            }
            /* picture type(4) mime_len(4) mime  desc_len(4) desc  w/h/depth/colors(16) data_len(4) data */
            size_t o = 4;
            if (o + 4 > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            size_t mlen = mm_be32(body + o); o += 4;
            if (o + mlen > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            bool png = mm_mime_is_png((const char *)(body + o));
            o += mlen;
            if (o + 4 > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            size_t dlen = mm_be32(body + o); o += 4;
            if (o + dlen + 16 + 4 > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            o += dlen + 16;
            size_t data_len = mm_be32(body + o); o += 4;
            if (o + data_len > len || data_len == 0) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            esp_err_t ret = mm_copy_image(out, out_len, is_png, body + o, data_len, png);
            free(body);
            return ret;
        }
        if (last) {
            break;
        }
        pos += 4 + len;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------- 对外入口 */

esp_err_t mm_cover_from_audio(const char *audio_path, int max_dim,
                              uint8_t **pixels, int *out_w, int *out_h)
{
    if (!audio_path || !pixels || !out_w || !out_h || max_dim <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *pixels = NULL;
    *out_w  = 0;
    *out_h  = 0;

    const char *dot = strrchr(audio_path, '.');
    if (!dot) {
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *raw    = NULL;
    size_t   raw_sz = 0;
    bool     is_png = false;   /* 只为日志：解码内部按魔数分派 */
    esp_err_t ret;
    if (strcasecmp(dot, ".mp3") == 0) {
        ret = mm_id3_cover(f, &raw, &raw_sz, &is_png);
    } else if (strcasecmp(dot, ".flac") == 0) {
        ret = flac_cover(f, &raw, &raw_sz, &is_png);
    } else {
        ret = ESP_ERR_NOT_FOUND;        /* M4A / OGG 的封面位置在后续里程碑补 */
    }
    fclose(f);

    if (ret != ESP_OK) {
        return ret;
    }
    /* PNG 也走同一条路（mm_image_to_rgb565 内部按魔数分派） */
    ret = mm_image_to_rgb565(raw, raw_sz, max_dim, pixels, out_w, out_h);
    free(raw);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "cover decoded %dx%d (max_dim=%d)", *out_w, *out_h, max_dim);
    } else {
        ESP_LOGW(TAG, "cover decode failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void mm_cover_free(uint8_t *pixels)
{
    if (pixels) {
        jpeg_free_align(pixels);
    }
}

/*
 * 独立的封面图文件（JPEG 或 PNG）→ RGB565。
 *
 * 在线歌曲（网易云/Meting）的封面是一张单独的文件，不在音频文件里，
 * 所以不能走 id3_cover/flac_cover 那条路；把整张图读进内存后复用同一个
 * mm_image_to_rgb565()，出来的像素格式与 mm_cover_from_audio() 完全一致
 * （RGB565 LE，16 字节对齐，用 mm_cover_free() 释放）。
 *
 * **JPEG / PNG 都能吃**：缓存下来的文件名带着 .jpg，但 Meting 的 pic 接口
 * 不保证给的就是 JPEG，所以这里同样按文件魔数分派（mm_image_to_rgb565 内部判断），
 * 扩展名只当提示。
 *
 * 封面图常见 100~300 KB。上限跟在线侧的 COVER_MAX_BYTES 对齐（都是 4 MB）——
 * 两边不一致会出现"下下来了却解不了"这种最难查的组合。
 * 内存安全不靠这个值兜：PNG 那条路另有"边长 > max_dim × 8 直接跳过"的判定
 * （见 mm_image_to_rgb565 里的 side_limit），JPEG 也是先按原始尺寸解、超限就拒。
 */
#define MEDIA_META_COVER_FILE_MAX (4u * 1024u * 1024u)

esp_err_t mm_cover_from_file(const char *img_path, int max_dim,
                             uint8_t **pixels, int *out_w, int *out_h)
{
    if (!img_path || !pixels || !out_w || !out_h || max_dim <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *pixels = NULL;
    *out_w  = 0;
    *out_h  = 0;

    FILE *f = fopen(img_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    fclose(f);
    if (sz <= 3 || (size_t)sz > MEDIA_META_COVER_FILE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    f = fopen(img_path, "rb");
    if (!f) {
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t ret = mm_image_to_rgb565(buf, got, max_dim, pixels, out_w, out_h);
    free(buf);
    return ret;
}

/*
 * 内存缓冲版：封面字节已经在 PSRAM 里（在线服务不再把封面写 SD），
 * 直接喂给同一套 mm_image_to_rgb565()，少一次 SD 读、也不占临时 malloc。
 * 上限与文件版一致（2 MB），解码行为完全相同。
 */
esp_err_t mm_cover_from_mem(const uint8_t *data, size_t len, int max_dim,
                            uint8_t **pixels, int *out_w, int *out_h)
{
    if (!data || !pixels || !out_w || !out_h || max_dim <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *pixels = NULL;
    *out_w  = 0;
    *out_h  = 0;
    if (len <= 3 || len > MEDIA_META_COVER_FILE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return mm_image_to_rgb565(data, len, max_dim, pixels, out_w, out_h);
}
