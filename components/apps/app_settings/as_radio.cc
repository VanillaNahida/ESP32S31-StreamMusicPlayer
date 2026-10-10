/*
 * as_radio.cc —— 无线总开关（Wi-Fi / 蓝牙）的**全部**跨任务面
 *
 * 原 app_settings.cc：
 *   - s_radio_busy（56）与 49–55 的注释
 *   - settings_on_delete（267–276）→ 仍留主页（它只管主页摘要），不在这里
 *   - radio_toggle_task（285–295）
 *   - on_radio_switch（297–329）
 *   - radio_sw_register（341–350）/ radio_sw_deleted（357–365）/ radio_sw_sync（373–394）
 *   - on_radio_switch_clicked（397–400）
 *   - make_switch_row（450–529）→ as_radio_switch_row
 *
 * ⚠ 本文件是组件里**唯一**有跨任务共享变量的地方：
 *   `s_radio_busy` 由 LVGL 任务读写、由 radio_sw 任务清 0。拆分时按蓝图 R1 的
 *   建议保留 `volatile`（语义与拆分前完全一致，不加原子/信号量 ⇒ 零行为变化）。
 */
#include "as_radio.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_core.h"
#include "ui_metrics.h"

#include "wifi_service.h"
#include "bt_service.h"

#include "as_widgets.h"

/*
 * 无线总开关。
 *   - `s_radio_busy`：1 = 正在切 Wi-Fi，2 = 正在切蓝牙（切换过程中开关位置不刷，
 *     免得跟手指打架）；
 *   - 开关控件本身登记在 s_radio_sw[] 里 —— 设置主页一行、Wi-Fi / 蓝牙子页顶部各一行，
 *     它们是**同一个开关的两个实例**，由 as_radio_sw_sync()（原 radio_sw_sync()）一起拉齐。
 */
static volatile int s_radio_busy;

int as_radio_busy(void)
{
    return (int)s_radio_busy;
}

/*
 * 无线总开关（需求："给 wifi 和蓝牙添加开关，记忆开关状态，只有开启的时候才使用，
 * 不开启的话就释放资源"）：
 *   - 动作在**后台任务**里做：init/deinit 都是阻塞的（几百毫秒~1 秒），
 *     不能在 LVGL 任务里跑；
 *   - 开关位置由各页 tick 按服务真实状态刷新（切换过程中先不刷，免得打架）。
 */
static void radio_toggle_task(void *arg)
{
    const int which = (int)(intptr_t)arg;
    if (which == 1) {
        wifi_service_set_enabled(!wifi_service_is_enabled());
    } else {
        bt_service_set_enabled(!bt_service_is_enabled());
    }
    s_radio_busy = 0;
    vTaskDelete(NULL);
}

static void on_radio_switch(lv_event_t *e)
{
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    /*
     * 值变了也拦下冒泡：这行本身是"点进子页"的，开关上的事件不该再传到行上
     * （CLICKED 那条路见 on_radio_switch_clicked）。
     */
    lv_event_stop_bubbling(e);
    if (s_radio_busy) {
        return;                     /* 上一个还在切，忽略这次（tick 会把位置刷回来） */
    }
    lv_obj_t *sw = lv_event_get_target_obj(e);
    const bool want_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    s_radio_busy = which;
    /*
     * ★ 必须**钉在 BTC 核**上跑（和 main 里的 radio_init_task 一样）：
     * esp_bt_audio 的 A2DP source 注册会和 BTC 任务里的 deinit 路径抢一个静态标志
     * （btc_av.c: g_a2dp_on_deinit），不同核/不同优先级下会撞成
     *   ESP_ERROR_CHECK failed: ESP_ERR_INVALID_STATE ... a2dp_src_register_sbc_sep(seid)
     * 然后 abort（用户实测的这次崩溃就是"关掉再打开蓝牙"触发的）。
     */
    if (xTaskCreatePinnedToCore(radio_toggle_task, "radio_sw", 8192, (void *)(intptr_t)which, 3,
                                NULL, CONFIG_BT_BLUEDROID_PINNED_TO_CORE) != pdPASS) {
        s_radio_busy = 0;
        ui_toast("任务创建失败", 1500);
        return;
    }
    if (which == 1) {
        ui_toast(want_on ? "Wi-Fi 开启中…" : "Wi-Fi 已关闭（资源已释放）", 1800);
    } else {
        ui_toast(want_on ? "蓝牙开启中…" : "蓝牙已关闭（资源已释放）", 1800);
    }
}

/* ---- 开关实例表：主页一行 + 子页顶部一行，共用一个同步函数 ---- */

#define RADIO_SW_MAX 4

static struct {
    int       which;
    lv_obj_t *sw;
} s_radio_sw[RADIO_SW_MAX];

static void radio_sw_register(int which, lv_obj_t *sw)
{
    for (size_t i = 0; i < RADIO_SW_MAX; i++) {
        if (!s_radio_sw[i].sw) {
            s_radio_sw[i].which = which;
            s_radio_sw[i].sw    = sw;
            return;
        }
    }
}

/*
 * ★ 开关对象销毁时必须注销。
 *   子页会被反复 push/pop，页面一销毁那个 lv_switch 就没了；不注销的话
 *   as_radio_sw_sync() 会拿一个已释放的指针去 add/remove_state（use-after-free）。
 */
static void radio_sw_deleted(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    for (size_t i = 0; i < RADIO_SW_MAX; i++) {
        if (s_radio_sw[i].sw == sw) {
            s_radio_sw[i].sw = NULL;
        }
    }
}

/*
 * 把所有开关实例拉回"服务真实状态"。
 *
 * 每个页面各自的 500 ms tick 都会调它（settings_tick / wifi_tick / bt_tick），
 * 所以不管当前在哪一页，主页与子页的开关都是同一个状态 —— 这就是"双向同步"。
 */
void as_radio_sw_sync(void)
{
    if (s_radio_busy) {
        return;                     /* 正在切：等服务给出结果再对齐 */
    }
    for (size_t i = 0; i < RADIO_SW_MAX; i++) {
        lv_obj_t *sw = s_radio_sw[i].sw;
        if (!sw) {
            continue;
        }
        const bool on = (s_radio_sw[i].which == AS_RADIO_WIFI) ? wifi_service_is_enabled()
                                                               : bt_service_is_enabled();
        if (on == lv_obj_has_state(sw, LV_STATE_CHECKED)) {
            continue;
        }
        if (on) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
    }
}

/* 点开关图案：只切开关。顺带拦下 CLICKED，别冒泡到行上变成"进子页" */
static void on_radio_switch_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

/*
 * 开关行（需求：开关与"进入子页"合并成一行，别再像以前那样
 * "上面一行开关 + 下面一行同名条目"）：
 *
 *     [图标] [名称 / 状态]  [开关] [›]
 *      └──────── 点这些进子页 ─────┘ └只切换┘
 *
 *   - 点**开关图案** → 只切开关，不跳页；
 *   - 点行里其它任何地方（图标 / 名称 / 状态 / › / 空白）→ 进子页；
 *   - 子页顶部放**同一个**开关（sub 传 NULL），两边状态由 as_radio_sw_sync()
 *     （原 radio_sw_sync()）对齐，子页里改完返回主页立刻一致。
 *
 * 两个实现要点：
 *   1) lv_switch 被点会发 CLICKED，而 LVGL 默认让事件**冒泡**到父对象 ——
 *      不挡住的话"点开关"会连带触发行的"进子页"（见 on_radio_switch_clicked）；
 *   2) 开关要能被各页的 tick 刷到，所以登记进 s_radio_sw[]，销毁时注销。
 *
 * @param sub        非 NULL = 点这一行进该子页；NULL = 只做开关（子页顶部用）
 * @param status_out 状态文字 label（可为 NULL）；主页两行由 settings_tick 填摘要
 */
lv_obj_t *as_radio_switch_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                              const char *name, int which,
                              const subpage_desc_t *sub, lv_obj_t **status_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(36), UI_PX_H(36));
    lv_obj_set_style_radius(icon, UI_PX_H(9), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    /* 不可点：点图标要落到行上（= 进子页），和名称区域一致 */
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    /* 名称 + 状态两行；状态那行由 tick 填摘要（"已连接 xx · 192.168.x.x · -50 dBm"） */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, UI_PX_H(2), LV_PART_MAIN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *nm = lv_label_create(col);
    lv_obj_set_style_text_font(nm, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(nm, name);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, LV_PCT(100));

    if (status_out) {
        lv_obj_t *st = lv_label_create(col);
        lv_obj_set_style_text_font(st, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(st, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(st, "…");
        lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
        lv_obj_set_width(st, LV_PCT(100));
        *status_out = st;
    }

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, UI_PX_W(52), UI_PX_H(28));
    lv_obj_set_style_bg_color(sw, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_COLOR_ACCENT,
                              (lv_style_selector_t)LV_PART_INDICATOR |
                                  (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_radio_switch, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)which);
    lv_obj_add_event_cb(sw, on_radio_switch_clicked, LV_EVENT_CLICKED, NULL);
    radio_sw_register(which, sw);
    lv_obj_add_event_cb(sw, radio_sw_deleted, LV_EVENT_DELETE, NULL);

    if (sub && sub->create) {
        lv_obj_t *chev = lv_label_create(row);
        lv_obj_set_style_text_font(chev, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(chev, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_remove_flag(chev, LV_OBJ_FLAG_CLICKABLE);   /* 点它也是进子页 */
        lv_obj_add_event_cb(row, as_on_switch_row_click, LV_EVENT_CLICKED, (void *)sub);
    }
    return row;
}
