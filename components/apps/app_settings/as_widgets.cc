/*
 * as_widgets.cc —— 设置页通用控件工厂
 *
 * 原 app_settings.cc 58–246 行的"公共部件" + 540–550 的 label_set_if_changed，
 * **逐字搬迁**（注释一并搬），只把函数名加了 `as_` 前缀以免和别处撞名。
 */
#include "as_widgets.h"

#include <stdlib.h>
#include <string.h>

#include "ui_core.h"
#include "ui_metrics.h"

/* ------------------------------------------------------------------ 公共部件 */

lv_obj_t *as_make_screen(void)
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

void as_on_back(lv_event_t *e)
{
    (void)e;
    ui_router_pop();
}

lv_obj_t *as_make_flat_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
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

/* 一行"图标 + 标题 + 右侧值"；返回行对象，*value_out 收到值 label（可为 NULL） */
lv_obj_t *as_make_row(lv_obj_t *parent, const char *sym, uint32_t accent,
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
void as_row_user_free(lv_event_t *e)
{
    void *p = lv_event_get_user_data(e);
    if (p) {
        free(p);
    }
}

void *as_row_user_dup(const void *src, size_t n)
{
    void *p = malloc(n);
    if (p) {
        memcpy(p, src, n);
    }
    return p;
}

lv_obj_t *as_make_row_btn(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = as_make_flat_btn(row, text, cb, user);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    return b;
}

/* 带"拷贝一份参数"的版本：src/n 是回调要用的数据（SSID 字符串或 6 字节 BDA） */
lv_obj_t *as_make_row_btn_copy(lv_obj_t *row, const char *text, lv_event_cb_t cb,
                               const void *src, size_t n)
{
    void *copy = src ? as_row_user_dup(src, n) : NULL;
    lv_obj_t *b = as_make_row_btn(row, text, cb, copy);
    lv_obj_add_event_cb(b, as_row_user_free, LV_EVENT_DELETE, copy);
    return b;
}

void as_label_set_if_changed(lv_obj_t *label, const char *text)
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
