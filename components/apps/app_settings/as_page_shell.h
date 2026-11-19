/*
 * as_page_shell.h —— 二级页外壳 + 子页描述/跳转
 *
 * 二级页的共同外壳（返回钮 + 标题 + 可选右上角动作钮 + 可滚动 body）在这里；
 * 五个子页的构建函数也在这里声明（各自的实现分散在 as_page_*.cc）。
 *
 * ★ 子页描述符必须是**静态存储期**：router 的 page_desc_t 里 id/create 是
 *   **按值拷贝指针**的，用栈上临时结构体会悬垂。
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *(*create)(void);
    const char *id;
} subpage_desc_t;

/*
 * 二级页外壳：返回钮 + 标题（+ 可选的右上角动作按钮）。
 * 返回 body 容器（纵向可滚动列），调用方往里面塞内容。
 */
lv_obj_t *as_page_shell(lv_obj_t *scr, const char *title,
                        const char *action_text, lv_event_cb_t action_cb);

/* 各子页的构建函数（定义在 as_page_*.cc；外壳要用它们的地址建描述符） */
lv_obj_t *page_wifi(void);
lv_obj_t *page_bt(void);
lv_obj_t *page_output(void);
lv_obj_t *page_display(void);      /* 显示设置：时钟显秒 + 手动校时（as_page_display.cc） */
lv_obj_t *page_storage(void);
lv_obj_t *page_about(void);

/* 静态存储期的子页描述符（Wi-Fi / 蓝牙 / 存储三页；router 直接存 id/create 指针） */
const subpage_desc_t *as_sub_wifi(void);
const subpage_desc_t *as_sub_bt(void);
const subpage_desc_t *as_sub_storage(void);
const subpage_desc_t *as_sub_display(void);

/* 点开关行的空白/名称区 = 进子页（点开关图案进不来，见 as_radio.cc） */
void as_on_switch_row_click(lv_event_t *e);

/* 按描述符压栈子页（no_back = true：子页外壳自带返回钮） */
void as_push_subpage(const subpage_desc_t *sp);

#ifdef __cplusplus
}
#endif
