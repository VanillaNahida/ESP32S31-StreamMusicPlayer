/*
 * ui_core.cc —— UI 框架层：top layer、Toast、息屏、初始化
 *
 * 层级（自下而上）：当前页面 screen → 上一层 screen（返回手势视差背景）
 *                   → lv_layer_top()（状态栏 / 控制中心 / 抽屉 / Toast / 手势）
 */
#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_internal.h"

#include "esp_log.h"
#include "esp_lv_adapter.h"

static const char *TAG = "ui_core";

static lv_obj_t *s_toast;
static lv_obj_t *s_toast_label;
static lv_timer_t *s_toast_timer;
static bool s_screen_off;

/* ------------------------------------------------------------------ Toast */

/* ------------------------------------------------------------------ 弹层动画
 *
 * 统一的"渐入渐出"：遮罩只淡入淡出，面板额外做一点上滑（起点在下面 10 px），
 * 时间 160 ms 进 / 140 ms 出，ease_out。数值刻意偏快 —— 这是交互反馈，不是过场，
 * 太慢会显得"卡"（LVGL 是软渲染，每帧都在重绘浮层这块区域）。
 *
 * 用 lv_obj_set_style_opa 而不是 bg_opa：前者是**整个对象及其子树**的透明度，
 * 一次调用就能把面板里的图标/文字一起淡出，不用逐个改子对象。
 */
#define POPUP_IN_MS    160
#define POPUP_OUT_MS   140
#define POPUP_SLIDE_PX UI_PX_H(10)

/* 只改透明度（遮罩用：整屏滑动看起来会晃） */
static void popup_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)(v < 0 ? 0 : (v > 255 ? 255 : v)),
                         LV_PART_MAIN);
}

/* 透明度 + 上滑（面板/Toast 用）：v 从 0→255 是"进入"，255→0 是"退出" */
static void popup_opa_slide_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    if (v < 0) {
        v = 0;
    } else if (v > 255) {
        v = 255;
    }
    lv_obj_set_style_opa(o, (lv_opa_t)v, LV_PART_MAIN);
    /* 透明时下沉 POPUP_SLIDE_PX，不透明时归位（显式转 int：UI_PX_H 可能是浮点表达式） */
    lv_obj_set_style_translate_y(o, (int32_t)((255 - v) * POPUP_SLIDE_PX / 255), LV_PART_MAIN);
}

static void popup_anim(lv_obj_t *obj, int32_t from, int32_t to, uint32_t ms, bool slide)
{
    if (!obj) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, slide ? popup_opa_slide_cb : popup_opa_cb);
    lv_anim_start(&a);
}

void ui_popup_in(lv_obj_t *scrim, lv_obj_t *panel)
{
    if (scrim) {
        popup_anim(scrim, LV_OPA_TRANSP, LV_OPA_COVER, POPUP_IN_MS, false);
    }
    popup_anim(panel, LV_OPA_TRANSP, LV_OPA_COVER, POPUP_IN_MS, true);
}

/* 淡出结束 → 删掉整块浮层（动画回调里删，所以用 async 版本） */
static void popup_out_done_cb(lv_anim_t *a)
{
    lv_obj_t *o = (lv_obj_t *)a->var;
    if (o) {
        lv_obj_delete_async(o);
    }
}

void ui_popup_out(lv_obj_t *scrim, lv_obj_t *panel)
{
    lv_obj_t *victim = scrim ? scrim : panel;      /* 删遮罩会连带删掉面板 */
    if (!victim) {
        return;
    }
    if (scrim) {
        popup_anim(scrim, LV_OPA_COVER, LV_OPA_TRANSP, POPUP_OUT_MS, false);
    }
    if (panel) {
        popup_anim(panel, LV_OPA_COVER, LV_OPA_TRANSP, POPUP_OUT_MS, true);
    }

    /* 用 victim 上的那条动画作为"收尾"：它跑完就删（透明度那条也差不多同时结束） */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, victim);
    lv_anim_set_values(&a, 0, 0);                  /* 不驱动任何属性，只当计时器 */
    lv_anim_set_duration(&a, POPUP_OUT_MS);
    lv_anim_set_exec_cb(&a, popup_opa_cb);
    lv_anim_set_ready_cb(&a, popup_out_done_cb);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------ Toast */

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    s_toast_timer = NULL;
    /* 淡出（不删对象：Toast 是常驻的，只是藏起来） */
    if (s_toast) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_toast);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, POPUP_OUT_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, popup_opa_slide_cb);
        lv_anim_set_ready_cb(&a, [](lv_anim_t *an) {
            if (an->var) {
                lv_obj_add_flag((lv_obj_t *)an->var, LV_OBJ_FLAG_HIDDEN);
            }
        });
        lv_anim_start(&a);
    }
}

void ui_toast(const char *text, uint32_t duration_ms)
{
    if (!s_toast || !text) {
        return;
    }
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    lv_label_set_text(s_toast_label, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    /* 渐入：连着来几个 Toast 也不会"跳"出来 */
    popup_anim(s_toast, LV_OPA_TRANSP, LV_OPA_COVER, POPUP_IN_MS, true);

    if (duration_ms == 0) {
        duration_ms = 1800;
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

lv_obj_t *ui_toast_create(lv_obj_t *parent)
{
    s_toast = lv_obj_create(parent);
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toast, UI_PX_H(18), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_toast, UI_PX_W(18), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_toast, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(90));
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);

    s_toast_label = lv_label_create(s_toast);
    lv_obj_set_style_text_font(s_toast_label, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_toast_label, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(s_toast_label, "");
    lv_obj_center(s_toast_label);
    return s_toast;
}

/* ------------------------------------------------------------------ 息屏 */

void ui_screen_off(void)
{
    if (s_screen_off) {
        return;
    }
    /* 息屏前先收掉音量条：息屏期间 LVGL 不再跑，弹簧与自动缩回定时器都停了，
     * 亮屏时会看到一根"卡在那儿"的音量条 */
    ui_volume_hide();
    /* 本板无软件可控背光：息屏 = 停 LVGL 刷新，音频继续 */
    esp_lv_adapter_pause(-1);
    s_screen_off = true;
    ESP_LOGI(TAG, "screen off (lvgl paused)");
}

void ui_screen_on(void)
{
    if (!s_screen_off) {
        return;
    }
    esp_lv_adapter_resume();
    s_screen_off = false;
    ESP_LOGI(TAG, "screen on (lvgl resumed)");
}

bool ui_screen_is_off(void)
{
    return s_screen_off;
}

/* ------------------------------------------------------------------ 控制中心 / 抽屉 状态转发 */

bool ui_cc_is_open(void)     { return ui_cc_state(); }
bool ui_drawer_is_open(void) { return ui_drawer_state(); }

/*
 * 控制中心/抽屉一展开就把音量条收掉：音量条也在 lv_layer_top 上、且位置正好在
 * 屏幕右中，压着控制中心的面板与右侧抽屉。这里属于"焦点被拿走"，立刻缩回，
 * 不要等 2.5 秒的自动缩回。
 */
void ui_cc_open(void)   { ui_volume_hide(); ui_cc_apply_open(true, true); }
void ui_cc_close(void)  { ui_cc_apply_open(false, true); }
void ui_cc_toggle(void) { ui_cc_apply_open(!ui_cc_state(), true); }

void ui_drawer_open(void)  { ui_volume_hide(); ui_drawer_apply_open(true, true); }
void ui_drawer_close(void) { ui_drawer_apply_open(false, true); }

/* ------------------------------------------------------------------ 页面返回键 */

/*
 * 全局返回键：建在 lv_layer_top 上的一个圆钮，由 ui_router 在
 * push / pop / init_home 之后调用 ui_back_button_update() 刷新显示状态。
 *
 * 显示条件（ui_router_top_needs_back）：栈深 > 1 且当前页 desc.no_back == false
 * —— 首页与"自带返回钮"的设置页不显示，其余二级页（播放器 / 在线音乐 /
 * 占位页…）自动就有返回键，页面里不用再画。
 *
 * z 序：本对象在 ui_core_init 里**最先**创建，排在它后面的控制中心 /
 * 状态栏 / 抽屉 / Toast / 手势热区都会盖在它上面 —— 模态遮罩（app 运行时
 * 新建在 layer_top 上的）也一样。于是：
 *   - 控制中心 / 抽屉展开、搜索遮罩打开时返回键被盖住、点不到（符合预期）；
 *   - 它们关闭后按钮自动恢复可见，无需额外代码。
 */
static lv_obj_t *s_back_btn;

static void back_button_clicked(lv_event_t *e)
{
    (void)e;
    ui_router_pop();
}

lv_obj_t *ui_back_button_global_create(lv_obj_t *parent)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BACK_BTN_SIZE, UI_BACK_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, back_button_clicked, LV_EVENT_CLICKED, NULL);
    /*
     * 位置：与页面内容区第一行对齐 —— 页面 pad_top 是
     * UI_STATUSBAR_H + UI_PAGE_PAD，标题行就落在那里；
     * x 用 UI_BACK_BTN_X 让开左边缘手势热区（同手动返回键）。
     */
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, UI_BACK_BTN_X, UI_STATUSBAR_H + UI_PAGE_PAD);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_center(l);

    s_back_btn = btn;
    ui_back_button_update();
    return btn;
}

void ui_back_button_update(void)
{
    if (!s_back_btn) {
        return;
    }
    const bool need = ui_router_top_needs_back();
    if (need) {
        lv_obj_remove_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *ui_back_button_create(lv_obj_t *parent, lv_align_t align)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BACK_BTN_SIZE, UI_BACK_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, back_button_clicked, LV_EVENT_CLICKED, NULL);
    /* UI_BACK_BTN_X 让开了左侧手势热区，贴 x=0 的话左半截点不到（见头文件） */
    lv_obj_align(btn, align, UI_BACK_BTN_X, 0);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_center(l);
    return btn;
}

/* ------------------------------------------------------------------ 主题 */

/*
 * LVGL 的默认主题是**浅色**：凡是没有显式设色的派生控件
 * （lv_obj_create / lv_button_create / lv_bar_create / lv_list ...）都会拿到
 * 浅底深字，页面看着就不够"黑"。这里统一初始化为深色主题，
 * 页面里显式设过的颜色不受影响。
 * 字体一并指定为 CJK 字体，避免默认控件里的中文变方框。
 */
static void ui_theme_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }
    lv_theme_t *th = lv_theme_default_init(disp,
                                           UI_COLOR_ACCENT,        /* primary */
                                           UI_COLOR_ACCENT_ALT,    /* secondary */
                                           true,                   /* dark = true */
                                           UI_FONT_TEXT);
    lv_display_set_theme(disp, th);
}

/* ------------------------------------------------------------------ 初始化 */

esp_err_t ui_core_init(void)
{
    /* ★ 第一件事：把字体 fallback 链建起来（中文主字体 → 常用日文 → 生僻日文）。
     * UI_FONT_TEXT/TITLE/SMALL 指向的 ui_font_text 在 ui_font_init() 之前是全零，
     * 任何标签都必须在它之后再创建 —— 所以放在主题与控件之前（见 ui_font.h）。 */
    ui_font_init();

    ui_theme_init();

    lv_obj_t *top = lv_layer_top();
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    ui_metrics_selfcheck();

    /* 全局层：返回键最先建（z 序最底，见 ui_back_button_global_create 的说明）：
     * 控制中心 / 抽屉 / Toast / 模态遮罩展开时都能盖住它 */
    ui_back_button_global_create(top);
    ui_cc_create(top);           /* 控制中心（默认隐藏） */
    lv_obj_t *sb = ui_status_bar_create(top);
    ui_toast_create(top);

    /* 手势：顶部下拉控制中心、左右边缘滑动 */
    ui_gesture_attach(sb);

    /* 音量悬浮条：放在手势热区**之后**创建，这样它显示时压在右边缘热区之上，
     * 拖音量不会误触功能抽屉；隐藏时整块在屏幕外，热区照常工作。 */
    ui_volume_panel_create(top);

    /* 首页 */
    esp_err_t ret = ui_router_init_home();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "home create failed");
        return ret;
    }

    ui_toast("系统就绪", 1200);
    ESP_LOGI(TAG, "ui_core ready");
    return ESP_OK;
}