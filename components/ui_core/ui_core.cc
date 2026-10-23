/*
 * ui_core.cc —— UI 框架层：top layer、Toast、息屏、初始化
 *
 * 层级（自下而上）：当前页面 screen → 上一层 screen（返回手势视差背景）
 *                   → lv_layer_top()（状态栏 / 控制中心 / Toast / 手势）
 */
#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_internal.h"
#include "ui_home_bg.h"
#include "ui_icons.h"      /* back_arrow：标题栏返回钮的位图 */
#include "ui_ba.h"         /* ui_ba_press_feedback：按下反馈（与弹窗按钮同一套） */

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
 * 照搬 assets/blue_archive_ui_replica.html 的那两条关键帧：
 *
 *   进场 slideInUp  0.38s cubic-bezier(0.16, 1, 0.3, 1)
 *       0%   translateY(100vh) scale(0.95) opacity 0
 *       70%  translateY(-8px)               opacity 1     ← 冲过头再回弹
 *       100% translateY(0)     scale(1)     opacity 1
 *
 *   退场 slideOutDown 0.3s cubic-bezier(0.7, 0, 0.84, 0)
 *       100% translateY(100vh) scale(0.92) opacity 0
 *
 * 三点移植上的取舍（都在 ESP32-S31 软渲染的前提下）：
 *
 * 1. **不做 scale**。LVGL 的缩放/旋转要先把整棵子树渲染进 ARGB8888 图层再变换，
 *    一张 480x300 的卡片就是 ~560 KB 图层缓冲、而且动画期间**逐帧**申请释放。
 *    这里只用 translate_y + opa（二者都是纯样式，不建图层），观感差别很小。
 * 2. **"100vh" 换成固定像素**。整屏高的位移在 480 px 的屏上会让浮层从屏幕外
 *    飞进来，配合 300 ms 的时长显得拖沓；取 UI_PX_H(46) 起步更有"弹"的感觉。
 * 3. 时长 380/300 ms 收到 300/240 ms：这是触摸反馈不是过场，太慢会被当成卡顿。
 *
 * 缓动：cubic-bezier(0.16,1,0.3,1) ≈ 先快后极慢（出四分之三路程只用 1/4 时间），
 * 用 1-(1-t)^4 很接近；退场那条 bezier 是"慢起快走"，用 t^3 近似。
 * 回弹靠"70% 处过冲到 -6 px"这段分段函数实现。
 *
 * 用 lv_obj_set_style_opa 而不是 bg_opa：前者是**整个对象及其子树**的透明度，
 * 一次调用就能把面板里的图标/文字一起淡出，不用逐个改子对象。
 */
#define POPUP_IN_MS     300
#define POPUP_OUT_MS    240
#define POPUP_SLIDE_PX  UI_PX_H(46)   /* 进场起点：卡片在目标位置下方这么远 */
#define POPUP_OVER_PX   UI_PX_H(6)    /* 70% 处冲过头的距离 */
#define BA_ANIM_RANGE   1500          /* 动画进度量程（0..1500） */

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 1-(1-t)^4：进场用（t 为 0..1000） */
static int32_t ease_out_quart(int32_t t)
{
    const int32_t u = 1000 - t;
    /* u^4 / 1000^3，保持整数运算 */
    int64_t p = (int64_t)u * u / 1000;
    p = p * u / 1000;
    p = p * u / 1000;
    return 1000 - (int32_t)p;
}

/* t^3：退场用 */
static int32_t ease_in_cubic(int32_t t)
{
    int64_t p = (int64_t)t * t / 1000;
    p = p * t / 1000;
    return (int32_t)p;
}

/* 只改透明度（遮罩用：整屏滑动看起来会晃） */
static void popup_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)clamp_i32(v, 0, 255), LV_PART_MAIN);
}

/*
 * 进场：进度 0..1000
 *   位移  0~700 段：从 +SLIDE 收到 -OVER（用 ease_out_quart）
 *         700~1000 段：从 -OVER 回落到 0（线性，很短）
 *   透明度 0~120 段：0 → 255（CSS 的 12% 处就到 1.0），之后保持
 */
static void popup_in_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    v = clamp_i32(v, 0, BA_ANIM_RANGE);

    int32_t opa;
    if (v >= 120) {
        opa = 255;
    } else {
        opa = v * 255 / 120;
    }
    lv_obj_set_style_opa(o, (lv_opa_t)opa, LV_PART_MAIN);

    int32_t ty;
    if (v < 700) {
        const int32_t e = ease_out_quart(v * 1000 / 700);   /* 0..1000 */
        ty = POPUP_SLIDE_PX + (int32_t)((int64_t)(-POPUP_OVER_PX - POPUP_SLIDE_PX) * e / 1000);
    } else {
        const int32_t e = (v - 700) * 1000 / 300;           /* 0..1000 */
        ty = -POPUP_OVER_PX + (int32_t)((int64_t)POPUP_OVER_PX * e / 1000);
    }
    lv_obj_set_style_translate_y(o, ty, LV_PART_MAIN);
}

/* 退场：往下滑 + 淡出（t^3，慢起快走） */
static void popup_out_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    v = clamp_i32(v, 0, BA_ANIM_RANGE);
    const int32_t e = ease_in_cubic(v);
    lv_obj_set_style_opa(o, (lv_opa_t)(255 - 255 * e / 1000), LV_PART_MAIN);
    lv_obj_set_style_translate_y(o, (int32_t)((int64_t)POPUP_SLIDE_PX * e / 1000), LV_PART_MAIN);
}

static void popup_anim(lv_obj_t *obj, int32_t from, int32_t to, uint32_t ms,
                       lv_anim_exec_xcb_t exec)
{
    if (!obj || !exec) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);   /* 缓动在 exec 里自己做 */
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_start(&a);
}

void ui_popup_in(lv_obj_t *scrim, lv_obj_t *panel)
{
    if (scrim) {
        /* 遮罩只淡入（整屏跟着滑会晃） */
        popup_anim(scrim, LV_OPA_TRANSP, LV_OPA_COVER, POPUP_IN_MS, popup_opa_cb);
    }
    popup_anim(panel, 0, BA_ANIM_RANGE, POPUP_IN_MS, popup_in_cb);
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
        popup_anim(scrim, LV_OPA_COVER, LV_OPA_TRANSP, POPUP_OUT_MS, popup_opa_cb);
    }
    if (panel) {
        popup_anim(panel, 0, BA_ANIM_RANGE, POPUP_OUT_MS, popup_out_cb);
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

/* ------------------------------------------------------------------ Toast
 *
 * ⚠ Toast **不走**碧蓝档案那套弹层动画/配色（用户明确要求保留原样）：
 * 它是"屏幕下方 90 px 的黑色半透明小胶囊 + 淡入淡出各带一点上滑"，
 * 与上面 ui_popup_in()/ui_popup_out() 的 BA 动效刻意分开。
 * 时长与位移也保持原来的 160/140 ms、10 px，别跟 BA 的常量混用。
 */
#define TOAST_IN_MS    160
#define TOAST_OUT_MS   140
#define TOAST_SLIDE_PX UI_PX_H(10)

/* 透明度 + 上滑：v 从 0→255 是"进入"，255→0 是"退出" */
static void toast_opa_slide_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    v = clamp_i32(v, 0, 255);
    lv_obj_set_style_opa(o, (lv_opa_t)v, LV_PART_MAIN);
    /* 透明时下沉 TOAST_SLIDE_PX，不透明时归位（显式转 int：UI_PX_H 可能是浮点表达式） */
    lv_obj_set_style_translate_y(o, (int32_t)((255 - v) * TOAST_SLIDE_PX / 255), LV_PART_MAIN);
}

static void toast_anim(lv_obj_t *o, int32_t from, int32_t to, uint32_t ms,
                       lv_anim_ready_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, toast_opa_slide_cb);
    if (done) {
        lv_anim_set_ready_cb(&a, done);
    }
    lv_anim_start(&a);
}

/* 淡出结束 → 藏起来（对象常驻，不删） */
static void toast_hidden_cb(lv_anim_t *an)
{
    if (an->var) {
        lv_obj_add_flag((lv_obj_t *)an->var, LV_OBJ_FLAG_HIDDEN);
    }
}

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    s_toast_timer = NULL;
    /* 淡出（不删对象：Toast 是常驻的，只是藏起来） */
    if (s_toast) {
        toast_anim(s_toast, LV_OPA_COVER, LV_OPA_TRANSP, TOAST_OUT_MS, toast_hidden_cb);
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
    toast_anim(s_toast, LV_OPA_TRANSP, LV_OPA_COVER, TOAST_IN_MS, NULL);

    if (duration_ms == 0) {
        duration_ms = 1800;
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

/*
 * 立刻收掉当前 Toast（不等到 duration 到点）。
 *
 * 给"启动图淡出"这种**由代码决定结束时刻**的场景用：main/splash.cc 在过渡动画开始
 * 时调一次，免得"正在初始化..."在首页上继续飘着（它的 duration 只是兜底）。
 * 没有 Toast 时是空操作。
 */
void ui_toast_hide(void)
{
    if (!s_toast) {
        return;
    }
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    /* 与 toast_hide_cb 同一套淡出：透明度动完就藏起来（对象常驻，不删） */
    toast_anim(s_toast, LV_OPA_COVER, LV_OPA_TRANSP, TOAST_OUT_MS, toast_hidden_cb);
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

/* ------------------------------------------------------------------ 控制中心状态转发 */

bool ui_cc_is_open(void)     { return ui_cc_state(); }

/*
 * 控制中心一展开就把音量条收掉：音量条也在 lv_layer_top 上、且位置正好在
 * 屏幕右中，压着控制中心的面板。这里属于"焦点被拿走"，立刻缩回，
 * 不要等 2.5 秒的自动缩回。
 */
void ui_cc_open(void)   { ui_volume_hide(); ui_cc_apply_open(true, true); }
void ui_cc_close(void)  { ui_cc_apply_open(false, true); }
void ui_cc_toggle(void) { ui_cc_apply_open(!ui_cc_state(), true); }

/* ------------------------------------------------------------------ 返回键 */

/*
 * 返回键（标题栏左侧那一个）：一张烘好的位图 back_arrow
 * （蓝色圆底 + 白色左箭头，见 assets/icons/back.png，按 UI_BACK_BTN_SIZE = 64 烘）。
 *
 * 尺寸与位置口径（2026-10 用户第二次调整）：
 *   - 用户原话："把返回按钮往左上角调整点，且按钮中心位置与其他元素对齐" ——
 *     圆心从"导航栏下沿"提到 **UI_TITLEBAR_MID（导航栏中线）**，与标题、页签、
 *     右上角动作按钮在同一条线上（比原来高 24 px）；
 *   - 尺寸 72 → **64**：圆心一旦落在中线上，按钮就上下各悬出 (SIZE - 栏高)/2 ——
 *     栏高 48 + 上下各一个 UI_PAGE_PAD(8) 正好 64：上边贴状态栏下沿、下边正好压在
 *     内容区第一行。取 72 会顶进状态栏 4 px（那是后画的半透明暗条，会把圆顶切平）；
 *   - 左边界 UI_BACK_BTN_X = UI_EDGE_HOTZONE —— **按得到的最左一列**
 *     （左边 24 px 是返回手势热区，挂在 lv_layer_top 上、z 序比按钮高；按钮再往左
 *     挪就会被它吃掉一部分，按了没反应）。
 *
 * 按下反馈（2026-10 用户要求"和弹窗里的按钮一样"）：**与弹窗按钮共用同一份实现** ——
 * 按住缩到 ≈92%（120 ms）、松手 150 ms 缓出复原，"复原动画跑完"才执行
 * ui_router_pop()；手指滑走（PRESS_LOST）只复原、不返回。手感常量只有 ui_ba.cc
 * "按下反馈"一节里那一套，想调就调那里，别在返回钮上另起一套。
 *
 * ⚠ 2026-10 修掉的真 bug：原来这里自己写了一套缩放动画，把**按钮**（lv_button）
 *   当作图片交给 `lv_image_set_scale()` —— 那个 API 只能作用在 lv_image 对象上，
 *   而本工程 `LV_USE_ASSERT_OBJ=n`，于是它按 `lv_image_t` 的字段去写一块
 *   `lv_obj_t` 大小的内存（**越界写堆**），偏偏按钮的绘制根本不看那些字段 ——
 *   结果是"按下去毫无反馈"（外加一颗随时可能踩到的内存炸弹），
 *   不管把 BACK_PRESS_SCALE / 时长怎么调都不会有反应。
 *   现在统一走 `transform_scale`（绘制期变换，不参与布局），别再改回图片缩放。
 *
 * ⚠ `back_arrow` 素材本身就是整块圆底，**不要再给 lv_button 画圆底/描边**
 *   （会露出两层边）。按钮只保留透明外壳 + 命中区域。
 */
static void back_click_after_restore(lv_event_t *e)
{
    (void)e;
    ui_router_pop();
}

/*
 * 建一个返回键。**只建控件、不摆位置**：
 *   父容器是 flex（设置页的标题行）→ 交给 flex 排；
 *   否则调用方自己 lv_obj_align（全局键用 UI_HEADER_TEXT 那一套度量）。
 */
lv_obj_t *ui_back_button_create(lv_obj_t *parent)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BACK_BTN_SIZE, UI_BACK_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);   /* 圆底在图里 */
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    /* 父容器是 flex 时（设置页的 scr），让 flex 跳过它，后面的 align 才生效 */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, &back_arrow);
    lv_image_set_antialias(img, true);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);    /* 点击都算在按钮上 */

    /* 按下反馈 + "复原后才返回上一级"：与弹窗按钮共用同一份实现（见上面的说明） */
    ui_ba_press_feedback(btn, back_click_after_restore, NULL);
    return btn;
}

/*
 * 把返回键摆到标题栏左侧：**圆心落在导航栏中线**（UI_TITLEBAR_MID，
 * 与标题、页签、右上角动作按钮同一条线），上下各悬出 UI_BACK_BTN_DIP；
 * 左边界是"按得到的最左一列" UI_BACK_BTN_X（左边 24 px 被返回手势热区占着）。
 * 全部度量见 ui_metrics.h 的 UI_BACK_BTN_* / UI_TITLEBAR_*。
 *
 * 为什么要在 ui_core 里统一摆、而不是各页自己 lv_obj_align：
 * `lv_obj_align` 的偏移是相对**父容器的内容区**（扣掉 padding）算的 ——
 * 全局键的父容器是 lv_layer_top()（无 padding），页面里的父容器是
 * `as_make_screen()` 造的 screen（pad_all = UI_PAGE_PAD，pad_top =
 * UI_TITLEBAR_Y）。同样写 (UI_BACK_BTN_X, UI_BACK_BTN_Y)，两边的**绝对**位置
 * 会差出一个 padding —— 于是"设置页的返回钮"和别的页对不齐。
 * 这里直接把父容器 padding 扣掉，调用方只管用，落点一定一致。
 */
void ui_back_button_place(lv_obj_t *btn)
{
    if (!btn) {
        return;
    }
    lv_obj_t *par = lv_obj_get_parent(btn);
    if (!par) {
        return;
    }
    const int pad_l = (int)lv_obj_get_style_pad_left(par, LV_PART_MAIN);
    const int pad_t = (int)lv_obj_get_style_pad_top(par, LV_PART_MAIN);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT,
                 UI_BACK_BTN_X - pad_l,
                 UI_BACK_BTN_Y - pad_t);
}

/*
 * 标题栏（状态栏正下方那一条，见 ui_metrics.h 的 UI_TITLEBAR_*）：
 * 返回键摆在这一条的垂直中线上（UI_BACK_BTN_Y）。
 *
 * 返回键建在 lv_layer_top 上、由 ui_router 在 push / pop / init_home 之后调用
 * ui_back_button_update() 刷新显示状态。
 *
 * 显示条件（ui_router_top_needs_back）：栈深 > 1 且当前页 desc.no_back == false
 * —— 首页与"自带返回钮"的设置页不显示，其余二级页（播放器 / 在线音乐 /
 * 本地音乐…）自动就有返回键，页面里不用再画。
 *
 * z 序：本对象在 ui_core_init 里**最先**创建，排在它后面的控制中心 /
 * 状态栏 / Toast / 手势热区都会盖在它上面 —— 模态遮罩（app 运行时
 * 新建在 layer_top 上的）也一样。于是：
 *   - 控制中心展开、搜索遮罩打开时返回键被盖住、点不到（符合预期）；
 *   - 它们关闭后按钮自动恢复可见，无需额外代码。
 */
static lv_obj_t *s_back_btn;
static bool      s_back_btn_suppressed;   /* 全屏看图时临时收起返回钮 */

lv_obj_t *ui_back_button_global_create(lv_obj_t *parent)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *btn = ui_back_button_create(parent);
    ui_back_button_place(btn);      /* 圆心落在导航栏中线（见函数注释） */
    s_back_btn = btn;
    ui_back_button_update();
    return btn;
}

/*
 * 临时收起全局返回钮（图库全屏看图）。只是置一个抑制位再刷新一次可见性 ——
 * 页面切换时 ui_back_button_update() 会重算，所以**必须在退出页面时**恢复成 false。
 * 收起后左边缘的返回手势热区仍然有效，不会把用户困住。
 */
void ui_back_button_suppress(bool suppress)
{
    s_back_btn_suppressed = suppress;
    ui_back_button_update();
}

void ui_back_button_update(void)
{
    if (!s_back_btn) {
        return;
    }
    const bool need = ui_router_top_needs_back() && !s_back_btn_suppressed;
    if (need) {
        lv_obj_remove_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/*
 * 标题该从这一条的顶往下偏多少，才落在中线上。
 *
 * 页面这样用（screen 的 pad_top 就是 UI_TITLEBAR_Y）：
 *     lv_obj_align(title, LV_ALIGN_TOP_LEFT, UI_HEADER_TEXT_X, ui_titlebar_text_dy());
 * 按标题字体的**行盒**真算，所以换了标题字体也不会偏。
 */
int ui_titlebar_text_dy(void)
{
    const int lh = (int)lv_font_get_line_height(UI_FONT_TITLE);
    return (UI_TITLEBAR_H - lh) / 2;
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
     * 控制中心 / Toast / 模态遮罩展开时都能盖住它 */
    ui_back_button_global_create(top);
    ui_cc_create(top);           /* 控制中心（默认隐藏） */
    lv_obj_t *sb = ui_status_bar_create(top);
    ui_toast_create(top);

    /* 手势：顶部下拉控制中心、左边缘返回（右边缘**故意不留热区**：
     * 原来那里挂的是功能抽屉，已按需求移除，留着会把页面的横滑手势吃掉） */
    ui_gesture_attach(sb);

    /* 音量悬浮条：屏幕右中，仍然放在手势热区**之后**创建 —— 确保它显示时
     * 一定压在热区之上（右边缘热区随抽屉一起删了，但"后建的在上面"这条纪律
     * 别丢，将来往边缘加东西时还靠它）。隐藏时整块在屏幕外，热区照常工作。 */
    ui_volume_panel_create(top);

    /*
     * 首页背景图（assets/background.jpg）：先把后台解码任务发出去，再去建首页 ——
     * 这样几百毫秒的 JPEG 软解与建首页、徽标绘制重叠，用户看不到"先纯色再跳出壁纸"。
     * 图是 screen 自己的 bg_image，贴图由 ui_home_bg.cc 负责（解好即贴）。
     */
    ui_home_bg_start();

    /* 首页 */
    esp_err_t ret = ui_router_init_home();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "home create failed");
        return ret;
    }

    /*
     * 这里**不再**弹"系统就绪"的 Toast：开机那一段现在由 main/splash.cc 的启动图
     * （assets/init.jpg + "正在初始化..."）负责，它盖在最上层，弹在这里也会被盖住
     * —— 谁开场由 app 层决定，框架层不抢这个时机。
     */
    ESP_LOGI(TAG, "ui_core ready");
    return ESP_OK;
}