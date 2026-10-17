/*
 * as_page_bt.cc —— 蓝牙二级页（搜索 / 配对 / 连接 / 断开 / 解除配对）
 *
 * 原 app_settings.cc 1038–1260 行**整段搬迁**（函数名 / static 名 / 注释都不动，
 * 只把通用部件的调用换成 as_* 前缀）。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
static char       s_bt_sig[96];
static bool       s_bt_dirty;

static void bt_rebuild_list(void);

/* 这台设备是不是"正在连 / 刚连失败"的目标 */
static bool bt_row_is_target(const uint8_t bda[6])
{
    const uint8_t *t = bt_service_connect_target();
    return t && memcmp(t, bda, 6) == 0;
}

/*
 * 行右侧的"转圈 + 连接中…"。
 *
 * 为什么必须有：以前点设备只弹一句 2 秒的 Toast，**行上什么都没有** —— 连接失败
 * （原来失败是完全静默的）之后用户看到的就是"点了没反应"。转圈是 lv_spinner 自带的
 * 无限循环动画，不用自己起定时器。
 */
static void bt_add_busy_badge(lv_obj_t *row)
{
    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, UI_PX_W(6), LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sp = lv_spinner_create(box);
    lv_obj_set_size(sp, UI_PX_H(18), UI_PX_H(18));
    lv_obj_set_style_arc_width(sp, UI_PX_H(3), LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, UI_PX_H(3), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(l, "连接中…");
}

/*
 * 目标行右侧的状态文字：
 *   连接中     → 值 label 清空（右边换成转圈）
 *   连接失败   → "连接失败 · 点此重试"（整行点击就是重试）
 */
static void bt_row_apply_connect_state(lv_obj_t *row, const uint8_t bda[6], lv_obj_t *val)
{
    if (!val || !bt_row_is_target(bda)) {
        return;
    }
    const bt_state_t st = bt_service_state();
    if (st == BT_ST_CONNECT_FAILED) {
        lv_label_set_text(val, "连接失败 · 点此重试");
        lv_obj_set_width(val, UI_PCT_X(45));
        lv_obj_set_style_text_color(val, UI_COLOR_ACCENT, LV_PART_MAIN);
    } else if (st == BT_ST_CONNECTING || bt_service_is_connecting()) {
        lv_label_set_text(val, "");
        bt_add_busy_badge(row);
    }
    /* 其它状态（比如关开关后的陈旧目标）什么都不画 */
}

static void on_bt_scan(lv_event_t *e)
{
    (void)e;
    if (bt_service_is_connecting()) {
        /* 连接中不让扫描：查询会和 page/AVDTP 抢射频，把正在进行的连接搅黄 */
        ui_toast("正在连接设备，请稍后再试", 1800);
        return;
    }
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
    if (bt_row_is_target(bda)) {
        ui_toast("正在处理这台设备，请稍候", 1600);
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

/*
 * 点设备 = 发起 A2DP 源连接；没配对过的会在这一步顺手配对（Just Works）。
 *
 * 连接的过程（停查询 → 必要时先断旧对端 → 超时 → 自动重试 2 次）全在
 * bt_service 的 bts_conn.cc 里；这里只负责"点了之后马上告诉用户发生了什么"：
 *   - 正在连这台 → 提示"正在连接，请稍后再试"（用户明确要求）；
 *   - 正在连另一台 → 改连这台（服务会先取消当前尝试/断开旧对端）；
 *   - 其余失败   → 提示，并把行上标成"连接失败 · 点此重试"。
 */
static void on_bt_dev_click(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    if (bt_row_is_target(bda) && bt_service_is_connecting()) {
        ui_toast("正在连接，请稍后再试", 1600);
        return;
    }
    const esp_err_t ret = bt_service_connect(bda);
    if (ret == ESP_OK) {
        ui_toast("正在连接（首次会自动配对）…", 1600);
    } else if (!bt_service_is_ready() || bt_service_state() == BT_ST_UNINIT) {
        ui_toast("蓝牙正在初始化，稍等 1~2 秒再试", 1800);
    } else {
        ui_toast("连接请求没发出去，请重试", 1600);
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
        lv_obj_t *val = NULL;
        lv_obj_t *row = as_make_row(s_bt_list, d->is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH,
                                    0x4A6FA5, d->name, &val);
        bt_row_apply_connect_state(row, d->bda, val);
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
        bt_row_apply_connect_state(row, d->bda, val);
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

    char buf[160];
    const uint8_t *tgt = bt_service_connect_target();
    if (bt_service_is_connecting()) {
        /* 连接中（含"等重试"阶段）：把目标名字显示出来，用户才知道在等谁 */
        snprintf(buf, sizeof(buf), "正在连接 %s…", bt_service_peer_name());
    } else if (bt_service_state() == BT_ST_CONNECT_FAILED) {
        snprintf(buf, sizeof(buf), "连接 %s 失败（%s）· 点该设备可重试",
                 bt_service_peer_name(), bt_service_connect_error());
    } else if (bt_service_is_connected()) {
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

    /*
     * 需要重建列表的判据：状态 / 设备数 / 配对表代次 / 搜索中 / **连接目标**。
     * 目标必须算进去 —— 否则"从连 A 改成连 B"时状态一直是 CONNECTING，
     * 转圈会留在 A 那一行上。
     */
    char sig[96];
    snprintf(sig, sizeof(sig), "%d|%u|%u|%d|%s",
             (int)bt_service_state(),
             (unsigned)bt_service_dev_count(),
             (unsigned)bt_service_bonded_count(),
             bt_service_is_discovering() ? 1 : 0,
             tgt ? "target" : "-");
    if (tgt) {
        size_t used = strlen(sig);
        for (int i = 0; i < 6 && used + 3 < sizeof(sig); i++) {
            used += (size_t)snprintf(sig + used, sizeof(sig) - used, "%02x", tgt[i]);
        }
    }

    if (strcmp(sig, s_bt_sig) != 0 || s_bt_dirty) {
        /*
         * ⚠ 手指正按着列表时**不要重建**：重建会把行对象删掉再建一份，LVGL 会给
         *   原对象发 PRESS_LOST —— 这次点击就凭空消失了（"点了没反应"的另一个
         *   来源，扫描期间尤其容易撞上）。延到松手后的下一个 tick 再建。
         */
        lv_indev_t *indev = lv_indev_get_next(NULL);
        if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            s_bt_dirty = true;
            return;
        }
        snprintf(s_bt_sig, sizeof(s_bt_sig), "%s", sig);
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
    s_bt_sig[0] = '\0';
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
    lv_label_set_text(note, "可在此搜索并连接蓝牙音频设备");

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
