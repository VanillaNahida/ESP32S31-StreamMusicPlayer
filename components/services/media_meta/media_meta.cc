/*
 * media_meta.cc —— 媒体元数据：歌词（LRC / ID3 内嵌）+ 内嵌封面提取/解码
 *
 * 歌词解析规则：
 *   - 行首可能有**多个** [mm:ss.xx] / [mm:ss] 标签，每个标签都要生成一行
 *   - 标签之后是歌词文本；[ti:] [ar:] [al:] 等元信息标签忽略
 *   - 解析完按时间排序（LRC 允许乱序）
 *   - 整段没有任何时间标签时按"纯文本歌词"逐行收下，timed=false
 *
 * 歌词来源优先级（media_meta_lyrics_load）：
 *   歌曲同目录同名 .lrc → <SD>/lrc/<同名>.lrc → 内嵌 ID3（USLT，或 SYLT 转 LRC）
 *   最后一档取到后会**自动写入 <SD>/lrc/<同名>.lrc**，下次直接命中第二档。
 *
 * 封面：MP3 的 ID3v2 APIC/PIC、FLAC 的 PICTURE 块 → 解成 RGB565（LVGL 可直接用）。
 * JPEG 走 esp_new_jpeg，PNG 走官方 libpng 组件；两条路在 image_to_rgb565() 里
 * 按文件魔数分派（APIC 的 MIME 字段常常是错的，不能拿它当依据）。
 */
#include "media_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "png.h"             /* 官方 libpng 组件的上游头（PNG 封面，见 image_to_rgb565） */
#include "esp_heap_caps.h"   /* 全尺寸封面缓冲走 PSRAM */

/* 全尺寸解码（JPEG/PNG）→ 盒式降采样。定义在文件末尾，这里先声明。 */
static esp_err_t image_to_rgb565(const uint8_t *data, size_t len, int max_dim,
                                 uint8_t **out_pixels, int *out_w, int *out_h);

static const char *TAG = "media_meta";

#define LRC_MAX_LINES 400

/* 前向声明：定义在文件后半段（读文件 + 解析） */
static esp_err_t load_lrc_file(const char *path, lrc_t *out);

void media_meta_lrc_reset(lrc_t *lrc)
{
    if (!lrc) {
        return;
    }
    free(lrc->lines);
    lrc->lines = NULL;
    lrc->count = 0;
    lrc->cap = 0;
    lrc->timed = true;
}

static bool ensure_cap(lrc_t *lrc)
{
    if (lrc->count < lrc->cap) {
        return true;
    }
    size_t ncap = lrc->cap ? lrc->cap * 2 : 32;
    if (ncap > LRC_MAX_LINES) {
        ncap = LRC_MAX_LINES;
    }
    if (lrc->count >= ncap) {
        return false;
    }
    lrc_line_t *nl = (lrc_line_t *)realloc(lrc->lines, ncap * sizeof(lrc_line_t));
    if (!nl) {
        return false;
    }
    lrc->lines = nl;
    lrc->cap = ncap;
    return true;
}

static void push_line(lrc_t *lrc, uint32_t ms, const char *text)
{
    if (!ensure_cap(lrc)) {
        return;
    }
    lrc_line_t *l = &lrc->lines[lrc->count++];
    l->time_ms = ms;
    snprintf(l->text, sizeof(l->text), "%s", text ? text : "");
    /* 去掉行尾空白 */
    size_t n = strlen(l->text);
    while (n > 0 && isspace((unsigned char)l->text[n - 1])) {
        l->text[--n] = '\0';
    }
}

/* 解析一个 [mm:ss.xx] 标签；成功返回 true 并前进 *p */
static bool parse_tag(const char **p, uint32_t *out_ms)
{
    const char *s = *p;
    if (*s != '[') {
        return false;
    }
    s++;

    /* 元信息标签 [ti:xx] [ar:xx] [al:xx] [by:xx] [offset:xx] —— 不是时间 */
    if (isalpha((unsigned char)*s)) {
        const char *close = strchr(s, ']');
        if (!close) {
            return false;
        }
        /* offset 标签可以调整整体偏移，这里只跳过 */
        *p = close + 1;
        *out_ms = 0xFFFFFFFFu;   /* 标记"非时间标签" */
        return true;
    }

    int mm = 0, ss = 0, frac = 0, frac_digits = 0;
    if (sscanf(s, "%d:%d", &mm, &ss) != 2) {
        return false;
    }
    const char *dot = s;
    while (*dot && *dot != ']' && *dot != '.' && *dot != ':') dot++;
    if (*dot == '.') {
        const char *f = dot + 1;
        while (isdigit((unsigned char)*f) && frac_digits < 3) {
            frac = frac * 10 + (*f - '0');
            frac_digits++;
            f++;
        }
    }
    while (frac_digits < 3) {
        frac *= 10;
        frac_digits++;
    }

    const char *close = strchr(s, ']');
    if (!close) {
        return false;
    }
    *p = close + 1;
    *out_ms = (uint32_t)((mm * 60 + ss) * 1000 + frac);
    return true;
}

static int cmp_line(const void *a, const void *b)
{
    const lrc_line_t *x = (const lrc_line_t *)a;
    const lrc_line_t *y = (const lrc_line_t *)b;
    if (x->time_ms < y->time_ms) return -1;
    if (x->time_ms > y->time_ms) return 1;
    return 0;
}

static esp_err_t lrc_parse_utf8(const char *text, lrc_t *out)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    media_meta_lrc_reset(out);

    bool any_timed = false;
    const char *p = text;
    while (*p) {
        /* 定位行尾 */
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        char line[512];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }

        /* 逐行：吃掉行首所有时间标签，剩下的是正文 */
        const char *cur = line;
        uint32_t ms = 0;
        bool any = false;
        uint32_t times[8];
        size_t ntimes = 0;

        for (;;) {
            const char *save = cur;
            uint32_t t = 0;
            if (!parse_tag(&cur, &t)) {
                cur = save;
                break;
            }
            if (t != 0xFFFFFFFFu) {
                any = true;
                if (ntimes < sizeof(times) / sizeof(times[0])) {
                    times[ntimes++] = t;
                } else {
                    ms = t;
                }
            }
        }

        if (any) {
            any_timed = true;
            for (size_t i = 0; i < ntimes; i++) {
                push_line(out, times[i], cur);
            }
            if (ntimes == 0) {
                push_line(out, ms, cur);
            }
        }

        if (!eol) {
            break;
        }
        p = eol + 1;
    }

    if (!any_timed) {
        /*
         * 整段没有任何时间标签 → 纯文本歌词（内嵌 USLT 里很常见）。
         * 逐行收下、time 记 0、标记 timed=false：界面只静态展示，
         * 不做高亮与"当前行居中"（没有时间轴，硬做只会乱跳）。
         */
        const char *q = text;
        while (*q && out->count < LRC_MAX_LINES) {
            const char *eol = strchr(q, '\n');
            size_t len = eol ? (size_t)(eol - q) : strlen(q);
            char line[512];
            size_t n = (len < sizeof(line) - 1) ? len : sizeof(line) - 1;
            memcpy(line, q, n);
            line[n] = '\0';
            if (n > 0 && line[n - 1] == '\r') {
                line[n - 1] = '\0';
            }
            const char *s = line;
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (*s != '\0') {
                push_line(out, 0, s);
            }
            if (!eol) {
                break;
            }
            q = eol + 1;
        }
        out->timed = false;
        ESP_LOGI(TAG, "parsed %u plain lyric lines (no time tags)", (unsigned)out->count);
        return ESP_OK;
    }

    out->timed = true;
    if (out->count > 1) {
        qsort(out->lines, out->count, sizeof(lrc_line_t), cmp_line);
    }

    ESP_LOGI(TAG, "parsed %u lyric lines", (unsigned)out->count);
    return ESP_OK;
}

esp_err_t media_meta_lrc_load_sidecar(const char *audio_path, lrc_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 缓冲要放得下整条 UTF-8 长路径（FATFS_MAX_LFN=255/段），
     * 原来 272 字节在长中文名上会截断成打不开的"短路径" */
    char lrc_path[600];
    int n = snprintf(lrc_path, sizeof(lrc_path), "%s", audio_path);
    if (n <= 0 || n >= (int)sizeof(lrc_path)) {
        ESP_LOGW(TAG, "audio path too long for lrc sidecar (%d bytes)", n);
        return ESP_ERR_INVALID_SIZE;
    }
    char *dot = strrchr(lrc_path, '.');
    if (!dot) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(dot, sizeof(lrc_path) - (size_t)(dot - lrc_path), ".lrc");

    /* 读取与解析复用 load_lrc_file（定义在文件后半段） */
    return load_lrc_file(lrc_path, out);
}

int media_meta_lrc_index_at(const lrc_t *lrc, uint32_t pos_ms)
{
    if (!lrc || lrc->count == 0) {
        return -1;
    }
    int lo = 0, hi = (int)lrc->count - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (lrc->lines[mid].time_ms <= pos_ms) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

/* ================================================================== 内嵌封面
 *
 * 支持的容器 / 位置：
 *   MP3  ID3v2.2 的 PIC 帧、ID3v2.3/2.4 的 APIC 帧
 *   FLAC 元数据块里的 PICTURE(type 6)
 * 其它容器（M4A 的 covr atom、OGG 的 METADATA_BLOCK_PICTURE）留到后续里程碑。
 *
 * 编码：JPEG 走 esp_new_jpeg，PNG 走官方 libpng 组件（见 image_to_rgb565）。
 * 实际按**文件魔数**分派，不看 MIME —— 不少 MP3 的 APIC 把 MIME 写成 image/jpeg
 * 却塞的是 PNG（或反过来），信 MIME 会解不出来。
 * 其它编码返回 ESP_ERR_NOT_SUPPORTED。
 */

/* ---------------------------------------------------------- 字节序小工具 */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* ID3v2 的长度字段是 7-bit synchsafe：每个字节只用低 7 位 */
static uint32_t synchsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

static bool mime_is_png(const char *mime)
{
    /* 只要前缀是 image/png（大小写不敏感）就算 PNG */
    static const char *k = "image/png";
    for (int i = 0; i < 9; i++) {
        char c = mime[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != k[i]) {
            return false;
        }
    }
    return true;
}

size_t media_meta_audio_data_offset(const char *audio_path)
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
    return 10u + (size_t)synchsafe32(h + 6);
}

/* 把整个文件里的图片字节拷出来；成功返回 ESP_OK 并填 out/out_len/is_png */
static esp_err_t copy_image(uint8_t **out, size_t *out_len, bool *is_png,
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

/* ---------------------------------------------------------- ID3v2 (MP3) */

static esp_err_t id3_cover(FILE *f, uint8_t **out, size_t *out_len, bool *is_png)
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

    const size_t tag_end = 10u + (size_t)synchsafe32(hdr + 6);
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
            fsz = be24(fh + 3);
        } else if (major == 3) {
            fsz = be32(fh + 4);
        } else {
            fsz = synchsafe32(fh + 4);
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
                png = mime_is_png(mime);
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

            esp_err_t ret = (o < got) ? copy_image(out, out_len, is_png, body + o, got - o, png)
                                      : ESP_ERR_NOT_FOUND;
            free(body);
            return ret;
        }
        pos += fhdr_sz + fsz;
    }
    return ESP_ERR_NOT_FOUND;
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
            size_t mlen = be32(body + o); o += 4;
            if (o + mlen > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            bool png = mime_is_png((const char *)(body + o));
            o += mlen;
            if (o + 4 > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            size_t dlen = be32(body + o); o += 4;
            if (o + dlen + 16 + 4 > len) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            o += dlen + 16;
            size_t data_len = be32(body + o); o += 4;
            if (o + data_len > len || data_len == 0) {
                free(body);
                return ESP_ERR_NOT_FOUND;
            }
            esp_err_t ret = copy_image(out, out_len, is_png, body + o, data_len, png);
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

/* ---------------------------------------------------------- 对外入口 */

esp_err_t media_meta_cover_rgb565(const char *audio_path, int max_dim,
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
        ret = id3_cover(f, &raw, &raw_sz, &is_png);
    } else if (strcasecmp(dot, ".flac") == 0) {
        ret = flac_cover(f, &raw, &raw_sz, &is_png);
    } else {
        ret = ESP_ERR_NOT_FOUND;        /* M4A / OGG 的封面位置在后续里程碑补 */
    }
    fclose(f);

    if (ret != ESP_OK) {
        return ret;
    }
    /* PNG 也走同一条路（image_to_rgb565 内部按魔数分派） */
    ret = image_to_rgb565(raw, raw_sz, max_dim, pixels, out_w, out_h);
    free(raw);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "cover decoded %dx%d (max_dim=%d)", *out_w, *out_h, max_dim);
    } else {
        ESP_LOGW(TAG, "cover decode failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void media_meta_cover_free(uint8_t *pixels)
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
 * image_to_rgb565()，出来的像素格式与 media_meta_cover_rgb565() 完全一致
 * （RGB565 LE，16 字节对齐，用 media_meta_cover_free() 释放）。
 *
 * **JPEG / PNG 都能吃**：缓存下来的文件名带着 .jpg，但 Meting 的 pic 接口
 * 不保证给的就是 JPEG，所以这里同样按文件魔数分派（image_to_rgb565 内部判断），
 * 扩展名只当提示。
 *
 * 封面图常见 100~300 KB。上限跟在线侧的 COVER_MAX_BYTES 对齐（都是 4 MB）——
 * 两边不一致会出现"下下来了却解不了"这种最难查的组合。
 * 内存安全不靠这个值兜：PNG 那条路另有"边长 > max_dim × 8 直接跳过"的判定
 * （见 image_to_rgb565 里的 side_limit），JPEG 也是先按原始尺寸解、超限就拒。
 */
#define MEDIA_META_COVER_FILE_MAX (4u * 1024u * 1024u)

esp_err_t media_meta_image_file_rgb565(const char *img_path, int max_dim,
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

    esp_err_t ret = image_to_rgb565(buf, got, max_dim, pixels, out_w, out_h);
    free(buf);
    return ret;
}

/*
 * 内存缓冲版：封面字节已经在 PSRAM 里（在线服务不再把封面写 SD），
 * 直接喂给同一套 image_to_rgb565()，少一次 SD 读、也不占临时 malloc。
 * 上限与文件版一致（2 MB），解码行为完全相同。
 */
esp_err_t media_meta_image_mem_rgb565(const uint8_t *data, size_t len, int max_dim,
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
    return image_to_rgb565(data, len, max_dim, pixels, out_w, out_h);
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

typedef struct {
    int      mpeg_ver;      /* 帧头原值：3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
    int      layer;         /* 1 / 2 / 3 */
    uint32_t sample_rate;
    uint32_t bitrate;       /* bit/s */
    uint32_t frame_size;    /* 整帧字节数 */
    uint32_t samples;       /* 每帧样本数 */
    int      channels;      /* 1 / 2 */
} mp3_frame_t;

static bool mp3_parse_header(const uint8_t *p, mp3_frame_t *f)
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

esp_err_t media_meta_probe_mp3(const char *audio_path, audio_probe_t *out)
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
    const uint32_t data_off = (uint32_t)media_meta_audio_data_offset(audio_path);
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

    size_t      pos   = 0;
    mp3_frame_t fr    = {};
    bool        found = false;
    for (; pos + 4 <= got; pos++) {
        if (!mp3_parse_header(buf + pos, &fr)) {
            continue;
        }
        /* 复核：下一帧也应落在 frame_size 之后；剩余数据不够就采信首帧 */
        if (pos + fr.frame_size + 4 > got) {
            found = true;
            break;
        }
        mp3_frame_t nxt = {};
        if (mp3_parse_header(buf + pos + fr.frame_size, &nxt)) {
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
        const uint32_t flags = be32(buf + xing + 4);
        const size_t   p     = xing + 8;
        if ((flags & 0x0001u) && p + 4 <= got) {
            const uint32_t frames = be32(buf + p);
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

/* 小端读取（WAV 用） */
static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 码率兜底：容器里没有"码率"字段时（FLAC/WAV…）用 音频字节数 × 8 ÷ 时长 当平均码率。
 * 这正是 seek 需要的那个"平均"值（VBR 也成立）。 */
static void probe_fill_bitrate(uint32_t audio_bytes, audio_probe_t *out)
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
 * 拿到总采样数就能算准时长；FLAC 没有码率字段，按文件大小估（见 probe_fill_bitrate）。
 * 音频起始偏移 = 元数据链走完的位置（**不去读封面**，只按长度跳过）。
 */
/*
 * FLAC 元数据块链的"留证"（2026-10）：见 probe_flac 里 data_off 算飞时的告警。
 * 真机上两个 FLAC 的偏移分别飞到 94 MB / 138 MB（文件只有 38 / 36 MB），
 * 需要知道是哪一块的长度字段异常，才能判断是文件本身不标准还是我们解析错了。
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
        const uint32_t len  = be24(bh + 1);
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
                const uint32_t nlen = be24(nb + 1);
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
            probe_fill_bitrate(audio_bytes, out);
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
        const uint32_t sz = le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            const size_t want = (sz < sizeof(fmt)) ? sz : sizeof(fmt);
            if (fread(fmt, 1, want, f) != want) {
                break;
            }
            out->channels    = (uint8_t)le16(fmt + 2);
            out->sample_rate = le32(fmt + 4);
            byte_rate        = le32(fmt + 8);
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

esp_err_t media_meta_probe_audio(const char *audio_path, audio_probe_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* MP3 走原有那条（它认得 ID3/Xing/Info，比这里重造一遍靠谱） */
    const char *dot = strrchr(audio_path, '.');
    if (dot && strcasecmp(dot, ".mp3") == 0) {
        return media_meta_probe_mp3(audio_path, out);
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
static size_t id3_text_to_utf8(uint8_t enc, const uint8_t *src, size_t src_len,
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
static char *sylt_to_lrc(const uint8_t *b, size_t len)
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
        id3_text_to_utf8(enc, b + o, tlen, txt, sizeof(txt));
        o += tlen + ((enc == 1 || enc == 2) ? 2u : 1u);
        if (o + 4 > len) {
            break;
        }
        const uint32_t raw = be32(b + o);
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
static esp_err_t id3_lyrics(FILE *f, char **out)
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

    const size_t tag_end = 10u + (size_t)synchsafe32(hdr + 6);
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
            fsz = be24(fh + 3);
        } else if (major == 3) {
            fsz = be32(fh + 4);
        } else {
            fsz = synchsafe32(fh + 4);
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
                    size_t n = id3_text_to_utf8(body[0], body + o, fsz - o, txt, (fsz - o) * 2 + 8);
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
                char *txt = sylt_to_lrc(body, fsz);
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

esp_err_t media_meta_embedded_lyrics(const char *audio_path, char **text)
{
    if (!audio_path || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    *text = NULL;
    const char *dot = strrchr(audio_path, '.');
    if (!dot || strcasecmp(dot, ".mp3") != 0) {
        return ESP_ERR_NOT_SUPPORTED;       /* FLAC/OGG 的 LYRICS 字段后续补 */
    }
    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = id3_lyrics(f, text);
    fclose(f);
    return ret;
}

void media_meta_lyrics_free(char *text)
{
    free(text);
}

/* ---------------------------------------------------------- 歌词查找/落盘 */

static esp_err_t load_lrc_file(const char *path, lrc_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) {
        fclose(f);
        return (sz <= 0) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_SIZE;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    esp_err_t ret = media_meta_lrc_parse(buf, out);
    free(buf);
    return ret;
}

/* "/sdcard/xx/yy.mp3" → "/sdcard/lrc" */
static bool lyrics_dir_for(const char *audio_path, char *dir, size_t cap)
{
    if (!audio_path || audio_path[0] != '/') {
        return false;
    }
    const char *second = strchr(audio_path + 1, '/');
    if (!second) {
        return false;
    }
    int n = snprintf(dir, cap, "%.*s/lrc", (int)(second - audio_path), audio_path);
    return (n > 0) && (n < (int)cap);
}

/* "/sdcard/xx/yy.mp3" → "/sdcard/lrc/yy.lrc" */
static bool lyrics_cache_path(const char *audio_path, char *path, size_t cap)
{
    char dir[600];
    if (!lyrics_dir_for(audio_path, dir, sizeof(dir))) {
        return false;
    }
    const char *base = strrchr(audio_path, '/');
    base = base ? base + 1 : audio_path;
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    int n = snprintf(path, cap, "%s/%.*s.lrc", dir, (int)stem, base);
    return (n > 0) && (n < (int)cap);
}

esp_err_t media_meta_lyrics_load(const char *audio_path, lrc_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    media_meta_lrc_reset(out);

    /* 1) 歌曲同目录的同名 .lrc */
    if (media_meta_lrc_load_sidecar(audio_path, out) == ESP_OK && out->count > 0) {
        ESP_LOGI(TAG, "lyrics from sidecar next to the song");
        return ESP_OK;
    }

    /* 2) <挂载点>/lrc/<同名>.lrc */
    char dir[600]  = { 0 };
    char lrc[600]  = { 0 };
    const bool cache_ok = lyrics_cache_path(audio_path, lrc, sizeof(lrc));
    if (cache_ok) {
        lyrics_dir_for(audio_path, dir, sizeof(dir));
        if (load_lrc_file(lrc, out) == ESP_OK && out->count > 0) {
            ESP_LOGI(TAG, "lyrics from %s", lrc);
            return ESP_OK;
        }
    }

    /* 3) 内嵌歌词：解析 + **落盘到 /lrc**，下次直接命中第 2 档 */
    char     *text = NULL;
    esp_err_t ret  = media_meta_embedded_lyrics(audio_path, &text);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "no embedded lyrics (%s)", esp_err_to_name(ret));
        return ret;
    }
    ret = media_meta_lrc_parse(text, out);
    if (ret == ESP_OK && out->count > 0 && cache_ok) {
        /* 目录可能不存在：SD 卡根下建一个 */
        if (dir[0]) {
            mkdir(dir, 0777);               /* 已存在返回 EEXIST，忽略 */
        }
        FILE *f = fopen(lrc, "wb");
        if (f) {
            size_t n = strlen(text);
            if (fwrite(text, 1, n, f) != n) {
                ESP_LOGW(TAG, "write %s failed (short write)", lrc);
            }
            fclose(f);
            ESP_LOGI(TAG, "lyrics from ID3, saved to %s (%u bytes)", lrc, (unsigned)n);
        } else {
            ESP_LOGW(TAG, "cannot save lyrics to %s", lrc);
        }
    }
    media_meta_lyrics_free(text);
    return ret;
}

/*
 * 独立 .lrc 文件 → lrc_t（在线流式播放用）。
 * 音频在网络上，本地只有我们缓存的歌词文件，所以不能走
 * "按音频路径找同名 .lrc"那条路；直接读文件 + 复用 lrc_parse。
 */
esp_err_t media_meta_lyrics_load_file(const char *lrc_path, lrc_t *out)
{
    if (!lrc_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(lrc_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const long sz = ftell(f);
    if (sz <= 0 || sz > (long)(512 * 1024)) {      /* 歌词不该大于 512 KB */
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    rewind(f);
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    const esp_err_t ret = media_meta_lrc_parse(buf, out);
    free(buf);
    return ret;
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
static esp_err_t image_to_rgb565(const uint8_t *data, size_t len, int max_dim,
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
/* ==================================================== 歌词编码自动检测（UTF-8 / GBK）
 *
 * 需求：UTF-8 与 GBK/GB2312 的歌词都要能显示。以前一律按 UTF-8 解析，
 * 于是从中文站点/播放器导出的 GBK 歌词整段乱码。
 *
 * 判定顺序（够用且不误判）：
 *   1) BOM：EF BB BF → UTF-8（跳过）；FF FE → UTF-16LE；FE FF → UTF-16BE；
 *   2) 整段按严格 UTF-8 走一遍：全部合法就当 UTF-8（纯 ASCII 也走这条，
 *      反正两种编码下 ASCII 完全一样）；
 *   3) 否则按 **GBK** 解码（GB2312 是 GBK 子集，同一张表覆盖），
 *      逐字节转成 UTF-8；查不到的字节退化成 '?'。
 *
 * 表在 gbk_table.c（Python 从本机 gbk codec 生成，见文件头注释），
 * 126*191*2 = 48 KB flash，只读常量。
 */
extern const uint16_t gbk_to_uni[126][191];

static char *utf8_put(char *d, uint32_t cp)
{
    if (cp < 0x80) {
        *d++ = (char)cp;
    } else if (cp < 0x800) {
        *d++ = (char)(0xC0 | (cp >> 6));
        *d++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *d++ = (char)(0xE0 | (cp >> 12));
        *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *d++ = (char)(0x80 | (cp & 0x3F));
    }
    return d;
}

/* 严格校验 UTF-8（含过长编码、代理区、超范围） */
static bool is_valid_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t extra;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

/* 原始字节 → UTF-8（返回 malloc 的字符串；调用方 free） */
static char *lrc_bytes_to_utf8(const uint8_t *raw, size_t n)
{
    if (!raw) {
        return NULL;
    }
    if (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        raw += 3;
        n -= 3;
    } else if (n >= 2 && (raw[0] == 0xFF) && (raw[1] == 0xFE)) {
        /* UTF-16LE → UTF-8 */
        char *out = (char *)malloc(n * 3 + 1);
        if (!out) return NULL;
        char *d = out;
        for (size_t i = 2; i + 1 < n; i += 2) {
            d = utf8_put(d, (uint32_t)(raw[i] | (raw[i + 1] << 8)));
        }
        *d = '\0';
        return out;
    } else if (n >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        char *out = (char *)malloc(n * 3 + 1);
        if (!out) return NULL;
        char *d = out;
        for (size_t i = 2; i + 1 < n; i += 2) {
            d = utf8_put(d, (uint32_t)((raw[i] << 8) | raw[i + 1]));
        }
        *d = '\0';
        return out;
    }

    if (is_valid_utf8(raw, n)) {
        char *out = (char *)malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, raw, n);
        out[n] = '\0';
        return out;
    }

    /* GBK/GB2312 → UTF-8 */
    char *out = (char *)malloc(n * 3 + 1);
    if (!out) {
        return NULL;
    }
    char *d = out;
    for (size_t i = 0; i < n;) {
        const uint8_t b = raw[i];
        if (b < 0x80) {
            *d++ = (char)b;
            i++;
            continue;
        }
        if (b >= 0x81 && b <= 0xFE && i + 1 < n) {
            const uint8_t t = raw[i + 1];
            if (t >= 0x40 && t <= 0xFE) {
                const uint16_t u = gbk_to_uni[b - 0x81][t - 0x40];
                if (u) {
                    d = utf8_put(d, u);
                    i += 2;
                    continue;
                }
            }
        }
        *d++ = '?';          /* 认不出的字节：不吞掉后面的内容 */
        i++;
    }
    *d = '\0';
    ESP_LOGD(TAG, "lrc encoding: GBK/GB2312 -> UTF-8 (%u bytes in)", (unsigned)n);
    return out;
}

/* 公开入口：检测编码后交给 UTF-8 解析器 */
esp_err_t media_meta_lrc_parse(const char *text, lrc_t *out)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char *u8 = lrc_bytes_to_utf8((const uint8_t *)text, strlen(text));
    const esp_err_t ret = lrc_parse_utf8(u8 ? u8 : text, out);
    free(u8);
    return ret;
}
