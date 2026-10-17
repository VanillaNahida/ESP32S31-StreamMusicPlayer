/*
 * as_widgets.h —— 设置页通用控件工厂（原 app_settings.cc 58–246 行的"公共部件"）
 *
 * 这里只有"造控件"和"管行的回调参数"两类东西，不含任何业务状态：
 *   - make_screen / make_page_shell / make_row / make_flat_btn：搭页面的积木；
 *   - row_user_dup / row_user_free：行回调参数的**稳定副本**（见下面的长注释）；
 *   - label_set_if_changed：500 ms 常驻刷新的堆抖动防护。
 *
 * ⚠ 这些函数的实现是**逐字搬迁**，行为必须与拆分前一致。
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 无样式 screen（背景色 + 页面内边距 + 顶部给状态栏让位）。
 * ⚠ pad_all 在前、pad_top 在后：否则状态栏那 32px 会被 pad_all 覆盖掉。
 */
lv_obj_t *as_make_screen(void);


/* 药丸形小按钮（UI_COLOR_PILL + 小字）；cb 为 NULL 时只造按钮不挂回调 */
lv_obj_t *as_make_flat_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user);

/* 一行"图标 + 标题 + 右侧值"；返回行对象，*value_out 收到值 label（可为 NULL） */
lv_obj_t *as_make_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                      const char *text, lv_obj_t **value_out);

/*
 * 行回调参数的**稳定副本**：拷贝挂在控件上（控件 DELETE 时自动 free）。
 * 为什么必须拷一份、以及"直接用外部 user_data"与"自己拷一份"的取舍，
 * 见 as_widgets.cc 里 as_row_user_free 上方的长注释（原 app_settings.cc 205–213）。
 */
/* 列表里的小按钮（忘记 / 连接），挂在行右侧 */
void  as_row_user_free(lv_event_t *e);
void *as_row_user_dup(const void *src, size_t n);

/* 行内右侧文字按钮（直接用外部 user_data） */
lv_obj_t *as_make_row_btn(lv_obj_t *row, const char *text, lv_event_cb_t cb, void *user);

/* 带"拷贝一份参数"的版本：src/n 是回调要用的数据（SSID 字符串或 6 字节 BDA） */
lv_obj_t *as_make_row_btn_copy(lv_obj_t *row, const char *text, lv_event_cb_t cb,
                               const void *src, size_t n);

/*
 * 文本没变就别写 label。
 *
 * 设置页的摘要行 / Wi-Fi 状态行都是 500 ms 一次的常驻定时器在刷，而内容绝大多数
 * 时候是不变的；lv_label_set_text() 每次都 free+malloc 新串并整块 lv_obj_invalidate()，
 * 无条件重写会让界面和堆持续抖动。
 *
 * 直接读 label 当前文本做比较，不维护额外缓存（缓存会和别处的直接写入不同步）。
 */
void as_label_set_if_changed(lv_obj_t *label, const char *text);

#ifdef __cplusplus
}
#endif
