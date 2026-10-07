/*
 * ui_spring.cc —— 常驻弹簧动画控制器实现
 *
 * 物理模型（半隐式欧拉，数值稳定）：
 *     a = k * (target - value) - c * velocity
 *     velocity += a * dt
 *     value    += velocity * dt
 * k = 300 / c = 34 时接近临界阻尼（2*sqrt(300) ≈ 34.6），到位不超调、不抖。
 */
#include "ui_spring.h"

#include <math.h>
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui_spring";

#define SPRING_STIFFNESS_DEFAULT  300.0f
#define SPRING_DAMPING_DEFAULT     34.0f
#define SPRING_TICK_MS             16      /* ≈60fps */
#define SPRING_DT_MAX              0.032f  /* 单帧 dt 上限：卡帧后不让动画飞出去 */
#define SPRING_SLEEP_EPS           0.35f   /* 到位判定 */

static ui_spring_t *s_head;
static lv_timer_t  *s_timer;

static void apply(ui_spring_t *s)
{
    if (s->apply) {
        s->apply(s->value, s->user);
    }
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void spring_step(ui_spring_t *s, float dt)
{
    if (s->held) {
        return;                       /* 手指接管中，物理不参与 */
    }

    float dx = s->target - s->value;
    if (fabsf(dx) < SPRING_SLEEP_EPS && fabsf(s->velocity) < SPRING_SLEEP_EPS) {
        if (s->value != s->target || s->velocity != 0.0f) {
            s->value = s->target;
            s->velocity = 0.0f;
            apply(s);
        }
        /* 到位回调：await 到这个 target 的收尾动作（如返回手势退栈）。
         * 只发一次；下次改 target 会重新武装。 */
        if (s->done && !s->done_fired) {
            s->done_fired = true;
            s->done(s, s->done_user);
        }
        return;
    }

    float a = dx * s->stiffness - s->velocity * s->damping;
    s->velocity += a * dt;
    s->value += s->velocity * dt;

    float clamped = clampf(s->value, s->vmin, s->vmax);
    if (clamped != s->value) {
        s->value = clamped;
        s->velocity = 0.0f;           /* 撞到限位就卸掉速度，避免持续顶住 */
    }
    apply(s);
}

void ui_spring_tick(void)
{
    static uint32_t last;
    uint32_t now = lv_tick_get();
    if (last == 0) {
        last = now;
    }
    float dt = (now - last) / 1000.0f;
    last = now;
    if (dt <= 0.0f) {
        dt = SPRING_TICK_MS / 1000.0f;
    }
    if (dt > SPRING_DT_MAX) {
        dt = SPRING_DT_MAX;           /* 夹紧：掉帧时宁可慢一点也不跳变 */
    }

    for (ui_spring_t *s = s_head; s; s = s->next) {
        spring_step(s, dt);
    }
}

static void spring_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_spring_tick();
}

static void ensure_timer(void)
{
    if (!s_timer) {
        s_timer = lv_timer_create(spring_timer_cb, SPRING_TICK_MS, NULL);
        ESP_LOGI(TAG, "spring controller started (%d ms)", SPRING_TICK_MS);
    }
}

void ui_spring_init(ui_spring_t *s, float initial, float vmin, float vmax,
                    ui_spring_apply_cb cb, void *user)
{
    if (!s) {
        return;
    }
    s->value     = clampf(initial, vmin, vmax);
    s->velocity  = 0.0f;
    s->target    = s->value;
    s->stiffness = SPRING_STIFFNESS_DEFAULT;
    s->damping   = SPRING_DAMPING_DEFAULT;
    s->vmin      = vmin;
    s->vmax      = vmax;
    s->held      = false;
    s->done_fired = true;   /* 初始"已在位"：不触发到位回调，等改过 target 才武装 */
    s->last_tick = 0;
    s->apply     = cb;
    s->user      = user;
    s->done      = NULL;
    s->done_user = NULL;
    s->next      = s_head;
    s_head       = s;

    ensure_timer();
    apply(s);
}

void ui_spring_set_done_cb(ui_spring_t *s, ui_spring_done_cb cb, void *user)
{
    if (!s) {
        return;
    }
    s->done      = cb;
    s->done_user = user;
}

void ui_spring_set_target(ui_spring_t *s, float target)
{
    if (!s) {
        return;
    }
    float t = clampf(target, s->vmin, s->vmax);
    if (t != s->target) {
        s->done_fired = false;   /* 目标变了：重新武装到位回调 */
    }
    s->target = t;
    /* 不重置 velocity：这正是"速度连续"的来源 */
}

void ui_spring_hold(ui_spring_t *s)
{
    if (!s) {
        return;
    }
    s->held = true;
    s->velocity = 0.0f;
    s->last_tick = lv_tick_get();
}

void ui_spring_set_value(ui_spring_t *s, float v)
{
    if (!s) {
        return;
    }
    v = clampf(v, s->vmin, s->vmax);

    /* 按 tick 差估算手指速度并做一阶平滑，供松手时继承 */
    uint32_t now = lv_tick_get();
    if (s->last_tick != 0) {
        float dt = (now - s->last_tick) / 1000.0f;
        if (dt > 0.001f && dt < 0.2f) {
            float inst = (v - s->value) / dt;
            s->velocity = s->velocity * 0.7f + inst * 0.3f;
        }
    }
    s->last_tick = now;
    s->value = v;
    /* 拖动期间目标跟手，松手后再改；目标变化同样要重新武装到位回调 */
    if (v != s->target) {
        s->done_fired = false;
    }
    s->target = v;
    apply(s);
}

void ui_spring_release(ui_spring_t *s, float target)
{
    if (!s) {
        return;
    }
    s->held = false;
    s->last_tick = 0;
    ui_spring_set_target(s, target);   /* 速度保留 → 从当前位置顺势继续 */
}

float ui_spring_value(const ui_spring_t *s)
{
    return s ? s->value : 0.0f;
}

bool ui_spring_settled(const ui_spring_t *s)
{
    if (!s) {
        return true;
    }
    return !s->held &&
           fabsf(s->target - s->value) < SPRING_SLEEP_EPS &&
           fabsf(s->velocity) < SPRING_SLEEP_EPS;
}