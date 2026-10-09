/*
 * app_settings.cc —— 设置页：Wi-Fi / 蓝牙 / 声音与输出 / 显示 / 存储 / 关于
 *
 * 结构（开发指南 5.16 + 需求）：
 *   一级页：分组条目 + 当前值摘要，点进二级页
 *   二级页：Wi-Fi（扫描/连接/保存/忘记）、蓝牙（扫描/配对状态/忘记）、
 *           声音与输出（板载功放 / 蓝牙(未接入) / 静音 + 音量）
 *
 * 规矩：
 *   - 服务（wifi_service / bt_service）只在后台任务里初始化，UI 用 lv_timer
 *     轮询状态；**绝不在事件回调里碰 LVGL**
 *   - 列表只在数据"签名"变化时重建，避免每 500 ms 重铺控件
 */
#include "app_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_app_desc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_ba.h"          /* 碧蓝档案风格弹窗工具箱（Wi-Fi 密码浮层用） */
#include "ui_app_registry.h"

#include "bsp.h"
#include "audio_service.h"
#include "wifi_service.h"
#include "bt_service.h"

static const char *TAG = "app_settings";

/*
 * 无线总开关。
 *   - `s_radio_busy`：1 = 正在切 Wi-Fi，2 = 正在切蓝牙（切换过程中开关位置不刷，
 *     免得跟手指打架）；
 *   - 开关控件本身登记在 s_radio_sw[] 里 —— 设置主页一行、Wi-Fi / 蓝牙子页顶部各一行，
 *     它们是**同一个开关的两个实例**，由 radio_sw_sync() 一起拉齐。
 */
static volatile int s_radio_busy;

/* ------------------------------------------------------------------ 公共部件 */

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* pad_all 在前、pad_top 在后：否则状态栏那 32px 会被 pad_all 覆盖掉 */
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PAGE_PAD, LV_PART_MAIN);
    return scr;
}

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_router_pop();
}

static lv_obj_t *make_flat_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, UI_PX_H(34));
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, UI_PX_H(17), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, UI_PX_W(14), LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

/*
 * 二级页外壳：返回钮 + 标题（+ 可选的右上角动作按钮）。
 * 返回 body 容器（纵向可滚动列），调用方往里面塞内容。
 */
static lv_obj_t *make_page_shell(lv_obj_t *scr, const char *title,
                                 const char *action_text, lv_event_cb_t action_cb)
{
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_top(body, UI_PX_H(44), LV_PART_MAIN);   /* 给标题行让位 */
    lv_obj_set_style_pad_row(body, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), UI_PX_H(38));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    /*
     * ★ 让开左边缘手势热区（UI_EDGE_HOTZONE，挂在 lv_layer_top 上、z 序高于页面）：
     * head 是 flex 行、返回键是第一个子对象，不加这个内边距就会落在屏幕 pad
     * （UI_PAGE_PAD = 8 px）处 —— 左半截压在热区里，按了没反应。
     * 宽度仍是 100%，加的是内边距，不会溢出。
     */
    lv_obj_set_style_pad_left(head, UI_BACK_BTN_X, LV_PART_MAIN);

    lv_obj_t *b = lv_button_create(head);
    lv_obj_set_size(b, UI_PX_H(34), UI_PX_H(34));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_t *bl = lv_label_create(b);
    lv_obj_set_style_text_font(bl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(head);
    lv_obj_set_style_text_font(t, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(t, title);

    if (action_text && action_cb) {
        /* 注意：head 是 flex 容器，子对象的位置由 flex 决定，lv_obj_align 会被
         * 布局覆盖 —— 所以右上角的动作按钮挂到 screen 上绝对定位 */
        lv_obj_t *a = make_flat_btn(scr, action_text, action_cb, NULL);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return body;
}

/* 一行"图标 + 标题 + 右侧值"；返回行对象，*value_out 收到值 label（可为 NULL） */
static lv_obj_t *make_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                          const char *text, lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(40), UI_PX_H(40));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_font(name, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(name, text);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, UI_PX_H(52), 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_width(val, UI_PCT_X(30));
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -UI_PX_W(18), 0);
    if (value_out) {
        *value_out = val;
    }
    return row;
}

/* 列表里的小按钮（忘记 / 连接），挂在行右侧 */
/*
 * 行的回调参数必须是**稳定副本**：
 *   - Wi-Fi 列表用的是 wifi_service 的 AP 数组，每次扫描完都会被整体改写，
 *     直接传 ssid 指针，回调触发时可能已经指向别的 AP、甚至空串
 *     （实测日志里出现过 "wifi_svc: forgot ''" —— 点了"忘记"却什么都没忘）；
 *   - 蓝牙列表同理（设备数组会被重建）。
 * 所以这里按字节拷一份挂到按钮上，按钮销毁时释放。
 */
static void row_user_free(lv_event_t *e)
{
    void *p = lv_event_get_user_data(e);
    if (p) {
        free(p);
    }
}

static void *row_user_dup(const void *src, size_t n)
{
    void *p = malloc(n);
    if (p) {
        memcpy(p, src, n);
    }
    return p;
}

static lv_obj_t *make_row_btn(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = make_flat_btn(row, text, cb, user);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    return b;
}

/* 带"拷贝一份参数"的版本：src/n 是回调要用的数据（SSID 字符串或 6 字节 BDA） */
static lv_obj_t *make_row_btn_copy(lv_obj_t *row, const char *text, lv_event_cb_t cb,
                                   const void *src, size_t n)
{
    void *copy = src ? row_user_dup(src, n) : NULL;
    lv_obj_t *b = make_row_btn(row, text, cb, copy);
    lv_obj_add_event_cb(b, row_user_free, LV_EVENT_DELETE, copy);
    return b;
}

/* ------------------------------------------------------------------ 二级页前置声明 */

static lv_obj_t *page_wifi(void);
static lv_obj_t *page_bt(void);
static lv_obj_t *page_output(void);
static lv_obj_t *page_storage(void);

/* 容量格式化（KB → "12.3 GB" / "345 MB" / "78 KB"）；实现在下面的"存储"一段 */
static void storage_fmt_size_kb(uint32_t kb, char *out, size_t cap);

/* ================================================================== 一级：设置列表 */

enum { SET_WIFI = 0, SET_BT, SET_OUT, SET_DISPLAY, SET_STORAGE, SET_ABOUT, SET_N };

static lv_obj_t *s_sum[SET_N];
static lv_timer_t *s_sum_timer;

static void settings_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_sum_timer) {
        lv_timer_delete(s_sum_timer);
        s_sum_timer = NULL;
    }
    /* 开关控件随页面一起销毁：它们的 LV_EVENT_DELETE 会自己从 s_radio_sw[] 注销 */
    memset(s_sum, 0, sizeof(s_sum));
}

/*
 * 无线总开关（需求："给 wifi 和蓝牙添加开关，记忆开关状态，只有开启的时候才使用，
 * 不开启的话就释放资源"）：
 *   - 动作在**后台任务**里做：init/deinit 都是阻塞的（几百毫秒~1 秒），
 *     不能在 LVGL 任务里跑；
 *   - 开关位置由 settings_tick 按服务真实状态刷新（切换过程中先不刷，免得打架）。
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

enum { RADIO_WIFI = 1, RADIO_BT = 2 };
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
 *   radio_sw_sync() 会拿一个已释放的指针去 add/remove_state（use-after-free）。
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
static void radio_sw_sync(void)
{
    if (s_radio_busy) {
        return;                     /* 正在切：等服务给出结果再对齐 */
    }
    for (size_t i = 0; i < RADIO_SW_MAX; i++) {
        lv_obj_t *sw = s_radio_sw[i].sw;
        if (!sw) {
            continue;
        }
        const bool on = (s_radio_sw[i].which == RADIO_WIFI) ? wifi_service_is_enabled()
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
 * 子页描述：router 的 page_desc_t 里 id/create 是**按值拷贝指针**的，
 * 所以这两个成员必须是静态存储期 —— 用静态常量描述符传给 make_switch_row 即可。
 */
typedef struct {
    lv_obj_t *(*create)(void);
    const char *id;
} subpage_desc_t;

/* Wi-Fi / 蓝牙子页（静态存储期：router 直接存 id/create 指针） */
static const subpage_desc_t SUB_WIFI = { page_wifi, "set_wifi" };
static const subpage_desc_t SUB_BT   = { page_bt,   "set_bt"   };
static const subpage_desc_t SUB_STORAGE = { page_storage, "set_storage" };

/* 点开关行的空白/名称区 = 进子页（点开关图案进不来，见 on_radio_switch_clicked） */
static void on_switch_row_click(lv_event_t *e)
{
    const subpage_desc_t *sp = (const subpage_desc_t *)lv_event_get_user_data(e);
    if (!sp || !sp->create) {
        return;
    }
    page_desc_t p = {};
    p.id      = sp->id;
    p.create  = sp->create;
    p.no_back = true;            /* 子页外壳自带返回钮，别再叠全局返回键 */
    ui_router_push(&p);
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
 *   - 子页顶部放**同一个**开关（sub 传 NULL），两边状态由 radio_sw_sync() 对齐，
 *     子页里改完返回主页立刻一致。
 *
 * 两个实现要点：
 *   1) lv_switch 被点会发 CLICKED，而 LVGL 默认让事件**冒泡**到父对象 ——
 *      不挡住的话"点开关"会连带触发行的"进子页"（见 on_radio_switch_clicked）；
 *   2) 开关要能被各页的 tick 刷到，所以登记进 s_radio_sw[]，销毁时注销。
 *
 * @param sub        非 NULL = 点这一行进该子页；NULL = 只做开关（子页顶部用）
 * @param status_out 状态文字 label（可为 NULL）；主页两行由 settings_tick 填摘要
 */
static lv_obj_t *make_switch_row(lv_obj_t *parent, const char *sym, uint32_t accent,
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
        lv_obj_add_event_cb(row, on_switch_row_click, LV_EVENT_CLICKED, (void *)sub);
    }
    return row;
}

/*
 * 文本没变就别写 label。
 *
 * 设置页的摘要行 / Wi-Fi 状态行都是 500 ms 一次的常驻定时器在刷，而内容绝大多数
 * 时候是不变的；lv_label_set_text() 每次都 free+malloc 新串并整块 lv_obj_invalidate()，
 * 无条件重写会让界面和堆持续抖动。
 *
 * 直接读 label 当前文本做比较，不维护额外缓存（缓存会和别处的直接写入不同步）。
 */
static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

static void settings_tick(lv_timer_t *t)
{
    (void)t;
    /* 总开关位置跟服务真实状态对齐（主页与子页的所有实例一起刷） */
    radio_sw_sync();
    if (!s_sum[SET_WIFI]) {
        return;
    }
    char buf[72];

    if (!wifi_service_is_enabled()) {
        label_set_if_changed(s_sum[SET_WIFI], "已关闭");
    } else switch (wifi_service_state()) {
    case WIFI_ST_CONNECTED:
        snprintf(buf, sizeof(buf), "已连接 %s", wifi_service_conn_ssid());
        break;
    case WIFI_ST_CONNECTING:
        snprintf(buf, sizeof(buf), "连接中 %s", wifi_service_conn_ssid());
        break;
    case WIFI_ST_SCANNING:
        snprintf(buf, sizeof(buf), "扫描中…");
        break;
    case WIFI_ST_FAILED:
        snprintf(buf, sizeof(buf), "连接失败");
        break;
    case WIFI_ST_UNINIT:
        snprintf(buf, sizeof(buf), "初始化中…");
        break;
    default:
        snprintf(buf, sizeof(buf), "未连接");
        break;
    }
    label_set_if_changed(s_sum[SET_WIFI], buf);

    if (bt_service_state() == BT_ST_FAILED) {
        snprintf(buf, sizeof(buf), "不可用");
    } else if (bt_service_is_discovering()) {
        snprintf(buf, sizeof(buf), "搜索中…");
    } else if (bt_service_bonded_count() > 0) {
        snprintf(buf, sizeof(buf), "已配对 %u 台", (unsigned)bt_service_bonded_count());
    } else {
        snprintf(buf, sizeof(buf), "未配对");
    }
    label_set_if_changed(s_sum[SET_BT], buf);

    snprintf(buf, sizeof(buf), "%s · %d%%",
             audio_service_get_output() == AUDIO_OUT_NONE ? "静音" : "扬声器",
             audio_service_get_volume());
    label_set_if_changed(s_sum[SET_OUT], buf);

    label_set_if_changed(s_sum[SET_DISPLAY], bsp_backlight_get() >= 0 ? "无软件背光" : "—");
    /*
     * 存储行摘要：把"已用百分比 + 可用容量"直接摆在设置主页上（用户点进去之前就能看到）。
     * ⚠ esp_vfs_fat_info() 每次都走一遍 FATFS（f_getfree），不能每 500 ms 调一次 ——
     *   这里自己节流到 5 s（容量变化本来就慢，看不出来）。
     */
    {
        static uint32_t s_storage_tick;
        /* 64 字节：`%u` GCC 按最多 10 位算、"已用/·/可用" 是中文（3 字节/字），
         * 40 字节时 -Wformat-truncation 会报"可能被截断" —— 别改小 */
        static char     s_storage_txt[64];
        if (lv_tick_elaps(s_storage_tick) >= 5000) {
            s_storage_tick = lv_tick_get();
            if (!bsp_sdcard_is_mounted()) {
                snprintf(s_storage_txt, sizeof(s_storage_txt), "无 SD 卡");
            } else {
                uint32_t total_kb = 0, free_kb = 0;
                if (bsp_sdcard_capacity_kb(&total_kb, &free_kb) == ESP_OK && total_kb > 0) {
                    const uint32_t used_pct =
                        (uint32_t)(((uint64_t)(total_kb - free_kb) * 100u) / total_kb);
                    char freebuf[24];
                    storage_fmt_size_kb(free_kb, freebuf, sizeof(freebuf));
                    snprintf(s_storage_txt, sizeof(s_storage_txt), "已用 %u%% · 可用 %.20s",
                             (unsigned)used_pct, freebuf);
                } else {
                    snprintf(s_storage_txt, sizeof(s_storage_txt), "SD 已挂载");
                }
            }
        }
        label_set_if_changed(s_sum[SET_STORAGE], s_storage_txt);
    }
    label_set_if_changed(s_sum[SET_ABOUT], esp_app_get_description()->version);
}

static void on_setting_row(lv_event_t *e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    page_desc_t p = {};
    /* 二级页外壳 make_page_shell 自带返回钮：no_back 置位，避免全局返回键重复 */
    p.no_back = true;
    switch (id) {
    /*
     * Wi-Fi / 蓝牙不在这里：它们现在走 make_switch_row 自带的行回调
     * （点名称区进子页、点开关只切换），见 on_switch_row_click。
     */
    case SET_OUT:
        p.id = "set_out";
        p.create = page_output;
        break;
    case SET_DISPLAY:
        ui_toast("本板板级配置没有背光器件，软件无法调亮度", 2000);
        return;
    case SET_STORAGE:
        /* 存储现在有详情页：容量 / 可用 / 占用条 / 卡的物理信息（见 page_storage） */
        p.id     = SUB_STORAGE.id;
        p.create = SUB_STORAGE.create;
        break;
    default:
        ui_toast("ESP32-S31-Korvo-1 · ESP-IDF 6.1 · LVGL 9.5", 2200);
        return;
    }
    ui_router_push(&p);
}

static lv_obj_t *page_settings(void)
{
    lv_obj_t *scr  = make_screen();
    lv_obj_t *body = make_page_shell(scr, "设置", NULL, NULL);

    /*
     * 无线总开关 + 进入子页：**合成一行**（需求）。
     * 关掉会真正释放驱动/协议栈资源，状态记在 NVS，开机恢复。
     */
    make_switch_row(body, LV_SYMBOL_WIFI, 0x3E9E86, "Wi-Fi", RADIO_WIFI,
                    &SUB_WIFI, &s_sum[SET_WIFI]);
    make_switch_row(body, LV_SYMBOL_BLUETOOTH, 0x4A6FA5, "蓝牙", RADIO_BT,
                    &SUB_BT, &s_sum[SET_BT]);

    static const struct {
        const char *sym;
        uint32_t    accent;
        const char *name;
        int         id;
    } rows[] = {
        { LV_SYMBOL_VOLUME_MAX, 0xE8973A, "声音与输出",   SET_OUT },
        { LV_SYMBOL_EYE_OPEN,   0x6E6E78, "显示与亮度",   SET_DISPLAY },
        { LV_SYMBOL_SD_CARD,    0x5B6FB5, "存储",         SET_STORAGE },
        { LV_SYMBOL_SETTINGS,   0x6E6E78, "关于本机",     SET_ABOUT },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *row = make_row(body, rows[i].sym, rows[i].accent, rows[i].name,
                                 &s_sum[rows[i].id]);
        lv_obj_add_event_cb(row, on_setting_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)rows[i].id);
    }

    lv_obj_add_event_cb(scr, settings_on_delete, LV_EVENT_DELETE, NULL);
    s_sum_timer = lv_timer_create(settings_tick, 500, NULL);
    settings_tick(NULL);
    ESP_LOGI(TAG, "settings page created");
    return scr;
}

/* ================================================================== Wi-Fi */

static lv_obj_t *s_wifi_status;
static lv_obj_t *s_wifi_list;
static lv_timer_t *s_wifi_timer;
static int        s_wifi_sig = -1;
static bool       s_wifi_dirty;

static void wifi_rebuild_list(void);

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    if (wifi_service_scan_start() != ESP_OK) {
        ui_toast("Wi-Fi 还没就绪，稍后再试", 1600);
        return;
    }
    s_wifi_dirty = true;
}

static void on_wifi_forget(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || !ssid[0]) {                 /* 空串不放行（历史上出现过 forgot ''） */
        ui_toast("列表已刷新，请重新选择", 1500);
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "已忘记 %s", ssid);
    wifi_service_forget(ssid);
    s_wifi_dirty = true;
    ui_toast(buf, 1500);
}

/* ---- 密码输入浮层（用 LVGL 自带键盘，ASCII 就够密码用） ---- */

static lv_obj_t *s_pw_overlay;
static ui_ba_dialog_t s_pw_dlg;     /* BA 对话框句柄（与 s_pw_overlay 同生共死） */
static lv_obj_t *s_pw_ta;
/*
 * 浮层要连的 SSID 自带一份。
 *
 * ★ 不能沿用列表行里那个 ssid_copy：wifi_tick 每 500 ms 跑一次，只要 AP 数量/
 *   连接状态/已保存数量有变就 wifi_rebuild_list() → lv_obj_clean() 把整列删掉，
 *   行上挂的 ssid_copy 会在 LV_EVENT_DELETE 里被 free。而 on_pw_ready 是通过
 *   键盘的 event user_data 拿到那个指针的 —— 用户正在输密码时列表一刷新，
 *   这个指针就成了悬垂指针，按"确认"等于拿已释放内存当 SSID 去连（轻则连错、
 *   重则崩）。自己存一份，生命周期与浮层一致。
 */
static char      s_pw_ssid[WIFI_SSID_MAX];

static void pw_close(void)
{
    if (s_pw_overlay) {
        /*
         * 关键：这个函数是在**键盘的事件回调**里被调的，而键盘正是这个浮层的
         * 子对象。同步 lv_obj_delete 会在事件派发器还在遍历这棵子树时把它删掉
         * → hard fault（工程规范 F4 的经典坑）。必须异步删。
         * ui_ba_close() → ui_popup_out() 内部就是 lv_obj_delete_async()，
         * 顺带还有"下滑 + 淡出"的退场动画，所以这里直接用它。
         */
        ui_ba_close(&s_pw_dlg);
        s_pw_overlay = NULL;               /* 指针先清，DELETE 回调里会再清一次 */
    }
    s_pw_ta = NULL;
}

static void on_pw_overlay_deleted(lv_event_t *e)
{
    (void)e;
    s_pw_overlay = NULL;
    s_pw_ta = NULL;
}

static void on_pw_ready(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    const char *pass = s_pw_ta ? lv_textarea_get_text(s_pw_ta) : "";
    if (ssid && ssid[0]) {
        /*
         * 密码留空 + 确认 = "用已保存的密码再连一次"（需求原话：
         * "点击后要求输入密码的时候留空确认应该重新用已保存的密码连接"）。
         * 连不上时用户往往就是这么操作的，没必要逼他重新输一遍。
         */
        if (!pass[0] && wifi_service_is_saved(ssid)) {
            wifi_service_connect_saved(ssid);
            ui_toast("用已保存的密码连接…", 1200);
        } else {
            wifi_service_connect(ssid, pass, true);
            ui_toast("正在连接…", 1200);
        }
    }
    pw_close();
}

static void on_pw_cancel(lv_event_t *e)
{
    (void)e;
    pw_close();
}

static void wifi_open_pw(const char *ssid)
{
    if (s_pw_overlay || !ssid || !ssid[0]) {
        return;
    }
    /* 自己存一份 SSID（原因见 s_pw_ssid 的注释）：后面 tip 与 READY 回调都用它 */
    snprintf(s_pw_ssid, sizeof(s_pw_ssid), "%s", ssid);

    /*
     * ★ 浮层挂在**当前页面 screen** 上（ui_ba_open_in 的第一个参数），不是
     *   lv_layer_top()：设置页是可以被 pop 掉的，挂在 layer_top 上的浮层会跟着
     *   用户跑到别的页面去，而它的回调还指着本页的状态。
     * 卡片停在上半屏，下半屏留给 LVGL 自带键盘。
     */
    lv_obj_t *scr = lv_screen_active();
    s_pw_dlg = ui_ba_open_in(scr, "连接 Wi-Fi", 64, LV_ALIGN_TOP_MID, UI_PX_H(8));
    s_pw_overlay = s_pw_dlg.scrim;
    lv_obj_add_event_cb(s_pw_overlay, on_pw_overlay_deleted, LV_EVENT_DELETE, NULL);
    ui_ba_on_close(&s_pw_dlg, [](void *user) { (void)user; pw_close(); }, NULL);

    char tip[WIFI_SSID_MAX + 32];
    snprintf(tip, sizeof(tip), "请输入 %.32s 的密码", s_pw_ssid);
    ui_ba_text(s_pw_dlg.body, tip);

    s_pw_ta = ui_ba_input(s_pw_dlg.body, "密码（留空 = 用已保存的密码连接）", NULL);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_max_length(s_pw_ta, 63);

    ui_ba_hint(s_pw_dlg.body, "※按键盘上的 ✓ 直接连接，留空则用已保存的密码重连。");

    ui_ba_btn(s_pw_dlg.btn_row, "取消", false, NULL, on_pw_cancel, NULL);
    ui_ba_btn(s_pw_dlg.btn_row, "连接", true, NULL, on_pw_ready, s_pw_ssid);

    lv_obj_t *kb = lv_keyboard_create(s_pw_overlay);
    lv_obj_set_size(kb, LV_PCT(100), UI_PCT_Y(52));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pw_ta);
    /* 键盘自带的 OK / 关闭键对应 READY / CANCEL 事件（传的是浮层自己那份 SSID） */
    lv_obj_add_event_cb(kb, on_pw_ready, LV_EVENT_READY, s_pw_ssid);
    lv_obj_add_event_cb(kb, on_pw_cancel, LV_EVENT_CANCEL, NULL);
}

static void on_wifi_ap_click(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || !ssid[0]) {
        return;
    }
    const wifi_ap_t *ap = NULL;
    for (size_t i = 0; i < wifi_service_ap_count(); i++) {
        const wifi_ap_t *a = wifi_service_ap(i);
        if (a && strcmp(a->ssid, ssid) == 0) {
            ap = a;
            break;
        }
    }
    if (ap && ap->saved) {
        wifi_service_connect_saved(ssid);
        ui_toast("用已保存的密码连接…", 1200);
        return;
    }
    if (ap && ap->authmode == 0) {          /* 开放网络 */
        wifi_service_connect(ssid, "", true);
        ui_toast("正在连接…", 1200);
        return;
    }
    wifi_open_pw(ssid);                     /* 需要密码 */
}

static void wifi_rebuild_list(void)
{
    if (!s_wifi_list) {
        return;
    }
    lv_obj_clean(s_wifi_list);

    /* 已保存的网络 */
    const size_t nsaved = wifi_service_saved_count();
    if (nsaved > 0) {
        lv_obj_t *h = lv_label_create(s_wifi_list);
        lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(h, "已保存的网络");
        for (size_t i = 0; i < nsaved; i++) {
            const char *ssid = wifi_service_saved_ssid(i);
            if (!ssid) {
                continue;
            }
            lv_obj_t *row = make_row(s_wifi_list, LV_SYMBOL_SAVE, 0x5B6FB5, ssid, NULL);
            /* 行本身也要拷一份：点击时列表可能已经被下一次扫描重建了 */
            void *ssid_copy = row_user_dup(ssid, strlen(ssid) + 1);
            lv_obj_add_event_cb(row, on_wifi_ap_click, LV_EVENT_CLICKED, ssid_copy);
            lv_obj_add_event_cb(row, row_user_free, LV_EVENT_DELETE, ssid_copy);
            make_row_btn_copy(row, "忘记", on_wifi_forget, ssid, strlen(ssid) + 1);
        }
    }

    /* 扫描结果 */
    lv_obj_t *h2 = lv_label_create(s_wifi_list);
    lv_obj_set_style_text_font(h2, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h2, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(h2, "附近的网络");

    const size_t n = wifi_service_ap_count();
    if (n == 0) {
        lv_obj_t *hint = lv_label_create(s_wifi_list);
        lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(hint, wifi_service_is_scanning() ? "扫描中…" : "点右上角「扫描」找网络");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const wifi_ap_t *ap = wifi_service_ap(i);
        if (!ap) {
            continue;
        }
        lv_obj_t *val = NULL;
        lv_obj_t *row = make_row(s_wifi_list, LV_SYMBOL_WIFI, 0x3E9E86, ap->ssid, &val);
        char info[48];
        snprintf(info, sizeof(info), "%d dBm · %s%s",
                 ap->rssi, (ap->authmode == 0) ? "开放" : "加密",
                 ap->saved ? " · 已保存" : "");
        lv_label_set_text(val, info);
        /* 拷一份 SSID：扫描完成后这个数组会被整体改写（见 make_row_btn 上方注释） */
        void *ssid_copy = row_user_dup(ap->ssid, strlen(ap->ssid) + 1);
        lv_obj_add_event_cb(row, on_wifi_ap_click, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(row, row_user_free, LV_EVENT_DELETE, ssid_copy);
    }
}

static void wifi_tick(lv_timer_t *t)
{
    (void)t;
    radio_sw_sync();          /* 本页顶部的总开关也要跟着服务状态走 */
    if (!s_wifi_status) {
        return;
    }
    char buf[128];
    const wifi_state_t st = wifi_service_state();
    switch (st) {
    case WIFI_ST_UNINIT:     snprintf(buf, sizeof(buf), "Wi-Fi 初始化中…"); break;
    case WIFI_ST_SCANNING:   snprintf(buf, sizeof(buf), "扫描中…"); break;
    case WIFI_ST_CONNECTING: snprintf(buf, sizeof(buf), "正在连接 %s …", wifi_service_conn_ssid()); break;
    case WIFI_ST_CONNECTED:
        snprintf(buf, sizeof(buf), "已连接 %s · %s · %d dBm",
                 wifi_service_conn_ssid(), wifi_service_ip(), wifi_service_rssi());
        break;
    case WIFI_ST_FAILED: {
        /* 把服务里记下的真实原因码翻出来显示（密码错 / 没找到 AP / 加密方式不匹配…），
         * 原来只有一个笼统的"密码错误或找不到 AP"，用户没法判断该干什么 */
        const char *why = wifi_service_last_reason_str();
        if (why && why[0]) {
            snprintf(buf, sizeof(buf), "连接失败：%s", why);
        } else {
            snprintf(buf, sizeof(buf), "连接失败（密码错误或找不到 AP）");
        }
        break;
    }
    default:
        snprintf(buf, sizeof(buf), "未连接 · 已保存 %u 个网络",
                 (unsigned)wifi_service_saved_count());
        break;
    }
    label_set_if_changed(s_wifi_status, buf);

    const int sig = (int)wifi_service_ap_count() * 32 + (int)st +
                    (wifi_service_is_scanning() ? 1000 : 0) +
                    (int)wifi_service_saved_count() * 4;
    if (sig != s_wifi_sig || s_wifi_dirty) {
        s_wifi_sig   = sig;
        s_wifi_dirty = false;
        wifi_rebuild_list();
    }
}

static void wifi_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_wifi_timer) {
        lv_timer_delete(s_wifi_timer);
        s_wifi_timer = NULL;
    }
    s_wifi_status = NULL;
    s_wifi_list   = NULL;
    s_wifi_sig    = -1;
}

static lv_obj_t *page_wifi(void)
{
    lv_obj_t *scr  = make_screen();
    lv_obj_t *body = make_page_shell(scr, "Wi-Fi", "扫描", on_wifi_scan);

    /*
     * 顶部放**同一个**总开关（sub 传 NULL：已经在子页里，这行只负责开关）。
     * 它就是设置主页那一行的另一个实例，由 radio_sw_sync() 对齐 ——
     * 在这一页关掉 Wi-Fi，返回主页那个开关立刻也是关的。
     */
    make_switch_row(body, LV_SYMBOL_WIFI, 0x3E9E86, "Wi-Fi", RADIO_WIFI, NULL, NULL);

    s_wifi_status = lv_label_create(body);
    lv_obj_set_style_text_font(s_wifi_status, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_status, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_width(s_wifi_status, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_wifi_status, "…");

    s_wifi_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_width(s_wifi_list, LV_PCT(100));
    lv_obj_set_height(s_wifi_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    wifi_rebuild_list();
    lv_obj_add_event_cb(scr, wifi_on_delete, LV_EVENT_DELETE, NULL);
    s_wifi_timer = lv_timer_create(wifi_tick, 500, NULL);
    wifi_tick(NULL);
    return scr;
}

/* ================================================================== 蓝牙 */

static lv_obj_t *s_bt_status;
static lv_obj_t *s_bt_list;
static lv_timer_t *s_bt_timer;
static int        s_bt_sig = -1;
static bool       s_bt_dirty;

static void bt_rebuild_list(void);

static void on_bt_scan(lv_event_t *e)
{
    (void)e;
    if (bt_service_start_discovery() != ESP_OK) {
        /* 区分"还没初始化完"和"真的起不来"：开机后蓝牙要 1~3 秒才 READY，
         * 这时候点扫描只是太早，不该吓唬用户说蓝牙坏了 */
        if (bt_service_state() == BT_ST_FAILED) {
            ui_toast("蓝牙不可用：控制器/协议栈初始化失败", 2000);
        } else {
            ui_toast("蓝牙正在初始化，稍等 1~2 秒再扫", 1800);
        }
        return;
    }
    s_bt_dirty = true;
}

static void on_bt_forget(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    char addr[18];
    bt_service_addr_str(bda, addr, sizeof(addr));
    if (bt_service_remove_bond(bda) == ESP_OK) {
        char buf[48];
        snprintf(buf, sizeof(buf), "已解除配对 %s", addr);
        ui_toast(buf, 1600);
    } else {
        ui_toast("解除配对失败", 1600);
    }
    s_bt_dirty = true;
}

/* 点设备 = 发起 A2DP 源连接；没配对过的会在这一步顺手配对（Just Works） */
static void on_bt_dev_click(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    if (bt_service_connect(bda) == ESP_OK) {
        ui_toast("正在连接（首次会自动配对）…", 2000);
    } else {
        ui_toast("连接失败", 1600);
    }
    s_bt_dirty = true;
}

static void on_bt_disconnect(lv_event_t *e)
{
    const uint8_t *bda = (const uint8_t *)lv_event_get_user_data(e);
    if (!bda) {
        return;
    }
    bt_service_disconnect(bda);
    s_bt_dirty = true;
}

static void bt_rebuild_list(void)
{
    if (!s_bt_list) {
        return;
    }
    lv_obj_clean(s_bt_list);

    /* 已连接 */
    if (bt_service_is_connected()) {
        lv_obj_t *val = NULL;
        char info[48];
        snprintf(info, sizeof(info), "%s%s", bt_service_peer_addr(),
                 bt_service_is_streaming() ? " · 推流中" : "");
        lv_obj_t *row = make_row(s_bt_list, LV_SYMBOL_AUDIO, 0x3E9E86,
                                 bt_service_peer_name(), &val);
        lv_label_set_text(val, info);
        make_row_btn(row, "断开", on_bt_disconnect, (void *)bt_service_peer_bda());    }

    /* 已配对 */
    const size_t nb = bt_service_bonded_count();
    lv_obj_t *h1 = lv_label_create(s_bt_list);
    lv_obj_set_style_text_font(h1, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h1, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(h1, "已配对的设备");
    if (nb == 0) {
        lv_obj_t *hint = lv_label_create(s_bt_list);
        lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(hint, "还没有配对过设备");
    }
    for (size_t i = 0; i < nb; i++) {
        const bt_dev_t *d = bt_service_bonded(i);
        if (!d) {
            continue;
        }
        lv_obj_t *row = make_row(s_bt_list, d->is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH,
                                 0x4A6FA5, d->name, NULL);
        /* BDA 也拷一份：设备数组在搜索完成后会重建 */
        void *bda_copy = row_user_dup(d->bda, 6);
        lv_obj_add_event_cb(row, on_bt_dev_click, LV_EVENT_CLICKED, bda_copy);
        lv_obj_add_event_cb(row, row_user_free, LV_EVENT_DELETE, bda_copy);
        make_row_btn_copy(row, "解除", on_bt_forget, d->bda, 6);
    }

    /* 搜索到的设备 */
    lv_obj_t *h2 = lv_label_create(s_bt_list);
    lv_obj_set_style_text_font(h2, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h2, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(h2, "搜索到的设备");

    const size_t n = bt_service_dev_count();
    if (n == 0) {
        lv_obj_t *hint = lv_label_create(s_bt_list);
        lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(hint, bt_service_is_discovering() ? "搜索中…（约 10 秒）"
                                                           : "点右上角「搜索」找设备");
    }
    for (size_t i = 0; i < n; i++) {
        const bt_dev_t *d = bt_service_dev(i);
        if (!d) {
            continue;
        }
        lv_obj_t *val = NULL;
        char addr[18];
        bt_service_addr_str(d->bda, addr, sizeof(addr));
        lv_obj_t *row = make_row(s_bt_list, d->is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH,
                                 0x3E9E86, d->name, &val);
        char info[56];
        snprintf(info, sizeof(info), "%s · %s", addr, d->bonded ? "已配对" : "未配对");
        lv_label_set_text(val, info);
        void *bda_copy2 = row_user_dup(d->bda, 6);
        lv_obj_add_event_cb(row, on_bt_dev_click, LV_EVENT_CLICKED, bda_copy2);
        lv_obj_add_event_cb(row, row_user_free, LV_EVENT_DELETE, bda_copy2);
    }
}

static void bt_tick(lv_timer_t *t)
{
    (void)t;
    radio_sw_sync();          /* 本页顶部的总开关也要跟着服务状态走 */
    if (!s_bt_status) {
        return;
    }
    char buf[128];
    if (bt_service_is_connected()) {
        snprintf(buf, sizeof(buf), "%s · %s%s",
                 bt_service_peer_name(), bt_service_state_str(),
                 bt_service_stream_rate() ? "" : "（未推流）");
    } else {
        snprintf(buf, sizeof(buf), "蓝牙%s · 已配对 %u 台 · 发现 %u 台",
                 bt_service_state_str(),
                 (unsigned)bt_service_bonded_count(),
                 (unsigned)bt_service_dev_count());
    }
    label_set_if_changed(s_bt_status, buf);

    const int sig = (int)bt_service_dev_count() * 64 + (int)bt_service_bonded_count() * 4 +
                    (int)bt_service_state() * 100000 +
                    (bt_service_is_discovering() ? 1000000 : 0);
    if (sig != s_bt_sig || s_bt_dirty) {
        s_bt_sig   = sig;
        s_bt_dirty = false;
        bt_rebuild_list();
    }
}

static void bt_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_bt_timer) {
        lv_timer_delete(s_bt_timer);
        s_bt_timer = NULL;
    }
    s_bt_status = NULL;
    s_bt_list   = NULL;
    s_bt_sig    = -1;
}

static lv_obj_t *page_bt(void)
{
    lv_obj_t *scr  = make_screen();
    lv_obj_t *body = make_page_shell(scr, "蓝牙", "搜索", on_bt_scan);

    /* 顶部同一个总开关（与设置主页那行共享状态，见 page_wifi 里的说明） */
    make_switch_row(body, LV_SYMBOL_BLUETOOTH, 0x4A6FA5, "蓝牙", RADIO_BT, NULL, NULL);

    s_bt_status = lv_label_create(body);
    lv_obj_set_style_text_font(s_bt_status, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_bt_status, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_width(s_bt_status, LV_PCT(100));
    lv_label_set_long_mode(s_bt_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_bt_status, "…");

    lv_obj_t *note = lv_label_create(body);
    lv_obj_set_style_text_font(note, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "说明：本机可搜索/配对/解除配对；音频串流（A2DP）还没接入，"
                            "所以暂时不能把声音送到耳机。");

    s_bt_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_bt_list);
    lv_obj_set_width(s_bt_list, LV_PCT(100));
    lv_obj_set_height(s_bt_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bt_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bt_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_bt_list, LV_OBJ_FLAG_SCROLLABLE);

    bt_rebuild_list();
    lv_obj_add_event_cb(scr, bt_on_delete, LV_EVENT_DELETE, NULL);
    s_bt_timer = lv_timer_create(bt_tick, 500, NULL);
    bt_tick(NULL);
    return scr;
}

/* ================================================================== 声音与输出 */

static lv_obj_t *s_out_list;
static lv_obj_t *s_out_pct;

static void out_rebuild(void)
{
    if (!s_out_list) {
        return;
    }
    lv_obj_clean(s_out_list);
    const audio_out_t cur = audio_service_get_output();
    const char *bt_hint = bt_service_is_connected()
                              ? (bt_service_is_streaming() ? "推流中" : "已连接")
                              : "未连接蓝牙设备";
    const struct {
        audio_out_t out;
        const char *sym;
        const char *name;
        const char *val;
    } items[] = {
        { AUDIO_OUT_SPEAKER,   LV_SYMBOL_VOLUME_MAX, "板载扬声器", "ES8389 + NS4150B" },
        { AUDIO_OUT_BLUETOOTH, LV_SYMBOL_BLUETOOTH,  "蓝牙音频设备", bt_hint },
        { AUDIO_OUT_NONE,      LV_SYMBOL_MUTE,       "静音",       "关功放" },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *val = NULL;
        lv_obj_t *row = make_row(s_out_list, items[i].sym, 0xE8973A, items[i].name, &val);
        lv_label_set_text(val, (cur == items[i].out) ? "已选择" : items[i].val);
        lv_obj_add_event_cb(row, [](lv_event_t *e) {
            const audio_out_t o = (audio_out_t)(intptr_t)lv_event_get_user_data(e);
            esp_err_t ret = audio_service_set_output(o);
            if (ret == ESP_ERR_INVALID_STATE) {
                ui_toast("请先在「蓝牙」里连接设备后再切输出", 2200);
                return;
            }
            if (ret != ESP_OK) {
                ui_toast("切换失败", 1500);
                return;
            }
            if (o == AUDIO_OUT_NONE) {
                ui_toast("已静音", 1200);
            } else if (o == AUDIO_OUT_BLUETOOTH) {
                ui_toast("输出：蓝牙音频设备", 1500);
            } else {
                ui_toast("输出：板载扬声器", 1200);
            }
            out_rebuild();
        }, LV_EVENT_CLICKED, (void *)(intptr_t)items[i].out);
    }
}

static void out_on_delete(lv_event_t *e)
{
    (void)e;
    s_out_list = NULL;
    s_out_pct  = NULL;
}

static lv_obj_t *page_output(void)
{
    lv_obj_t *scr  = make_screen();
    lv_obj_t *body = make_page_shell(scr, "声音与输出", NULL, NULL);

    s_out_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_out_list);
    lv_obj_set_width(s_out_list, LV_PCT(100));
    lv_obj_set_height(s_out_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_out_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_out_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_out_list, LV_OBJ_FLAG_SCROLLABLE);
    out_rebuild();

    /* 音量：0~100，松手即下发（拖动中只更新数字） */
    lv_obj_t *vol_row = lv_obj_create(body);
    lv_obj_remove_style_all(vol_row);
    lv_obj_set_size(vol_row, LV_PCT(100), UI_PX_H(56));
    lv_obj_set_style_bg_color(vol_row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(vol_row, UI_PX_W(14), LV_PART_MAIN);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vl = lv_label_create(vol_row);
    lv_obj_set_style_text_font(vl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(vl, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(vl, "音量");
    lv_obj_align(vl, LV_ALIGN_LEFT_MID, 0, 0);

    s_out_pct = lv_label_create(vol_row);
    lv_obj_set_style_text_font(s_out_pct, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_out_pct, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text_fmt(s_out_pct, "%d%%", audio_service_get_volume());
    lv_obj_align(s_out_pct, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *sl = lv_slider_create(vol_row);
    lv_obj_set_size(sl, UI_PCT_X(50), UI_PX_H(10));
    lv_obj_align(sl, LV_ALIGN_CENTER, UI_PX_W(20), 0);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, audio_service_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, UI_PX_H(6), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        lv_obj_t *s = lv_event_get_target_obj(e);
        int v = (int)lv_slider_get_value(s);
        audio_service_set_volume(v);
        if (s_out_pct) {
            lv_label_set_text_fmt(s_out_pct, "%d%%", v);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(scr, out_on_delete, LV_EVENT_DELETE, NULL);
    return scr;
}

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
 *   - 右上角"重新检测"：板子只在启动时 mount 一次，**开机后才插卡**就必须再挂一次
 *     （bsp_sdcard_mount() 是幂等的）
 *
 * 页面是**一次性**读取：进来读一次，不挂常驻定时器 —— 容量变化很慢，要看新的点
 * "重新检测"即可。
 */
static lv_obj_t *s_storage_body;

/* KB → 人看的字符串。整数运算，不碰浮点（-Werror 下 %f 还牵扯 newlib 配置） */
static void storage_fmt_size_kb(uint32_t kb, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    if (kb >= 1024u * 1024u) {                      /* ≥ 1 GB：保留 1 位小数 */
        const uint32_t t = (uint32_t)(((uint64_t)kb * 10u) / (1024u * 1024u));
        snprintf(out, cap, "%u.%u GB", (unsigned)(t / 10u), (unsigned)(t % 10u));
    } else if (kb >= 1024u) {                       /* ≥ 1 MB */
        const uint32_t t = (uint32_t)(((uint64_t)kb * 10u) / 1024u);
        snprintf(out, cap, "%u.%u MB", (unsigned)(t / 10u), (unsigned)(t % 10u));
    } else {
        snprintf(out, cap, "%u KB", (unsigned)kb);
    }
}

/* 一行"图标 + 名称 + 右侧值"：直接复用 make_row（它自带右对齐的 value label） */
static void storage_add_info(const char *sym, uint32_t accent, const char *name, const char *value)
{
    lv_obj_t *val = NULL;
    make_row(s_storage_body, sym, accent, name, &val);
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
    storage_fmt_size_kb(used_kb, used, sizeof(used));
    storage_fmt_size_kb(total_kb, total, sizeof(total));
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
    storage_fmt_size_kb(free_kb, freebuf, sizeof(freebuf));
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

    if (!bsp_sdcard_is_mounted()) {
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

    storage_fmt_size_kb(total_kb, buf, sizeof(buf));
    storage_add_info(LV_SYMBOL_DRIVE, 0x3E9E86, "总容量", buf);
    storage_fmt_size_kb(total_kb - free_kb, buf, sizeof(buf));
    storage_add_info(LV_SYMBOL_BARS, 0xE8973A, "已用容量", buf);
    storage_fmt_size_kb(free_kb, buf, sizeof(buf));
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
            storage_fmt_size_kb((uint32_t)(ci.card_bytes / 1024u), buf, sizeof(buf));
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
    /* 幂等：已经挂上就直接返回 ESP_OK；没挂上再试一次（开机后才插卡的场景） */
    const esp_err_t ret = bsp_sdcard_mount();
    storage_rebuild();
    ui_toast(ret == ESP_OK ? "已检测到 SD 卡" : "还是没检测到 SD 卡", 1800);
}

static void storage_on_delete(lv_event_t *e)
{
    (void)e;
    s_storage_body = NULL;
}

static lv_obj_t *page_storage(void)
{
    lv_obj_t *scr  = make_screen();
    lv_obj_t *body = make_page_shell(scr, "存储", "重新检测", storage_on_rescan);
    s_storage_body = body;
    storage_rebuild();
    lv_obj_add_event_cb(scr, storage_on_delete, LV_EVENT_DELETE, NULL);
    ESP_LOGI(TAG, "storage page created (mounted=%d)", (int)bsp_sdcard_is_mounted());
    return scr;
}

/* ================================================================== 注册 */

/*
 * 从别处（音乐页长按输出设备）直接跳到"设置的蓝牙页"。
 *
 * 直接 push 蓝牙子页而不是先 push 设置主页：用户的目标就是蓝牙，多一层
 * 主页只是让他多按一次返回。子页外壳自带返回钮，返回即回到来处。
 */
esp_err_t app_settings_open_bt(void)
{
    page_desc_t p = {};
    p.id      = SUB_BT.id;
    p.create  = SUB_BT.create;
    p.no_back = true;            /* make_page_shell 自带返回钮 */
    return ui_router_push(&p);
}

static void on_settings_enter(void) {}
static void on_settings_exit(void) {}

void app_settings_register_apps(void)
{
    static const app_desc_t settings = {
        "settings", "设置", LV_SYMBOL_SETTINGS, 0x6E6E78,
        page_settings, on_settings_enter, on_settings_exit, 60,
        APP_FLAG_NO_BACK      /* make_page_shell 自带返回钮，路由不再叠全局键 */
    };
    ui_app_register(&settings);
    ESP_LOGI(TAG, "settings app registered");
}
