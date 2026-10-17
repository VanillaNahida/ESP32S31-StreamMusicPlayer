/*
 * ui_ba.h —— 碧蓝档案（Blue Archive）风格弹窗工具箱
 *
 * 视觉参照 assets/blue_archive_ui_replica.html：
 *
 *   ┌────────────────────────────────────────────┐ ← 白色圆角卡片 + 投影
 *   │ ╲ 浅蓝几何装饰（左上/右下，随圆角裁掉）   ✕ │
 *   │              通 知                          │
 *   │           ▬▬▬▬（黄色下划线）                │
 *   │            是否删除这条记录？                │
 *   │                                             │
 *   │      ╱▔▔▔▔▔▔╲        ╱▔▔▔▔▔▔╲              │
 *   │      ╲▁▁▁▁▁▁╱        ╲▁▁▁▁▁▁╱              │
 *   │        Esc              Enter               │
 *   └────────────────────────────────────────────┘
 *
 * 用它的三类入口：
 *   1. ui_ba_open()      —— 自己搭内容（输入框 / 二维码 / 菜单…）；
 *   2. ui_ba_confirm()   —— 一句话二次确认（取消 / 确认）；
 *   3. ui_ba_close()     —— 带"下滑缩小"退场动画地关掉。
 *
 * 纪律（和项目其它弹层一致，别破坏）：
 *   - 一律在 **CLICKED** 回调里打开，不要在 PRESSED/LONG_PRESSED 里建（见 AGENTS 14.6）；
 *   - 关闭走 ui_ba_close()，**不要**自己 lv_obj_delete()，否则没有退场动画，
 *     而且会绕过 ui_popup_out() 的收尾（它负责动画结束后删遮罩）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 配色 */

#define BA_CARD          0xFFFFFF   /* 卡片底 */
#define BA_TEXT          0x1E293B   /* 标题 / 正文（slate-800） */
#define BA_TEXT_BODY     0x475569   /* 次要正文（slate-600） */
#define BA_TEXT_DIM      0x94A3B8   /* 提示文字（slate-400） */
#define BA_BLUE          0x0096E6   /* 主蓝 */
#define BA_BLUE_LIGHT    0x38C8FF   /* 主蓝渐变亮端 */
#define BA_BLUE_EDGE     0x7CE3FF   /* 主按钮描边 */
#define BA_BLUE_PALE     0xC8EBFF   /* 角落装饰 */
#define BA_BLUE_SOFT     0xDCF2FF   /* 选中行底色 */
#define BA_YELLOW        0xFCC800   /* 标题下划线（BA 标志黄） */
#define BA_CANCEL_A      0xEEF5F9   /* 次按钮渐变亮端 */
#define BA_CANCEL_B      0xD5E2EB   /* 次按钮渐变暗端 */
#define BA_CANCEL_FG     0x3B5166   /* 次按钮文字 */
#define BA_INPUT_BG      0xF1F5F9   /* 输入框底 */
#define BA_INPUT_LINE    0x64748B   /* 输入框下边框 */
#define BA_DANGER        0xE5484D   /* 危险动作（删除） */
#define BA_SCRIM         0x020617   /* 遮罩（slate-950） */

/* ------------------------------------------------------------------ 对话框 */

typedef struct {
    lv_obj_t *scrim;      /* 全屏遮罩（可点空白关闭时挂回调用；不需要就忽略） */
    lv_obj_t *card;       /* 白卡片本体 */
    lv_obj_t *title;      /* 标题 label */
    lv_obj_t *body;       /* 内容区（竖向 flex，宽 100%）：往这里加控件 */
    lv_obj_t *btn_row;    /* 底部按钮行（横向 flex，居中）：ui_ba_btn() 的 parent */
} ui_ba_dialog_t;

/*
 * 开一个 BA 风格对话框。
 *
 *   title     标题（NULL 或空串 = 不要标题栏）
 *   width_pct 卡片宽度（屏宽百分比，40~96 之间比较好看）
 *   align     卡片在遮罩里的对齐方式；配 y_ofs 用（例如键盘占下半屏时
 *             传 LV_ALIGN_TOP_MID + 小偏移，卡片就停在键盘上方）
 *   y_ofs     对齐后的纵向偏移（px）
 *
 * 返回的结构体**按值返回**：调用方自己存一份（通常是个 static 变量），
 * 关闭时把它的地址交给 ui_ba_close()。
 */
ui_ba_dialog_t ui_ba_open(const char *title, int width_pct, lv_align_t align, int y_ofs);

/*
 * 同上，但把浮层挂到指定父对象上（默认是 lv_layer_top()）。
 * 给"必须随页面一起销毁"的浮层用：挂在页面 screen 上的浮层在页面 pop 时会
 * 跟着删掉，不会跟着用户跑到别的页面去（例如设置页的 Wi-Fi 密码输入）。
 */
ui_ba_dialog_t ui_ba_open_in(lv_obj_t *parent, const char *title, int width_pct,
                             lv_align_t align, int y_ofs);

/* 关闭：下滑 + 缩小 + 淡出（动画结束后自动删除整块浮层）。
 * 传空结构体或已关闭的句柄是空操作。调用后 *d 被清零。 */
void ui_ba_close(ui_ba_dialog_t *d);

/* 只要手里有 scrim 指针的场合用这个（内部取第一个子对象当卡片）。 */
void ui_ba_close_scrim(lv_obj_t *scrim);

/* 暂停/恢复"点空白关闭"（默认**不**开启：调用方自己决定要不要挂回调） */
void ui_ba_scrim_dismissible(ui_ba_dialog_t *d, bool on);

/*
 * 右上角 ✕ 的行为。不设时 ✕ 直接关掉浮层；设了就先调 cb(user)，
 * 由 cb 自己决定怎么关（例如顺手把输入法收起来再 ui_ba_close_scrim()）。
 */
void ui_ba_on_close(ui_ba_dialog_t *d, void (*cb)(void *user), void *user);

/* ------------------------------------------------------------------ 卡片内控件 */

/*
 * ★ 按下反馈（弹窗按钮的那套手感）：按住缩到 ≈92%（120 ms）、松手用 150 ms 缓出
 * 复原，**复原动画跑完**才把 on_click 当成一次点击派发；手指滑出对象
 * （PRESS_LOST）只复原、不派发。on_click 传 NULL 就是"只要反馈、不要回调"。
 *
 * 弹窗里的按钮 / 选项行 / ✕ 都是它挂上去的；**标题栏的返回钮也共用这一份**
 * （ui_core.cc 的 ui_back_button_create）—— 手感与常量只有一处，别在别处复制一套。
 * 实现与三条铁律见 ui_ba.cc 的"按下反馈"一节。
 */
void ui_ba_press_feedback(lv_obj_t *obj, lv_event_cb_t on_click, void *user);

/*
 * 底部按钮：斜切平行四边形 + 渐变。
 *
 *   primary true = 青蓝渐变（确认），false = 浅灰蓝渐变（取消）
 *   返回按钮对象（回调挂在它上面）
 */
lv_obj_t *ui_ba_btn(lv_obj_t *row, const char *text, bool primary,
                    lv_event_cb_t cb, void *user);

/* 同上，但可以指定样式：0 = 次按钮（浅灰蓝）、1 = 主按钮（青蓝）、2 = 危险（红） */
lv_obj_t *ui_ba_btn_styled(lv_obj_t *row, const char *text, int style,
                           lv_event_cb_t cb, void *user);

/* 列表行按钮（长按菜单 / 输出设备列表那种）：浅底 + 左侧色条 + 深色文字。
 * accent 传 BA_BLUE / BA_DANGER / 其它强调色（0 = 不画色条）。 */
lv_obj_t *ui_ba_opt_row(lv_obj_t *col, const char *text, uint32_t accent,
                        lv_event_cb_t cb, void *user);

/* BA 输入框：浅灰底 + 2px 下边框 + 深色文字（textarea 由调用方接管键盘） */
lv_obj_t *ui_ba_input(lv_obj_t *parent, const char *placeholder, const char *text);

/* 一行说明 / 星号小字 */
lv_obj_t *ui_ba_hint(lv_obj_t *parent, const char *text);

/* 一行正文（可换行，居中）——确认框的正文用它 */
lv_obj_t *ui_ba_text(lv_obj_t *parent, const char *text);

/* ------------------------------------------------------------------ 二次确认 */

/*
 * 二次确认框：标题（默认「通 知」）+ 正文 + 取消/确认。
 * 确认后先关掉自己，再回调 on_ok(user)。
 *
 * 同一时间只会存在一个确认框：重复调用会把前一个立刻删掉再开新的。
 * 页面被关掉时（浮层随 screen 销毁）内部句柄会自动失效，不会野指针。
 */
void ui_ba_confirm(const char *title, const char *msg, const char *ok_text,
                   void (*on_ok)(void *user), void *user);

/* 取消文案默认「取消」；危险动作用这个把确认键变红。 */
void ui_ba_confirm_ex(const char *title, const char *msg, const char *ok_text,
                      const char *cancel_text, bool danger,
                      void (*on_ok)(void *user), void *user);

void ui_ba_confirm_close(void);
bool ui_ba_confirm_is_open(void);

#ifdef __cplusplus
}
#endif
