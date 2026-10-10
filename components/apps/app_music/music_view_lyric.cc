/*
 * music_view_lyric.cc —— 歌词引擎：数据 + 渲染 + 高亮/居中不变式
 *
 * 搬迁自 app_music.cc 的 1182–1258（高亮与重贴）、2446–2488（重建）、
 * 2549–2589（可见性）、908–917（轻点封面展开/收起）。
 * 注释原样保留 —— 那段"先读旧值再赋值"的说明是防回归的关键。
 */
#include "music_view_lyric.h"

#include <string.h>

#include "esp_log.h"

#include "media_meta.h"   /* 歌词解析与查表（lrc_t 由本模块持有） */
#include "ui_metrics.h"

#include "music_state.h"

static const char *TAG = "app_music";

/*
 * 歌词列最多建多少个标签。
 * ★ 从 80 提到 240（2026-10 用户报"歌词显示不全"）：网易云的 LRC 经常带翻译/罗马音
 *   双语行，一首 4 分钟的歌轻松超过 80 行 —— 超出的部分以前**根本不建标签**，
 *   所以后面整段歌词都看不到。240 个指针的数组只有 ~1 KB，可接受。
 */
#define LYRIC_VIEW_MAX_LINES 240

static lrc_t s_lrc;                     /* 当前歌词（最多 240 行 + 时间轴） */
static int   s_lyric_cur = -1;          /* 当前高亮行（**不变式：换行时必须先读旧值**） */
static bool  s_lyric_mode;              /* true = 歌词已展开（现恒为 true，历史分支保留） */

/* 歌词控件（由播放器页建好后绑定；页面销毁时解绑） */
static lv_obj_t *s_lyric_box;
static lv_obj_t *s_lyric_hint;
static lv_obj_t *s_hint_row;
static lv_obj_t *s_buf_spin;
static lv_obj_t *s_lyric_col;
static lv_obj_t *s_lyric_labels[LYRIC_VIEW_MAX_LINES];
static size_t    s_lyric_n;

void music_view_lyric_bind(lv_obj_t *lyric_box, lv_obj_t *lyric_hint,
                           lv_obj_t *hint_row, lv_obj_t *buf_spin, lv_obj_t *lyric_col)
{
    s_lyric_box  = lyric_box;
    s_lyric_hint = lyric_hint;
    s_hint_row   = hint_row;
    s_buf_spin   = buf_spin;
    s_lyric_col  = lyric_col;
}

void music_view_lyric_reset(void)
{
    media_meta_lrc_reset(&s_lrc);
}

/*
 * 歌词三级查找（media_meta 内部按顺序试）：
 *   1) 歌曲同目录同名 .lrc
 *   2) /sdcard/lrc/<同名>.lrc
 *   3) 内嵌 ID3 歌词（USLT / SYLT）→ 取到后自动落盘到 /sdcard/lrc
 * 返回是否真的拿到了歌词（调用方要据此决定"暂无歌词"还是保留）。
 */
bool music_view_lyric_load(const char *audio_path)
{
    if (media_meta_lyrics_load(audio_path, &s_lrc) != ESP_OK) {
        ESP_LOGD(TAG, "no lyrics for %s", audio_path);
        return false;
    }
    return s_lrc.count > 0;
}

/* 在线歌词：直接从内存里的 LRC 文本解析（不读 SD 上的 .lrc 文件） */
bool music_view_lyric_parse(const char *lrc_text)
{
    return (media_meta_lrc_parse(lrc_text, &s_lrc) == ESP_OK && s_lrc.count > 0);
}

bool music_view_lyric_expanded(void)
{
    return s_lyric_mode;
}

bool music_view_lyric_timed(void)
{
    return s_lrc.timed;
}

int music_view_lyric_count(void)
{
    return (int)s_lyric_n;
}

lv_obj_t *music_view_lyric_hint_label(void)
{
    return s_lyric_hint;
}

void music_view_lyric_spin_show(void)
{
    if (s_buf_spin) {
        lv_obj_remove_flag(s_buf_spin, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 按当前歌词缓冲重建歌词行；换曲与首次建页都要调 */
void music_view_lyric_rebuild(void)
{
    if (s_lyric_col) {
        lv_obj_clean(s_lyric_col);
        lv_obj_set_style_translate_y(s_lyric_col, 0, LV_PART_MAIN);
    }
    s_lyric_n = 0;
    s_lyric_cur = -1;
    memset(s_lyric_labels, 0, sizeof(s_lyric_labels));

    if (!s_lyric_col) {
        return;
    }
    size_t n = s_lrc.count < LYRIC_VIEW_MAX_LINES ? s_lrc.count : LYRIC_VIEW_MAX_LINES;
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *l = lv_label_create(s_lyric_col);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(l, LV_PCT(100));
        /* ★ WRAP 而不是 DOT：歌词一句可能很长，DOT 会把它截成"…"（"显示不全"的另一半原因） */
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, s_lrc.lines[i].text);
        s_lyric_labels[i] = l;
    }
    s_lyric_n = n;
    music_view_lyric_apply_mode();     /* 行数变了，提示/歌词列的可见性要跟着变 */
}

/*
 * 把"当前行"移到 idx：**复位上一行 → 点亮 idx → 整列平移居中**。
 *
 * ⚠ 这里读的 `s_lyric_cur`（= 上一行）必须在**赋值之前**读出来 —— 见下面的
 *   music_view_lyric_sync()：早先拖动定位处直接把 s_lyric_cur 清成 -1 再调高亮，
 *   于是"上一行"永远是 -1，**旧那行再也没被复位**，多拖几次就亮一片
 *   （2026-10 用户报"多次拖动进度条后歌词高亮异常"）。
 */
static void lyric_apply_cur(int idx)
{
    const int prev = s_lyric_cur;
    s_lyric_cur = idx;

    /*
     * 只重设"上一行"和"当前行"。
     *
     * 歌词标签建的时候统一是 UI_FONT_SMALL + UI_COLOR_FG_DIM（见 lyric_rebuild），
     * 不变式是"只有第 s_lyric_cur 行是强调色 + 大一号字"，所以换行时动这两行就够。
     *
     * 原来对全部（最多 LYRIC_VIEW_MAX_LINES）行调 set_style_text_font：
     * 改字号等于改行高，会触发整列 flex 重新布局 —— 每换一行歌词就卡一下。
     */
    const int touched[2] = { prev, idx };
    for (size_t k = 0; k < 2; k++) {
        const int i = touched[k];
        if (i < 0 || (size_t)i >= s_lyric_n || !s_lyric_labels[i]) {
            continue;
        }
        const bool cur = (i == idx);
        lv_obj_set_style_text_color(s_lyric_labels[i],
                                    cur ? UI_COLOR_ACCENT : UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_lyric_labels[i],
                                   cur ? UI_FONT_TEXT : UI_FONT_SMALL, LV_PART_MAIN);
    }

    /*
     * 用 translate_y 把整列平移到"当前行居中"。
     * 居中基准必须是**歌词区自己的高度**：歌词区在封面右侧（右栏），
     * 用 UI_H/2 会把整列推到屏幕外 —— 这正是旧版"看不到歌词"的原因。
     */
    if (idx >= 0 && idx < (int)s_lyric_n && s_lyric_box) {
        lv_area_t a;
        lv_obj_get_coords(s_lyric_box, &a);
        const int32_t box_h  = lv_area_get_height(&a);
        const int32_t line_h = UI_PX_H(26);
        const int32_t target = box_h / 2 - line_h / 2 - (int32_t)idx * line_h;
        lv_obj_set_style_translate_y(s_lyric_col, target, LV_PART_MAIN);
    }
}

/* 定时器里用：位置变了才动（没变就一行都不碰） */
void music_view_lyric_tick(void)
{
    /* 纯文本歌词（内嵌 USLT 没有时间轴）：静态展示，不高亮也不滚动 */
    if (!s_lrc.timed || !s_lyric_col) {
        return;
    }
    const int idx = media_meta_lrc_index_at(&s_lrc, music_state()->elapsed_ms);
    if (idx == s_lyric_cur) {
        return;
    }
    lyric_apply_cur(idx);
}

/*
 * **定位之后**（拖动进度条 / 从上次位置续播）强制重新对齐高亮与居中。
 *
 * 与 music_view_lyric_tick() 的区别：即使行号没变也要重贴一遍（居中位置可能因为
 * 换页/重建而偏），但绝不能先把 s_lyric_cur 清成 -1 —— 那样上一行的复位就丢了。
 */
void music_view_lyric_sync(void)
{
    if (!s_lrc.timed || !s_lyric_col) {
        return;
    }
    lyric_apply_cur(media_meta_lrc_index_at(&s_lrc, music_state()->elapsed_ms));
}

/*
 * 歌词**常驻显示**（需求：不再用"点封面显示/隐藏"）。
 * 有歌词就显示歌词列、没歌词就显示"暂无歌词"，封面那块也不再是切换按钮，
 * 只是封面本身。s_lyric_mode 保留但恒为 true，历史分支不再改可见性。
 */
void music_view_lyric_apply_mode(void)
{
    const bool has_lyric = (s_lyric_n > 0);
    s_lyric_mode = true;

    /* 走到这里说明"缓冲中"结束了（出声 / 换曲）：把转圈藏起来 */
    if (s_buf_spin) {
        lv_obj_add_flag(s_buf_spin, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_lyric_hint) {
        if (has_lyric) {
            lv_obj_add_flag(s_lyric_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_lyric_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_lyric_hint, "暂无歌词");
        }
    }
    if (s_lyric_col) {
        if (has_lyric) {
            lv_obj_remove_flag(s_lyric_col, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lyric_col, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 轻点封面：在封面**右侧**展开 / 收起歌词 */
void music_view_lyric_toggle(void)
{
    s_lyric_mode = !s_lyric_mode;
    music_view_lyric_apply_mode();
    if (s_lyric_mode) {
        s_lyric_cur = -1;               /* 强制重算高亮与居中 */
        music_view_lyric_tick();
    }
}

void music_view_lyric_on_page_delete(void)
{
    s_lyric_box  = NULL;
    s_lyric_hint = NULL;
    s_hint_row   = NULL;
    s_buf_spin   = NULL;
    s_lyric_col  = NULL;
    s_lyric_n    = 0;
    s_lyric_cur  = -1;
    s_lyric_mode = false;
    memset(s_lyric_labels, 0, sizeof(s_lyric_labels));
}
