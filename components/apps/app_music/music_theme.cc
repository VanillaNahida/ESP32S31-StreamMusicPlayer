/*
 * music_theme.cc —— 封面主色提取 + 当前主题色
 *
 * 全部是整数运算（没有浮点、没有 log —— 只在解码线程跑一次，但保持零 FPU 依赖，
 * 免得以后有人把它搬到别的上下文里踩 -Werror=double-promotion 之类）。
 */
#include "music_theme.h"

#include <stdbool.h>
#include <stddef.h>

#include "ui_metrics.h"     /* UI_COLOR_PILL：次按钮底色 */

#include "music_state.h"

/* 采样上限：240x240 全采也就 57600 次，但没必要 —— 4096 个点足够稳 */
#define THEME_MAX_SAMPLES   4096
#define THEME_HUE_BUCKETS   24

typedef struct {
    int h;      /* 0..359 */
    int s;      /* 0..255 */
    int v;      /* 0..255 */
} hsv_t;

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static void rgb565_to_rgb888(uint16_t c, int *r, int *g, int *b)
{
    /* 5/6/5 → 8/8/8（右移后补高位，避免整体偏暗） */
    const int r5 = (c >> 11) & 0x1F;
    const int g6 = (c >> 5) & 0x3F;
    const int b5 = c & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

static void rgb_to_hsv(int r, int g, int b, hsv_t *out)
{
    const int max = imax(r, imax(g, b));
    const int min = imin(r, imin(g, b));
    const int d = max - min;

    out->v = max;
    out->s = (max <= 0) ? 0 : (d * 255) / max;
    if (d == 0) {
        out->h = 0;
        return;
    }
    int h;
    if (max == r) {
        h = (g - b) * 60 / d;
    } else if (max == g) {
        h = 120 + (b - r) * 60 / d;
    } else {
        h = 240 + (r - g) * 60 / d;
    }
    if (h < 0) {
        h += 360;
    }
    out->h = h;
}

static void hsv_to_rgb(const hsv_t *in, int *r, int *g, int *b)
{
    int h = in->h % 360;
    if (h < 0) {
        h += 360;
    }
    const int region = h / 60;                       /* 0..5 */
    const int rem = (h - region * 60) * 255 / 60;    /* 0..255 */
    const int v = in->v;
    const int s = in->s;
    const int p = v * (255 - s) / 255;
    const int q = v * (255 - (s * rem) / 255) / 255;
    const int t = v * (255 - (s * (255 - rem)) / 255) / 255;

    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

uint32_t music_theme_rgb_from_cover(const uint16_t *px, int w, int h)
{
    if (!px || w <= 0 || h <= 0) {
        return 0;
    }

    /* 采样步长：保证最多 THEME_MAX_SAMPLES 个点（大封面按 2/3/4… 跳着采） */
    int step = 1;
    while ((w / step) * (h / step) > THEME_MAX_SAMPLES) {
        step++;
    }

    uint32_t bucket_w[THEME_HUE_BUCKETS] = { 0 };
    uint64_t bucket_r[THEME_HUE_BUCKETS] = { 0 };
    uint64_t bucket_g[THEME_HUE_BUCKETS] = { 0 };
    uint64_t bucket_b[THEME_HUE_BUCKETS] = { 0 };

    /* 兜底统计：所有"不太黑"的像素的平均色（给低饱和/粉彩封面用） */
    uint64_t all_r = 0, all_g = 0, all_b = 0;
    uint32_t all_n = 0;

    for (int y = 0; y < h; y += step) {
        const uint16_t *row = px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x += step) {
            int r, g, b;
            rgb565_to_rgb888(row[x], &r, &g, &b);
            hsv_t c;
            rgb_to_hsv(r, g, b, &c);

            if (c.v < 40) {
                continue;               /* 近黑：深色背景/阴影，不参与取色 */
            }
            all_r += (uint64_t)r; all_g += (uint64_t)g; all_b += (uint64_t)b;
            all_n++;

            if (c.s < 26) {
                continue;               /* 灰 / 白：色相不可信 */
            }
            /* 权重：越饱和、越亮越像"主色"（纯色块比噪点权重高得多） */
            const uint32_t wgt = (uint32_t)c.s * (uint32_t)(c.v / 4 + 32);
            int bi = (c.h * THEME_HUE_BUCKETS) / 360;
            if (bi < 0) {
                bi = 0;
            } else if (bi >= THEME_HUE_BUCKETS) {
                bi = THEME_HUE_BUCKETS - 1;
            }
            bucket_w[bi] += wgt;
            bucket_r[bi] += (uint64_t)r * wgt;
            bucket_g[bi] += (uint64_t)g * wgt;
            bucket_b[bi] += (uint64_t)b * wgt;
        }
    }

    int best = -1;
    uint32_t best_w = 0;
    for (int i = 0; i < THEME_HUE_BUCKETS; i++) {
        if (bucket_w[i] > best_w) {
            best_w = bucket_w[i];
            best = i;
        }
    }

    int r, g, b;
    if (best >= 0 && best_w > 0) {
        r = (int)(bucket_r[best] / best_w);
        g = (int)(bucket_g[best] / best_w);
        b = (int)(bucket_b[best] / best_w);
    } else {
        /* 整张图没有一个"高饱和"的像素（粉彩 / 单色封面）：退回平均色，
         * 但色相要可信 —— 纯灰白的色相是算出来的 0（红），不能拿它当主题色。 */
        if (all_n == 0) {
            return 0;
        }
        r = (int)(all_r / all_n);
        g = (int)(all_g / all_n);
        b = (int)(all_b / all_n);
        hsv_t avg;
        rgb_to_hsv(r, g, b, &avg);
        if (avg.s < 8) {
            return 0;                   /* 纯黑白灰：交给默认蔚蓝 */
        }
    }

    /* 规整到"暗底上的强调色"：饱和度抬高、亮度压到不刺眼但够醒目的区间。
     * 不这么做的话，深色封面会给出几乎看不见的暗色按钮。 */
    hsv_t out;
    rgb_to_hsv(r, g, b, &out);
    out.s = imin(imax(out.s, 150), 240);
    out.v = imin(imax(out.v, 215), 245);
    hsv_to_rgb(&out, &r, &g, &b);

    return ((uint32_t)(r & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}

uint32_t music_theme_accent_rgb(void)
{
    const uint32_t c = music_state()->cover_accent;
    return c ? c : MUSIC_THEME_DEFAULT_RGB;
}

uint32_t music_theme_dim_rgb(void)
{
    /* 主题色 42% + 面板灰 58%：既看得出"同一套颜色"，又不跟播放键抢眼 */
    const uint32_t a = music_theme_accent_rgb();
    /* UI_COLOR_PILL 是 lv_color_t（不是 0xRRGGBB 的整数），必须转一道：
     * lv_color_to_u32() 给的是 XRGB8888（高字节恒 0xFF），所以再 & 0xFFFFFF 掐掉 alpha。 */
    const uint32_t p = lv_color_to_u32(UI_COLOR_PILL) & 0xFFFFFFu;
    const int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    const int pr = (int)((p >> 16) & 0xFF), pg = (int)((p >> 8) & 0xFF), pb = (int)(p & 0xFF);
    const int r = (ar * 42 + pr * 58) / 100;
    const int g = (ag * 42 + pg * 58) / 100;
    const int b = (ab * 42 + pb * 58) / 100;
    return ((uint32_t)(r & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}
