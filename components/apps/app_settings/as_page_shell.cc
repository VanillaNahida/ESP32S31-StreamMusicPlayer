/*
 * as_page_shell.cc —— 二级页外壳 + 子页描述符 + 跳转
 *
 * 原 app_settings.cc：
 *   - make_page_shell（104–156）→ as_page_shell
 *   - 子页描述符 SUB_WIFI / SUB_BT / SUB_STORAGE（402–414）
 *   - on_switch_row_click（417–428）→ as_on_switch_row_click
 *   - app_settings_open_bt 里的 push 逻辑（2015–2022）→ as_push_subpage
 * 以上全部**逐字搬迁**，只把描述符从文件级 `static const` 改成
 * "函数内 static const + 取地址"（保持静态存储期 ⇒ router 按值拷指针依然安全）。
 */
#include "as_page_shell.h"

#include <stdbool.h>

#include "ui_core.h"
#include "ui_metrics.h"

#include "as_widgets.h"

/*
 * 二级页外壳：返回钮 + 标题（+ 可选的右上角动作按钮）。
 * 返回 body 容器（纵向可滚动列），调用方往里面塞内容。
 */
lv_obj_t *as_page_shell(lv_obj_t *scr, const char *title,
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
    lv_obj_add_event_cb(b, as_on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(head);
    lv_obj_set_style_text_font(t, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(t, title);

    if (action_text && action_cb) {
        /* 注意：head 是 flex 容器，子对象的位置由 flex 决定，lv_obj_align 会被
         * 布局覆盖 —— 所以右上角的动作按钮挂到 screen 上绝对定位 */
        lv_obj_t *a = as_make_flat_btn(scr, action_text, action_cb, NULL);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return body;
}

/*
 * 子页描述：router 的 page_desc_t 里 id/create 是**按值拷贝指针**的，
 * 所以这两个成员必须是静态存储期 —— 用静态常量描述符即可。
 * （原来是文件级 `static const subpage_desc_t SUB_*` + 传给 make_switch_row；
 *   现在收进函数里的 static 局部量，存储期不变，只把"谁能看到"收窄成取地址。）
 */
const subpage_desc_t *as_sub_wifi(void)
{
    /* Wi-Fi 子页（静态存储期：router 直接存 id/create 指针） */
    static const subpage_desc_t SUB_WIFI = { page_wifi, "set_wifi" };
    return &SUB_WIFI;
}

const subpage_desc_t *as_sub_bt(void)
{
    static const subpage_desc_t SUB_BT = { page_bt, "set_bt" };
    return &SUB_BT;
}

const subpage_desc_t *as_sub_storage(void)
{
    static const subpage_desc_t SUB_STORAGE = { page_storage, "set_storage" };
    return &SUB_STORAGE;
}

void as_push_subpage(const subpage_desc_t *sp)
{
    if (!sp || !sp->create) {
        return;
    }
    page_desc_t p = {};
    p.id      = sp->id;
    p.create  = sp->create;
    p.no_back = true;            /* 子页外壳自带返回钮，别再叠全局返回键 */
    ui_router_push(&p);
}

/* 点开关行的空白/名称区 = 进子页（点开关图案进不来，见 as_radio.cc 的 on_radio_switch_clicked） */
void as_on_switch_row_click(lv_event_t *e)
{
    const subpage_desc_t *sp = (const subpage_desc_t *)lv_event_get_user_data(e);
    if (!sp || !sp->create) {
        return;
    }
    as_push_subpage(sp);
}
