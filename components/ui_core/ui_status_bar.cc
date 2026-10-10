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

#include <stdio.h>

#include "esp_log.h"

#include "storage_service.h"   /* SD 热插拔：代次 + 挂载状态 */
#include "bsp.h"               /* bsp_sdcard_capacity_kb（插入时报个容量） */

static const char *TAG = "ui_status";

#define SD_POLL_MS 500         /* SD 热插拔检查间隔（只是一个整数比较） */

static lv_obj_t *s_clock;
static lv_obj_t *s_wifi;
static lv_obj_t *s_bt;
static lv_obj_t *s_out;
static lv_obj_t *s_sd;
static lv_timer_t *s_sd_timer;
static uint32_t  s_sd_gen_seen;
static bool      s_sd_mounted_seen;

static void sd_poll_cb(lv_timer_t *t);

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

    /*
     * SD 卡状态由这里统一盯着（状态栏是常驻的，不管当前在哪个页面）。
     *
     * 板子没有卡检测脚，storage_service 里有个低优先级任务在轮询"卡在不在"，
     * 每次插入/拔出把代号次 +1。这里只做三件事：更新图标、弹一句 Toast、
     * 让正在看存储页/本地列表的页面自己去刷新（它们各自也记着上次的代次）。
     *
     * 为什么不在业务页面里做：状态栏、本地音乐页、设置存储页三处都要看这个
     * 状态，谁都不是"拥有者"；放在常驻的状态栏上，插入/拔出在任何页面都能
     * 看见反馈。500 ms 的轮询只是一个整数比较，开销可以忽略。
     */
    s_sd_gen_seen = storage_service_sd_generation();
    s_sd_mounted_seen = storage_service_sd_is_mounted();
    ui_status_set_sd(s_sd_mounted_seen);
    s_sd_timer = lv_timer_create(sd_poll_cb, SD_POLL_MS, NULL);

    ESP_LOGD(TAG, "status bar created h=%d", (int)UI_STATUSBAR_H);
    return bar;
}

/* SD 热插拔轮询（见 ui_status_bar_create 里的说明） */
static void sd_poll_cb(lv_timer_t *t)
{
    (void)t;
    const uint32_t gen = storage_service_sd_generation();
    if (gen == s_sd_gen_seen) {
        return;
    }
    s_sd_gen_seen = gen;

    const bool mounted = storage_service_sd_is_mounted();
    if (mounted == s_sd_mounted_seen) {
        return;
    }
    s_sd_mounted_seen = mounted;

    ui_status_set_sd(mounted);
    if (mounted) {
        uint32_t total_kb = 0;
        if (bsp_sdcard_capacity_kb(&total_kb, NULL) == ESP_OK && total_kb > 0) {
            char buf[48];
            /* 容量按 GB/MB 给，和设置页口径一致（都从 KB 换算） */
            if (total_kb >= 1024u * 1024u) {
                snprintf(buf, sizeof(buf), "已插入 SD 卡（%.1f GB）",
                         (double)total_kb / (1024.0 * 1024.0));
            } else {
                snprintf(buf, sizeof(buf), "已插入 SD 卡（%u MB）", (unsigned)(total_kb / 1024u));
            }
            ui_toast(buf, 2000);
        } else {
            ui_toast("已插入 SD 卡", 2000);
        }
    } else {
        ui_toast("SD 卡已拔出", 2000);
    }
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