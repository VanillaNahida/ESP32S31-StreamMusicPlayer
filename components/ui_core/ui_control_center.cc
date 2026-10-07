/*
 * ui_control_center.cc —— 控制中心（下拉，iOS 药丸风格）+ 右侧功能抽屉
 *
 * 三段式（开发指南 5.6，800x480 横屏）：
 *   行1 连接模块药丸（2x2 圆钮）+ 媒体药丸（封面/歌名/歌手/播放暂停）+ 两个小药丸
 *   行2 两个**横向**药丸滑条：亮度、音量（横屏下纵向改横向）
 *   行3 一排圆形快捷按钮：主界面 / 在线搜索 / 设置 / 息屏
 *
 * 每次打开重新渲染状态（避免陈旧显示）；拖动中只更新填充，不重建控件树。
 */
#include "ui_core.h"
#include "ui_internal.h"
#include "ui_metrics.h"
#include "ui_spring.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "audio_service.h"

static const char *TAG = "ui_cc";

static lv_obj_t  *s_root;        /* 药丸与抽屉的共同父容器（top layer） */
static lv_obj_t  *s_cc;          /* 控制中心面板 */
static lv_obj_t  *s_scrim;       /* 遮罩：点击关闭 + 压暗下层 */
static lv_obj_t  *s_drawer;      /* 右侧功能抽屉 */
static lv_obj_t  *s_bri_fill;
static lv_obj_t  *s_vol_fill;
static lv_obj_t  *s_media_title;
static lv_obj_t  *s_media_artist;
static lv_obj_t  *s_media_btn_label;
static lv_obj_t  *s_out_label;
static lv_obj_t  *s_sd_label;
static bool       s_cc_open;
static bool       s_drawer_open;
static int        s_bri = 80;
static int        s_vol = 55;
static bool       s_muted;

lv_obj_t *ui_sheet_root(void) { return s_root; }
bool ui_cc_state(void)        { return s_cc_open; }
bool ui_drawer_state(void)    { return s_drawer_open; }

/* ------------------------------------------------------------------ 通用小部件 */

static void style_pill(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(o, UI_PX_H(26), LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void set_fill(lv_obj_t *fill, int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    lv_obj_set_width(fill, LV_PCT(percent));
}

/* 圆形 / 方形按钮：图标 + 可选文字，点击回调 */
static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, lv_color_t bg_on)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_PX_H(46), UI_PX_H(46));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, bg_on, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x101014), LV_PART_MAIN);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    return btn;
}

/* 横向药丸滑条（亮度 / 音量）：底槽 + 填充，填充自左向右 */
static lv_obj_t *pill_slider(lv_obj_t *parent, const char *symbol, lv_obj_t **fill_out)
{
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, UI_PX_W(300), UI_PX_H(72));
    lv_obj_set_style_bg_color(track, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(track, UI_PX_H(18), LV_PART_MAIN);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, LV_PCT(50), LV_PCT(100));
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(fill, UI_COLOR_PILL_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(fill, UI_PX_H(18), LV_PART_MAIN);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(track);
    lv_obj_set_style_text_font(icon, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x2A2A33), LV_PART_MAIN);
    lv_label_set_text(icon, symbol);
    lv_obj_align(icon, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(8));

    if (fill_out) {
        *fill_out = fill;
    }
    return track;
}

/* ------------------------------------------------------------------ 交互回调 */

static void on_scrim_click(lv_event_t *e)
{
    (void)e;
    if (s_cc_open) {
        ui_cc_close();
    }
    if (s_drawer_open) {
        ui_drawer_close();
    }
}

/*
 * 药丸滑条的跟手取值：按触点 x 在轨道内的比例设值。
 *
 * 两个坑，之前就是踩在这上面才"不跟手"：
 *   1) 原来只挂了 LV_EVENT_CLICKED —— LVGL 只在"短按"结束时发 CLICKED，
 *      手指一拖就变成 PRESSING，CLICKED 根本不发，滑条自然纹丝不动；
 *      必须挂 PRESSED + PRESSING 才跟手。
 *   2) 触点要换算到轨道坐标系，必须用 **屏幕绝对坐标** lv_obj_get_coords()，
 *      不能用 lv_obj_get_x()：后者是相对父对象的 x，而轨道在 flex 布局里
 *      父对象并不在原点，相减得到的是错的比例。
 */
static void slider_follow_pointer(lv_obj_t *track, int *state, lv_obj_t *fill)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(track, &a);
    int w = lv_area_get_width(&a);
    if (w <= 0) {
        return;
    }
    int rel = (int)p.x - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    *state = rel * 100 / w;
    set_fill(fill, *state);
}

static void on_bri_move(lv_event_t *e)
{
    slider_follow_pointer(lv_event_get_current_target_obj(e), &s_bri, s_bri_fill);
}

/* 手指离开才真正下发到硬件：拖动过程中每帧调用会刷屏 toast */
static void on_bri_settle(lv_event_t *e)
{
    (void)e;
    if (bsp_backlight_set(s_bri) != ESP_OK) {
        ui_toast("本板无软件背光控制", 1200);
    }
}

static void on_vol_move(lv_event_t *e)
{
    slider_follow_pointer(lv_event_get_current_target_obj(e), &s_vol, s_vol_fill);
}

static void on_vol_settle(lv_event_t *e)
{
    (void)e;
    /*
     * 必须走 audio_service_set_volume()：它才是音量的唯一真相
     * （内部会同步 ES8389 音量寄存器、0% 时自动静音、并把音量落盘记忆）。
     * 旧实现直接调 bsp_audio_set_volume()，audio_service 里的 s_volume 仍是旧值 ——
     * 之后任何一次"设音量"（硬件 VOL±、播放器页音量条）都会把滑块的值覆盖回去，
     * 设置页显示的音量百分比也跟着不对。
     */
    audio_service_set_volume(s_vol);
    s_muted = (s_vol == 0);
}

/* 滑条轨道：按下即跟手，拖动中持续跟手，松手下发硬件 */
static void bind_pill_slider(lv_obj_t *track, lv_event_cb_t move, lv_event_cb_t settle)
{
    lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
    /* 轨道只有 72px 高，手指纵向滑出边界就会断掉手势 —— 与状态栏同理 */
    lv_obj_add_flag(track, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(track, move, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(track, move, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(track, settle, LV_EVENT_RELEASED, NULL);
}

static void on_media_play(lv_event_t *e)
{
    (void)e;
    ui_toast("播放器将在 M2 接入", 1200);
}

static void on_quick_home(lv_event_t *e)   { (void)e; ui_cc_close(); ui_router_reset_to_home(); }
static void on_quick_search(lv_event_t *e) { (void)e; ui_toast("在线搜索将在 M3 接入", 1200); }
static void on_quick_settings(lv_event_t *e){ (void)e; ui_toast("设置将在 M5 完善", 1200); }
/* 息屏自动恢复定时器（回调在 esp_timer 任务上下文，安全：它不是 LVGL worker） */
static esp_timer_handle_t s_off_timer;
static void off_timer_cb(void *arg)
{
    (void)arg;
    ui_screen_on();
}

static void on_quick_off(lv_event_t *e)
{
    (void)e;
    ui_cc_close();
    /* 本板没有软件可控背光，息屏只能停 LVGL 刷新；而暂停期间触摸到不了 LVGL，
     * 所以必须给一条自动恢复路径，否则就是永久卡死。这里做成"息屏 15 秒"。 */
    ui_toast("息屏 15 秒", 1500);
    if (!s_off_timer) {
        esp_timer_create_args_t args = {};
        args.callback = off_timer_cb;
        args.name = "screen_off";
        esp_timer_create(&args, &s_off_timer);
    }
    if (s_off_timer) {
        esp_timer_stop(s_off_timer);
        esp_timer_start_once(s_off_timer, 15ULL * 1000ULL * 1000ULL);
    }
    ui_screen_off();
}
static void on_cc_any_touch(lv_event_t *e) { (void)e; /* 展开时报告活动，推迟息屏 */ }

/* ================================================================== 动画层
 *
 * 按 docs/UI-suggest.md 的建议：**不做"停掉动画 A → 新建动画 B"**，
 * 而是常驻两个弹簧，任何时刻只改 target，由物理继续推进：
 *
 *   s_spr_cc      值 = 面板 y 坐标   -UI_CC_PANEL_H（关） → 0（开）
 *   s_spr_drawer  值 = 抽屉 x 坐标   UI_W（关）           → UI_W-UI_DRAWER_W（开）
 *
 * 位置/速度连续 ⇒ 打断不回跳、松手不顿；两个弹簧共用一个 apply 回调，
 * 遮罩浓淡、可见性、可点性全部由**同一次求值**派生 ⇒ 物理上不可能失配。
 * 由此彻底消灭了旧实现里"关闭动画被打断 → 透明遮罩残留吞触摸"的卡死路径。
 */

static ui_spring_t s_spr_cc;
static ui_spring_t s_spr_drawer;
static bool       s_gesture_suspended;   /* 手势热区当前是否已挂起（避免每帧改 flag） */

/*
 * 派生视觉的唯一出口：
 *   面板 y / 抽屉 x 直接取弹簧值；遮罩浓度与可点性取两者中较大的"展开度"。
 *
 * 遮罩必须用 **BG_OPA** 而不是对象级 OPA：对象级 OPA < 255 会让 LVGL 为这个
 * 800x480 的全屏对象每帧分配约 750 KB 中间图层（framebuffer 已占 PSRAM）。
 */
static void sheet_visual_sync(void)
{
    float cc_v = ui_spring_value(&s_spr_cc);
    float dr_v = ui_spring_value(&s_spr_drawer);

    if (s_cc) {
        lv_obj_set_y(s_cc, (int32_t)lroundf(cc_v));
    }
    if (s_drawer) {
        lv_obj_set_x(s_drawer, (int32_t)lroundf(dr_v));
    }
    if (!s_scrim) {
        return;
    }

    /* 展开度：0 = 完全关闭，1 = 完全展开 */
    float cc_p = (cc_v + (float)UI_CC_PANEL_H) / (float)UI_CC_PANEL_H;
    float dr_p = ((float)UI_W - dr_v) / (float)UI_DRAWER_W;
    float p    = cc_p > dr_p ? cc_p : dr_p;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    if (p <= 0.001f) {
        /* 完全关闭：必须**既不可见也不可点**，否则会留下一个透明但仍吞触摸的
         * "幽灵遮罩"压在真实 UI 之上，表现就是整个界面卡死。 */
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_scrim, (lv_opa_t)(p * (float)LV_OPA_50), LV_PART_MAIN);
    }

    /* 热区在 z 序上高于遮罩：只要面板还有一点露头就必须整体让路，
     * 否则点最左/最右 24 px 关不掉面板，看起来也像卡死 */
    bool want_suspend = p > 0.001f;
    if (want_suspend != s_gesture_suspended) {
        s_gesture_suspended = want_suspend;
        ui_gesture_suspend(want_suspend);
    }
}

static void cc_apply_cb(float v, void *user)
{
    (void)v; (void)user;
    sheet_visual_sync();
}

static void drawer_apply_cb(float v, void *user)
{
    (void)v; (void)user;
    sheet_visual_sync();
}

/* 打开时刷新一遍面板内容（亮度/音量填充可能已被别处改动） */
static void cc_refresh_content(void)
{
    /* 音量可能在别处变过（播放器页音量条 / 硬件 VOL± / 开机时恢复的记忆值），
     * 所以打开控制中心都从 audio_service 重新取一次，别用上次的旧值 */
    s_vol   = audio_service_get_volume();
    s_muted = (s_vol == 0);
    set_fill(s_bri_fill, s_bri);
    set_fill(s_vol_fill, s_vol);
}

/* ------------------------------------------------------------------ 跟手拖动 */

static bool  s_cc_dragging;
static bool  s_drawer_dragging;
static float s_cc_drag_base;
static float s_drawer_drag_base;

/* 松手判决用的速度门限（px/s）：超过它就直接按甩动方向定胜负 */
#define SHEET_FLING_VEL  350.0f

void ui_cc_drag(int dy)
{
    if (!s_cc) {
        return;
    }
    if (!s_cc_dragging) {
        s_cc_dragging  = true;
        /* 手指接管：冻结物理，从弹簧**当前**位置出发（这就是"可打断"） */
        ui_spring_hold(&s_spr_cc);
        s_cc_drag_base = ui_spring_value(&s_spr_cc);
        s_cc_open      = true;   /* 拖动期间视为展开：遮罩可见、热区让路 */
        lv_obj_move_foreground(s_scrim);
        lv_obj_move_foreground(s_cc);
    }
    ui_spring_set_value(&s_spr_cc, s_cc_drag_base + (float)dy);
    /* 验证 PRESS_LOCK 是否生效：手指离开 32px 状态栏后这条日志仍应持续输出 */
    ESP_LOGD(TAG, "cc drag dy=%d y=%.0f", dy, (double)ui_spring_value(&s_spr_cc));
}

void ui_cc_drag_end(int dy, int vel)
{
    (void)dy; (void)vel;
    if (!s_cc || !s_cc_dragging) {
        return;
    }
    s_cc_dragging = false;

    float v  = ui_spring_value(&s_spr_cc);
    float sp = s_spr_cc.velocity;      /* 弹簧在 set_value 里估算的手指速度 */
    bool open;
    if (sp > SHEET_FLING_VEL) {
        open = true;                   /* 快速下甩 */
    } else if (sp < -SHEET_FLING_VEL) {
        open = false;                  /* 快速上甩 */
    } else {
        open = v > -(float)UI_CC_PANEL_H * 0.5f;
    }
    s_cc_open = open;
    /* 只改 target，速度留给弹簧继承 —— 松手不会"顿一下" */
    ui_spring_release(&s_spr_cc, open ? 0.0f : -(float)UI_CC_PANEL_H);
    ESP_LOGD(TAG, "cc drag_end y=%.0f v=%.0f -> %s",
             (double)v, (double)sp, open ? "open" : "close");
}

void ui_drawer_drag(int dx)
{
    if (!s_drawer) {
        return;
    }
    if (!s_drawer_dragging) {
        s_drawer_dragging  = true;
        ui_spring_hold(&s_spr_drawer);
        s_drawer_drag_base = ui_spring_value(&s_spr_drawer);
        s_drawer_open      = true;
        lv_obj_move_foreground(s_scrim);
        lv_obj_move_foreground(s_drawer);
    }
    ui_spring_set_value(&s_spr_drawer, s_drawer_drag_base + (float)dx);
    ESP_LOGD(TAG, "drawer drag dx=%d x=%.0f", dx, (double)ui_spring_value(&s_spr_drawer));
}

void ui_drawer_drag_end(int dx, int vel)
{
    (void)dx; (void)vel;
    if (!s_drawer || !s_drawer_dragging) {
        return;
    }
    s_drawer_dragging = false;

    float v  = ui_spring_value(&s_spr_drawer);
    float sp = s_spr_drawer.velocity;
    float open_x = (float)(UI_W - UI_DRAWER_W);
    bool open;
    /* 注意抽屉的 x 越小越"开"，速度符号与直觉相反 */
    if (sp < -SHEET_FLING_VEL) {
        open = true;
    } else if (sp > SHEET_FLING_VEL) {
        open = false;
    } else {
        open = v < ((float)UI_W + open_x) * 0.5f;
    }
    s_drawer_open = open;
    ui_spring_release(&s_spr_drawer, open ? open_x : (float)UI_W);
    ESP_LOGD(TAG, "drawer drag_end x=%.0f v=%.0f -> %s",
             (double)v, (double)sp, open ? "open" : "close");
}

/* ------------------------------------------------------------------ 打开 / 关闭 */

/* animate 参数保留只是为了不改动既有调用签名：现在所有运动都交给弹簧，
 * "动画"与"瞬时"的差别只剩弹簧自己走完这一段，不再有第二套代码路径。 */
void ui_cc_apply_open(bool open, bool animate)
{
    (void)animate;
    if (!s_cc) {
        return;
    }
    s_cc_open = open;
    if (open) {
        cc_refresh_content();
        lv_obj_move_foreground(s_scrim);
        lv_obj_move_foreground(s_cc);
    }
    if (!s_cc_dragging) {
        ui_spring_set_target(&s_spr_cc, open ? 0.0f : -(float)UI_CC_PANEL_H);
    }
    ESP_LOGD(TAG, "cc %s", open ? "open" : "close");
}

void ui_drawer_apply_open(bool open, bool animate)
{
    (void)animate;
    if (!s_drawer) {
        return;
    }
    s_drawer_open = open;
    if (open) {
        lv_obj_move_foreground(s_scrim);
        lv_obj_move_foreground(s_drawer);
    }
    if (!s_drawer_dragging) {
        ui_spring_set_target(&s_spr_drawer,
                             open ? (float)(UI_W - UI_DRAWER_W) : (float)UI_W);
    }
    ESP_LOGD(TAG, "drawer %s", open ? "open" : "close");
}

/* ------------------------------------------------------------------ 构建 */

static lv_obj_t *build_connection_pill(lv_obj_t *parent)
{
    lv_obj_t *pill = lv_obj_create(parent);
    style_pill(pill);
    lv_obj_set_size(pill, UI_PX_W(230), UI_PX_H(150));
    lv_obj_set_style_pad_all(pill, UI_PX_H(16), LV_PART_MAIN);

    lv_obj_t *grid = lv_obj_create(pill);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    round_button(grid, LV_SYMBOL_WIFI, UI_COLOR_ACCENT);       /* Wi-Fi */
    round_button(grid, LV_SYMBOL_BLUETOOTH, UI_COLOR_ACCENT);  /* 蓝牙 */
    round_button(grid, LV_SYMBOL_AUDIO, UI_COLOR_ACCENT);      /* 输出通道 */
    round_button(grid, LV_SYMBOL_MUTE, UI_COLOR_PILL_LIGHT);   /* 静音 */
    return pill;
}

static lv_obj_t *build_media_pill(lv_obj_t *parent)
{
    lv_obj_t *pill = lv_obj_create(parent);
    style_pill(pill);
    lv_obj_set_size(pill, UI_PX_W(330), UI_PX_H(96));
    lv_obj_set_style_pad_all(pill, UI_PX_H(10), LV_PART_MAIN);

    lv_obj_t *cover = lv_obj_create(pill);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, UI_PX_H(72), UI_PX_H(72));
    lv_obj_align(cover, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(cover, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cover, UI_COLOR_ICON_LOCAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *col = lv_obj_create(pill);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, UI_PX_W(180), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, UI_PX_H(84), 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    s_media_title = lv_label_create(col);
    lv_obj_set_style_text_font(s_media_title, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_media_title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(s_media_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_media_title, LV_PCT(100));
    lv_label_set_text(s_media_title, "未在播放");

    s_media_artist = lv_label_create(col);
    lv_obj_set_style_text_font(s_media_artist, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_media_artist, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_media_artist, "—");

    lv_obj_t *btn = lv_button_create(pill);
    lv_obj_set_size(btn, UI_PX_H(52), UI_PX_H(52));
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PILL_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_media_play, LV_EVENT_CLICKED, NULL);

    s_media_btn_label = lv_label_create(btn);
    lv_obj_set_style_text_font(s_media_btn_label, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_media_btn_label, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(s_media_btn_label, LV_SYMBOL_PAUSE);
    lv_obj_center(s_media_btn_label);
    return pill;
}

static lv_obj_t *build_small_pill(lv_obj_t *parent, const char *text, lv_obj_t **label_out)
{
    lv_obj_t *pill = lv_obj_create(parent);
    style_pill(pill);
    lv_obj_set_size(pill, UI_PX_W(150), UI_PX_H(42));
    lv_obj_set_style_radius(pill, UI_PX_H(21), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, UI_PX_W(14), LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(pill);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (label_out) {
        *label_out = l;
    }
    return pill;
}

static void build_quick_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_PX_H(80));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PX_W(34), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *sym; lv_event_cb_t cb; } items[] = {
        { LV_SYMBOL_HOME,     on_quick_home },
        { LV_SYMBOL_LIST,     on_quick_search },
        { LV_SYMBOL_SETTINGS, on_quick_settings },
        { LV_SYMBOL_EYE_CLOSE,on_quick_off },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *b = round_button(row, items[i].sym, UI_COLOR_PILL);
        /* 快捷按钮是深底浅图标，覆盖 round_button 的浅底深图标 */
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
        lv_obj_add_event_cb(b, items[i].cb, LV_EVENT_CLICKED, NULL);
    }
}

lv_obj_t *ui_cc_create(lv_obj_t *parent)
{
    s_root = parent;

    /* 遮罩：点击关闭 + 压暗下层（不做实时高斯模糊） */
    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, UI_W, UI_H);
    lv_obj_align(s_scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    /* 可点性与可见性由 sheet_visual_sync() 统一管理：关闭后必须既不可见也不可点，
     * 否则会变成压在真实 UI 之上、吞掉所有触摸的"幽灵遮罩" */
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, on_scrim_click, LV_EVENT_CLICKED, NULL);

    /* 控制中心面板 */
    s_cc = lv_obj_create(parent);
    lv_obj_remove_style_all(s_cc);
    lv_obj_set_size(s_cc, UI_W, UI_CC_PANEL_H);
    lv_obj_set_style_bg_color(s_cc, UI_COLOR_BG_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cc, UI_PX_H(28), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_cc, UI_PX_H(14), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_cc, UI_PX_H(12), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 不设 HIDDEN：关闭态由弹簧值把它摆在屏幕外（y = -UI_CC_PANEL_H），
     * 可见性/可点性统一由 sheet_visual_sync() 派生，避免出现第二处状态源 */
    lv_obj_add_event_cb(s_cc, on_cc_any_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_set_y(s_cc, -UI_CC_PANEL_H);

    /* 行1：左连接药丸 + 右媒体区 */
    lv_obj_t *row1 = lv_obj_create(s_cc);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, LV_PCT(100), UI_PX_H(150));
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row1, UI_PX_W(18), LV_PART_MAIN);
    lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

    build_connection_pill(row1);

    lv_obj_t *right = lv_obj_create(row1);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, UI_PX_W(340), LV_PCT(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(right, UI_PX_H(12), LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    build_media_pill(right);

    lv_obj_t *small = lv_obj_create(right);
    lv_obj_remove_style_all(small);
    lv_obj_set_size(small, LV_PCT(100), UI_PX_H(42));
    lv_obj_set_flex_flow(small, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(small, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(small, LV_OBJ_FLAG_SCROLLABLE);
    build_small_pill(small, "输出 扬声器", &s_out_label);
    build_small_pill(small, "存储卡 --", &s_sd_label);

    /* 行2：两个横向药丸滑条 */
    lv_obj_t *row2 = lv_obj_create(s_cc);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, LV_PCT(100), UI_PX_H(80));
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row2, UI_PX_W(20), LV_PART_MAIN);
    lv_obj_remove_flag(row2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bri = pill_slider(row2, LV_SYMBOL_EYE_OPEN, &s_bri_fill);
    bind_pill_slider(bri, on_bri_move, on_bri_settle);

    lv_obj_t *vol = pill_slider(row2, LV_SYMBOL_VOLUME_MAX, &s_vol_fill);
    bind_pill_slider(vol, on_vol_move, on_vol_settle);

    /* 行3：快捷圆钮 */
    build_quick_row(s_cc);

    /* 右侧功能抽屉（默认在屏幕外） */
    s_drawer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_drawer);
    lv_obj_set_size(s_drawer, UI_DRAWER_W, UI_H);
    lv_obj_set_style_bg_color(s_drawer, UI_COLOR_BG_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_x(s_drawer, UI_W);
    lv_obj_set_y(s_drawer, 0);

    lv_obj_t *dt = lv_label_create(s_drawer);
    lv_obj_set_style_text_font(dt, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(dt, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(dt, "功能抽屉\n（M2 起接入播放队列）");
    lv_obj_align(dt, LV_ALIGN_CENTER, 0, 0);

    /* 弹簧必须在两个面板都建好之后再初始化：init 会立刻 apply 一次，
     * 由 sheet_visual_sync() 把初始视觉（面板在屏幕外、遮罩不可见）落地 */
    ui_spring_init(&s_spr_cc, -(float)UI_CC_PANEL_H,
                   -(float)UI_CC_PANEL_H, 0.0f, cc_apply_cb, NULL);
    ui_spring_init(&s_spr_drawer, (float)UI_W,
                   (float)(UI_W - UI_DRAWER_W), (float)UI_W, drawer_apply_cb, NULL);

    ESP_LOGI(TAG, "control center built (panel_h=%d drawer_w=%d)",
             (int)UI_CC_PANEL_H, (int)UI_DRAWER_W);
    return s_cc;
}