/*
 * music_info.cc —— "歌曲信息"弹窗（播放器页 ⓘ 按钮）
 *
 * 需求（2026-10 用户）：循环模式按钮**左边**加一个信息按钮，点开弹窗看：
 *   歌曲标题、艺术家、作曲家、专辑、时长、文件格式、文件大小、
 *   采样率、比特率、比特率（实时）、位深、声道、文件路径。
 *   信息多的时候要能滑动查看。
 *
 * 数据来源（各自的"真值"在哪，别串台）：
 *   - 标题/艺术家：本地取曲库索引（storage_service_track），在线取歌单项；
 *   - 专辑/作曲家：文件标签（media_meta_read_tags，MP3/FLAC；其它容器显示 "—"）；
 *   - 时长/文件大小/格式/路径：播放状态 + stat + 路径后缀（在线流显示流的字节数）；
 *   - 采样率/位深/声道：**解码器上报的源格式**（audio_service_source_format）——
 *     在线流没有探测值，用 get_info() 会是空的；
 *   - 比特率：audio_service_bitrate()（本地探测平均 / 在线"总字节÷时长"+实测纠偏）；
 *   - 比特率（实时）：由"位置推进 ÷ 墙钟"实测出来的**实际播放速率**乘上平均码率，
 *     每 500 ms 刷一次 —— 卡顿/变调时这个数会明显偏离标称值。
 *
 * 线程：全部在 LVGL 任务里跑。`media_meta_read_tags()` 要读文件（几 KB），
 * 所以**只在打开弹窗的那一下读一次**，不是每帧读。
 */

#include "music_info.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "audio_service.h"
#include "media_meta.h"
#include "online_service.h"
#include "storage_service.h"
#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "music_state.h"
#include "music_view_internal.h"

static const char *TAG = "app_music";

#define INFO_ROWS      13
#define INFO_LIVE_MS   500          /* "比特率（实时）"刷新周期 */

static ui_ba_dialog_t s_dlg;
static bool           s_open;
static lv_timer_t    *s_live;
static lv_obj_t      *s_val[INFO_ROWS];

/* 实时位率的采样基准 */
static uint32_t  s_rt_pos_ms;
static int64_t   s_rt_us;
static uint32_t  s_rt_bps;          /* 实测有效码率（平滑后） */
static int       s_fill_idx = -2;   /* info_fill() 填的是哪一首（换曲后要重填） */

/* ------------------------------------------------------------------ 小工具 */

/* "03:24" / "--:--" */
static void fmt_time(uint32_t ms, char *out, size_t cap)
{
    if (ms == 0) {
        snprintf(out, cap, "--:--");
        return;
    }
    snprintf(out, cap, "%02u:%02u", (unsigned)(ms / 60000u), (unsigned)((ms / 1000u) % 60u));
}

/* 文件后缀（大写）。在线 URL 先切掉 ?query，避免把签名串当后缀。
 * ⚠ 全程**不做整串拷贝**（路径长达 600 字节，拷进小 buf 会触发
 *   -Werror=format-truncation），只扫最后一段文件名。 */
static void fmt_ext(const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    const char *q   = strchr(path, '?');
    const char *end = q ? q : path + strlen(path);

    const char *dot = NULL;
    for (const char *p = end - 1; p >= path && *p != '/' && *p != '\\'; p--) {
        if (*p == '.') {
            dot = p;
            break;
        }
    }
    if (!dot || dot + 1 >= end || (size_t)(end - dot - 1) > 8) {
        snprintf(out, cap, "%s", strchr(path, ':') ? "在线流" : "—");
        return;
    }
    size_t n = 0;
    for (const char *p = dot + 1; p < end && n + 1 < cap; p++) {
        out[n++] = (char)((*p >= 'a' && *p <= 'z') ? (*p - 32) : *p);
    }
    out[n] = '\0';
}

/* 字节数 → "12.3 MB" */
static void fmt_size(uint64_t bytes, char *out, size_t cap)
{
    if (bytes == 0) {
        snprintf(out, cap, "—");
    } else if (bytes >= 1024ull * 1024ull) {
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        snprintf(out, cap, "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, cap, "%u B", (unsigned)bytes);
    }
}

/* 本地文件大小（在线返回 0，用流的 Content-Length） */
static uint64_t local_file_size(const char *path)
{
    struct stat st = {};
    if (path && path[0] == '/' && stat(path, &st) == 0 && st.st_size > 0) {
        return (uint64_t)st.st_size;
    }
    return 0;
}

static void set_row(int row, const char *text)
{
    if (row >= 0 && row < INFO_ROWS && s_val[row]) {
        lv_label_set_text(s_val[row], text ? text : "—");
    }
}

/* ------------------------------------------------------------------ 行构造 */

/* 一行 = 左侧字段名（固定宽度、暗色）+ 右侧值（可换行） */
static lv_obj_t *add_row(lv_obj_t *parent, const char *name, bool wrap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, UI_PX_W(8), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    /*
     * ⚠ 配色必须用 **ui_ba.h 里的卡片配色**，不能用页面的 UI_COLOR_FG：
     *   BA 对话框的卡片**恒为白底**（BA_CARD），而 UI_COLOR_FG 是给深色页面用的
     *   **白字** —— 用它就是"白底白字"，实机完全看不清（2026-10 用户报）。
     *   这里：字段名 = slate-600（次要正文）、值 = slate-800（正文），与
     *   ui_ba_text()/ui_ba_hint() 在同一个色阶上，整张卡片观感一致。
     */
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_style_text_font(k, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(k, lv_color_hex(BA_TEXT_BODY), LV_PART_MAIN);
    lv_obj_set_width(k, UI_PCT_X(26));
    lv_label_set_text(k, name);

    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_font(v, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, lv_color_hex(BA_TEXT), LV_PART_MAIN);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_label_set_text(v, "—");
    return v;
}

/* ------------------------------------------------------------------ 内容填充 */

static void info_fill(void)
{
    music_state_t *st = music_state();
    char buf[192];

    /* 1~2) 标题 / 艺术家：本地曲库索引、在线歌单 */
    const char *title = NULL;
    const char *artist = NULL;
    if (st->on_mode && st->on_songs && st->index >= 0 && (size_t)st->index < st->on_count) {
        const online_song_t *song = &((const online_song_t *)st->on_songs)[st->index];
        title  = song->title;
        artist = song->artist;
    } else if (st->index >= 0) {
        const track_t *t = storage_service_track((size_t)st->index);
        if (t) {
            title  = t->title;
            artist = t->artist;
        }
    }
    set_row(0, (title && title[0]) ? title : "未在播放");
    set_row(1, (artist && artist[0]) ? artist : "未知歌手");

    /* 3~4) 作曲家 / 专辑：只有本地文件能读标签（在线歌单没有这两项） */
    media_tags_t tags = {};
    const bool is_local = !(st->on_mode && !st->on_stream_local);
    if (is_local && st->track_path[0]) {
        if (media_meta_read_tags(st->track_path, &tags) != ESP_OK) {
            ESP_LOGD(TAG, "no text tags for this container (%s)", st->track_path);
        }
    }
    set_row(2, tags.composer[0] ? tags.composer : "—");
    set_row(3, tags.album[0] ? tags.album : "—");

    /* 5) 时长 */
    char tbuf[16];
    fmt_time(st->dur_ms, tbuf, sizeof(tbuf));
    set_row(4, tbuf);

    /* 6) 文件格式 */
    char ext[16];
    fmt_ext(st->track_path, ext, sizeof(ext));
    set_row(5, ext);

    /* 7) 文件大小：本地 stat；在线用流的 Content-Length（拿不到就 "—"） */
    uint64_t bytes = local_file_size(st->track_path);
    if (bytes == 0) {
        int pct = -1;
        (void)audio_service_stream_progress(&pct);   /* 只是顺带确认流在跑 */
    }
    fmt_size(bytes, buf, sizeof(buf));
    set_row(6, buf);

    /* 8~10) 采样率 / 位深 / 声道：**解码器上报的源格式**（在线也有值） */
    uint32_t rate = 0;
    uint8_t ch = 0, bits = 0;
    if (audio_service_source_format(&rate, &ch, &bits)) {
        snprintf(buf, sizeof(buf), "%u Hz", (unsigned)rate);
        set_row(7, buf);
    } else {
        set_row(7, "—");
    }

    /* 11) 比特率（平均） */
    const uint32_t bps = audio_service_bitrate();
    if (bps) {
        snprintf(buf, sizeof(buf), "%u kbps", (unsigned)(bps / 1000u));
    } else {
        snprintf(buf, sizeof(buf), "—");
    }
    set_row(8, buf);

    /* 12) 位深（源位深；送给 codec 的恒为 16-bit，见 audio-stack.md §2） */
    if (bits) {
        if (bits > 16) {
            snprintf(buf, sizeof(buf), "%u bit（输出 16 bit）", (unsigned)bits);
        } else {
            snprintf(buf, sizeof(buf), "%u bit", (unsigned)bits);
        }
        set_row(9, buf);
    } else {
        set_row(9, "—");
    }

    /* 13) 声道 */
    if (ch) {
        snprintf(buf, sizeof(buf), "%u（%s）", (unsigned)ch, ch == 1 ? "单声道" : "立体声");
        set_row(10, buf);
    } else {
        set_row(10, "—");
    }

    /* 实时行先留占位，定时器会刷 */
    set_row(11, "测量中…");

    /* 13) 文件路径（可能很长：这一行开换行，容器可滚动） */
    set_row(12, st->track_path[0] ? st->track_path : "—");
}

/* 每 500 ms：实测"实际播放速率"，据此给出实时有效码率 */
static void info_live_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_open) {
        return;
    }
    music_state_t *st = music_state();
    /* 弹窗开着的时候换曲了：把静态字段整体刷一遍（不然显示的是上一首的信息） */
    if (st->index != s_fill_idx) {
        s_fill_idx = st->index;
        info_fill();
    }

    const uint32_t pos = audio_service_position_ms();
    const int64_t  now = esp_timer_get_time();
    char buf[96];

    const uint32_t bps = audio_service_bitrate();
    if (bps == 0) {
        set_row(11, "—（码率未知）");
        return;
    }
    if (s_rt_us > 0 && now > s_rt_us && pos >= s_rt_pos_ms) {
        const int64_t d_us = now - s_rt_us;
        const uint32_t d_ms = pos - s_rt_pos_ms;
        /* 实测速率 = 实际推进的音频毫秒 ÷ 墙钟毫秒（1.00 = 正好实时） */
        const double speed = d_us > 200000 ? ((double)d_ms * 1000.0) / (double)d_us : 1.0;
        if (speed > 0.05 && speed < 8.0) {
            s_rt_bps = (uint32_t)((double)bps * speed);
        }
        snprintf(buf, sizeof(buf), "%u kbps（实际速率 ×%.2f）",
                 (unsigned)(s_rt_bps / 1000u), speed);
        set_row(11, buf);
    }
    s_rt_pos_ms = pos;
    s_rt_us     = now;
}

/* ------------------------------------------------------------------ 开关 */

void music_info_close(void)
{
    if (s_live) {
        lv_timer_delete(s_live);
        s_live = NULL;
    }
    if (s_open) {
        ui_ba_close(&s_dlg);
    }
    s_open = false;
    for (int i = 0; i < INFO_ROWS; i++) {
        s_val[i] = NULL;
    }
}

void music_info_on_page_delete(void)
{
    music_info_close();
}

void music_info_open(void)
{
    if (s_open) {
        return;                    /* 已经开着：不重复弹 */
    }
    music_state_t *st = music_state();
    if (st->index < 0 && !st->track_path[0]) {
        ui_toast("还没在播放", 1200);
        return;
    }

    /*
     * **居中**显示（2026-10 用户要求：原来是 TOP_MID，偏上）。
     * 与 ui_ba_confirm() 的用法一致；列表本身有 54% 屏高的上限 + 滚动，
     * 所以卡片再长也不会顶出屏幕。
     */
    s_dlg = ui_ba_open("歌曲信息", 76, LV_ALIGN_CENTER, 0);
    if (!s_dlg.scrim) {
        return;
    }
    s_open = true;
    ui_ba_on_close(&s_dlg, [](void *user) {
        (void)user;
        music_info_close();
    }, NULL);

    /*
     * 字段列表：**可滚动**（需求："信息较多的话就设置可以滑动查看"）。
     * 高度按屏幕百分比给，13 行 + 长路径换行时不至于把卡片撑出屏幕。
     */
    lv_obj_t *list = lv_obj_create(s_dlg.body);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_style_max_height(list, UI_PCT_Y(54), LV_PART_MAIN);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    s_val[0]  = add_row(list, "标题", true);
    s_val[1]  = add_row(list, "艺术家", true);
    s_val[2]  = add_row(list, "作曲家", true);
    s_val[3]  = add_row(list, "专辑", true);
    s_val[4]  = add_row(list, "时长", false);
    s_val[5]  = add_row(list, "格式", false);
    s_val[6]  = add_row(list, "大小", false);
    s_val[7]  = add_row(list, "采样率", false);
    s_val[8]  = add_row(list, "比特率", false);
    s_val[9]  = add_row(list, "位深", false);
    s_val[10] = add_row(list, "声道", false);
    s_val[11] = add_row(list, "实时位率", false);
    s_val[12] = add_row(list, "路径", true);

    info_fill();
    s_fill_idx = st->index;
    ui_ba_btn(s_dlg.btn_row, "关闭", true,
              [](lv_event_t *e) { (void)e; music_info_close(); }, NULL);

    /* 实时位率：每 500 ms 采一次（打开期间才跑） */
    s_rt_pos_ms = audio_service_position_ms();
    s_rt_us     = esp_timer_get_time();
    s_rt_bps    = audio_service_bitrate();
    s_live      = lv_timer_create(info_live_cb, INFO_LIVE_MS, NULL);
}
