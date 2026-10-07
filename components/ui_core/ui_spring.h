/*
 * ui_spring.h —— 常驻弹簧动画控制器（ui_core 内部）
 *
 * 按 docs/UI-suggest.md 的建议实现"连续 + 可打断"动画：
 *   **不做"停掉动画 A → 新建动画 B"**，而是常驻一份运动状态
 *   {value, velocity, target}，任何时刻只改 target，由物理继续推进。
 *
 * 这样得到的性质：
 *   - 位置连续：改 target 时从当前 value 出发，不会回跳
 *   - 速度连续：拖动中松手时保留手指速度，不会"顿一下"
 *   - 可打断：动画进行到一半，用户再触摸 → hold() 冻结物理、手指从当前
 *     位置接管；松手 → release() 只改 target
 *   - 不抖动：一个对象一个 spring，不存在两个动画抢同一属性
 *
 * 由一个 16ms 的 lv_timer 统一驱动全部 spring（dt 取真实 tick 差并夹紧，
 * 避免某一帧被 Wi-Fi/Flash 卡住后动画"飞出去"）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* value 变化时由控制器回调，调用方在这里把值写到 LVGL 对象上 */
typedef void (*ui_spring_apply_cb)(float v, void *user);

struct ui_spring;
typedef struct ui_spring ui_spring_t;

/* 到位回调：弹簧收敛到 target 时调用一次。用于"动画跑完才做的事"
 * （例如返回手势要等页面滑出屏幕后再退栈）。**改 target 会重新武装**，
 * 所以它同时也是"打断后不再触发"的保证。 */
typedef void (*ui_spring_done_cb)(ui_spring_t *s, void *user);

struct ui_spring {
    float value;        /* 当前位置 */
    float velocity;     /* 当前速度（像素/秒） */
    float target;       /* 目标位置：唯一需要改的量 */
    float stiffness;    /* 刚度 k */
    float damping;      /* 阻尼 c（临界阻尼约 2*sqrt(k)） */
    float vmin, vmax;   /* 行程限位 */
    bool  held;         /* 手指接管中：物理冻结 */
    bool  done_fired;   /* 本次 target 的到位回调是否已发过 */
    uint32_t last_tick; /* 拖动速度估算用 */
    ui_spring_apply_cb apply;
    void *user;
    ui_spring_done_cb done;
    void *done_user;
    ui_spring_t *next;        /* 控制器内部链表 */
};

/* 注册一个 spring 并立即 apply 一次初始值 */
void ui_spring_init(ui_spring_t *s, float initial, float vmin, float vmax,
                    ui_spring_apply_cb apply, void *user);

/* 只改目标（动画/回弹/打断都走这里） */
void ui_spring_set_target(ui_spring_t *s, float target);

/* 注册到位回调（可选）：弹簧收敛到 target 时调用一次 */
void ui_spring_set_done_cb(ui_spring_t *s, ui_spring_done_cb cb, void *user);

/* 手指接管：冻结物理 */
void ui_spring_hold(ui_spring_t *s);

/* 拖动中直接赋值（内部按 tick 差估算并平滑速度） */
void ui_spring_set_value(ui_spring_t *s, float v);

/* 松手：解除冻结，保留当前速度，只改目标 —— 速度连续的关键 */
void ui_spring_release(ui_spring_t *s, float target);

float ui_spring_value(const ui_spring_t *s);
bool  ui_spring_settled(const ui_spring_t *s);

/* 手动推进所有 spring 一帧（一般不用，timer 会自动驱动） */
void ui_spring_tick(void);

#ifdef __cplusplus
}
#endif