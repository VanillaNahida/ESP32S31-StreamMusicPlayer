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
    /*
     * ★ 结构：**标题栏行（固定高）+ body（flex_grow 1，可滚动）** 两个盒子。
     *
     * 为什么不能像以前那样"body 铺满整屏 + 靠 pad_top 把内容推到标题下面"
     * （2026-10 真机照片暴露）：**LVGL 裁剪子对象用的是对象自己的盒子，不算
     * padding** —— body 往上滚时，行会画进 pad 区，把标题文字盖掉。
     * 两个盒子一分，body 的盒子物理上就在标题栏下面，滚动永远够不着标题。
     */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);

    /* 导航栏：高 = UI_TITLEBAR_H，只放标题（返回钮悬在它下沿，见下） */
    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), UI_TITLEBAR_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    /* 标题让开返回钮（钮宽 + 间距）；钮本身不在这里，由下面的 ui_back_button_place 摆 */
    lv_obj_set_style_pad_left(head, UI_HEADER_TEXT_X, LV_PART_MAIN);

    lv_obj_t *t = lv_label_create(head);
    lv_obj_set_style_text_font(t, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(t, title);

    /* 内容区：导航栏下面，吃掉剩余高度，纵向可滚动。
     * pad_top 让开悬到内容区的下半截返回钮（UI_BACK_BTN_DIP，见 ui_metrics.h）：
     * 按钮 z 序在页面内容之上，不让开的话第一行会被它压住、也点不到。 */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_top(body, UI_BACK_BTN_DIP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    if (action_text && action_cb) {
        /* 右上角动作按钮：父容器 scr 现在是 flex，位置由 flex 决定，lv_obj_align
         * 会被布局覆盖 —— 所以给它 FLOATING（LVGL 的 flex **跳过** FLOATING 子对象），
         * align 才生效。y 偏 (UI_TITLEBAR_H - 按钮高)/2：让它与标题同处栏的中线。 */
        lv_obj_t *a = as_make_flat_btn(scr, action_text, action_cb, NULL);
        lv_obj_add_flag(a, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, 0,
                     (UI_TITLEBAR_H - UI_PX_H(34)) / 2);
    }

    /*
     * ★ 返回键**最后建**，而且**不放进 head**：
     *  - 不放进 head：head 只有 UI_TITLEBAR_H 高，会把悬出来的下半截裁掉
     *    （LVGL 默认裁剪子对象）；挂到 scr 上、由 ui_back_button_place() 统一摆到
     *    "圆心压在导航栏下沿"的位置（它自己扣掉 scr 的 padding，落点与其他页一致）。
     *    scr 现在是 flex，故 ui_back_button_create 里已带 LV_OBJ_FLAG_FLOATING。
     *  - 放最后：它悬出来的下半截正好落在 body 的地盘上，而 LVGL 是"后建的画在上面"——
     *    建在 body 之前的话，body 里往上滚的行会盖住按钮。放最后 = 永远压在页面内容之上
     *    （与全局返回键挂在 lv_layer_top 上的效果一致）。
     */
    ui_back_button_place(ui_back_button_create(scr));

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

const subpage_desc_t *as_sub_display(void)
{
    static const subpage_desc_t SUB_DISPLAY = { page_display, "set_display" };
    return &SUB_DISPLAY;
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
