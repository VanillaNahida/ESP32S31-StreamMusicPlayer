/*
 * music_menu.cc —— 播放器设置弹窗（BA 风格卡片，与歌曲信息同一套外壳）
 *
 *   ┌──────────────────────────────┐
 *   │  播放器设置                ✕ │
 *   │  ▬▬▬▬                        │
 *   │  音频可视化 FFT      ( ●━ )  │  ← lv_switch，落 NVS
 *   │  播放时在进度条上方显示频谱   │  ← ui_ba_hint
 *   │  ┌────────────────────────┐  │
 *   │  │▎歌曲信息             › │  │  ← ui_ba_opt_row → music_info_open()
 *   │  └────────────────────────┘  │
 *   │           [ 关闭 ]           │
 *   └──────────────────────────────┘
 */
#include "music_menu.h"

#include <stdio.h>

#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "music_info.h"
#include "music_spectrum.h"

static ui_ba_dialog_t s_dlg;
static bool s_open;

void music_menu_on_page_delete(void)
{
    if (s_open) {
        ui_ba_close(&s_dlg);
    }
    s_open = false;
}

/* FFT 开关：值一变立刻生效（控件在另一个文件里，这里只转一次调用） */
static void fft_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    music_spectrum_set_enabled(on);
    ui_toast(on ? "已开启音频可视化" : "已关闭音频可视化", 1400);
}

/* 进了"歌曲信息"就把本弹窗关掉：两个都是居中卡片，叠着看不清也没必要 */
static void info_row_cb(lv_event_t *e)
{
    (void)e;
    music_menu_on_page_delete();
    music_info_open();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    music_menu_on_page_delete();
}

void music_menu_open(void)
{
    if (s_open) {
        return;
    }

    s_dlg = ui_ba_open("播放器设置", 72, LV_ALIGN_CENTER, 0);
    if (!s_dlg.scrim) {
        return;
    }
    s_open = true;
    ui_ba_on_close(&s_dlg, [](void *user) {
        (void)user;
        music_menu_on_page_delete();
    }, NULL);

    /* ---- 第 1 行：音频可视化 FFT + 开关 ---- */
    lv_obj_t *row = lv_obj_create(s_dlg.body);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(row, UI_PX_H(4), LV_PART_MAIN);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_font(name, UI_FONT_TEXT, LV_PART_MAIN);
    /* BA 卡片恒白底：正文一律用 BA_TEXT 这一套深色，不能用 UI_COLOR_FG */
    lv_obj_set_style_text_color(name, lv_color_hex(BA_TEXT), LV_PART_MAIN);
    lv_label_set_text(name, "音频可视化 FFT");

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, UI_PX_W(52), UI_PX_H(28));
    lv_obj_set_style_bg_color(sw, lv_color_hex(BA_CANCEL_B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(BA_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(BA_CARD), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    if (music_spectrum_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, fft_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ui_ba_hint(s_dlg.body, "播放时在进度条上方显示频谱柱");

    /* ---- 第 2 行：歌曲信息入口 ---- */
    ui_ba_opt_row(s_dlg.body, "歌曲信息", BA_BLUE, info_row_cb, NULL);

    /* ---- 底部 ---- */
    ui_ba_btn(s_dlg.btn_row, "关闭", true, close_cb, NULL);
}
