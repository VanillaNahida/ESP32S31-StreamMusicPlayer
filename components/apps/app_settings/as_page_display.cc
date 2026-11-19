/*
 * as_page_display.cc —— 显示设置二级页（时钟显秒 + 手动 NTP 同步）
 *
 * 为什么有这一页（2026-11 用户要求）：
 *   原来设置主页那一行叫「显示与亮度」，点它只弹一句 Toast「本板没有背光器件，
 *   软件无法调亮度」—— 等于**没有任何显示相关的设置**。现在改名「显示设置」并
 *   真的做成子页，先装两件确实有用的事：
 *
 *   ① **时钟显秒**：首页大时钟和状态栏时钟都跟着它（同一个 `ui_clock_text()`）；
 *      开关状态存 NVS（`clock_cfg/secs`，服务的 API 在 wifi_service 里 ——
 *      ui_core 不许直接碰 NVS，见 AGENTS.md 第 6 节分层规则）。
 *   ② **手动 NTP 同步**：SNTP 平时是"拿到 IP 就自己跑"，但校时偶尔会失败
 *      （DNS 抽风 / 只连了内网 / 开机时 AP 还没通），以前**只能重启**。
 *      现在点一下立即重试；`wifi_service_time_sync_now()` 是阻塞的（最长 8 s），
 *      所以在后台任务里跑，跑完用 lv_async_call 回到 LVGL 任务弹结果。
 *
 * 亮度仍然没有：本板板级配置里根本没有背光器件（AGENTS.md 第 5 节），
 * 页面底部保留一句说明，免得用户以为"藏在别处"。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ui_core.h"
#include "ui_metrics.h"

#include "wifi_service.h"     /* 时钟显秒（NVS）+ 手动校时 + 时间是否就绪 */

#include "as_page_shell.h"
#include "as_widgets.h"

#define DISP_SYNC_TIMEOUT_MS 8000

static lv_obj_t  *s_disp_time_txt;
static lv_obj_t  *s_disp_sync_btn;
static lv_timer_t *s_disp_tick;

/* 同步任务正在跑（LVGL 任务写、任务里读；只有一个任务，用 volatile 足够） */
static volatile bool s_disp_syncing;
/* 后台任务的结果：0 = 还没结果，1 = 成功，-1 = 失败；由 lv_async_call 消费 */
static volatile int s_disp_sync_result;

/* ---------------------------------------------------------------- 小部件 */

/*
 * 开/关行：和 as_radio_switch_row 同款外观，但**没有** radio 那套注册表
 * （那是给"开关 + 进子页合并成一行 + 多实例同步"用的，这里只有一个开关）。
 *   点开关图案 → 只切开关；点行里别处 → 也切开关（这页没有子页可进）。
 */
static lv_obj_t *disp_switch_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                                 const char *name, const char *hint, bool on,
                                 lv_event_cb_t cb, lv_obj_t **status_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(36), UI_PX_H(36));
    lv_obj_set_style_radius(icon, UI_PX_H(9), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, UI_PX_H(2), LV_PART_MAIN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *nm = lv_label_create(col);
    lv_obj_set_style_text_font(nm, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(nm, name);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, LV_PCT(100));

    if (hint) {
        lv_obj_t *st = lv_label_create(col);
        lv_obj_set_style_text_font(st, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(st, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(st, hint);
        lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
        lv_obj_set_width(st, LV_PCT(100));
        if (status_out) {
            *status_out = st;
        }
    }

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, UI_PX_W(52), UI_PX_H(28));
    lv_obj_set_style_bg_color(sw, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_COLOR_ACCENT,
                              (lv_style_selector_t)LV_PART_INDICATOR |
                                  (lv_style_selector_t)LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    /* 开关被点会发 CLICKED 并冒泡到行上（行的回调也是"切一下"），
     * 拦掉冒泡免得一次点击切两下 */
    lv_obj_add_event_cb(sw, [](lv_event_t *e) {
        lv_event_stop_bubbling(e);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* 点行的空白处 = 也切开关（页面里没有别的可进） */
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, sw);
    return row;
}

/* ---------------------------------------------------------------- 时间文本 */

static void disp_time_text_update(void)
{
    if (!s_disp_time_txt) {
        return;
    }
    if (!wifi_service_time_ready()) {
        as_label_set_if_changed(s_disp_time_txt,
                                wifi_service_has_ip() ? "等待校时…（点右侧「同步」）"
                                                      : "Wi-Fi 未连接，无法校时");
        return;
    }
    const time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d · %d月%d日",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tmv.tm_mon + 1, tmv.tm_mday);
    as_label_set_if_changed(s_disp_time_txt, buf);
}

/* ---------------------------------------------------------------- 回调 */

static void disp_secs_changed(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    bool on;
    if (lv_obj_check_type(target, &lv_switch_class)) {
        on = lv_obj_has_state(target, LV_STATE_CHECKED);
    } else {
        /* 走的是"点行"这条路：把开关翻过来 */
        lv_obj_t *sw = (lv_obj_t *)lv_event_get_user_data(e);
        if (!sw) {
            return;
        }
        on = !lv_obj_has_state(sw, LV_STATE_CHECKED);
        if (on) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
    }
    lv_event_stop_bubbling(e);

    const esp_err_t err = wifi_service_set_clock_show_seconds(on);
    ui_toast(on ? "时钟显示秒" : "时钟只显示时分", 1500);
    if (err != ESP_OK) {
        /* NVS 写失败：本次开机有效，但重启会还原 —— 值得说一句 */
        ESP_LOGW("app_settings", "clock secs save failed: %s", esp_err_to_name(err));
    }
}

/* 后台任务：阻塞等 SNTP（最长 DISP_SYNC_TIMEOUT_MS） */
static void disp_sync_task(void *arg)
{
    (void)arg;
    const esp_err_t err = wifi_service_time_sync_now(DISP_SYNC_TIMEOUT_MS);
    s_disp_sync_result = (err == ESP_OK) ? 1 : -1;
    s_disp_syncing = false;
    vTaskDelete(NULL);
}

/* lv_async_call 回调：回到 LVGL 任务里弹结果 */
static void disp_sync_done(void *arg)
{
    (void)arg;
    if (s_disp_sync_result > 0) {
        ui_toast("校时成功", 1600);
    } else if (!wifi_service_has_ip()) {
        ui_toast("没连上 Wi-Fi，无法校时", 2000);
    } else {
        ui_toast("校时失败：服务器没响应", 2200);
    }
    s_disp_sync_result = 0;
    if (s_disp_sync_btn) {
        lv_obj_remove_state(s_disp_sync_btn, LV_STATE_DISABLED);
    }
    disp_time_text_update();
}

static void on_disp_sync(lv_event_t *e)
{
    (void)e;
    if (s_disp_syncing) {
        return;                       /* 上一次还没回来，别叠任务 */
    }
    if (!wifi_service_has_ip()) {
        ui_toast("没连上 Wi-Fi，无法校时", 2000);
        return;
    }
    s_disp_syncing = true;
    s_disp_sync_result = 0;
    if (s_disp_sync_btn) {
        lv_obj_add_state(s_disp_sync_btn, LV_STATE_DISABLED);
    }
    ui_toast("正在校时…", 1500);
    /* 2 KB 栈够：sync_wait + 日志，没有大缓冲 */
    if (xTaskCreate(disp_sync_task, "time_sync", 2048, NULL, 3, NULL) != pdPASS) {
        s_disp_syncing = false;
        if (s_disp_sync_btn) {
            lv_obj_remove_state(s_disp_sync_btn, LV_STATE_DISABLED);
        }
        ui_toast("任务创建失败", 1500);
    }
}

static void disp_tick_cb(lv_timer_t *t)
{
    (void)t;
    disp_time_text_update();
    if (!s_disp_syncing && s_disp_sync_result != 0 && s_disp_sync_btn) {
        /* 任务已结束但结果还没被消费（正常情况下 sync_done 会先跑） */
        lv_async_call(disp_sync_done, NULL);
    }
}

static void disp_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_disp_tick) {
        lv_timer_delete(s_disp_tick);
        s_disp_tick = NULL;
    }
    s_disp_time_txt = NULL;
    s_disp_sync_btn = NULL;
}

/* ---------------------------------------------------------------- 页面 */

lv_obj_t *page_display(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "显示设置", NULL, NULL);

    /* ① 时钟显秒 */
    disp_switch_row(body, LV_SYMBOL_EYE_OPEN, 0x6E6E78, "时钟显秒",
                    "首页大时钟与状态栏同步生效", wifi_service_clock_show_seconds(),
                    disp_secs_changed, NULL);

    /* ② 时间与校时 */
    lv_obj_t *row = as_make_row(body, LV_SYMBOL_REFRESH, 0x4A6FA5, "时间与校时",
                                &s_disp_time_txt);
    s_disp_sync_btn = as_make_row_btn(row, "立即同步", on_disp_sync, NULL);
    disp_time_text_update();

    /* ③ 亮度说明（本板确实没有背光器件，别再让人找） */
    lv_obj_t *hint = lv_label_create(body);
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint,
                      "亮度：本板板级配置里没有背光器件（无 LCD_BRIGHTNESS），"
                      "软件无法调亮度，息屏只能整屏停刷新。\n"
                      "时间：开机连上 Wi-Fi 会自动校时；校时失败时点上面的「立即同步」重试。");

    s_disp_tick = lv_timer_create(disp_tick_cb, 1000, NULL);
    lv_obj_add_event_cb(scr, disp_on_delete, LV_EVENT_DELETE, NULL);
    return scr;
}
