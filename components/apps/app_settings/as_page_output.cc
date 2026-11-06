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
 *
 * 2026-10-21 两处补丁（用户反馈）：
 *   ① **USB 声卡也出现在这里**（之前只加在播放器页的输出弹窗里，设置页里没有）；
 *      仍然遵循"插着才出现"——`usb_audio_is_ready()` 为假就不建那一行。
 *   ② **音量滑块要跟着硬件 VOL± 实时走**：以前只在建页时读一次，
 *      按键改音量后面板（ui_volume_panel）会动、这个滑块不动，看起来像卡死。
 *      现在挂一个 500 ms 的 lv_timer：音量变了就同步滑块/百分比，
 *      输出通道或 USB 插拔状态变了就重建列表；页面 DELETE 时把定时器删掉。
 */
#include <stdint.h>
#include <stdio.h>

#include "ui_core.h"
#include "ui_metrics.h"

#include "audio_service.h"
#include "bt_service.h"
#include "usb_audio_service.h"   /* 插着才列出 USB 那一行 + 显示实际位深 */

#include "as_page_shell.h"
#include "as_widgets.h"

/* ================================================================== 声音与输出 */

static lv_obj_t  *s_out_list;
static lv_obj_t  *s_out_pct;
static lv_obj_t  *s_out_slider;
static lv_timer_t *s_out_tick;
static int        s_out_route_seen = -1;
static bool       s_out_usb_seen;

static void out_rebuild(void);

/* 一行输出项：图标块 + 名字 + 右侧状态（当前在用的写"已选择"） */
static void out_row_add(audio_out_t o, const char *sym, uint32_t accent,
                        const char *name, const char *val)
{
    lv_obj_t *value = NULL;
    lv_obj_t *row = as_make_row(s_out_list, sym, accent, name, &value);
    if (value) {
        lv_label_set_text(value, val);
    }
    lv_obj_add_event_cb(row, [](lv_event_t *e) {
        const audio_out_t which = (audio_out_t)(intptr_t)lv_event_get_user_data(e);
        esp_err_t ret = audio_service_set_output(which);
        if (ret == ESP_ERR_INVALID_STATE) {
            if (which == AUDIO_OUT_USB) {
                ui_toast("USB 声卡不在了（刚被拔出？）", 2000);
            } else {
                ui_toast("请先在「蓝牙」里连接设备后再切输出", 2200);
            }
            /* 立刻重扫：把已经失效的那一行去掉 */
            s_out_route_seen = (int)audio_service_get_output();
            s_out_usb_seen   = usb_audio_is_ready();
            out_rebuild();
            return;
        }
        if (ret != ESP_OK) {
            ui_toast("切换失败", 1500);
            return;
        }
        switch (which) {
        case AUDIO_OUT_USB:       ui_toast("输出：USB 声卡", 1500);     break;
        case AUDIO_OUT_BLUETOOTH: ui_toast("输出：蓝牙音频设备", 1500); break;
        case AUDIO_OUT_NONE:      ui_toast("已静音", 1200);            break;
        default:                  ui_toast("输出：板载扬声器", 1200);   break;
        }
        s_out_route_seen = (int)audio_service_get_output();
        out_rebuild();
    }, LV_EVENT_CLICKED, (void *)(intptr_t)o);
}

static void out_rebuild(void)
{
    if (!s_out_list) {
        return;
    }
    lv_obj_clean(s_out_list);
    const audio_out_t cur = audio_service_get_output();

    out_row_add(AUDIO_OUT_SPEAKER, LV_SYMBOL_VOLUME_MAX, 0xE8973A, "板载扬声器",
                cur == AUDIO_OUT_SPEAKER ? "已选择" : "ES8389 + NS4150B");

    out_row_add(AUDIO_OUT_BLUETOOTH, LV_SYMBOL_BLUETOOTH, 0x4A6FA5, "蓝牙音频设备",
                cur == AUDIO_OUT_BLUETOOTH
                    ? "已选择"
                    : (bt_service_is_connected()
                           ? (bt_service_is_streaming() ? "已连接 · 推流中" : "已连接")
                           : "未连接蓝牙设备"));

    /*
     * USB 声卡：**只有插着（且流已起来）才列出**（与播放器页弹窗同一套规则）。
     * 位深是自动判断的（有 24bit alt 就用 24），这里如实显示。
     */
    if (usb_audio_is_ready()) {
        static char usb_name[80];
        static char usb_val[48];
        snprintf(usb_name, sizeof(usb_name), "USB 声卡 %s", usb_audio_device_name());
        const int bd = usb_audio_bit_depth();
        if (cur == AUDIO_OUT_USB) {
            snprintf(usb_val, sizeof(usb_val), "已选择");
        } else if (bd) {
            snprintf(usb_val, sizeof(usb_val), "48 kHz · %dbit", bd);
        } else {
            snprintf(usb_val, sizeof(usb_val), "已连接");
        }
        out_row_add(AUDIO_OUT_USB, LV_SYMBOL_USB, 0x2E9E6B, usb_name, usb_val);
    }

    out_row_add(AUDIO_OUT_NONE, LV_SYMBOL_MUTE, 0x6E6E78, "静音",
                cur == AUDIO_OUT_NONE ? "已选择" : "关功放");
}

/*
 * 500 ms 巡检（LVGL 任务里跑）：
 *   ① 音量被**硬件 VOL±** 或音量面板改过 → 同步滑块与百分比（拖动中不动，让手指说了算）；
 *   ② 输出通道被别处改了（播放器页 / USB 拔插自动切回）或 USB 插拔 → 重建列表。
 */
static void out_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_out_slider && !lv_obj_has_state(s_out_slider, LV_STATE_PRESSED)) {
        const int v = audio_service_get_volume();
        if ((int)lv_slider_get_value(s_out_slider) != v) {
            lv_slider_set_value(s_out_slider, v, LV_ANIM_OFF);
        }
        if (s_out_pct) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", v);
            as_label_set_if_changed(s_out_pct, buf);
        }
    }

    const int  route = (int)audio_service_get_output();
    const bool usb   = usb_audio_is_ready();
    if (route != s_out_route_seen || usb != s_out_usb_seen) {
        s_out_route_seen = route;
        s_out_usb_seen   = usb;
        out_rebuild();
    }
}

static void out_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_out_tick) {
        lv_timer_delete(s_out_tick);   /* 页面没了就别再跑（回调里用的是本文件的 static） */
        s_out_tick = NULL;
    }
    s_out_list   = NULL;
    s_out_pct    = NULL;
    s_out_slider = NULL;
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
    s_out_route_seen = (int)audio_service_get_output();
    s_out_usb_seen   = usb_audio_is_ready();
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

    s_out_slider = sl;
    if (s_out_tick) {
        lv_timer_delete(s_out_tick);
    }
    s_out_tick = lv_timer_create(out_tick_cb, 500, NULL);

    lv_obj_add_event_cb(scr, out_on_delete, LV_EVENT_DELETE, NULL);
    return scr;
}
