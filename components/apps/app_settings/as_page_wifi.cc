/*
 * as_page_wifi.cc —— Wi-Fi 二级页（扫描 / 连接 / 保存 / 忘记）+ 密码输入浮层
 *
 * 原 app_settings.cc 714–1034 行**整段搬迁**（函数名 / static 名 / 注释都不动，
 * 只把通用部件的调用换成 as_* 前缀）。
 *
 * ★★ 本文件有两个必须原样保留的历史踩坑点（AGENTS.md §11.5.4）：
 *   1. 密码浮层用**自己的** `s_pw_ssid` 缓冲，不能拿列表行上的堆字符串当 user_data
 *      —— 列表每 500 ms 就可能 wifi_rebuild_list() → lv_obj_clean() 整列重建，
 *      那个指针会悬垂；
 *   2. `pw_close()` 必须走 ui_ba_close()（异步删）—— 它是在键盘的事件回调里调的，
 *      同步删会在事件派发器还在遍历这棵子树时把它删掉 → hard fault。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_ba.h"          /* 碧蓝档案风格弹窗工具箱（Wi-Fi 密码浮层用） */

#include "wifi_service.h"

#include "as_page_shell.h"
#include "as_radio.h"
#include "as_widgets.h"

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

    ui_ba_btn(s_pw_dlg.btn_row, "取消", false, on_pw_cancel, NULL);
    ui_ba_btn(s_pw_dlg.btn_row, "连接", true, on_pw_ready, s_pw_ssid);

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
            lv_obj_t *row = as_make_row(s_wifi_list, LV_SYMBOL_SAVE, 0x5B6FB5, ssid, NULL);
            /* 行本身也要拷一份：点击时列表可能已经被下一次扫描重建了 */
            void *ssid_copy = as_row_user_dup(ssid, strlen(ssid) + 1);
            lv_obj_add_event_cb(row, on_wifi_ap_click, LV_EVENT_CLICKED, ssid_copy);
            lv_obj_add_event_cb(row, as_row_user_free, LV_EVENT_DELETE, ssid_copy);
            as_make_row_btn_copy(row, "忘记", on_wifi_forget, ssid, strlen(ssid) + 1);
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
        lv_obj_t *row = as_make_row(s_wifi_list, LV_SYMBOL_WIFI, 0x3E9E86, ap->ssid, &val);
        char info[48];
        snprintf(info, sizeof(info), "%d dBm · %s%s",
                 ap->rssi, (ap->authmode == 0) ? "开放" : "加密",
                 ap->saved ? " · 已保存" : "");
        lv_label_set_text(val, info);
        /* 拷一份 SSID：扫描完成后这个数组会被整体改写（见 as_make_row_btn / make_row_btn 上方注释） */        void *ssid_copy = as_row_user_dup(ap->ssid, strlen(ap->ssid) + 1);
        lv_obj_add_event_cb(row, on_wifi_ap_click, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(row, as_row_user_free, LV_EVENT_DELETE, ssid_copy);
    }
}

static void wifi_tick(lv_timer_t *t)
{
    (void)t;
    as_radio_sw_sync();       /* 本页顶部的总开关也要跟着服务状态走 */
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
    as_label_set_if_changed(s_wifi_status, buf);

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

lv_obj_t *page_wifi(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "Wi-Fi", "扫描", on_wifi_scan);

    /*
     * 顶部放**同一个**总开关（sub 传 NULL：已经在子页里，这行只负责开关）。
     * 它就是设置主页那一行的另一个实例，由 as_radio_sw_sync()（原 radio_sw_sync()）对齐 ——
     * 在这一页关掉 Wi-Fi，返回主页那个开关立刻也是关的。
     */
    as_radio_switch_row(body, LV_SYMBOL_WIFI, 0x3E9E86, "Wi-Fi", AS_RADIO_WIFI, NULL, NULL);

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
