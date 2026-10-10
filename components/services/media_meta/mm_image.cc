/*
 * mm_image.cc —— 封面/图片解码：JPEG（esp_new_jpeg）与 PNG（官方 libpng）→ RGB565
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 * 原 551–639（JPEG → RGB565）、1751–1948（全尺寸 → 缩放）。**纯搬迁**，
 * 注释随函数一起搬（"为什么不再用 esp_new_jpeg 的 scale"那段是真机踩过的坑）。
 *
 * ⚠⚠ 输出像素缓冲只能用 **jpeg_calloc_align** 分配（原 1894 / 1909 两处），
 *   因为释放端是 mm_cover_free() 的 **jpeg_free_align**。换成 malloc /
 *   heap_caps_malloc 就是堆损坏（蓝图 R2）。搬运时一个字都没动。
 *
 * 解码器在这里、容器解析在 mm_probe.cc / mm_id3.cc —— 拆开后"解不出来"与
 * "读不出来"能一眼区分。
 */
#include "internal/mm_image.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "png.h"             /* 官方 libpng 组件的上游头（PNG 封面，见 mm_image_to_rgb565） */
#include "esp_heap_caps.h"   /* 全尺寸封面缓冲走 PSRAM */

static const char *TAG = "media_meta";

/* ---------------------------------------------------------- JPEG → RGB565 */

static int pick_div(int long_side, int max_dim)
{
    int d = 1;
    while (d < 8 && long_side / (d * 2) >= max_dim) {
        d *= 2;
    }
    return d;
}

static esp_err_t jpeg_to_rgb565(const uint8_t *jpg, size_t jpg_len, int max_dim,
                                uint8_t **out_pixels, int *out_w, int *out_h)
{
    /* 第一次只开一个"不缩放"的句柄拿原始尺寸：esp_new_jpeg 的 scale 必须在
     * jpeg_dec_open 时就给出，而尺寸要 parse_header 之后才知道 —— 只能开两次 */
    jpeg_dec_config_t probe_cfg = DEFAULT_JPEG_DEC_CONFIG();
    probe_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t probe = NULL;
    if (jpeg_dec_open(&probe_cfg, &probe) != JPEG_ERR_OK) {
        return ESP_FAIL;
    }
    jpeg_dec_io_t probe_io = {};
    probe_io.inbuf     = (uint8_t *)jpg;
    probe_io.inbuf_len = (int)jpg_len;
    jpeg_dec_header_info_t hdr = {};
    jpeg_error_t jerr = jpeg_dec_parse_header(probe, &probe_io, &hdr);
    jpeg_dec_close(probe);
    if (jerr != JPEG_ERR_OK || hdr.width == 0 || hdr.height == 0) {
        return ESP_FAIL;
    }

    const int long_side = (hdr.width > hdr.height) ? hdr.width : hdr.height;
    const int div       = pick_div(long_side, max_dim);
    /* scale 要求 8 的整数倍，且不能超过原图（最大只支持 1/8） */
    int sw = (hdr.width / div) & ~7;
    int sh = (hdr.height / div) & ~7;
    if (sw < 8) sw = 8;
    if (sh < 8) sh = 8;
    if (div == 1) {
        sw = hdr.width;
        sh = hdr.height;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.scale.width  = (uint16_t)sw;
    cfg.scale.height = (uint16_t)sh;

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = {};
    io.inbuf     = (uint8_t *)jpg;
    io.inbuf_len = (int)jpg_len;
    jpeg_dec_header_info_t info = {};
    jerr = jpeg_dec_parse_header(dec, &io, &info);
    if (jerr != JPEG_ERR_OK) {
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)jpeg_calloc_align((size_t)out_len, 16);
    if (!buf) {
        jpeg_dec_close(dec);
        return ESP_ERR_NO_MEM;
    }
    io.outbuf   = buf;
    io.out_size = out_len;
    jerr = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    if (jerr != JPEG_ERR_OK) {
        jpeg_free_align(buf);
        return ESP_FAIL;
    }

    *out_pixels = buf;
    *out_w      = info.width;
    *out_h      = info.height;
    return ESP_OK;
}

/* ============================================================ 封面：全尺寸 → 缩放
 *
 * 为什么不再用 esp_new_jpeg 的 scale：
 *   它的 scale 必须是 8 的倍数、且最大 1/8，小图（例如 248x248 缩到 240）会直接报
 *   "The scaled width(248) should be greater than the minimum 1/8 scale width(250)"
 *   然后解码失败（封面就没了）。所以改成：
 *     1) 按原始尺寸解到一块**临时缓冲**（优先 PSRAM，16 字节对齐）；
 *     2) 自己做盒式平均（box filter）等比缩到 max_dim；
 *     3) 输出用 jpeg_calloc_align 分配，保证 media_meta_cover_free() 的语义不变。
 *   PNG 走官方 libpng 组件的简化 API（png_image）：先解成 RGB888 再**就地**压成
 *   RGB565（PNG_FORMAT_RGB，不要 alpha），之后与 JPEG 完全共用同一段降采样。
 */
esp_err_t mm_image_to_rgb565(const uint8_t *data, size_t len, int max_dim,
                             uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (!data || len < 8 || max_dim <= 0 || !out_pixels || !out_w || !out_h) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool is_png = (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G');

    uint8_t *full = NULL;
    int fw = 0, fh = 0;

    if (is_png) {
        /*
         * 官方 libpng 组件的**简化 API**（png_image）：一次调用就把整张图解成
         * 想要的像素格式，不用自己写 read 回调、也不用 png_read_row 逐行搬。
         * 调色板 / 灰度 / 16 bit / 隔行扫描这些形态 libpng 会自己转换到目标格式。
         *
         * 目标格式选 PNG_FORMAT_RGB（不带 alpha）：封面在界面上是不透明的，
         * RGB 比 RGBA 少 1/4 内存，还省掉逐像素丢 alpha 的那一步。
         */
        png_image img;
        memset(&img, 0, sizeof(img));
        img.version = PNG_IMAGE_VERSION;

        if (!png_image_begin_read_from_memory(&img, data, len)) {
            ESP_LOGW(TAG, "png: header read failed (%s)", img.message);
            png_image_free(&img);
            return ESP_ERR_NOT_SUPPORTED;
        }

        /*
         * begin_read 之后 IHDR 已经解出来，**先按尺寸挡掉畸形图再申请内存**：
         * PNG 是高压缩格式，"文件只有几十 KB、解出来 8000x8000"（png bomb）
         * 完全可能，而这条路和 JPEG 一样是按**原始尺寸**解进临时缓冲的。
         * 封面最终只缩到 max_dim，比它大 8 倍以上的图再解也没有意义
         * （盒式 8:1 降采样已经把细节抹平了）。
         */
        const int64_t side_limit = (int64_t)max_dim * 8;
        if (img.width == 0 || img.height == 0 ||
            (int64_t)img.width > side_limit || (int64_t)img.height > side_limit) {
            ESP_LOGW(TAG, "png: %ux%u exceeds the %d px side limit, skipped",
                     (unsigned)img.width, (unsigned)img.height, (int)side_limit);
            png_image_free(&img);
            return ESP_ERR_NOT_SUPPORTED;
        }

        img.format = PNG_FORMAT_RGB;
        const size_t need = PNG_IMAGE_SIZE(img);
        full = (uint8_t *)heap_caps_aligned_alloc(16, need,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!full) {
            png_image_free(&img);
            return ESP_ERR_NO_MEM;
        }

        if (!png_image_finish_read(&img, NULL, full, 0, NULL)) {
            ESP_LOGW(TAG, "png: decode failed (%s)", img.message);
            png_image_free(&img);
            free(full);
            return ESP_FAIL;
        }
        fw = (int)img.width;
        fh = (int)img.height;
        png_image_free(&img);   /* 成功时 finish_read 内部已清过，这里是空操作 */

        /*
         * RGB888 → RGB565 little-endian，**就地压缩**：
         * 读第 3i..3i+2 字节、写第 2i..2i+1 字节。2i <= 3i 且 i 递增，
         * 被覆盖的永远是已经读完的数据，所以不用再要一块 w*h*2 的输出缓冲。
         * 位序与 JPEG 那条路（JPEG_PIXEL_FORMAT_RGB565_LE）完全一致。
         */
        const size_t px_num = (size_t)fw * (size_t)fh;
        for (size_t i = 0; i < px_num; i++) {
            const uint8_t r = full[i * 3];
            const uint8_t g = full[i * 3 + 1];
            const uint8_t b = full[i * 3 + 2];
            const uint16_t px = (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                                           ((uint16_t)(g & 0xFC) << 3) |
                                           ((uint16_t)b >> 3));
            full[i * 2]     = (uint8_t)(px & 0xFF);
            full[i * 2 + 1] = (uint8_t)(px >> 8);
        }
    } else {
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        cfg.scale.width  = 0;      /* ★ 不缩放：自己缩，绕开 1/8 限制 */
        cfg.scale.height = 0;

        jpeg_dec_handle_t dec = NULL;
        if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
            return ESP_FAIL;
        }
        jpeg_dec_io_t io = {};
        io.inbuf     = (uint8_t *)data;
        io.inbuf_len = (int)len;
        jpeg_dec_header_info_t info = {};
        if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
            jpeg_dec_close(dec);
            return ESP_FAIL;
        }
        int out_len = 0;
        if (jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK || out_len <= 0) {
            jpeg_dec_close(dec);
            return ESP_FAIL;
        }
        full = (uint8_t *)heap_caps_aligned_alloc(16, (size_t)out_len,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!full) {
            /* PSRAM 拿不到就退回组件自带的缩放路径（宁可糊一点也别没封面） */
            jpeg_dec_close(dec);
            return jpeg_to_rgb565(data, len, max_dim, out_pixels, out_w, out_h);
        }
        io.outbuf   = full;
        io.out_size = out_len;
        const jpeg_error_t jerr = jpeg_dec_process(dec, &io);
        jpeg_dec_close(dec);
        if (jerr != JPEG_ERR_OK) {
            free(full);
            return ESP_FAIL;
        }
        fw = info.width;
        fh = info.height;
    }

    if (fw <= 0 || fh <= 0) {
        free(full);
        return ESP_FAIL;
    }

    const int long_side = (fw > fh) ? fw : fh;
    if (long_side <= max_dim) {
        /* ★ 输出必须是 jpeg_calloc_align：释放端是 mm_cover_free() 的 jpeg_free_align */
        uint8_t *out = (uint8_t *)jpeg_calloc_align((size_t)fw * (size_t)fh * 2, 16);
        if (!out) {
            free(full);
            return ESP_ERR_NO_MEM;
        }
        memcpy(out, full, (size_t)fw * (size_t)fh * 2);
        free(full);
        *out_pixels = out;
        *out_w = fw;
        *out_h = fh;
        return ESP_OK;
    }

    const int dw = (fw >= fh) ? max_dim : (int)(((int64_t)fw * max_dim + fh / 2) / fh);
    const int dh = (fw >= fh) ? (int)(((int64_t)fh * max_dim + fw / 2) / fw) : max_dim;
    /* ★ 同上：只能用 jpeg_calloc_align 分配 */
    uint8_t *out = (uint8_t *)jpeg_calloc_align((size_t)dw * (size_t)dh * 2, 16);
    if (!out) {
        free(full);
        return ESP_ERR_NO_MEM;
    }
    for (int y = 0; y < dh; y++) {
        const int sy0 = (int)((int64_t)y * fh / dh);
        int sy1 = (int)((int64_t)(y + 1) * fh / dh);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        for (int x = 0; x < dw; x++) {
            const int sx0 = (int)((int64_t)x * fw / dw);
            int sx1 = (int)((int64_t)(x + 1) * fw / dw);
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = full + ((size_t)sy * fw + sx0) * 2;
                for (int sx = sx0; sx < sx1; sx++, row += 2) {
                    const uint16_t px = (uint16_t)(row[0] | (row[1] << 8));
                    r += (px >> 11) & 0x1F;
                    g += (px >> 5) & 0x3F;
                    b += px & 0x1F;
                    n++;
                }
            }
            const uint16_t o = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
            uint8_t *d = out + ((size_t)y * dw + x) * 2;
            d[0] = (uint8_t)(o & 0xFF);
            d[1] = (uint8_t)(o >> 8);
        }
    }
    free(full);
    *out_pixels = out;
    *out_w = dw;
    *out_h = dh;
    return ESP_OK;
}
