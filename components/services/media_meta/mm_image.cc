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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "driver/jpeg_decode.h"   /* S31 硬件 JPEG 解码器（esp_driver_jpeg） */
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
        /*
         * ⚠ 千万别写成 sw = hdr.width —— esp_new_jpeg 要求 "设了的 scale" 必须是 8 的
         * 倍数，700 这种宽度会直接报
         *     E JPEG_DEC: After scale and clipper, width(700) isn't multiply of 8.
         * 然后整张图解不出来（真机日志里那张 700 宽的 jpg 就是这么挂的）。
         * scale 填 0 才是"不缩放"（mm_image_to_rgb565 里也是这么用的）。
         */
        sw = 0;
        sh = 0;
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

/* JPEG 的 SOF 信息（渐进式 / 算术编码 / CMYK 这些 esp_new_jpeg 不支持的形态先判出来） */
typedef struct {
    int sof;      /* SOF 标记字节：0xC0 = baseline，0xC2 = progressive，0xC9 = 算术编码 ... */
    int comps;    /* 分量数：1 = 灰度，3 = YCbCr，4 = CMYK/YCCK */
    int w, h;
} jpeg_sof_t;

static bool jpeg_sof_info(const uint8_t *d, size_t n, jpeg_sof_t *out)
{
    if (!d || n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return false;
    }
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        const uint8_t m = d[i + 1];
        if (m == 0xFF) {                 /* 填充 */
            i++;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;                      /* 无长度的标记 */
            continue;
        }
        const size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (len < 2) {
            return false;
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            /* 段布局：FF Cx | len(2) | 精度(1) | 高(2) | 宽(2) | 分量数(1) */
            if (i + 10 > n) {
                return false;
            }
            out->sof   = m;
            out->h     = ((int)d[i + 5] << 8) | d[i + 6];
            out->w     = ((int)d[i + 7] << 8) | d[i + 8];
            out->comps = d[i + 9];
            return (out->w > 0 && out->h > 0);
        }
        i += 2 + len;
    }
    return false;
}

/* ============================ 大图 PNG：逐行解 + 边解边缩（图库专用）
 *
 * 原来的 PNG 路（mm_image_to_rgb565 的 png_image 简化 API）必须先把**整张**图按
 * 原始尺寸解成 RGB888 再降采样：
 *      1080x1920 截图 → 6.2 MB + 输出 4.1 MB ≈ 10 MB
 *      2000x3000     → 18 MB + 12 MB
 * PSRAM 只有 16 MB，于是真机日志里就是 decode failed (ESP_ERR_NO_MEM)。
 *
 * 这里换 libpng 的**逐行**接口 png_read_row()：常驻内存只有"一行 RGB" + 输出缓冲，
 * 每读满 div 行就按 div×div 块做盒式平均写出一行 RGB565 → 峰值 ≈ 最终输出（约 1 MB）。
 *
 * 只给图库用（mm_photo_file_rgb565）。封面/歌词那条路（mm_image_to_rgb565）**不动**：
 * 那边图都在 max_dim 附近、整解没问题，而且是被验证过的老路径。
 *
 * ⚠ 已知小瑕疵：最后一个输出列 / 最后一行若不满 div 个采样点，用固定除数 cnt 会让
 *   最右/最下一列略暗一格（照片上看不出来，不去为它加一份逐列计数）。
 */

typedef struct {
    png_structp    png;
    png_infop      info;
    uint8_t       *row;      /* 一行 RGB（未缩放，row_bytes 字节） */
    uint8_t       *out;      /* RGB565 输出（成功时移交给调用方） */
    uint32_t      *acc;      /* 当前输出行的累加器，每列 3 个 uint32 */
    const uint8_t *src;
    size_t         len;
    size_t         pos;
} png_read_ctx_t;

static void png_mem_read_cb(png_structp png, png_bytep dst, png_size_t n)
{
    png_read_ctx_t *c = (png_read_ctx_t *)png_get_io_ptr(png);
    if (!c || c->pos + n > c->len) {
        png_error(png, "unexpected end of png data");
        return;
    }
    memcpy(dst, c->src + c->pos, n);
    c->pos += n;
}

static void png_ctx_release(png_read_ctx_t *c)
{
    if (!c) {
        return;
    }
    if (c->png) {
        png_destroy_read_struct(&c->png, c->info ? &c->info : NULL, NULL);
    }
    free(c->row);
    free(c->acc);
    free(c->out);       /* 成功时已被置 NULL（所有权转给调用方） */
    free(c);
}

static esp_err_t png_stream_to_rgb565(const uint8_t *data, size_t len, int max_dim,
                                      uint8_t **out_pixels, int *out_w, int *out_h)
{
    png_read_ctx_t *c = (png_read_ctx_t *)calloc(1, sizeof(*c));
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    c->src  = data;
    c->len  = len;
    c->png  = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!c->png) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    c->info = png_create_info_struct(c->png);
    if (!c->info) {
        png_ctx_release(c);
        return ESP_ERR_NO_MEM;
    }

    /* libpng 的所有错误都从这里 longjmp 出来（含读到坏数据） */
    if (setjmp(png_jmpbuf(c->png))) {
        ESP_LOGW(TAG, "png: decode error");
        png_ctx_release(c);
        return ESP_FAIL;
    }

    png_set_read_fn(c->png, c, png_mem_read_cb);
    png_read_info(c->png, c->info);

    const png_uint_32 w = png_get_image_width(c->png, c->info);
    const png_uint_32 h = png_get_image_height(c->png, c->info);
    /* 防 png bomb：文件几十 KB、解出来 20000x20000 这种事真的有 */
    if (w == 0 || h == 0 || w > 20000u || h > 20000u) {
        ESP_LOGW(TAG, "png: %ux%u rejected", (unsigned)w, (unsigned)h);
        png_ctx_release(c);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 归一到"8 bit / RGB"三通道：调色板→RGB、低位灰度→8、tRNS→alpha、16bit→8、灰→RGB、丢 alpha */
    const int ctype = png_get_color_type(c->png, c->info);
    const int depth = png_get_bit_depth(c->png, c->info);
    if (depth == 16) {
        png_set_strip_16(c->png);
    }
    if (ctype == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(c->png);
    }
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) {
        png_set_expand_gray_1_2_4_to_8(c->png);
    }
    if (png_get_valid(c->png, c->info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(c->png);
    }
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(c->png);
    }
    png_set_strip_alpha(c->png);     /* 屏幕上是不透明底，alpha 直接丢 */

    const int passes = png_set_interlace_handling(c->png);
    png_read_update_info(c->png, c->info);

    if (png_get_channels(c->png, c->info) != 3) {
        ESP_LOGW(TAG, "png: unexpected channel layout");
        png_ctx_release(c);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const size_t row_bytes = png_get_rowbytes(c->png, c->info);
    c->row = (uint8_t *)heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->row) {
        c->row = (uint8_t *)malloc(row_bytes);
    }
    if (!c->row) {
        png_ctx_release(c);
        return ESP_ERR_NO_MEM;
    }

    /*
     * 降采样倍数：与 JPEG 那条路的 pick_div() 同一个口径 ——
     * 长边落到 [max_dim, 2*max_dim) 区间（1/2^n，最大 1/16）。
     */
    const png_uint_32 long_side = (w > h) ? w : h;
    uint32_t div = 1;
    while (div < 16u && (long_side / (div * 2u)) >= (uint32_t)max_dim) {
        div *= 2u;
    }
    const uint32_t dw = (w + div - 1u) / div;
    const uint32_t dh = (h + div - 1u) / div;

    /* 输出必须是 jpeg_calloc_align：释放端是 mm_cover_free() 的 jpeg_free_align */
    c->out = (uint8_t *)jpeg_calloc_align((size_t)dw * (size_t)dh * 2u, 16);
    c->acc = (uint32_t *)calloc((size_t)dw * 3u, sizeof(uint32_t));
    if (!c->out || !c->acc) {
        png_ctx_release(c);
        return ESP_ERR_NO_MEM;
    }

    uint32_t rows_in_block = 0;
    for (int pass = 0; pass < passes; pass++) {
        const bool last = (pass == passes - 1);   /* 隔行图只有最后一遍是完整的 */
        for (png_uint_32 y = 0; y < h; y++) {
            png_read_row(c->png, c->row, NULL);
            if (!last) {
                continue;
            }
            /* 横向累加到所属输出列 */
            for (png_uint_32 x = 0; x < w; x++) {
                const uint8_t *p = c->row + (size_t)x * 3u;
                uint32_t      *a = c->acc + (size_t)(x / div) * 3u;
                a[0] += p[0];
                a[1] += p[1];
                a[2] += p[2];
            }
            rows_in_block++;
            if (rows_in_block == div || y == h - 1u) {
                const uint32_t oy  = y / div;
                const uint32_t cnt = rows_in_block * div;
                uint8_t       *dst = c->out + (size_t)oy * (size_t)dw * 2u;
                for (uint32_t ox = 0; ox < dw; ox++) {
                    const uint32_t *a = c->acc + (size_t)ox * 3u;
                    const uint8_t r = (uint8_t)(a[0] / cnt);
                    const uint8_t g = (uint8_t)(a[1] / cnt);
                    const uint8_t b = (uint8_t)(a[2] / cnt);
                    const uint16_t px = (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                                                   ((uint16_t)(g & 0xFC) << 3) |
                                                   ((uint16_t)b >> 3));
                    dst[ox * 2]     = (uint8_t)(px & 0xFF);
                    dst[ox * 2 + 1] = (uint8_t)(px >> 8);
                }
                memset(c->acc, 0, (size_t)dw * 3u * sizeof(uint32_t));
                rows_in_block = 0;
            }
        }
    }

    *out_pixels = c->out;
    *out_w      = (int)dw;
    *out_h      = (int)dh;
    c->out      = NULL;      /* 所有权转给调用方（png_ctx_release 别再放） */
    png_ctx_release(c);
    return ESP_OK;
}

/* ============================================ 硬件 JPEG（S31 内置编解码器）
 *
 * S31 有**硬件** JPEG 解码器（soc_caps.h: SOC_JPEG_CODEC_SUPPORTED=1），走
 * esp_driver_jpeg（driver/jpeg_decode.h）+ 2DDMA 直接写 PSRAM。图库用它替掉软解的
 * esp_new_jpeg —— 软解一张 1080p 要几百毫秒 CPU，硬解是几十毫秒且几乎不占核。
 *
 * 四条硬约束（都在这块代码里处理，改之前先看懂）：
 *
 *  1. **输出缓冲必须缓存行对齐、长度也要对齐**，否则 jpeg_decoder_process() 直接
 *     ESP_ERR_INVALID_ARG（见 jpeg_decode.c 的 _check_buffer_alignment）。所以只能用
 *     jpeg_alloc_decoder_mem() 分配 —— 它按 MAX(PSRAM 缓存行, 16) 对齐，并回填实际长度。
 *     该缓冲是 heap_caps_aligned_calloc 出来的，**可以**用 media_meta_cover_free()
 *     （底层 jpeg_free_align → heap_caps_free）释放，与软解那条路同一套堆。
 *
 *  2. **4:2:0/4:2:2 的图会按 16 像素对齐渲染**（头文件明说），行跨距可能是
 *     align16(w)*2 而不是 w*2 —— 必须自己压紧，否则画面会斜（每行错开一截）。
 *
 *  3. **没有解码期缩放**：只能整张解。所以只用于输出 <= MM_HW_JPEG_MAX_OUT 的图；
 *     4000x3000 的照片整张 RGB565 要 24 MB，继续走软解那条带 scale 的路。
 *
 *  4. **缓存同步不用我们操心**：驱动内部对输入做 C2M 写回、对输出做 M2C 失效
 *     （jpeg_decode.c 的 esp_cache_msync 两处）。
 */

/*
 * 硬解输出上限 5 MB（≈2.4 MP）。取这个值是为了刚好装下最常见的
 * **1080x1920 手机截图**（align16 后 1088x1920 = 4.18 MB）；1600x1200 (3.8 MB)、
 * 1920x1080 (4.1 MB) 也都走硬解。再大（手机 12 MP 照片整张要 24 MB）整张解不下，
 * 交回软解 —— 那条路能在解码期按 1/2^n 缩。
 * 分配失败/硬解不支持时同样回落软解，不会把这张图变成"打不开"。
 */
#define MM_HW_JPEG_MAX_OUT (5u * 1024u * 1024u)

/* RGB565 的元素顺序：用 IDF 官方测试用例的默认值（BGR）。
 * 如果真机上发现**红蓝互换**，把这里改成 JPEG_DEC_RGB_ELEMENT_ORDER_RGB 即可。 */
#define MM_HW_JPEG_RGB_ORDER JPEG_DEC_RGB_ELEMENT_ORDER_BGR

/* 引擎句柄：懒创建、之后一直复用（只被解码工作线程访问，不需要锁）。
 * 建一次约几 ms，且要在内部 RAM 里挂 DMA 描述符与中断。 */
static jpeg_decoder_handle_t s_hw_jpeg;

static bool hw_jpeg_to_rgb565(const uint8_t *jpg, size_t jpg_len,
                              uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (!jpg || jpg_len < 128) {
        return false;
    }
    /* 只解头部拿尺寸（纯解析，不碰硬件） */
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(jpg, (uint32_t)jpg_len, &info) != ESP_OK) {
        return false;
    }
    if (info.width == 0 || info.height == 0) {
        return false;
    }

    const uint32_t w  = info.width;
    const uint32_t h  = info.height;
    const uint32_t pw = (w + 15u) & ~15u;      /* 2DDMA 按 16 像素一块渲染 */
    const uint32_t ph = (h + 15u) & ~15u;
    const uint64_t need = (uint64_t)pw * (uint64_t)ph * 2u;
    if (need > (uint64_t)MM_HW_JPEG_MAX_OUT) {
        return false;                          /* 太大：交回软解（它能解码期缩放） */
    }

    if (!s_hw_jpeg) {
        jpeg_decode_engine_cfg_t ecfg = {};
        ecfg.intr_priority = 0;
        ecfg.timeout_ms    = 3000;
        if (jpeg_new_decoder_engine(&ecfg, &s_hw_jpeg) != ESP_OK) {
            s_hw_jpeg = NULL;
            return false;
        }
    }

    jpeg_decode_memory_alloc_cfg_t mcfg = {};
    mcfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t  buf_size = 0;
    uint8_t *buf = (uint8_t *)jpeg_alloc_decoder_mem((size_t)need, &mcfg, &buf_size);
    if (!buf) {
        return false;
    }

    jpeg_decode_cfg_t dcfg = {};
    dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dcfg.rgb_order     = MM_HW_JPEG_RGB_ORDER;
    dcfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT601;

    uint32_t out_size = 0;
    const esp_err_t err = jpeg_decoder_process(s_hw_jpeg, &dcfg, jpg, (uint32_t)jpg_len,
                                               buf, (uint32_t)buf_size, &out_size);
    if (err != ESP_OK) {
        jpeg_free_align(buf);
        return false;
    }

    /* 行跨距压紧：src 在 dst 之后（或相等），正向逐行 memmove 安全 */
    if (pw != w) {
        const size_t row = (size_t)w * 2u;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *src = buf + (size_t)y * pw * 2u;
            uint8_t *dst = buf + (size_t)y * row;
            if (src != dst) {
                memmove(dst, src, row);
            }
        }
    }

    *out_pixels = buf;
    *out_w      = (int)w;
    *out_h      = (int)h;
    return true;
}

/* ==================================================== 大图（图库 App）：直接缩放解码
 *
 * 与封面那条路（mm_image_to_rgb565，先整张解再盒式降采样）的区别：
 *   封面最大也就 300~500 px，整解无所谓；而**手机照片 4000x3000** 整解要
 *   24 MB RGB565，直接 OOM。所以这条路对 JPEG 先用 **S31 硬件解码器**（输出 <= 4 MB
 *   时），装不下再用 esp_new_jpeg 自带的 scale（按 1/2^n 直接解到目标尺寸）。
 *
 * PNG 没有"解码期缩放"，仍走 mm_image_to_rgb565（它自己带 png bomb 防护）；
 * 实际使用里 PNG 多是截图，尺寸不会离谱。
 *
 * ⚠ 文件上限 12 MB：再大就是相机 RAW 级别，PSRAM 里塞不下整份。
 */
#define MM_PHOTO_FILE_MAX (12u * 1024u * 1024u)

esp_err_t mm_photo_file_rgb565(const char *path, int max_dim,
                               uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (!path || !out_pixels || !out_w || !out_h || max_dim <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const long sz = ftell(f);
    if (sz <= 8 || (unsigned long)sz > MM_PHOTO_FILE_MAX) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    /* 整份读进 PSRAM：解码器要连续输入缓冲 */
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc((size_t)sz);
    }
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }

    /*
     * 先挡掉"扩展名骗人"的两种情况 —— 真机日志里就有一张 .png 实际是 WebP
     * （抖音分享图），喂给 JPEG 解析器只会得到一句莫名其妙的
     * `E JPEG_DEC: JPEG segment format error.`，用户看到的是"这张打不开"。
     */
    if (sz >= 12 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P') {
        ESP_LOGW(TAG, "webp is not supported (yet): %s", path);
        free(buf);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sz >= 12 && buf[4] == 'f' && buf[5] == 't' && buf[6] == 'y' && buf[7] == 'p') {
        ESP_LOGW(TAG, "heif/isobmff image is not supported: %s", path);
        free(buf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * 渐进式(SOF2) / 算术编码(SOF9~CB) / CMYK(4 分量) 的 JPEG，
     * esp_new_jpeg 和 S31 硬解都不支持 —— 真机日志里那几张
     * `E JPEG_DEC: Not supported JPEG standard.` 就是它们（BG_CS_*.jpg 这类网页素材，
     * 常见于渐进式导出）。提前判出来，省掉一次注定失败的解码，也给 UI 一个准确原因。
     */
    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        jpeg_sof_t sof = {};
        if (jpeg_sof_info(buf, (size_t)sz, &sof)) {
            const bool bad_sof  = (sof.sof != 0xC0 && sof.sof != 0xC1);
            const bool bad_comp = (sof.comps != 1 && sof.comps != 3);
            if (bad_sof || bad_comp) {
                ESP_LOGW(TAG, "jpeg unsupported: SOF%02X %dch %dx%d (%s)", sof.sof,
                         sof.comps, sof.w, sof.h, path);
                free(buf);
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
    }

    const bool is_png = (buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G');
    esp_err_t err;
    if (is_png) {
        /* 逐行解 + 边解边缩：大截图不再 ESP_ERR_NO_MEM（见 png_stream_to_rgb565 的说明） */
        err = png_stream_to_rgb565(buf, (size_t)sz, max_dim, out_pixels, out_w, out_h);
        if (err != ESP_OK) {
            /* 兜底：老路径（png_image 整解），小图本来也够用 */
            err = mm_image_to_rgb565(buf, (size_t)sz, max_dim, out_pixels, out_w, out_h);
        }
    } else if (hw_jpeg_to_rgb565(buf, (size_t)sz, out_pixels, out_w, out_h)) {
        /* 硬解成功：整张解出来（尺寸已封顶，见 MM_HW_JPEG_MAX_OUT） */
        err = ESP_OK;
        ESP_LOGI(TAG, "jpeg %dx%d -> hw", *out_w, *out_h);
    } else {
        /* 软解兜底：超大图（整张 RGB565 放不下）或硬解不支持/失败时走这条 */
        err = jpeg_to_rgb565(buf, (size_t)sz, max_dim, out_pixels, out_w, out_h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "jpeg %dx%d -> sw", *out_w, *out_h);
        }
    }
    free(buf);
    return err;
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
