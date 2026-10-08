/*
 * ui_gesture.cc —— 手势导航（横屏版）
 *
 * 判定优先级（开发指南 5.7）：
 *   1. 顶部状态栏按下         → 控制中心下拉（跟手）
 *   2. 左边缘 24px 内按下     → 返回（跟手 + 底层页面视差）
 *   3. 右边缘 24px 内按下     → 打开右侧功能抽屉
 *   4. 其余                   → 交给控件
 *
 * 动画层（docs/UI-suggest.md）：三种手势全部走 ui_spring 常驻弹簧，**没有**
 * "停掉动画 A → 新建动画 B"的第二套代码路径。
 *   拖动中 → ui_spring_hold() 冻结物理，ui_spring_set_value() 由手指驱动；
 *   松手后 → ui_spring_release() 只改 target，位置与速度都连续。
 * 因此任一动画跑到一半时再触摸，都会从**当前**位置/速度被手指接管。
 *
 * 实现方式：在 top layer 上放三个边缘热区对象（顶部状态栏本身 + 左右各 24px 的
 * 透明条），这样不会拦截页面中部控件的正常触摸。
 */
#include "ui_core.h"
#include "ui_internal.h"
#include "ui_metrics.h"
#include "ui_spring.h"

#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "ui_gesture";

typedef enum { G_IDLE = 0, G_CC, G_BACK, G_DRAWER } gesture_t;

/* 返回手势的运动阶段：退栈只在 BACK_SETTLE_COMMIT 到位后发生，
 * 因此在它之前任何时刻都能被新触摸改回 target（= 可打断） */
typedef enum {
    BACK_IDLE = 0,
    BACK_DRAG,
    BACK_SETTLE_CANCEL,
    BACK_SETTLE_COMMIT,
} back_phase_t;

static gesture_t   s_mode = G_IDLE;
static lv_point_t  s_start;

/* 手势热区对象（状态栏 + 左右边缘条），面板展开时统一停用 */
static lv_obj_t *s_hot[3];
static int       s_hot_n;

/* 提交判决：位移超过热区跨度 35% 或速度超过 450 px/s（开发指南 5.7） */
static bool should_commit(int32_t dist, int32_t vel, int32_t span)
{
    if (abs((int)dist) > span * 35 / 100) return true;
    if (abs((int)vel) > 450) return true;
    return false;
}

/* ------------------------------------------------------------------ 返回过渡
 *
 * 两个常驻弹簧分别驱动两层 screen 的 translate_x：
 *   s_spr_back_top   顶层页：0（在位） → UI_W（滑出屏幕）
 *   s_spr_back_under 底层页：-(UI_W/4)（视差起点） → 0
 *
 * **退栈被推迟到弹簧到位回调**（back_settle_cb），这一点是"可打断"的关键：
 * 在退栈真正发生之前，任何新触摸都能把 target 改回去，页面原路返回。
 */

static ui_spring_t  s_spr_back_top;
static ui_spring_t  s_spr_back_under;
static lv_obj_t    *s_back_top;      /* 本次手势锁定的两层 screen */
static lv_obj_t    *s_back_under;
static back_phase_t s_back_phase = BACK_IDLE;
static float        s_back_grab_top;    /* 手指按下瞬间顶层页的位移 */
static float        s_back_grab_under;  /* 手指按下瞬间底层页的位移 */
static bool         s_back_spr_ready;

static void back_finish(bool commit);   /* 前向声明：on_press_lost 需要它收尾 */

static void on_pressed(lv_event_t *e, gesture_t mode)
{
    (void)e;

    /* 有面板展开时，边缘热区/状态栏都不能再抢触摸：
     * 点"任意处关闭"要交给遮罩，否则触摸会被热区吃掉，表现为点了没反应。 */
    if (ui_cc_is_open() || ui_drawer_is_open()) {
        s_mode = G_IDLE;
        return;
    }

    /* 返回过渡进行中：只有"再按左边缘"允许接管（这正是可打断）；
     * 其他手势此时忽略，免得跟正在飞出去的页面抢焦点 */
    if ((s_back_phase == BACK_SETTLE_CANCEL || s_back_phase == BACK_SETTLE_COMMIT) &&
        mode != G_BACK) {
        s_mode = G_IDLE;
        return;
    }

    s_mode = mode;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        s_mode = G_IDLE;
        return;
    }
    lv_indev_get_point(indev, &s_start);
}

/* 手势被别的组件抢走（如列表开始滚动）时 LVGL 发 PRESS_LOST 而不是 RELEASED，
 * 不处理就会把 s_mode 卡在跟踪态，后续触摸会按上个手势解释 */
static void on_press_lost(lv_event_t *e)
{
    (void)e;
    if (s_mode == G_BACK && s_back_phase == BACK_DRAG) {
        /* 交给弹簧弹回原位（不是硬置 0），保持位置/速度连续 */
        back_finish(false);
    }
    /* 其余情况（含正在进行中的过渡）交给它自己的到位回调收尾 */
    s_mode = G_IDLE;
}

/* ------------------------------------------------------------------ 返回过渡实现 */

static void back_top_apply(float v, void *user)
{
    (void)user;
    /* 对象引用在退栈前被摘掉，这里必须判空，否则会写到已销毁的 screen */
    if (s_back_top) {
        lv_obj_set_style_translate_x(s_back_top, (int32_t)v, LV_PART_MAIN);
    }
}

static void back_under_apply(float v, void *user)
{
    (void)user;
    if (s_back_under) {
        lv_obj_set_style_translate_x(s_back_under, (int32_t)v, LV_PART_MAIN);
    }
}

/* 统一的收尾动作：复位阶段与对象引用，必要时退栈。
 * 由"弹簧到位回调"和"起止点重合因而无需运动"两条路径共用。 */
static void back_settle_apply(bool commit)
{
    if (s_back_phase == BACK_IDLE) {
        return;   /* 已经收尾过（或从未开始）：幂等，避免重复退栈 */
    }
    s_back_phase = BACK_IDLE;

    /* 底层页行程（≤UI_W/4）始终短于顶层页（≤UI_W），必然先到位；
     * 这里再兜一次底，保证它不会停在残余位移上 */
    if (s_back_under) {
        lv_obj_set_style_translate_x(s_back_under, 0, LV_PART_MAIN);
    }
    /* 先摘引用再退栈：顶层 screen 即将被异步销毁，
     * 不能让 apply 回调再写到悬垂指针上 */
    s_back_top   = NULL;
    s_back_under = NULL;

    if (commit) {
        /*
         * 用"立即退栈"：手指已经 1:1 把顶层页滑出屏幕了，再放一次屏幕切换
         * 动画会先把位置复位、重滑一遍（见 ui_router.cc 的 router_pop）。
         */
        ui_router_pop_immediate();
    }
    ESP_LOGD(TAG, "back settle (%s)", commit ? "commit" : "cancel");
}

/* 弹簧到位：此时才真正退栈。
 * 运行在 lv_timer 上下文（不是 LVGL 事件派发中）；即便被 on_pressed 阶段的
 * 同步调用，ui_router_pop() 内部也是"先切屏、再 delete_async"，安全。 */
static void back_settle_cb(ui_spring_t *s, void *user)
{
    (void)s;
    (void)user;
    /* 阶段可能已被新触摸改成 CANCEL：那样就只是回弹结束，不退栈 */
    back_settle_apply(s_back_phase == BACK_SETTLE_COMMIT);
}

static void back_spr_ensure(void)
{
    if (s_back_spr_ready) {
        return;
    }
    s_back_spr_ready = true;
    /* init 会立刻 apply 一次，此时两层引用都还是 NULL，正好不会误写 */
    ui_spring_init(&s_spr_back_top, 0.0f, 0.0f, (float)UI_W, back_top_apply, NULL);
    ui_spring_init(&s_spr_back_under, 0.0f, -(float)(UI_W / 4), 0.0f,
                   back_under_apply, NULL);
    ui_spring_set_done_cb(&s_spr_back_top, back_settle_cb, NULL);
}

/* 手指开始/继续拖动返回手势：冻结物理，从当前位置接管 */
static void back_drag_begin(void)
{
    back_spr_ensure();

    bool resuming = (s_back_phase == BACK_SETTLE_CANCEL ||
                     s_back_phase == BACK_SETTLE_COMMIT);

    s_back_top   = ui_router_top_screen();
    s_back_under = ui_router_under_screen();

    ui_spring_hold(&s_spr_back_top);
    ui_spring_hold(&s_spr_back_under);

    if (resuming) {
        /* 动画没跑完就被抓住：从当前位置继续，位置/速度都连续 */
        s_back_grab_top   = ui_spring_value(&s_spr_back_top);
        s_back_grab_under = ui_spring_value(&s_spr_back_under);
    } else {
        /* 新一次手势：顶层页回到 0，底层页摆到视差起点 */
        s_back_grab_top   = 0.0f;
        s_back_grab_under = -(float)(UI_W / 4);
        ui_spring_set_value(&s_spr_back_top, s_back_grab_top);
        ui_spring_set_value(&s_spr_back_under, s_back_grab_under);
    }
    s_back_phase = BACK_DRAG;
}

/* 跟手位移。**不要**给 screen 对象设 opacity：对象级 OPA < 255 会让 LVGL
 * 为整屏分配中间图层，在本板（framebuffer 已占 PSRAM）代价很大。 */
static void back_drag(int32_t dx)
{
    if (s_back_phase != BACK_DRAG) {
        back_drag_begin();
    }
    if (!s_back_top) {
        return;
    }
    if (dx < 0) {
        dx = 0;
    }
    ui_spring_set_value(&s_spr_back_top, s_back_grab_top + (float)dx);
    ui_spring_set_value(&s_spr_back_under, s_back_grab_under + (float)dx / 4.0f);
}

/*
 * 松手收尾：
 *   commit → 顶层页滑出屏，**到位后**才退栈（back_settle_cb）；
 *   cancel → 两层都弹回原位。
 * 只改 target，速度由弹簧继承 → 松手不会"顿一下"。
 */
static void back_finish(bool commit)
{
    if (!s_back_top) {
        /* 连 PRESSING 都没触发（点了一下边缘）：无需动画 */
        if (commit) {
            ui_router_pop();
        }
        s_back_phase = BACK_IDLE;
        return;
    }
    s_back_phase = commit ? BACK_SETTLE_COMMIT : BACK_SETTLE_CANCEL;
    ui_spring_release(&s_spr_back_top, commit ? (float)UI_W : 0.0f);
    ui_spring_release(&s_spr_back_under, 0.0f);
    ESP_LOGD(TAG, "back %s (from %.0f)", commit ? "commit" : "cancel",
             (double)ui_spring_value(&s_spr_back_top));

    /* 起止点本来就重合（例如只在边缘点了一下）：弹簧不会产生任何帧，
     * 也就永远不会触发到位回调 —— 这种"零行程"必须在这里立刻收尾，
     * 否则阶段会永久停在 SETTLE_*，后续手势全被 on_pressed 挡掉。 */
    if (ui_spring_settled(&s_spr_back_top)) {
        back_settle_apply(commit);
    }
}

static void on_pressing(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int32_t dx = p.x - s_start.x;
    int32_t dy = p.y - s_start.y;

    switch (s_mode) {
    case G_CC:
        /* 不做 dy>0 过滤：过原点后继续上拖也应跟手（弹簧内部会夹到行程端点），
         * 否则手指回拖到起点以上时面板会"卡住不动" */
        ui_cc_drag((int)dy);
        break;
    case G_BACK:
        back_drag(dx);
        break;
    case G_DRAWER:
        /* 同理：过原点后回拖也应跟手 */
        ui_drawer_drag((int)dx);
        break;
    default:
        break;
    }
}

static void on_released(lv_event_t *e)
{
    (void)e;
    if (s_mode == G_IDLE) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        s_mode = G_IDLE;
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_start.x;
    int32_t dy = p.y - s_start.y;

    switch (s_mode) {
    case G_CC:
        ui_cc_drag_end((int)dy, 0);
        break;
    case G_BACK:
        /* 只在手指真正拖动过（BACK_DRAG）时才做判决；
         * 若此时正处在过渡中（例如滑出动画进行到一半被点了一下），
         * 不去干扰它，让 back_settle_cb 收尾 */
        if (s_back_phase == BACK_DRAG) {
            back_finish(should_commit(dx, 0, UI_W) && ui_router_depth() > 1);
        }
        break;
    case G_DRAWER:
        ui_drawer_drag_end((int)dx, 0);
        break;
    default:
        break;
    }
    s_mode = G_IDLE;
}

/* ------------------------------------------------------------------ 热区对象 */

static lv_obj_t *make_edge_strip(lv_obj_t *parent, bool left)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_EDGE_HOTZONE, UI_H);
    lv_obj_align(o, left ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /*
     * **必须**加 PRESS_LOCK：热区只有 24px 宽，手指横向拖出去后
     * LVGL 会在 PRESSING 阶段重新做 hit-test、发 PRESS_LOST 给本对象，
     * 手势就断了（表现：拖动不跟手、划完才动）。
     * PRESS_LOCK 的语义正是"手指滑出对象也保持按下目标"。
     */
    lv_obj_add_flag(o, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void ui_gesture_suspend(bool suspend)
{
    for (int i = 0; i < s_hot_n; i++) {
        if (!s_hot[i]) {
            continue;
        }
        if (suspend) {
            lv_obj_remove_flag(s_hot[i], LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_flag(s_hot[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void ui_gesture_attach(lv_obj_t *status_bar)
{
    s_hot_n = 0;

    /* 1) 顶部状态栏 → 控制中心下拉（跟手） */
    if (status_bar) {
        lv_obj_add_flag(status_bar, LV_OBJ_FLAG_CLICKABLE);
        /* 状态栏只有 32px 高，手指往下拖必然滑出边界。不加 PRESS_LOCK 的话
         * LVGL 会重新 hit-test 并给状态栏发 PRESS_LOST，下拉就永远不跟手。 */
        lv_obj_add_flag(status_bar, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(status_bar, [](lv_event_t *e) { on_pressed(e, G_CC); },
                            LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(status_bar, on_pressing, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(status_bar, on_released, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(status_bar, on_press_lost, LV_EVENT_PRESS_LOST, NULL);
        s_hot[s_hot_n++] = status_bar;
    }

    lv_obj_t *top = lv_layer_top();

    /* 2) 左边缘 → 返回（跟手 + 视差） */
    lv_obj_t *left = make_edge_strip(top, true);
    lv_obj_add_event_cb(left, [](lv_event_t *e) { on_pressed(e, G_BACK); },
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(left, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(left, on_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(left, on_press_lost, LV_EVENT_PRESS_LOST, NULL);
    s_hot[s_hot_n++] = left;

    /* 3) 右边缘 → 功能抽屉 */
    lv_obj_t *right = make_edge_strip(top, false);
    lv_obj_add_event_cb(right, [](lv_event_t *e) { on_pressed(e, G_DRAWER); },
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(right, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(right, on_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(right, on_press_lost, LV_EVENT_PRESS_LOST, NULL);
    s_hot[s_hot_n++] = right;

    ESP_LOGI(TAG, "gestures attached (edge=%d, hotzones=%d)",
             (int)UI_EDGE_HOTZONE, s_hot_n);
}