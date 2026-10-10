/*
 * mm_lrc.cc —— LRC 歌词解析 + 时间轴 + 编码归一化（UTF-8 / UTF-16 / GBK）
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 * 原 39、44–296、1604–1629、1950–2104 行。**纯搬迁**，注释随函数一起搬。
 *
 * 歌词解析规则：
 *   - 行首可能有**多个** [mm:ss.xx] / [mm:ss] 标签，每个标签都要生成一行
 *   - 标签之后是歌词文本；[ti:] [ar:] [al:] 等元信息标签忽略
 *   - 解析完按时间排序（LRC 允许乱序）
 *   - 整段没有任何时间标签时按"纯文本歌词"逐行收下，timed=false
 *
 * 编码判定顺序（够用且不误判）：
 *   1) BOM：EF BB BF → UTF-8（跳过）；FF FE → UTF-16LE；FE FF → UTF-16BE；
 *   2) 整段按严格 UTF-8 走一遍：全部合法就当 UTF-8（纯 ASCII 也走这条，
 *      反正两种编码下 ASCII 完全一样）；
 *   3) 否则按 **GBK** 解码（GB2312 是 GBK 子集，同一张表覆盖），
 *      逐字节转成 UTF-8；查不到的字节退化成 '?'。
 *
 * 表在 gbk_table.c（Python 从本机 gbk codec 生成，见文件头注释），
 * 126*191*2 = 48 KB flash，只读常量。
 *
 * 本文件除了 esp_err（错误码）与 esp_log（诊断，TAG 仍是 "media_meta"）之外
 * **不依赖任何 ESP-IDF / 硬件**：下面的解析与编码函数拿到 host 上配一个空
 * esp_log/esp_err 桩就能直接单测（拆分蓝图 §6.3 / §C）。
 */
#include "internal/mm_lrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"

static const char *TAG = "media_meta";

#define LRC_MAX_LINES 400

void mm_lrc_reset(lrc_t *lrc)
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

esp_err_t mm_lrc_parse_utf8(const char *text, lrc_t *out)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    mm_lrc_reset(out);

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

/* 读一个 .lrc 文件（≤256 KB）→ 解析（复用 mm_lrc_parse；边文件与 /lrc 缓存都用它） */
esp_err_t mm_lrc_load_file(const char *path, lrc_t *out)
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

    esp_err_t ret = mm_lrc_parse(buf, out);
    free(buf);
    return ret;
}

esp_err_t mm_lrc_load_sidecar(const char *audio_path, lrc_t *out)
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

    /* 读取与解析复用 mm_lrc_load_file */
    return mm_lrc_load_file(lrc_path, out);
}

int mm_lrc_index_at(const lrc_t *lrc, uint32_t pos_ms)
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

/* ==================================================== 歌词编码自动检测（UTF-8 / GBK）
 *
 * 需求：UTF-8 与 GBK/GB2312 的歌词都要能显示。以前一律按 UTF-8 解析，
 * 于是从中文站点/播放器导出的 GBK 歌词整段乱码。
 *
 * 判定顺序见文件头。表在 gbk_table.c（只读常量，126*191*2 = 48 KB flash）。
 */
extern const uint16_t gbk_to_uni[126][191];

char *mm_lrc_utf8_put(char *d, uint32_t cp)
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
bool mm_lrc_utf8_valid(const uint8_t *s, size_t n)
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
char *mm_lrc_bytes_to_utf8(const uint8_t *raw, size_t n)
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
            d = mm_lrc_utf8_put(d, (uint32_t)(raw[i] | (raw[i + 1] << 8)));
        }
        *d = '\0';
        return out;
    } else if (n >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        char *out = (char *)malloc(n * 3 + 1);
        if (!out) return NULL;
        char *d = out;
        for (size_t i = 2; i + 1 < n; i += 2) {
            d = mm_lrc_utf8_put(d, (uint32_t)((raw[i] << 8) | raw[i + 1]));
        }
        *d = '\0';
        return out;
    }

    if (mm_lrc_utf8_valid(raw, n)) {
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
                    d = mm_lrc_utf8_put(d, u);
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

/* 公开入口的实现：检测编码后交给 UTF-8 解析器（对外的 media_meta_lrc_parse 转发到这里） */
esp_err_t mm_lrc_parse(const char *text, lrc_t *out)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char *u8 = mm_lrc_bytes_to_utf8((const uint8_t *)text, strlen(text));
    const esp_err_t ret = mm_lrc_parse_utf8(u8 ? u8 : text, out);
    free(u8);
    return ret;
}
