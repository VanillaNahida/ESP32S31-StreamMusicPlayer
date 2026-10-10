/*
 * as_page_storage.cc —— 存储二级页（容量卡 / 占用条 / 卡的物理信息 / 热插拔重画）
 *
 * 原 app_settings.cc：
 *   - 存储详情页的说明注释（1381–1397）+ STORAGE_SD_POLL_MS
 *   - s_storage_body / s_storage_timer / s_storage_sd_gen（1399–1401）
 *   - storage_fmt_size_kb（1404–1418）→ 移到 as_fmt.cc，改名 as_fmt_size_kb
 *     （**纯逻辑**，与 as_fmt_bytes 放在一起，可 host 单测）
 *   - storage_add_info / storage_add_hint / storage_add_gauge / storage_rebuild /
 *     storage_on_rescan / storage_sd_tick / storage_on_delete / page_storage
 *     （1421–1625）**逐字搬迁**
 */
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "ui_core.h"
#include "ui_metrics.h"

#include "bsp.h"
#include "storage_service.h"   /* SD 热插拔：状态代次 + 重新检测入口 */

#include "as_fmt.h"
#include "as_page_shell.h"
#include "as_widgets.h"

static const char *TAG = "app_settings";

/*
 * 存储详情页（用户需求："存储需要设置为可点开，点击后显示 SD 卡的全部详细信息，
 * 包括总容量、可用容量，并绘制一个存储状态占用条"）。
 *
 * 构成：
 *   - 状态行：已挂载 / 未检测到；挂载点；文件系统
 *   - **容量卡**：`已用 x / 共 y` + **占用条** + `可用 z`（lv_bar；用了 90% 以上
 *     换成红色提醒快满）
 *   - 明细行：总容量 / 已用 / 可用 / 卡类型 / 数据线宽度 / 工作频率 / 卡容量 /
 *     卡片名称 / 序列号
 *   - 右上角"重新检测"：热插拔监控任务每 1.5 秒已经在试了，这个按钮是"立刻再试
 *     一次"的手动入口（storage_service_sd_rescan()，与监控任务共用一把锁）
 *
 * 页面进来读一次，之后由 storage_sd_tick 盯着 SD 状态代次：插拔卡（或换卡）
 * 会自动重画，不用退出去重进。
 */
#define STORAGE_SD_POLL_MS 500

static lv_obj_t *s_storage_body;
static lv_timer_t *s_storage_timer;
static uint32_t  s_storage_sd_gen;

/* 一行"图标 + 名称 + 右侧值"：直接复用 as_make_row / make_row（它自带右对齐的 value label） */
static void storage_add_info(const char *sym, uint32_t accent, const char *name, const char *value)
{
    lv_obj_t *val = NULL;
    as_make_row(s_storage_body, sym, accent, name, &val);
    if (val) {
        lv_label_set_text(val, value);
    }
}

/* 一句提示（未插卡 / 读容量失败时用），占满行的宽度、可换行 */
static void storage_add_hint(const char *text)
{
    lv_obj_t *h = lv_label_create(s_storage_body);
    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_text(h, text);
}

/* 容量卡：占用条 + 已用/总数 + 可用 */
static void storage_add_gauge(uint32_t total_kb, uint32_t free_kb)
{
    const uint32_t used_kb = (total_kb > free_kb) ? (total_kb - free_kb) : 0;
    const uint32_t pct     = total_kb ? (uint32_t)(((uint64_t)used_kb * 100u) / total_kb) : 0;
    /* ⚠ LVGL 9 的 lv_color_t 是**结构体**，不能用 uint32_t 存（老代码里 lv_color_hex()
     *   返回的就是 lv_color_t）—— 类型写 lv_color_t，后面直接传，别再套一次 lv_color_hex() */
    const lv_color_t tint = (pct >= 90u) ? lv_color_hex(0xB5504E) : UI_COLOR_ACCENT;

    lv_obj_t *card = lv_obj_create(s_storage_body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, UI_PX_H(12), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, UI_PX_H(14), LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 第一行：左边"已用 x / 共 y"，右边大号百分比。
     * ⚠ 高度给常量、不要 LV_SIZE_CONTENT：这一行里的两个 label 是 lv_obj_align()
     *   按父容器尺寸定位的，而 LV_SIZE_CONTENT 的高度又要反过来由子对象算 ——
     *   这种"互相依赖"在 LVGL 里会算出 0 高（经典坑），直接给一个行高最省事。 */    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_height(line, UI_PX_H(32));
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    char used[24];
    char total[24];
    char buf[72];
    as_fmt_size_kb(used_kb, used, sizeof(used));
    as_fmt_size_kb(total_kb, total, sizeof(total));
    snprintf(buf, sizeof(buf), "已用 %.16s / 共 %.16s", used, total);

    lv_obj_t *used_l = lv_label_create(line);
    lv_obj_set_style_text_font(used_l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(used_l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(used_l, buf);
    lv_obj_align(used_l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *pct_l = lv_label_create(line);
    lv_obj_set_style_text_font(pct_l, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(pct_l, tint, LV_PART_MAIN);
    lv_label_set_text_fmt(pct_l, "%u%%", (unsigned)pct);
    lv_obj_align(pct_l, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 占用条：背景是"底槽"（PART_MAIN），填充是"已用"（PART_INDICATOR） */
    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, LV_PCT(100), UI_PX_H(16));
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, (int32_t)pct, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, UI_PX_H(8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, tint, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);

    char freebuf[24];
    as_fmt_size_kb(free_kb, freebuf, sizeof(freebuf));
    lv_obj_t *free_l = lv_label_create(card);
    lv_obj_set_style_text_font(free_l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(free_l, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text_fmt(free_l, "可用 %.20s", freebuf);
}

static void storage_rebuild(void)
{
    if (!s_storage_body) {
        return;
    }
    lv_obj_clean(s_storage_body);      /* 旧的容量卡/行都在这里被删掉 */

    if (!storage_service_sd_is_mounted()) {
        storage_add_info(LV_SYMBOL_SD_CARD, 0x5B6FB5, "SD 卡", "未检测到");
        storage_add_hint("没插卡，或者卡接触不良。\n插好卡后点右上角「重新检测」。");
        return;
    }

    uint32_t total_kb = 0, free_kb = 0;
    if (bsp_sdcard_capacity_kb(&total_kb, &free_kb) != ESP_OK || total_kb == 0) {
        storage_add_info(LV_SYMBOL_SD_CARD, 0x5B6FB5, "SD 卡", "已挂载");
        storage_add_hint("读文件系统容量失败（卡可能已被拔出）。点右上角「重新检测」再试。");
        return;
    }

    char buf[40];
    storage_add_info(LV_SYMBOL_SD_CARD, 0x3E9E86, "状态", "已挂载");
    storage_add_info(LV_SYMBOL_DIRECTORY, 0x5B6FB5, "挂载点", bsp_sdcard_mount_point());
    storage_add_info(LV_SYMBOL_FILE, 0x6E6E78, "文件系统", "FATFS");

    storage_add_gauge(total_kb, free_kb);

    as_fmt_size_kb(total_kb, buf, sizeof(buf));
    storage_add_info(LV_SYMBOL_DRIVE, 0x3E9E86, "总容量", buf);
    as_fmt_size_kb(total_kb - free_kb, buf, sizeof(buf));
    storage_add_info(LV_SYMBOL_BARS, 0xE8973A, "已用容量", buf);
    as_fmt_size_kb(free_kb, buf, sizeof(buf));
    storage_add_info(LV_SYMBOL_SAVE, 0x5B6FB5, "可用容量", buf);

    /* 卡的物理信息（BSP 里照抄 IDF 的解码口径，取不到就少显示几行） */
    bsp_sd_card_info_t ci;
    if (bsp_sdcard_card_info(&ci) == ESP_OK) {
        if (ci.type[0]) {
            storage_add_info(LV_SYMBOL_SD_CARD, 0x6E6E78, "卡类型", ci.type);
        }
        if (ci.bus_width > 0) {
            snprintf(buf, sizeof(buf), "%d-bit%s", ci.bus_width, ci.ddr ? " DDR" : "");
            storage_add_info(LV_SYMBOL_LIST, 0x6E6E78, "数据线宽度", buf);
        }
        if (ci.freq_khz > 0) {
            snprintf(buf, sizeof(buf), "%d.%d MHz", ci.freq_khz / 1000,
                     (ci.freq_khz % 1000) / 100);
            storage_add_info(LV_SYMBOL_LOOP, 0x6E6E78, "工作频率", buf);
        }
        if (ci.card_bytes > 0) {
            as_fmt_size_kb((uint32_t)(ci.card_bytes / 1024u), buf, sizeof(buf));
            storage_add_info(LV_SYMBOL_DRIVE, 0x6E6E78, "卡容量（CSD）", buf);
        }
        if (ci.name[0]) {
            storage_add_info(LV_SYMBOL_EDIT, 0x6E6E78, "卡片名称", ci.name);
        }
        if (ci.serial) {
            snprintf(buf, sizeof(buf), "0x%08X", (unsigned)ci.serial);
            storage_add_info(LV_SYMBOL_BARS, 0x6E6E78, "序列号", buf);
        }
    }
}

static void storage_on_rescan(lv_event_t *e)
{
    (void)e;
    /*
     * 走 storage_service 的入口而不是直接 bsp_sdcard_mount()：那边和热插拔
     * 监控任务共用一把锁，不会出现"按钮和轮询同时去 init 同一个 SDMMC 设备"。
     * 已挂载时它什么都不做，返回 true。
     */
    const bool mounted = storage_service_sd_rescan();
    s_storage_sd_gen = storage_service_sd_generation();
    storage_rebuild();
    ui_toast(mounted ? "已检测到 SD 卡" : "还是没检测到 SD 卡", 1800);
}

/*
 * SD 热插拔：插拔之后这张卡的信息要跟着变，不用退出去重进。
 * 状态栏会弹 Toast，这边只负责把这一页重画。
 */
static void storage_sd_tick(lv_timer_t *t)
{
    (void)t;
    const uint32_t gen = storage_service_sd_generation();
    if (gen == s_storage_sd_gen) {
        return;
    }
    s_storage_sd_gen = gen;
    storage_rebuild();
}

static void storage_on_delete(lv_event_t *e)
{
    (void)e;
    s_storage_body = NULL;
    if (s_storage_timer) {
        lv_timer_delete(s_storage_timer);
        s_storage_timer = NULL;
    }
}

lv_obj_t *page_storage(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "存储", "重新检测", storage_on_rescan);
    s_storage_body = body;
    s_storage_sd_gen = storage_service_sd_generation();
    if (!s_storage_timer) {
        s_storage_timer = lv_timer_create(storage_sd_tick, STORAGE_SD_POLL_MS, NULL);
    }
    storage_rebuild();
    lv_obj_add_event_cb(scr, storage_on_delete, LV_EVENT_DELETE, NULL);
    ESP_LOGI(TAG, "storage page created (mounted=%d)", (int)storage_service_sd_is_mounted());
    return scr;
}
