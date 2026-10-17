/*
 * ui_home.cc —— 首页：大字号时钟 + 日期 + APP 图标网格
 *
 * 网格用 flex-wrap 自动布局（不写死列数），换方向后能重排；
 * 图标统一用 LVGL 内置 symbol + 纯色圆角块，**不用 emoji**。
 *
 * 两条按需求定下的布局纪律：
 *   1) 网格**左对齐**（LV_FLEX_ALIGN_START，主轴左起）：换行后的最后一行
 *      从最左边开始排，与第一行首列对齐；用 CENTER 的话"设置"这种单独
 *      占一行的图标会漂到屏幕中间，跟上面的音乐图标对不齐。
 *   2) 首页底部**不放浮动迷你播放条**（已按需求移除）。
 *
 * M1 阶段各 APP 的 create() 还是占位页，M2/M3 会替换成真实页面。
 */
#include "ui_core.h"
#include "ui_internal.h"
#include "ui_metrics.h"
#include "ui_app_registry.h"
#include "ui_home_bg.h"

#include <time.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ui_home";

/* ------------------------------------------------------------------ 占位页 */

static lv_obj_t *placeholder_page(const app_desc_t *app)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(app->accent_hex), LV_PART_MAIN);
    lv_label_set_text(title, app->name);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -UI_PX_H(16));

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(hint, "页面开发中 · 左边缘右滑返回");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, UI_PX_H(18));
    return scr;
}

static lv_obj_t *page_music(void)    { return placeholder_page(ui_app_find("music")); }
static lv_obj_t *page_online(void)   { return placeholder_page(ui_app_find("online")); }
static lv_obj_t *page_local(void)    { return placeholder_page(ui_app_find("local")); }
/* 暂时隐藏：日历、蓝牙不在主界面显示
static lv_obj_t *page_calendar(void) { return placeholder_page(ui_app_find("calendar")); }
static lv_obj_t *page_bt(void)       { return placeholder_page(ui_app_find("bluetooth")); }
*/
static lv_obj_t *page_settings(void) { return placeholder_page(ui_app_find("settings")); }

static const app_desc_t k_apps[] = {
    { "music",     "音乐",     LV_SYMBOL_AUDIO,     0xE8973A, page_music,    NULL, NULL, 10, 0 },
    { "online",    "网易云音乐", LV_SYMBOL_WIFI,      0x3E9E86, page_online,   NULL, NULL, 20, 0 },
    { "local",     "文件管理", LV_SYMBOL_DIRECTORY,   0x5B6FB5, page_local,    NULL, NULL, 30, 0 },
    /* 暂时隐藏：日历、蓝牙不在主界面显示
    { "calendar",  "日历",     LV_SYMBOL_LIST,      0xC0584B, page_calendar, NULL, NULL, 40, 0 },
    { "bluetooth", "蓝牙",     LV_SYMBOL_BLUETOOTH, 0x4A6FA5, page_bt,       NULL, NULL, 50, 0 },
    */
    { "settings",  "设置",     LV_SYMBOL_SETTINGS,  0x6E6E78, page_settings, NULL, NULL, 60, 0 },
};

static void register_default_apps(void)
{
    /* 只补位：已经注册过的（如 app_music 提供的真实页面）不覆盖 */
    for (size_t i = 0; i < sizeof(k_apps) / sizeof(k_apps[0]); i++) {
        if (!ui_app_find(k_apps[i].id)) {
            ui_app_register(&k_apps[i]);
        }
    }
}

/* ------------------------------------------------------------------ 点击 */

static void on_app_clicked(lv_event_t *e)
{
    const app_desc_t *app = (const app_desc_t *)lv_event_get_user_data(e);
    if (!app) {
        return;
    }
    if (!app->create) {
        ui_toast("该功能尚未实现", 1400);
        return;
    }
    /* ui_router_push 会**拷贝**描述符本身，所以这里传栈上临时量是安全的 */
    page_desc_t page = { app->id, app->create, app->on_enter, app->on_exit };
    page.no_back = (app->flags & APP_FLAG_NO_BACK) != 0;   /* 自带返回钮的页不叠全局键 */
    ui_router_push(&page);
}

/* ------------------------------------------------------------------ 时钟 */

static lv_obj_t *s_big_clock;
static lv_obj_t *s_date_row;
static lv_timer_t *s_clock_timer;

static const char *weekday_cn(int wday)
{
    static const char *k[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    return (wday >= 0 && wday < 7) ? k[wday] : "";
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    /* 未校时（SNTP 前）显示占位，避免显示 1970 年 */
    char hhmm[8];
    if (tmv.tm_year + 1900 < 2024) {
        snprintf(hhmm, sizeof(hhmm), "--:--");
        if (s_big_clock) lv_label_set_text(s_big_clock, "--:--");
        if (s_date_row)  lv_label_set_text(s_date_row, "等待校时");
    } else {
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        if (s_big_clock) lv_label_set_text_fmt(s_big_clock, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        if (s_date_row) {
            lv_label_set_text_fmt(s_date_row, "%d月%d日 · %s",
                                  tmv.tm_mon + 1, tmv.tm_mday, weekday_cn(tmv.tm_wday));
        }
    }
    ui_status_set_clock(hhmm, NULL);
}

/* ------------------------------------------------------------------ 首页 */

static lv_obj_t *s_home_scr;      /* 当前首页 screen（解码任务要往它身上贴背景图） */

static void home_deleted(lv_event_t *e)
{
    (void)e;
    s_home_scr = NULL;
    /* 首页没了：把解码任务那边的目标也清掉，免得它往已销毁的对象上抹背景图 */
    ui_home_bg_bind(NULL);
    /* 时钟/日期的 label 也跟着 screen 一起没了：不清指针的话下一个 tick 会写到
     * 已释放的对象上（use-after-free）。timer 一并销毁，重建首页时会重新建。 */
    s_big_clock = NULL;
    s_date_row  = NULL;
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }
}

lv_obj_t *ui_home_create(void)
{
    static bool s_registered;
    if (!s_registered) {
        register_default_apps();
        s_registered = true;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 状态栏预留上边距 */
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H, LV_PART_MAIN);

    /* 大字号时钟 */
    s_big_clock = lv_label_create(scr);
    lv_obj_set_style_text_font(s_big_clock, UI_FONT_CLOCK, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_big_clock, lv_color_hex(0xEDE6D8), LV_PART_MAIN);
    lv_label_set_text(s_big_clock, "--:--");
    lv_obj_align(s_big_clock, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + UI_PX_H(6));

    /* 日期 + 星期
     *
     * ★ 必须给**满宽 + 文字居中**，不能只靠下面那句 align_to() 居中 ——
     *   `lv_obj_align_to()` 是**一次性**定位：它内部先
     *   `lv_obj_set_style_align(obj, LV_ALIGN_TOP_LEFT)` 再 `lv_obj_set_pos(x, y)`，
     *   x/y 只按**调用那一刻**的宽高算一次，之后文字再变长就只会往**右**长
     *   （label 默认宽度是"内容宽"）——于是"等待校时"换成"10月8日 · 星期三"之后
     *   整行看起来就偏右了（2026-10 用户报）。
     *   给满宽 + LV_TEXT_ALIGN_CENTER 后，位置只跟屏幕有关，与文字长短无关。
     *   （align_to 内部会自己 update_layout，所以 LV_PCT 宽度在这里已经生效。）
     */
    s_date_row = lv_label_create(scr);
    lv_obj_set_style_text_font(s_date_row, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date_row, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_date_row, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_date_row, LV_PCT(100));
    lv_label_set_text(s_date_row, "等待校时");
    lv_obj_align_to(s_date_row, s_big_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX_H(4));

    /* APP 图标网格：flex-wrap 自动换行、左对齐（最后一行与首列对齐） */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, UI_W - UI_PAGE_PAD * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(grid, UI_PX_W(24), LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, UI_PX_H(16), LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    /* 主轴（行方向）左对齐：换行后最后一行与第一行首列对齐（见文件头第 1 条） */
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align_to(grid, s_date_row, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX_H(22));
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    size_t n = ui_app_count(true);
    for (size_t i = 0; i < n; i++) {
        const app_desc_t *app = ui_app_at(i, true);
        if (!app) {
            break;
        }

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, UI_HOME_CELL_W, UI_HOME_CELL_H);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, UI_PX_H(8), LV_PART_MAIN);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, on_app_clicked, LV_EVENT_CLICKED, (void *)app);

        /* 图标块 */
        lv_obj_t *icon = lv_obj_create(cell);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, UI_HOME_ICON, UI_HOME_ICON);
        lv_obj_set_style_radius(icon, UI_PX_H(22), LV_PART_MAIN);
        lv_obj_set_style_bg_color(icon, lv_color_hex(app->accent_hex), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

        if (app->img) {
            /* 位图图标（如网易云的 logo）：铺满图标块。
             * 图像由 gen_icon.py 按 UI_HOME_ICON 尺寸烘好，这里不做缩放 ——
             * 运行期缩放是每帧都要重算的，图标这种固定尺寸没理由付那个代价。 */
            lv_obj_t *im = lv_image_create(icon);
            lv_image_set_src(im, app->img);
            lv_obj_center(im);
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *sym = lv_label_create(icon);
            lv_obj_set_style_text_font(sym, UI_FONT_CLOCK, LV_PART_MAIN);
            lv_obj_set_style_text_color(sym, lv_color_hex(0x14141A), LV_PART_MAIN);
            lv_label_set_text(sym, app->symbol);
            lv_obj_center(sym);
        }

        /* 名称 */
        lv_obj_t *name = lv_label_create(cell);
        lv_obj_set_style_text_font(name, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, UI_COLOR_FG, LV_PART_MAIN);
        lv_label_set_text(name, app->name);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(name, LV_PCT(100));
    }

    /* 1 s 时钟刷新（只在首页存活期间跑） */
    if (!s_clock_timer) {
        s_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    }
    clock_timer_cb(NULL);

    /*
     * 背景图（assets/background.jpg）：图已解好就立刻贴上（它是 screen 自己的
     * bg_image，不占子对象、也不会挡住时钟与图标网格）；还没解好则由解码任务
     * 完成后补上。首页被销毁时 home_deleted() 会把目标清掉。
     */
    s_home_scr = scr;
    lv_obj_add_event_cb(scr, home_deleted, LV_EVENT_DELETE, NULL);
    ui_home_bg_bind(scr);

    ESP_LOGI(TAG, "home created (%u apps)", (unsigned)n);
    return scr;
}
