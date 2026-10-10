/*
 * music_view_output.cc —— 输出设备按钮 / 选择弹窗 / 音量键
 *
 * 搬迁自 app_music.cc 的 700–705、919–1137（含 1126–1137 的按钮图标同步）。
 * 注释原样保留：弹窗必须挂 lv_layer_top、必须异步删、短按必须用 SHORT_CLICKED。
 */
#include "music_view_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "audio_service.h"
#include "app_settings.h"   /* 长按输出设备按钮 → 跳到设置的蓝牙页 */
#include "bt_service.h"     /* 输出设备弹窗要显示蓝牙连接/推流状态 */
#include "ui_ba.h"          /* 碧蓝档案风格弹窗工具箱 */
#include "ui_core.h"
#include "ui_metrics.h"

#include "music_view_internal.h"
#include "music_view_player.h"   /* music_view_make_round_btn_ex：与其它控制键同一套圆按钮 */

/* 本文件只画控件、不打日志（未使用的 TAG 会触发 -Wunused-variable；
 * 要加日志时补 `static const char *TAG = "app_music";`）。 */

static lv_obj_t  *s_out_btn_icon;                 /* 控制排上输出按钮里的图标 */
static lv_obj_t  *s_out_overlay;                  /* 输出设备弹窗（lv_layer_top 上） */
static ui_ba_dialog_t s_out_dlg;                  /* BA 对话框句柄（与 s_out_overlay 同生共死） */
static audio_out_t s_out_shown = (audio_out_t)-1; /* 按钮图标当前画的是哪个输出 */
static bool       s_open_bt_req;                  /* 长按跳设置的待处理请求 */

/* 音量键 → 从右边缘滑出竖向音量条（ui_volume_panel.cc） */
void music_view_output_on_volume(lv_event_t *e)
{
    (void)e;
    ui_volume_show();
}

static const char *out_icon_of(audio_out_t o)
{
    switch (o) {
    case AUDIO_OUT_BLUETOOTH: return LV_SYMBOL_BLUETOOTH;
    case AUDIO_OUT_NONE:      return LV_SYMBOL_MUTE;
    default:                  return LV_SYMBOL_VOLUME_MAX;
    }
}

static const char *out_name_of(audio_out_t o)
{
    switch (o) {
    case AUDIO_OUT_BLUETOOTH: {
        /*
         * 需求：这里显示"蓝牙音频设备 <设备名> 已连接"，而不是干巴巴一个类别名。
         * 名字来自 bt_service_peer_name()（A2DP 连上时写进去的对端名）。
         * 用 static 缓冲返回：调用方都在 LVGL 任务里、拿到就立刻拷进 label，单线程安全。
         * 没连上（或还没拿到名字）时退回原来的类别名。
         */
        static char bt_desc[BT_NAME_MAX + 32];
        const char *nm = bt_service_peer_name();
        /*
         * ★ 只认**已连接**（2026-10 用户报"配对了但没连接，还是显示已连接 XX"）：
         *   bt_service_peer_name() 是"上次连过的对端名"，只配对过就会一直留着，
         *   所以这里必须再用 bt_service_is_connected() 卡一道 —— 没连接就退回类别名，
         *   既不显示设备名、也不显示"已连接"。
         */
        const bool conn = bt_service_is_connected();
        if (conn && nm && nm[0]) {
            snprintf(bt_desc, sizeof(bt_desc), "蓝牙音频设备 %s 已连接", nm);
            return bt_desc;
        }
        return "蓝牙音频设备";
    }
    case AUDIO_OUT_NONE:      return "静音";
    default:                  return "板载扬声器";
    }
}

static void out_picker_close(void)
{
    if (s_out_overlay) {
        /* 下滑 + 缩小 + 淡出（BA 退场），动画结束由 ui_popup_out 自己删 */
        ui_ba_close(&s_out_dlg);
        s_out_overlay = NULL;
    }
}

static void on_out_pick(lv_event_t *e)
{
    const audio_out_t o = (audio_out_t)(intptr_t)lv_event_get_user_data(e);
    esp_err_t ret = audio_service_set_output(o);
    if (ret == ESP_ERR_INVALID_STATE) {
        ui_toast("请先在「设置 → 蓝牙」里连接设备", 2000);   /* 弹窗留着，方便连完再来 */
        return;
    }
    if (ret != ESP_OK) {
        ui_toast("切换失败", 1500);
        return;
    }
    ui_toast(out_name_of(o), 1200);
    out_picker_close();
}

static void on_out_scrim(lv_event_t *e)
{
    (void)e;
    out_picker_close();
}

/* 一行可选输出：图标块 + 名字 + 状态（当前在用的标"使用中"） */
static void out_picker_row(lv_obj_t *col, audio_out_t o, const char *sym, uint32_t accent)
{
    const bool cur = (audio_service_get_output() == o);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    /* BA 列表行：浅底 + 左侧色条；当前输出那行换成浅蓝底 + 蓝字 */
    lv_obj_set_style_bg_color(row, lv_color_hex(cur ? BA_BLUE_SOFT : 0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(row, UI_PX_W(4), LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(cur ? BA_BLUE : accent), LV_PART_MAIN);

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
    lv_obj_set_style_text_color(is, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, lv_color_hex(cur ? BA_BLUE : BA_TEXT), LV_PART_MAIN);
    lv_label_set_text(nm, out_name_of(o));
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, UI_PX_H(52), 0);

    const char *st = NULL;
    if (o == AUDIO_OUT_BLUETOOTH) {
        st = bt_service_is_connected()
                 ? (bt_service_is_streaming() ? "已连接 · 推流中" : "已连接 · 未推流")
                 : "未连接设备";
    } else if (o == AUDIO_OUT_SPEAKER) {
        st = "ES8389 + NS4150B";
    } else {
        st = "关功放（完全静音）";
    }
    lv_obj_t *sv = lv_label_create(row);
    lv_obj_set_style_text_font(sv, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(sv, lv_color_hex(cur ? BA_BLUE : BA_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(sv, cur ? "使用中" : st);
    lv_obj_align(sv, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(row, on_out_pick, LV_EVENT_CLICKED, (void *)(intptr_t)o);
}

/* 开/关弹窗（再点一次按钮就关掉） */
static void on_output_click(lv_event_t *e)
{
    (void)e;
    if (s_out_overlay) {
        out_picker_close();
        return;
    }
    ui_volume_hide();      /* 焦点转移：把音量条收掉 */

    /* 碧蓝档案风格白卡片（标题 + 黄色下划线 + 右上角 ✕） */
    s_out_dlg = ui_ba_open("选择输出设备", 58, LV_ALIGN_CENTER, 0);
    s_out_overlay = s_out_dlg.scrim;
    /* 点卡片之外的空白 = 关闭（lv_obj 默认不带 EVENT_BUBBLE，点卡片自己不会冒上来） */
    lv_obj_add_event_cb(s_out_overlay, on_out_scrim, LV_EVENT_CLICKED, NULL);
    ui_ba_on_close(&s_out_dlg, [](void *user) { (void)user; out_picker_close(); }, NULL);

    out_picker_row(s_out_dlg.body, AUDIO_OUT_SPEAKER,   LV_SYMBOL_VOLUME_MAX, 0xE8973A);
    out_picker_row(s_out_dlg.body, AUDIO_OUT_BLUETOOTH, LV_SYMBOL_BLUETOOTH,  0x4A6FA5);
    out_picker_row(s_out_dlg.body, AUDIO_OUT_NONE,      LV_SYMBOL_MUTE,       0x6E6E78);

    ui_ba_hint(s_out_dlg.body, "长按这个按钮可以直接进「设置 → 蓝牙」");
}

/* --------------------------------------------------- 长按输出设备 → 蓝牙设置页
 *
 * 需求：长按输出设备按钮直接跳到"设置 → 蓝牙"，省掉"进设置 → 点蓝牙"两步。
 *
 * ★ 长按里**不能**直接 push 页面（和 app_online 的长按菜单是同一个坑，见 AGENTS 14.6）：
 *   LVGL 在按压期间会不断重新命中手指下的对象（lv_indev.c 的 pointer_search_obj），
 *   而 indev_proc_release() 里**长按之后照样补发 CLICKED**（它只跳过 short_click）。
 *   跳过去的蓝牙页里，手指那个位置正好可能是设备列表的一行 —— 松手那一下就会
 *   莫名其妙去连一台设备。所以长按只置一个请求，等**手指抬起来**再跳。
 */
static void open_bt_deferred(void *arg)
{
    (void)arg;
    if (!s_open_bt_req) {
        return;
    }
    const lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lv_async_call(open_bt_deferred, NULL);   /* 还按着：下一轮再看 */
        return;
    }
    s_open_bt_req = false;
    if (app_settings_open_bt() != ESP_OK) {
        ui_toast("打不开蓝牙设置页", 1500);
    }
}

static void on_output_long(lv_event_t *e)
{
    (void)e;
    out_picker_close();          /* 弹窗开着就先收掉，别跟着一起被压在下面 */
    s_open_bt_req = true;
    lv_async_call(open_bt_deferred, NULL);
}

lv_obj_t *music_view_output_make_btn(lv_obj_t *parent)
{
    /* 短按必须挂 LV_EVENT_SHORT_CLICKED：见 music_view_output.h 的说明。
     * 按钮本体用播放器页那套圆形按钮工厂（图标 label 的样式与其它控制键一致）。 */
    lv_obj_t *out_btn = music_view_make_round_btn_ex(parent, out_icon_of(audio_service_get_output()),
                                                     on_output_click, LV_EVENT_SHORT_CLICKED,
                                                     UI_COLOR_PILL, UI_COLOR_FG);
    lv_obj_add_event_cb(out_btn, on_output_long, LV_EVENT_LONG_PRESSED, NULL);
    s_out_btn_icon = lv_obj_get_child(out_btn, 0);
    s_out_shown    = audio_service_get_output();
    return out_btn;
}

/* 按钮图标跟着实际输出走（由 tick 调用，改动才写 LVGL） */
void music_view_output_sync(void)
{
    if (!s_out_btn_icon) {
        return;
    }
    const audio_out_t cur = audio_service_get_output();
    if (cur != s_out_shown) {
        s_out_shown = cur;
        lv_label_set_text(s_out_btn_icon, out_icon_of(cur));
    }
}

void music_view_output_on_page_delete(void)
{
    /* 弹窗挂在 top layer 上，不随页面销毁：离开播放器页就显式关掉它，
     * 否则它会一直盖在别的页面上 */
    out_picker_close();
    s_out_btn_icon = NULL;
    s_out_shown    = (audio_out_t)-1;
}
