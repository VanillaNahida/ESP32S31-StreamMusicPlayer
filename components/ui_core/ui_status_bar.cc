/*
 * ui_status_bar.cc —— 常驻状态栏（高 32px）
 *
 * 左：时钟；右：Wi-Fi 信号 → 蓝牙 → 输出通道 → microSD。
 * **不画电池**（本板无电池、无电量计）。
 * 时钟与日期使用等宽数字字体（开发指南 5.5），M1 暂用内置字体，M5 换文泉驿等宽微米黑。
 */
#include "ui_core.h"
#include "ui_internal.h"
#include "ui_metrics.h"

#include "esp_log.h"

static const char *TAG = "ui_status";

static lv_obj_t *s_clock;
static lv_obj_t *s_wifi;
static lv_obj_t *s_bt;
static lv_obj_t *s_out;
static lv_obj_t *s_sd;

static lv_obj_t *make_icon(lv_obj_t *parent, const char *symbol)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(l, symbol);
    return l;
}

lv_obj_t *ui_status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_W, UI_STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x08080C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：时钟 */
    s_clock = lv_label_create(bar);
    lv_obj_set_style_text_font(s_clock, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);

    /* 右：SD → 输出 → 蓝牙 → Wi-Fi（对齐时按从右到左依次排） */
    s_wifi = make_icon(bar, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, 0, 0);

    s_bt = make_icon(bar, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_bt, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_align_to(s_bt, s_wifi, LV_ALIGN_OUT_LEFT_MID, -UI_PX_W(10), 0);

    s_out = make_icon(bar, LV_SYMBOL_AUDIO);
    lv_obj_align_to(s_out, s_bt, LV_ALIGN_OUT_LEFT_MID, -UI_PX_W(10), 0);

    s_sd = make_icon(bar, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(s_sd, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_align_to(s_sd, s_out, LV_ALIGN_OUT_LEFT_MID, -UI_PX_W(10), 0);

    ESP_LOGD(TAG, "status bar created h=%d", (int)UI_STATUSBAR_H);
    return bar;
}

void ui_status_set_clock(const char *hhmm, const char *date_text)
{
    (void)date_text;   /* 日期在首页大字下方显示 */
    if (s_clock && hhmm) {
        lv_label_set_text(s_clock, hhmm);
    }
}

void ui_status_set_wifi(int level, bool connected)
{
    if (!s_wifi) {
        return;
    }
    if (!connected) {
        lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(s_wifi, UI_COLOR_FG_DIM, LV_PART_MAIN);
        return;
    }
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    lv_label_set_text_fmt(s_wifi, "%s%d", LV_SYMBOL_WIFI, level);
    lv_obj_set_style_text_color(s_wifi, UI_COLOR_FG, LV_PART_MAIN);
}

void ui_status_set_bt(bool connected, const char *device_name)
{
    (void)device_name;
    if (!s_bt) {
        return;
    }
    lv_obj_set_style_text_color(s_bt, connected ? UI_COLOR_ACCENT : UI_COLOR_FG_DIM, LV_PART_MAIN);
}

void ui_status_set_output(const char *channel_name)
{
    (void)channel_name;
    if (!s_out) {
        return;
    }
    lv_obj_set_style_text_color(s_out, UI_COLOR_FG, LV_PART_MAIN);
}

void ui_status_set_sd(bool mounted)
{
    if (!s_sd) {
        return;
    }
    lv_obj_set_style_text_color(s_sd, mounted ? UI_COLOR_FG : UI_COLOR_FG_DIM, LV_PART_MAIN);
}