/*
 * music_view_player.cc —— 播放器页（视图层）的**状态与事件**部分
 *
 * 搬迁自 app_music.cc 的 354–433（模式与来源徽标）、700–705、1170–1180、
 * 1837–1870（圆按钮工厂）、2427–2425 之后的封面贴图、2460–2538（缓冲提示）、
 * 665–780（进度条手势）、1792–1835（页面销毁）。
 *
 * 页面**布局**在 music_view_page.cc（它把建好的控件指针填进 music_view_internal.h
 * 的控件表），歌词在 music_view_lyric.cc —— 三个文件只共享那张控件表，不互相回调。
 *
 * ★ 本文件只**读** music_state（要显示什么）并调用 music_player / music_online
 *   暴露的动作；唯一的例外是缓冲提示取数（audio_service_stream_*，纯查询，取值给绘制用）。
 */
#include "music_view_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "audio_service.h"   /* 缓冲提示要取 速度/百分比（纯查询，取值给绘制用） */
#include "ui_core.h"
#include "ui_metrics.h"

#include "music_app.h"       /* music_view_player_ensure_timers（两个常驻定时器） */
#include "music_cover.h"
#include "music_info.h"
#include "music_logic.h"
#include "music_player.h"
#include "music_state.h"
#include "music_view_internal.h"
#include "music_view_lyric.h"
#include "music_view_output.h"

static const char *TAG = "app_music";

/* 播放器页控件表（页面不在时全部为 NULL，见 music_view_internal.h 的说明） */
static music_player_ctrls_t s_c;

music_player_ctrls_t *music_player_ctrls(void)
{
    return &s_c;
}

/* ------------------------------------------------------------------ 小工具 */

static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

/*
 * 圆形按钮。
 * code 一般传 LV_EVENT_CLICKED；**有长按动作的按钮要传 LV_EVENT_SHORT_CLICKED**——
 * 见输出设备按钮处的说明。
 */
lv_obj_t *music_view_make_round_btn_ex(lv_obj_t *parent, const char *sym, lv_event_cb_t cb,
                                       lv_event_code_t code, lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, UI_PX_H(56), UI_PX_H(56));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, fg, LV_PART_MAIN);
    lv_label_set_text(l, sym);
    lv_obj_center(l);

    if (cb) {
        lv_obj_add_event_cb(b, cb, code, NULL);
    }
    return b;
}

lv_obj_t *music_view_make_round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb,
                                    lv_color_t bg, lv_color_t fg)
{
    return music_view_make_round_btn_ex(parent, sym, cb, LV_EVENT_CLICKED, bg, fg);
}

/* ------------------------------------------------------------------ 来源徽标与模式 */

/*
 * 播放来源徽标（需求：播放器页要**显式**说明现在放的是本地还是在线）。
 *
 * 在线曲目和本地曲目的体验差别很大（在线要缓冲、地址会过期、拖动定位受限），
 * 光看标题看不出来源。这里在标题**上方**放一个小药丸：
 *   在线播放 → 强调色底 + 深色字（显眼）
 *   本地播放 → 灰底 + 暗字（安静）
 *
 * 为什么单独一行而不是挤进标题行：标题是整宽的 LONG_SCROLL_CIRCULAR label，
 * 同一行放别的对象会把它的滚动宽度吃掉。
 */
void music_view_player_apply_src_badge(void)
{
    const music_state_t *st = music_state();
    if (!s_c.src_badge) {
        return;
    }
    if (s_c.src_badge_state == (int)st->on_mode) {
        return;                      /* 没变就别每 tick 重设样式 */
    }
    s_c.src_badge_state = (int)st->on_mode;
    if (st->on_mode) {
        lv_label_set_text(s_c.src_badge, "在线播放");
        lv_obj_set_style_bg_color(s_c.src_badge, UI_COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_c.src_badge, lv_color_hex(0x14141A), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_c.src_badge, "本地播放");
        lv_obj_set_style_bg_color(s_c.src_badge, UI_COLOR_PILL, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_c.src_badge, UI_COLOR_FG_DIM, LV_PART_MAIN);
    }
}

/*
 * 三个模式的图标：
 *   列表循环 → LV_SYMBOL_LOOP
 *   单曲循环 → LV_SYMBOL_LOOP + 右上角 "1" 角标（等价 repeat-one）
 *   随机循环 → LV_SYMBOL_SHUFFLE
 *
 * 为什么不用"真正的" repeat-one 字形：随固件编译的位图字体 ui_font_sc16
 * 只收了生成时指定的一批 FontAwesome5 码点（见 components/ui_font/
 * ui_font_sc16.c 头部 Opts），其中没有 repeat-one，也没有 repeat。
 * 直接用会渲染成缺字方框，所以用 LOOP + "1" 角标等价表达；
 * 三个图形外形差异明显，再叠加 Toast 文案，不会认错。
 */
/* 把当前模式画到按钮上（页面尚未创建时是空操作） */
void music_view_player_apply_play_mode(void)
{
    const music_play_mode_t mode = music_state()->play_mode;
    if (s_c.mode_btn) {
        lv_label_set_text(s_c.mode_btn, (mode == MUSIC_MODE_SHUFFLE)
                                            ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP);
    }
    if (s_c.mode_badge) {
        if (mode == MUSIC_MODE_ONE) {
            lv_obj_remove_flag(s_c.mode_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_c.mode_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 点击循环模式按钮：列表循环 → 单曲循环 → 随机循环 → 列表循环
 * ★ 在线队列**不允许随机循环**：在线每换一首都要重新解析地址（还会重新缓冲），
 *   随机在在线歌单上没有意义，而且用户报过"在线循环模式失效"（见 tick 里换曲那段）。
 *   所以在线时这个按钮只在 列表循环 ↔ 单曲循环 之间切；想切到随机就挡下来并说明原因，
 *   别让按钮"看着能切、切了没反应"。 */
void music_view_player_on_mode_click(lv_event_t *e)
{
    (void)e;
    music_state_t *st = music_state();
    switch (st->play_mode) {
    case MUSIC_MODE_LIST:
        st->play_mode = MUSIC_MODE_ONE;
        break;
    case MUSIC_MODE_ONE:
        if (st->on_mode) {
            st->play_mode = MUSIC_MODE_LIST;
            music_view_player_apply_play_mode();
            ui_toast("在线播放不支持随机循环", 1500);
            ESP_LOGI(TAG, "play mode: shuffle blocked on online queue -> %s",
                     music_play_mode_name(st->play_mode));
            return;
        }
        st->play_mode = MUSIC_MODE_SHUFFLE;
        break;
    default:                        /* SHUFFLE（只可能来自本地）→ 回到列表循环 */
        st->play_mode = MUSIC_MODE_LIST;
        break;
    }
    music_view_player_apply_play_mode();
    ui_toast(music_play_mode_name(st->play_mode), 1200);
    ESP_LOGI(TAG, "play mode -> %s", music_play_mode_name(st->play_mode));
}

/* ------------------------------------------------------------------ 数值刷新 */

void music_view_player_set_now_playing(const char *title, const char *artist)
{
    if (s_c.title) {
        lv_label_set_text(s_c.title, title ? title : "");
    }
    if (s_c.artist) {
        lv_label_set_text(s_c.artist, (artist && artist[0]) ? artist : "未知歌手");
    }
}

void music_view_player_set_play_icon(bool playing)
{
    if (s_c.btn) {
        lv_label_set_text(lv_obj_get_child(s_c.btn, 0),
                          playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

/*
 * 加载态：播放按钮换成转圈 + 禁用点击。
 *
 * 需求原话（2026-10 用户）："在加载的时候禁用播放按钮，用加载动画来代替，
 * 可以避免一些奇奇怪怪的 bug" —— 确实是：加载期间（解析地址 / 抓封面歌词 / 缓冲）
 * 按播放会走 `on_play_pause()` → `audio_service_toggle()`，而这时会话可能刚建到一半，
 * toggle 失败后走"重新起播"分支 → **重新起播会释放屏上的封面再重新请求**，
 * 而这一次的封面数据可能已经被消费掉 → 封面就"没了"（用户报的现象）。
 *
 * 实现要点：
 *   - 转圈懒建在播放按钮**里面**（居中），与按钮同生共死，页面销毁时随按钮一起走；
 *   - 图标 label 是按钮的第 0 个子对象（见 music_view_page.cc 的建按钮处），
 *     加载时把它藏起来，转圈顶上；
 *   - `LV_STATE_DISABLED` 只负责"看起来禁用"（变暗）；**是否真的吃掉点击**
 *     由 `on_play_pause` 里那道门负责（别依赖 LVGL 的 disabled 语义）。
 *   - 幂等：tick 每 500 ms 调一次，状态没变直接返回，不动控件。
 */
void music_view_player_set_busy(bool busy)
{
    if (!s_c.btn || s_c.btn_busy == busy) {
        return;
    }
    s_c.btn_busy = busy;
    lv_obj_t *icon = lv_obj_get_child(s_c.btn, 0);

    if (busy) {
        if (!s_c.btn_spinner) {
            s_c.btn_spinner = lv_spinner_create(s_c.btn);
            lv_obj_remove_style_all(s_c.btn_spinner);
            lv_obj_set_size(s_c.btn_spinner, UI_PX_H(26), UI_PX_H(26));
            lv_obj_set_style_arc_width(s_c.btn_spinner, 3, LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_c.btn_spinner, 3, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(s_c.btn_spinner, UI_COLOR_PILL, LV_PART_MAIN);
            lv_obj_set_style_arc_color(s_c.btn_spinner, UI_COLOR_ACCENT, LV_PART_INDICATOR);
            lv_obj_center(s_c.btn_spinner);
            lv_obj_remove_flag(s_c.btn_spinner, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_remove_flag(s_c.btn_spinner, LV_OBJ_FLAG_HIDDEN);
        if (icon) {
            lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_state(s_c.btn, LV_STATE_DISABLED);
    } else {
        if (s_c.btn_spinner) {
            lv_obj_add_flag(s_c.btn_spinner, LV_OBJ_FLAG_HIDDEN);
        }
        if (icon) {
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_state(s_c.btn, LV_STATE_DISABLED);
    }
}

bool music_view_player_is_busy(void)
{
    return s_c.btn_busy;
}

void music_view_player_set_elapsed_text(uint32_t elapsed_ms)
{
    if (s_c.cur) {
        char buf[16];
        music_fmt_time(elapsed_ms, buf, sizeof(buf));
        label_set_if_changed(s_c.cur, buf);
    }
}

void music_view_player_set_total_text(uint32_t dur_ms)
{
    if (s_c.tot) {
        char buf[16];
        music_fmt_time(dur_ms, buf, sizeof(buf));
        label_set_if_changed(s_c.tot, buf);
    }
}

void music_view_player_set_total_text_placeholder(void)
{
    if (s_c.tot) {
        lv_label_set_text(s_c.tot, "--:--");
    }
}

void music_view_player_set_progress(int32_t permille, uint32_t dur_ms)
{
    if (s_c.seek && dur_ms > 0) {
        lv_bar_set_value(s_c.seek, permille, LV_ANIM_OFF);
    }
}

void music_view_player_reset_progress(void)
{
    if (s_c.seek) {
        lv_bar_set_value(s_c.seek, 0, LV_ANIM_OFF);
    }
}

void music_view_player_set_lyric_mode(bool on)
{
    (void)on;   /* 歌词模式的 owner 是 music_view_lyric.cc：恒为展开（见其 apply_mode） */
}

void music_view_player_set_seeking(bool on)
{
    s_c.seeking = on;
}

/* tick 用：刷进度条 + 已播/总时长（手指正在拖时整段跳过） */
void music_view_player_refresh_timesliders(void)
{
    const music_state_t *st = music_state();
    if (s_c.seeking) {
        return;
    }
    music_view_player_set_progress(music_progress_permille(st->elapsed_ms, st->dur_ms),
                                   st->dur_ms);
    music_view_player_set_elapsed_text(st->elapsed_ms);
    music_view_player_set_total_text(st->dur_ms);
}

/* 歌词相关的转发（真身在 music_view_lyric.cc；业务层只认这几个名字） */
void music_view_player_lyric_reset(void)
{
    music_view_lyric_reset();
}

bool music_view_player_lyric_load(const char *audio_path)
{
    return music_view_lyric_load(audio_path);
}

bool music_view_player_lyric_parse(const char *lrc_text)
{
    return music_view_lyric_parse(lrc_text);
}

void music_view_player_lyric_rebuild(void)
{
    music_view_lyric_rebuild();
}

void music_view_player_lyric_tick(void)
{
    music_view_lyric_tick();
}

void music_view_player_lyric_sync(void)
{
    music_view_lyric_sync();
}

void music_view_player_lyric_apply_mode(void)
{
    music_view_lyric_apply_mode();
}

bool music_view_player_lyric_expanded(void)
{
    return music_view_lyric_expanded();
}

bool music_view_player_lyric_timed(void)
{
    return music_view_lyric_timed();
}

/* ------------------------------------------------------------------ 缓冲提示 */

/*
 * 缓冲提示文字（需求："缓冲中 20 KB/s" + 一个转圈，别让用户以为卡死）。
 * 文字拼装是纯逻辑（music_logic.cc 的 music_buffer_hint_text），这里只负责取数与上屏。
 */
void music_view_player_refresh_buffer_hint(void)
{
    /*
     * 加载态与缓冲提示共用同一个判据（`buf_show || on_busy`），在这里对齐：
     * 只要还在"解析地址 / 抓封面歌词 / 缓冲"，播放按钮就是转圈 + 禁用。
     * 出声后 tick 会把 buf_show 清掉，按钮自动恢复成播放/暂停图标。
     */
    const music_state_t *st = music_state();
    music_view_player_set_busy(st->buf_show || st->on_busy);

    lv_obj_t *hint = music_view_lyric_hint_label();
    if (!hint) {
        return;
    }
    /*
     * 转圈的可见性在这里一并保证（而不是只在 set_buffering_hint() 里开）：
     * 起播流程里 music_online_start_track() → lyric_rebuild() → lyric_apply_mode()
     * 会先把转圈藏起来，而这时 HTTP 流**才刚开始缓冲**（buf_show 仍为真）——
     * 只看 set_buffering_hint() 的话，最该转圈的那一段反而没有转圈。
     * 所以"有文字就有转圈"，两处状态由这一个函数对齐。
     */
    music_view_lyric_spin_show();
    int kbps = 0;
    int pct  = -1;
    const bool has_speed = audio_service_stream_speed(&kbps);
    (void)audio_service_stream_progress(&pct);      /* 拿不到就保持 -1 */

    char buf[MUSIC_BUF_TEXT_MAX];
    music_buffer_hint_text(has_speed, kbps, pct, buf, sizeof(buf));
    lv_label_set_text(hint, buf);
}

/*
 * 在线歌的"解析地址 + 抓封面 + 取歌词"是异步的（实测 1~3 秒）。
 * 这段时间右栏说明行显示"缓冲中 <速度>"，**并且把转圈转起来**（需求：
 * 只写一行字看着像卡死）；起播时 music_online_start_track() 会走
 * lyric_rebuild() → lyric_apply_mode() 把它恢复成 歌词 / 暂无歌词。
 *
 * 置 buf_show 之后，tick 会周期性刷新速度/百分比，
 * 并在真的出声时交回 lyric_apply_mode()。
 */
void music_view_player_set_buffering_hint(void)
{
    music_state()->buf_show = true;
    lv_obj_t *hint = music_view_lyric_hint_label();
    if (!hint) {
        return;
    }
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
    music_view_player_refresh_buffer_hint();   /* 首帧就有速度，并把转圈转起来 */
}

/* ------------------------------------------------------------------ 封面 */

static void cover_set_src(void *user, const void *dsc)
{
    (void)user;
    if (s_c.cover_img) {
        lv_image_set_src(s_c.cover_img, (const lv_image_dsc_t *)dsc);
    }
}

static void cover_set_visible(void *user, bool visible)
{
    (void)user;
    if (!s_c.cover_img) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_c.cover_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_c.cover_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void music_view_player_cover_show(uint8_t *px, int w, int h)
{
    music_cover_place(px, w, h, cover_set_src, cover_set_visible, NULL);
}

/* tick 用：先把工作线程的产出搬上屏（下标对不上就丢掉），再按需拉起下一次解码 */
void music_view_player_cover_pump(void)
{
    uint8_t *px = NULL;
    int w = 0, h = 0;
    if (music_cover_pump(music_state()->index, &px, &w, &h) == MUSIC_COVER_PUMP_OK) {
        music_view_player_cover_show(px, w, h);
    }
    music_cover_kick();
}

/* 进播放器页时把留着的封面直接贴回来（不重新下载、不重新解码）。
 * ⚠ **不要**走 music_view_player_cover_show(NULL,…)：那会先把留着的像素释放掉。 */
void music_view_player_cover_reattach(void)
{
    const void *dsc = music_cover_dsc();
    if (dsc && music_cover_pixels()) {
        cover_set_src(NULL, dsc);
        cover_set_visible(NULL, true);
    }
}

/* ------------------------------------------------------------------ 进度条手势 */

/* 触点 x 在抓取区里的比例 → 千分比（0~1000） */
static int32_t seek_pointer_permille(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !s_c.seek_hit) {
        return 0;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(s_c.seek_hit, &a);
    int w = lv_area_get_width(&a);
    if (w <= 0) {
        return 0;
    }
    int rel = (int)p.x - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    return (int32_t)((int64_t)rel * 1000 / w);
}

/* 按下即跟手：拖动中只更新进度条与"已播"时间，**松手才真定位**
 * （seek 要暂停/恢复管线，边拖边定位会一顿一顿）
 *
 * ★ 时长未知（dur_ms == 0）时**也允许进入拖动**（2026-10 修"进度条完全不可用"）：
 *   原来这里 `|| s_dur_ms == 0` 直接 return —— 于是 seeking 一直是 false，
 *   拇指不跟手，而且 on_seek_release() 在 `if (!seeking) return;` 就退出，
 *   **下面那句"时长未知，暂时无法拖动定位"的提示永远执行不到**。
 *   表现就是"按下去毫无反应、也没有任何解释"。
 *   现在：照样进拖动（拇指跟手，让用户看到控件是活的），松手时由 on_seek_release
 *   给出原因。视觉归位由 tick / 松手分支负责。
 */
static void on_seek_press(lv_event_t *e)
{
    (void)e;
    const music_state_t *st = music_state();
    if (st->index < 0 || !s_c.seek) {
        return;
    }
    s_c.seeking = true;
    const int32_t permille = seek_pointer_permille();
    if (st->dur_ms == 0) {
        /* 时长未知：没法换算时间，但拇指照样跟手（纯视觉反馈） */
        lv_bar_set_value(s_c.seek, permille, LV_ANIM_OFF);
        return;
    }
    lv_bar_set_value(s_c.seek, permille, LV_ANIM_OFF);
    if (s_c.cur) {
        char buf[16];
        music_fmt_time(music_seek_target_ms(permille, st->dur_ms), buf, sizeof(buf));
        lv_label_set_text(s_c.cur, buf);
    }
}

/* 手指滑出抓取区：只把"正在拖动"清掉，不做定位 */
static void on_seek_lost(lv_event_t *e)
{
    (void)e;
    s_c.seeking = false;
}

static void on_seek_release(lv_event_t *e)
{
    (void)e;
    const music_state_t *st = music_state();
    if (!s_c.seeking) {
        return;
    }
    s_c.seeking = false;
    if (st->index < 0) {
        return;
    }
    if (st->dur_ms == 0) {
        ui_toast("时长未知，暂时无法拖动定位", 1600);
        if (s_c.seek) {
            lv_bar_set_value(s_c.seek, 0, LV_ANIM_OFF);
        }
        return;
    }

    const int32_t permille = seek_pointer_permille();
    const uint32_t target  = music_seek_target_ms(permille, st->dur_ms);

    /*
     * 两道关都要给用户一句解释 —— 进度条是"活的"（拇指跟手），要是拖完什么都不发生，
     * 用户只会以为界面卡了（2026-10 用户报的原话就是"点了没反应"）。
     *   1) 容器：MP4/M4A/OGG 这类要索引/setup 头的格式，按字节跳过去解码器对不上帧；
     *   2) 位置算不算得出来：本地要有 probe 到的平均码率，在线要有"总字节 + 时长"。
     */
    if (!music_player_seek_begin(target)) {
        return;
    }

    if (s_c.seek) {
        lv_bar_set_value(s_c.seek, permille, LV_ANIM_OFF);
    }
    if (s_c.cur) {
        char buf[16];
        music_fmt_time(target, buf, sizeof(buf));
        lv_label_set_text(s_c.cur, buf);
    }
    /*
     * ★ 高亮跟着新位置走。用 lyric_sync() 而**不是**"先清高亮行再调 lyric_tick()"：
     *   后者会让"上一行"变成 -1，旧那行的高亮永远复位不掉 ——
     *   多拖几次进度条，歌词区就亮起好几行（2026-10 用户报）。
     */
    music_view_player_lyric_sync();
}

void music_view_player_bind_seek(lv_obj_t *hit)
{
    s_c.seek_hit = hit;
    lv_obj_add_event_cb(hit, on_seek_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, on_seek_press, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hit, on_seek_release, LV_EVENT_RELEASED, NULL);
    /* 手指滑出抓取区时 LVGL 只发 PRESS_LOST，不发 RELEASED：不处理的话
     * seeking 会永久停在 true，tick 里的进度更新全被跳过 —— 表现就是
     * "进度条概率性不再动/像消失了"。这里一并按松手处理。 */
    lv_obj_add_event_cb(hit, on_seek_lost, LV_EVENT_PRESS_LOST, NULL);
}

/* 轻点封面：在封面**右侧**展开 / 收起歌词 */
void music_view_player_on_cover_click(lv_event_t *e)
{
    (void)e;
    music_view_lyric_toggle();
}

static void on_play_pause(lv_event_t *e)
{
    (void)e;
    music_player_on_play_pause();
}

static void on_prev(lv_event_t *e) { (void)e; music_player_play_next(-1); }
static void on_next(lv_event_t *e) { (void)e; music_player_play_next(+1); }

void music_view_player_wire_transport(lv_obj_t *prev_btn, lv_obj_t *play_btn, lv_obj_t *next_btn)
{
    lv_obj_add_event_cb(prev_btn, on_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(play_btn, on_play_pause, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next_btn, on_next, LV_EVENT_CLICKED, NULL);
    s_c.btn = play_btn;
}

void music_view_player_bind_mode_btn(lv_obj_t *mode_btn)
{
    s_c.mode_btn = lv_obj_get_child(mode_btn, 0);
}

void music_view_player_bind_mode_badge(lv_obj_t *badge)
{
    s_c.mode_badge = badge;
}

void music_view_player_bind_cover(lv_obj_t *box, lv_obj_t *img)
{
    s_c.cover_box = box;
    s_c.cover_img = img;
}

void music_view_player_bind_src_badge(lv_obj_t *badge)
{
    s_c.src_badge = badge;
    s_c.src_badge_state = -1;       /* 页面刚建：强制重画一次 */
    music_view_player_apply_src_badge();
}

void music_view_player_bind_times(lv_obj_t *cur, lv_obj_t *tot, lv_obj_t *seek)
{
    s_c.cur  = cur;
    s_c.tot  = tot;
    s_c.seek = seek;
}

/*
 * 标题/歌手必须绑定（2026-10 修）。
 *
 * 拆分页面时漏了这一步：label 建出来直接 set 了文本，却没有登记进控件表，
 * 于是 `s_c.title/s_c.artist` 永远是 NULL —— `music_view_player_set_now_playing()`
 * 变成空操作，表现就是"**页面初次进入显示正常，之后换曲再也不会更新**"
 * （标题/歌手一直停在建页面那一刻的那首）。所有 `bind_*` 都是这个用途，
 * 新增控件时别只 set 文本忘了 bind。
 */
void music_view_player_bind_now_playing(lv_obj_t *title, lv_obj_t *artist)
{
    s_c.title  = title;
    s_c.artist = artist;
}

/* ------------------------------------------------------------------ 事件与销毁 */

void music_view_player_out_btn_sync(void)
{
    music_view_output_sync();
}

void music_view_player_on_delete(void)
{
    /* 弹窗挂在 top layer 上，不随页面销毁：离开播放器页就显式关掉它 */
    music_view_output_on_page_delete();
    music_info_on_page_delete();      /* 歌曲信息弹窗同理（含它的实时刷新定时器） */
    /* 歌词控件归 music_view_lyric.cc；只解绑，**歌词数据留着**（重进页面直接重建） */
    music_view_lyric_on_page_delete();

    /* ★ 这里**不要**删 s_tick / s_media_timer：它们是开机建的常驻定时器
     *（见 app_music_register_apps 里的说明）——删掉就等于"离开播放器页就不再续播/
     * 不再响应耳机按键"，正是 2026-10 那个"后台播完不继续"的 bug。 */
    /* 页面控件指针全部清空：定时器与回调别再往已销毁对象里写（tick 常驻，必须清）。
     * 一次 memset 清掉整张表 —— 这是原实现里 17 行逐个置 NULL 的等价形式。 */
    memset(&s_c, 0, sizeof(s_c));
    s_c.src_badge_state = -1;       /* 下次建页面要重新画一次 */

    /*
     * ★ 解码好的封面**故意不释放**（2026-10 用户报"切页面回来封面就没了"）：
     *   封面像素是 PSRAM 里的 RGB565（240×240 ≈ 115 KB），描述符是指向它的堆内存
     *   —— 两者都不属于 LVGL 对象，页面销毁后依然有效（产权在 music_cover.cc）。
     *   留着它，下次进播放器页直接 lv_image_set_src() 贴回去，不用重新下载/解码；
     *   真换曲时 start_track_*() 会 cover_show(NULL,...) 把它释放掉（有界，不会累积）。
     */
}

void music_view_player_page_delete_cb(lv_event_t *e)
{
    (void)e;
    music_view_player_on_delete();
    /* 业务侧的收尾（落盘进度 / 丢弃没起播的在线结果 / 清掉在线残留） */
    music_player_on_view_destroyed();
}

void music_view_player_build_prepare(void)
{
    /* 页面刚建：控件表由 music_view_page.cc 逐个绑定；这里只保证常驻定时器在跑 */
    music_view_player_ensure_timers();
}
