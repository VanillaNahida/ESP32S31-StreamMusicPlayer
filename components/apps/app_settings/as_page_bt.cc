/*
 * as_page_bt.cc —— 蓝牙二级页（搜索 / 配对 / 连接 / 断开 / 解除配对）
 *
 * 原 app_settings.cc 1038–1260 行**整段搬迁**（函数名 / static 名 / 注释都不动，
 * 只把通用部件的调用换成 as_* 前缀）。
 */
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "ui_core.h"
#include "ui_metrics.h"

#include "bt_service.h"

#include "as_page_shell.h"
#include "as_radio.h"
#include "as_widgets.h"

/* ================================================================== 蓝牙 */

static lv_obj_t *s_bt_status;
static lv_obj_t *s_bt_list;
static lv_timer_t *s_bt_timer;
static int        s_bt_sig = -1;
static bool       s_bt_dirty;

static void bt_rebuild_list(void);

static void on_bt_scan(lv_event_t *e)
{
    (void)e;
    if (bt_service_start_discovery() != ESP_OK) {
        /* 区分"还没初始化完"和"真的起不来"：开机后蓝牙要 1~3 秒才 READY，
         * 这时候点扫描只是太早，不该吓唬用户说蓝牙坏了 */
        if (bt_service_state() == BT_ST_FAILED) {
            ui_toast("蓝牙不可用：控制器/协议栈初始化失败", 2000);
        } else {
            ui_toast("蓝牙正在初始化，稍等 1~2 秒再扫", 1800);
        }
        return;
    }
    s_bt_dirty = true;
}

static void on_bt_forget(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    char addr[18];
    bt_service_addr_str(bda, addr, sizeof(addr));
    if (bt_service_remove_bond(bda) == ESP_OK) {
        char buf[48];
        snprintf(buf, sizeof(buf), "已解除配对 %s", addr);
        ui_toast(buf, 1600);
    } else {
        ui_toast("解除配对失败", 1600);
    }
    s_bt_dirty = true;
}

/* 点设备 = 发起 A2DP 源连接；没配对过的会在这一步顺手配对（Just Works） */
static void on_bt_dev_click(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    if (bt_service_connect(bda) == ESP_OK) {
        ui_toast("正在连接（首次会自动配对）…", 2000);
    } else {
        ui_toast("连接失败", 1600);
    }
    s_bt_dirty = true;
}

static void on_bt_disconnect(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    bt_service_disconnect(bda);
    s_bt_dirty = true;
}

static void bt_rebuild_list(void)
{
    if (!s_bt_list) {
        return;
    }
    lv_obj_clean(s_bt_list);

    /* 已连接 */
    if (bt_service_is_connected()) {
        lv_obj_t *val = NULL;
        char info[48];
        snprintf(info, sizeof(info), "%s%s", bt_service_peer_addr(),
                 bt_service_is_streaming() ? " · 推流中" : "");
        lv_obj_t *row = as_make_row(s_bt_list, LV_SYMBOL_AUDIO, 0x3E9E86,
                                    bt_service_peer_name(), &val);
        lv_label_set_text(val, info);
        as_make_row_btn(row, "断开", on_bt_disconnect, (void *)bt_service_peer_bda());    }

    /* 已配对 */
    const size_t nb = bt_service_bonded_count();
    lv_obj_t *h1 = lv_label_create(s_bt_list);
    lv_obj_set_style_text_font(h1, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h1, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(h1, "已配对的设备");
    if (nb == 0) {
        lv_obj_t *hint = lv_label_create(s_bt_list);
        lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(hint, "还没有配对过设备");
    }
    for (size_t i = 0; i < nb; i++) {
        const bt_dev_t *d = bt_service_bonded(i);
        if (!d) {
            continue;
        }
        lv_obj_t *row = as_make_row(s_bt_list, d->is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH,
                                    0x4A6FA5, d->name, NULL);
        /* BDA 也拷一份：设备数组在搜索完成后会重建 */
        void *bda_copy = as_row_user_dup(d->bda, 6);
        lv_obj_add_event_cb(row, on_bt_dev_click, LV_EVENT_CLICKED, bda_copy);
        lv_obj_add_event_cb(row, as_row_user_free, LV_EVENT_DELETE, bda_copy);
        as_make_row_btn_copy(row, "解除", on_bt_forget, d->bda, 6);
    }

    /* 搜索到的设备 */
    lv_obj_t *h2 = lv_label_create(s_bt_list);
    lv_obj_set_style_text_font(h2, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h2, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(h2, "搜索到的设备");

    const size_t n = bt_service_dev_count();
    if (n == 0) {
        lv_obj_t *hint = lv_label_create(s_bt_list);
        lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(hint, bt_service_is_discovering() ? "搜索中…（约 10 秒）"
                                                           : "点右上角「搜索」找设备");
    }
    for (size_t i = 0; i < n; i++) {
        const bt_dev_t *d = bt_service_dev(i);
        if (!d) {
            continue;
        }
        lv_obj_t *val = NULL;
        char addr[18];
        bt_service_addr_str(d->bda, addr, sizeof(addr));
        lv_obj_t *row = as_make_row(s_bt_list, d->is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH,
                                    0x3E9E86, d->name, &val);
        char info[56];
        snprintf(info, sizeof(info), "%s · %s", addr, d->bonded ? "已配对" : "未配对");
        lv_label_set_text(val, info);
        void *bda_copy2 = as_row_user_dup(d->bda, 6);
        lv_obj_add_event_cb(row, on_bt_dev_click, LV_EVENT_CLICKED, bda_copy2);
        lv_obj_add_event_cb(row, as_row_user_free, LV_EVENT_DELETE, bda_copy2);
    }
}

static void bt_tick(lv_timer_t *t)
{
    (void)t;
    as_radio_sw_sync();       /* 本页顶部的总开关也要跟着服务状态走 */
    if (!s_bt_status) {
        return;
    }
    char buf[128];
    if (bt_service_is_connected()) {
        snprintf(buf, sizeof(buf), "%s · %s%s",
                 bt_service_peer_name(), bt_service_state_str(),
                 bt_service_stream_rate() ? "" : "（未推流）");
    } else {
        snprintf(buf, sizeof(buf), "蓝牙%s · 已配对 %u 台 · 发现 %u 台",
                 bt_service_state_str(),
                 (unsigned)bt_service_bonded_count(),
                 (unsigned)bt_service_dev_count());
    }
    as_label_set_if_changed(s_bt_status, buf);

    const int sig = (int)bt_service_dev_count() * 64 + (int)bt_service_bonded_count() * 4 +
                    (int)bt_service_state() * 100000 +
                    (bt_service_is_discovering() ? 1000000 : 0);
    if (sig != s_bt_sig || s_bt_dirty) {
        s_bt_sig   = sig;
        s_bt_dirty = false;
        bt_rebuild_list();
    }
}

static void bt_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_bt_timer) {
        lv_timer_delete(s_bt_timer);
        s_bt_timer = NULL;
    }
    s_bt_status = NULL;
    s_bt_list   = NULL;
    s_bt_sig    = -1;
}

lv_obj_t *page_bt(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "蓝牙", "搜索", on_bt_scan);

    /* 顶部同一个总开关（与设置主页那行共享状态，见 page_wifi 里的说明） */
    as_radio_switch_row(body, LV_SYMBOL_BLUETOOTH, 0x4A6FA5, "蓝牙", AS_RADIO_BT, NULL, NULL);

    s_bt_status = lv_label_create(body);
    lv_obj_set_style_text_font(s_bt_status, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_bt_status, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_width(s_bt_status, LV_PCT(100));
    lv_label_set_long_mode(s_bt_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_bt_status, "…");

    lv_obj_t *note = lv_label_create(body);
    lv_obj_set_style_text_font(note, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "说明：本机可搜索/配对/解除配对；音频串流（A2DP）还没接入，"
                            "所以暂时不能把声音送到耳机。");

    s_bt_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_bt_list);
    lv_obj_set_width(s_bt_list, LV_PCT(100));
    lv_obj_set_height(s_bt_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bt_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bt_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_bt_list, LV_OBJ_FLAG_SCROLLABLE);

    bt_rebuild_list();
    lv_obj_add_event_cb(scr, bt_on_delete, LV_EVENT_DELETE, NULL);
    s_bt_timer = lv_timer_create(bt_tick, 500, NULL);
    bt_tick(NULL);
    return scr;
}
