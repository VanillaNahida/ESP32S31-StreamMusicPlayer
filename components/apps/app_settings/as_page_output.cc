/*
 * as_page_output.cc —— 声音与输出二级页（输出方式选择 + 音量滑块）
 *
 * 原 app_settings.cc 1262–1379 行**整段搬迁**（函数名 / static 名 / 注释都不动，
 * 只把通用部件的调用换成 as_* 前缀）。
 *
 * 行为要点（拆分时不许改）：
 *   - 点"蓝牙音频设备"时若 `!bt_service_is_connected()`，`audio_service_set_output()`
 *     返回 ESP_ERR_INVALID_STATE → Toast「请先在「蓝牙」里连接设备后再切输出」；
 *   - 音量滑块在 VALUE_CHANGED 里**直接**下发（拖动中就生效），只顺带更新百分比 label。
 */
#include <stdint.h>

#include "ui_core.h"
#include "ui_metrics.h"

#include "audio_service.h"
#include "bt_service.h"

#include "as_page_shell.h"
#include "as_widgets.h"

/* ================================================================== 声音与输出 */

static lv_obj_t *s_out_list;
static lv_obj_t *s_out_pct;

static void out_rebuild(void)
{
    if (!s_out_list) {
        return;
    }
    lv_obj_clean(s_out_list);
    const audio_out_t cur = audio_service_get_output();
    const char *bt_hint = bt_service_is_connected()
                              ? (bt_service_is_streaming() ? "推流中" : "已连接")
                              : "未连接蓝牙设备";
    const struct {
        audio_out_t out;
        const char *sym;
        const char *name;
        const char *val;
    } items[] = {
        { AUDIO_OUT_SPEAKER,   LV_SYMBOL_VOLUME_MAX, "板载扬声器", "ES8389 + NS4150B" },
        { AUDIO_OUT_BLUETOOTH, LV_SYMBOL_BLUETOOTH,  "蓝牙音频设备", bt_hint },
        { AUDIO_OUT_NONE,      LV_SYMBOL_MUTE,       "静音",       "关功放" },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *val = NULL;
        lv_obj_t *row = as_make_row(s_out_list, items[i].sym, 0xE8973A, items[i].name, &val);
        lv_label_set_text(val, (cur == items[i].out) ? "已选择" : items[i].val);
        lv_obj_add_event_cb(row, [](lv_event_t *e) {
            const audio_out_t o = (audio_out_t)(intptr_t)lv_event_get_user_data(e);
            esp_err_t ret = audio_service_set_output(o);
            if (ret == ESP_ERR_INVALID_STATE) {
                ui_toast("请先在「蓝牙」里连接设备后再切输出", 2200);
                return;
            }
            if (ret != ESP_OK) {
                ui_toast("切换失败", 1500);
                return;
            }
            if (o == AUDIO_OUT_NONE) {
                ui_toast("已静音", 1200);
            } else if (o == AUDIO_OUT_BLUETOOTH) {
                ui_toast("输出：蓝牙音频设备", 1500);
            } else {
                ui_toast("输出：板载扬声器", 1200);
            }
            out_rebuild();
        }, LV_EVENT_CLICKED, (void *)(intptr_t)items[i].out);
    }
}

static void out_on_delete(lv_event_t *e)
{
    (void)e;
    s_out_list = NULL;
    s_out_pct  = NULL;
}

lv_obj_t *page_output(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "声音与输出", NULL, NULL);

    s_out_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_out_list);
    lv_obj_set_width(s_out_list, LV_PCT(100));
    lv_obj_set_height(s_out_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_out_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_out_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_out_list, LV_OBJ_FLAG_SCROLLABLE);
    out_rebuild();

    /* 音量：0~100，松手即下发（拖动中只更新数字） */
    lv_obj_t *vol_row = lv_obj_create(body);
    lv_obj_remove_style_all(vol_row);
    lv_obj_set_size(vol_row, LV_PCT(100), UI_PX_H(56));
    lv_obj_set_style_bg_color(vol_row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(vol_row, UI_PX_W(14), LV_PART_MAIN);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vl = lv_label_create(vol_row);
    lv_obj_set_style_text_font(vl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(vl, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(vl, "音量");
    lv_obj_align(vl, LV_ALIGN_LEFT_MID, 0, 0);

    s_out_pct = lv_label_create(vol_row);
    lv_obj_set_style_text_font(s_out_pct, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_out_pct, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text_fmt(s_out_pct, "%d%%", audio_service_get_volume());
    lv_obj_align(s_out_pct, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *sl = lv_slider_create(vol_row);
    lv_obj_set_size(sl, UI_PCT_X(50), UI_PX_H(10));
    lv_obj_align(sl, LV_ALIGN_CENTER, UI_PX_W(20), 0);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, audio_service_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, UI_PX_H(6), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        lv_obj_t *s = lv_event_get_target_obj(e);
        int v = (int)lv_slider_get_value(s);
        audio_service_set_volume(v);
        if (s_out_pct) {
            lv_label_set_text_fmt(s_out_pct, "%d%%", v);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(scr, out_on_delete, LV_EVENT_DELETE, NULL);
    return scr;
}
