/*
 * ui_volume_panel.cc —— 音量悬浮条（屏幕**靠右居中**，自右边缘滑入）
 *
 * 交互（按需求）：
 *   - 播放器页点音量键 / 硬件 VOL± 按键 → 一根**竖向**音量条从右边缘滑入，
 *     停在屏幕右侧的垂直中间位置；
 *   - 按住上下拖动跟手调节：**往上拖 = 音量变大**，填充条自下而上增长；
 *   - 硬件 VOL±：按一下步进一格（VP_STEP %），长按连续步进（见 ui_volume_key_step）；
 *   - **非焦点（没人碰它）就自动缩回**：最后一次触摸/按键之后 VP_IDLE_MS 缩回；
 *     手指还按在条上时**绝不**缩回（见 vp_tick_cb 的注释），
 *     控制中心/抽屉/换页/息屏这些"焦点被拿走"的场合**立刻**缩回。
 *
 * 条内布局（按需求：上方数值、下方图标）：
 *   ┌────┐
 *   │ 55 │  ← 当前音量数值（深色药丸底，压在琥珀填充或深色底槽上都清楚）
 *   │    │
 *   │ ▓▓ │  ← 填充自下而上长
 *   │ 🔊 │  ← 音量图标（随音量在 静音/中/最大 之间切换）
 *   └────┘
 *
 * 方向约定（这里是曾经的一个 bug，务必别改回去）：
 *   顶部 = 100%、底部 = 0%，填充从**底部**向上长。手指往上拖 → 音量变大 →
 *   填充上沿跟着手指往上走，拖动方向与视觉方向一致。
 *   旧实现把填充锚在顶部（LV_ALIGN_TOP_MID），于是"往上拖"的时候填充的
 *   下沿反而往下跑 —— 看起来拖动方向和填充方向正好相反，本次修复。
 *
 * 动画复用 ui_spring（docs/UI-suggest.md 的"只改 target"模型）：
 *   滑入/滑出都是改目标值，中途再触摸由手指接管，不回跳、不顿。
 *
 * 位置：建在 lv_layer_top() 上，且**在左右边缘手势热区之后创建**，因此
 *   显示时它压在右边缘热区之上（拖音量不会误触功能抽屉）；
 *   隐藏时整块在屏幕外，LVGL 命中测试不到它，右边缘手势照常工作。
 */
#include "ui_core.h"
#include "ui_internal.h"
#include "ui_metrics.h"
#include "ui_spring.h"

#include <math.h>
#include "esp_log.h"

#include "bsp.h"
#include "audio_service.h"

static const char *TAG = "ui_vol";

#define VP_W          UI_PX_W(64)    /* 面板宽 */
#define VP_H          UI_PX_H(260)   /* 面板高 */
#define VP_PAD        UI_PX_H(8)
#define VP_IDLE_MS    2500           /* 最后一次交互后多久自动缩回 */
#define VP_TICK_MS    100            /* 自动缩回巡检周期 */
#define VP_STEP       5              /* 硬件 VOL± 每次步进的音量百分比（一格） */
#define VP_KEY_TICK   40             /* 抽按键步进的周期（ms），任意任务写入、这里消费 */

/*
 * 图标切换门限：>= 该值用"中音量"图标，>= VP_ICON_LOUD 用"最大音量"。
 * 单调、无回滞，音量来回扫不会闪烁。
 */
#define VP_ICON_LOW   34
#define VP_ICON_LOUD  67

static lv_obj_t   *s_panel;
static lv_obj_t   *s_fill;
static lv_obj_t   *s_pct;          /* 条内上方：音量数值 */
static lv_obj_t   *s_icon;         /* 条内下方：音量图标 */
static ui_spring_t s_spr_x;        /* 值 = 面板 x：UI_W（藏）→ UI_W-VP_W（出） */
static bool        s_visible;
static uint32_t    s_last_act;     /* 最后一次交互（触摸/按键）的时刻 */

/* 标记"刚刚有人操作过"：所有交互入口都调它，自动缩回计时从这里重新数 */
static void vp_touch(void)
{
    s_last_act = lv_tick_get();
}

/*
 * 硬件按键累计的步数（+ 增大 / - 减小）。
 *
 * 按键回调跑在 iot_button 的 esp_timer 周期任务里，**不能**在那里碰 LVGL，
 * 所以那边只做一次加法，真正的音量与 UI 更新由这里的 lv_timer 在 LVGL
 * 任务上下文消费。写入方只有这一个定时器线程（iot_button 全部按键共用
 * 一个 esp_timer 回调，本身是串行的），读写在 32 位对齐整型上是原子的。
 */
static volatile int32_t s_key_pending;

/* 面板 x 由弹簧值派生：唯一一处把运动写进 LVGL */
static void vp_apply_cb(float v, void *user)
{
    (void)user;
    if (!s_panel) {
        return;
    }
    lv_obj_set_x(s_panel, (int32_t)lroundf(v));
    /* 完全缩回时置为"不可点"，避免残留状态干扰（此时本就在屏幕外） */
    if (v >= (float)UI_W - 0.5f) {
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void vp_refresh(void)
{
    if (!s_fill) {
        return;
    }
    int vol = audio_service_get_volume();
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    /* 填充自下而上增长：顶部=100%，所以高度就是百分比 */
    lv_obj_set_height(s_fill, LV_PCT(vol));
    if (s_pct) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", vol);
        lv_label_set_text(s_pct, buf);
    }
    if (s_icon) {
        /* 音量图标跟档位走：静音 / 中 / 最大 —— 0 时同时换成暗色，一眼看出是静音 */
        const char *sym;
        lv_color_t  col;
        if (vol == 0) {
            sym = LV_SYMBOL_MUTE;
            col = UI_COLOR_FG_DIM;
        } else if (vol < VP_ICON_LOW) {
            sym = LV_SYMBOL_VOLUME_MID;
            col = UI_COLOR_FG;
        } else if (vol < VP_ICON_LOUD) {
            sym = LV_SYMBOL_VOLUME_MID;
            col = UI_COLOR_ACCENT;
        } else {
            sym = LV_SYMBOL_VOLUME_MAX;
            col = UI_COLOR_ACCENT;
        }
        lv_label_set_text(s_icon, sym);
        lv_obj_set_style_text_color(s_icon, col, LV_PART_MAIN);
    }
}

/* 触点 y → 音量百分比。必须用屏幕绝对坐标（与控制中心滑条同一个坑） */
static void vp_follow_pointer(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(s_panel, &a);
    int h = lv_area_get_height(&a);
    if (h <= 0) {
        return;
    }
    int rel = (int)p.y - a.y1;
    if (rel < 0) rel = 0;
    if (rel > h) rel = h;
    /* 顶部 = 100%，往上拖就是变大，与填充增长方向一致 */
    int vol = 100 - rel * 100 / h;
    audio_service_set_volume(vol);
    vp_refresh();
}

void ui_volume_show(void)
{
    if (!s_panel) {
        return;
    }
    vp_refresh();
    vp_touch();                 /* 重新开始"没人碰就缩回"的计时 */
    s_visible = true;
    lv_obj_move_foreground(s_panel);
    ui_spring_set_target(&s_spr_x, (float)(UI_W - VP_W));
}

void ui_volume_hide(void)
{
    if (!s_panel || !s_visible) {
        return;
    }
    s_visible = false;
    ui_spring_set_target(&s_spr_x, (float)UI_W);
    ESP_LOGD(TAG, "hide");
}

/* ------------------------------------------------------------------ 硬件按键 */

void ui_volume_key_step(int dir)
{
    if (dir == 0) {
        return;
    }
    int32_t v = s_key_pending;
    /* 夹紧：息屏期间 LVGL 定时器不跑，避免长时间按住后解屏"爆发"式加减 */
    if (dir > 0) {
        v = (v > 60) ? 60 : v + 1;
    } else {
        v = (v < -60) ? -60 : v - 1;
    }
    s_key_pending = v;
}

static void vp_key_cb(lv_timer_t *t)
{
    (void)t;
    int32_t steps = s_key_pending;
    if (steps == 0) {
        return;
    }
    s_key_pending = 0;

    int vol = audio_service_get_volume() + (int)steps * VP_STEP;
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    audio_service_set_volume(vol);
    ui_volume_show();      /* 按键即弹条，并重新开始自动隐藏计时 */
    /* 用 DEBUG：长按连续调整时这一步会以 ~8 次/秒的频率发生，
     * 走 INFO 会把串口 monitor 刷满（按键本身已在 bsp 里按次打 INFO） */
    ESP_LOGD(TAG, "volume key %+d step -> %d%%", (int)steps, vol);
}

/* ------------------------------------------------------------------ 事件 */

/*
 * 自动缩回巡检（LVGL 任务，100 ms 一次）。
 *
 * 这里刻意**不**用"可重置的 lv_timer"来计时：那种写法只要有一次交互忘了
 * reset（或者 reset 与 pause 的顺序被改动），条子就会永久留在屏幕上；
 * 反过来，定时器在手指按住不动的期间照样到点，条子会从手指底下被抽走
 * （旧实现就是这个毛病：LV_EVENT_PRESSING 只在滑动时才有，按住不动
 * 就等不到 reset）。
 *
 * 现在只有"最后一次交互时刻"这一个状态：
 *   - 手指还按在条上（LV_STATE_PRESSED，PRESS_LOCK 生效时滑出面板也算）
 *     → 每 100 ms 刷新一次时刻，永远不会缩回；
 *   - 没人碰且已经过了 VP_IDLE_MS → 缩回，并停在这里等下一次 ui_volume_show。
 */
static void vp_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_visible) {
        return;
    }
    if (s_panel && lv_obj_has_state(s_panel, LV_STATE_PRESSED)) {
        vp_touch();
        return;
    }
    if (lv_tick_elaps(s_last_act) >= VP_IDLE_MS) {
        ESP_LOGD(TAG, "auto hide (idle %u ms)", (unsigned)lv_tick_elaps(s_last_act));
        ui_volume_hide();
    }
}

static void on_vp_press(lv_event_t *e)
{
    (void)e;
    vp_touch();
    vp_follow_pointer();
}

static void on_vp_release(lv_event_t *e)
{
    (void)e;
    vp_follow_pointer();
    vp_touch();
    ESP_LOGI(TAG, "volume %d%%", audio_service_get_volume());
}

lv_obj_t *ui_volume_panel_create(lv_obj_t *parent)
{
    s_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, VP_W, VP_H);
    lv_obj_set_style_radius(s_panel, VP_W / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_panel, UI_COLOR_BG_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel, VP_PAD, LV_PART_MAIN);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    /* 手指纵向滑出面板后仍保持按下目标（与控制中心滑条同理） */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_panel, on_vp_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_panel, on_vp_press, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_panel, on_vp_release, LV_EVENT_RELEASED, NULL);

    /*
     * 位置：屏幕最右侧，**垂直居中**（需求："屏幕靠右中间"）。
     * y 是固定的，运动只发生在 x 上（由弹簧驱动）。
     */
    lv_obj_set_x(s_panel, UI_W);
    lv_obj_set_y(s_panel, (UI_H - VP_H) / 2);

    /* 填充：自下而上，高度 = 音量百分比（顶部 = 100%） */
    s_fill = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_fill);
    lv_obj_set_width(s_fill, LV_PCT(100));
    lv_obj_set_height(s_fill, LV_PCT(50));
    lv_obj_align(s_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_fill, VP_W / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_fill, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_fill, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 条内**上方**：当前音量数值。
     *
     * 为什么要给它垫一层深色半透明底：填充条会从下面顶上来，读数背后的颜色
     * 既可能是琥珀色（已填充）也可能是深色底槽（未填充），任何单一字色都会在
     * 某一端失去对比度；垫一层深色底 + 浅色字则两种情况都清晰。
     * 面板净宽只有 VP_W - 2*VP_PAD，所以这里用 16 px 字体（"100" 约 30 px 宽），
     * 刚好落在面板顶部半圆的有效宽度里。
     */
    s_pct = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_pct, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pct, UI_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pct, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pct, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pct, UI_PX_H(9), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_pct, UI_PX_W(5), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_pct, UI_PX_H(1), LV_PART_MAIN);
    lv_label_set_text(s_pct, "55");
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, 0);

    /*
     * 条内**下方**：音量图标（静音 / 中 / 最大）。
     *
     * 尺寸固定成"小图标"：字体用 montserrat_20（UI_FONT_NUM），**并且把 label 的
     * 盒子也写死**（宽 28 / 高 26），文字居中。这样无论字形怎么变都不会溢出面板 ——
     * 面板净宽只有 VP_W - 2*VP_PAD = 48 px，而面板上下两端是半径 32 的半圆，
     * 越靠边可用宽度越窄；之前用 montserrat_40（40 px 字形 + 深色底 + 左右 padding
     * 约 48 px 宽）正好顶到弧边上，看起来就是"图标比条还宽"。
     * 深色药丸底保留：填充条会从下面顶上来，垫一层深色底才能两端都看得清。
     */
    s_icon = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_icon, UI_FONT_NUM, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_icon, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_icon, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(s_icon, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_size(s_icon, UI_PX_H(28), UI_PX_H(26));
    lv_label_set_text(s_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(s_icon, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 用弹簧驱动 x：UI_W（完全在屏幕右侧之外）起步 */
    ui_spring_init(&s_spr_x, (float)UI_W, (float)(UI_W - VP_W), (float)UI_W,
                   vp_apply_cb, NULL);

    /* 硬件 VOL± 步进 + 自动缩回巡检：都在 LVGL 任务上下文 */
    lv_timer_create(vp_key_cb, VP_KEY_TICK, NULL);
    lv_timer_create(vp_tick_cb, VP_TICK_MS, NULL);

    vp_refresh();
    ESP_LOGI(TAG, "volume panel ready (w=%d h=%d y=%d)", (int)VP_W, (int)VP_H,
             (int)((UI_H - VP_H) / 2));
    return s_panel;
}
