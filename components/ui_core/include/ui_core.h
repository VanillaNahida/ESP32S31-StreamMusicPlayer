/*
 * ui_core.h —— UI 框架层对外接口
 *
 * 全局 UI（状态栏 / 控制中心 / Toast / 手势）挂在 lv_layer_top()，
 * 跨页面常驻；页面切换走 router 的页面栈，不用 lv_screen_load 裸调。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 页面路由 */

typedef struct page_desc {
    const char *id;
    lv_obj_t  *(*create)(void);   /* 返回一个 lv_obj_create(NULL) 建出的 screen */
    void       (*on_enter)(void);
    void       (*on_exit)(void);
    /*
     * no_back = true：该页**自带**返回钮（或本身就是二级页外壳的一部分），
     * 不要自动叠全局返回键 —— 见 ui_back_button_update() 与 ui_router.cc。
     * 设置页（make_page_shell 自带返回钮）必须置位，否则会出两个返回键。
     */
    bool no_back;
} page_desc_t;

/* 全局 UI 初始化：top layer（状态栏/控制中心/手势/Toast）+ 首页 */
esp_err_t ui_core_init(void);

/* 压栈。**描述符内容会被拷贝**，因此可以安全地传栈上临时量；
 * 但 page->create 返回的 screen 由 router 接管（pop 时销毁）。
 * page->id 必须指向静态存储期字符串（会被日志/查找使用）。 */
esp_err_t ui_router_push(const page_desc_t *page);
esp_err_t ui_router_pop(void);
esp_err_t ui_router_reset_to_home(void);
/* 当前栈深（首页为 1） */
int ui_router_depth(void);

/* ------------------------------------------------------------------ 页面返回键 */

/*
 * 标题栏（状态栏正下方那一条）—— 版面口径见 ui_metrics.h 的 UI_TITLEBAR_*：
 *
 *     [ ◀返回钮 ]    <标题/信息…>
 *
 * 返回钮是烘好的位图 back_arrow（assets/icons/back.png：蓝圆底 + 白左箭头），
 * 交互是**按下缩小、松手还原、还原动画跑完才返回上一级**（见 ui_core.cc）。
 *
 * 两种用法，二选一，别同时用（会同屏出现两个返回键）：
 *
 * 1) **全局返回键（推荐）**：ui_core_init 时在 lv_layer_top 叠一个，
 *    ui_router_push/pop 之后自动刷新显示状态：**栈深 > 1 且当前页
 *    desc.no_back == false 时显示**，否则隐藏。所以：
 *      - 首页（栈深 1）不显示；
 *      - 普通二级页（播放器 / 在线音乐 / 本地音乐…）自动就有返回键，页面里
 *        不用再画；标题按下面的写法摆到同一条中线上即可：
 *            lv_obj_align(title, LV_ALIGN_TOP_LEFT,
 *                         UI_HEADER_TEXT_X, ui_titlebar_text_dy());
 *      - 页面 screen 的 `pad_top` 要是 UI_TITLEBAR_Y（= 状态栏高 + 页面安全区），
 *        这行才是对的（各二级页本来就是这么设的）。
 * 2) **自带返回钮的页面**（设置页 make_page_shell）：用 ui_back_button_create()
 *    建按钮，并把 page_desc_t.no_back 置 true。
 *
 * 该键建在 lv_layer_top 上、z 序低于控制中心/Toast/遮罩：
 * 模态遮罩打开时它会一并被盖住（点不到，符合预期），关闭后自动恢复。
 */

/*
 * 标题从标题栏顶端往下偏多少才落在**中线上**（按标题字体的行盒真算，
 * 换字体也不会偏）。配合 UI_HEADER_TEXT_X 用，见上面的用法 1。
 *
 * ⚠ 只适用于"标题直接挂在 screen 上、且**页面里没有会滚到这一条的滚动区**"的页面。
 *   只要页面有滚动内容，就必须按下面这条排：
 *     父容器 flex 列 → ① 标题栏盒子（固定高 UI_TITLEBAR_H）② 滚动盒子（flex_grow 1）
 *   —— LVGL 裁剪子对象用的是对象自己的盒子、**不算 padding**，靠 pad_top 错开的话
 *   滚动内容会把标题盖掉（真机踩过，见 docs/lvgl-pitfalls.md §4）。
 */
int ui_titlebar_text_dy(void);

/*
 * 手动返回键（给"自带返回钮"的页面用，如设置页子页外壳）：
 *   按下缩小 → 松手还原 → **还原动画结束**才 ui_router_pop()。
 *
 * @param parent 父容器。**建议直接传 screen**，然后调
 *               ui_back_button_place() 统一摆位；父容器是 flex 也没关系
 *               （建出来的按钮自带 LV_OBJ_FLAG_FLOATING，flex 会跳过它）。
 * @return 按钮对象，不需要时忽略返回值
 *
 * ⚠ 屏幕最左 UI_EDGE_HOTZONE(24 px) 是返回手势热区，它 z 序高于页面，
 *   所以按钮不能贴 x=0（左半截会被热区吃掉、点不动）：位置统一由
 *   ui_back_button_place() 给（UI_BACK_BTN_X / UI_BACK_BTN_Y）。
 * ⚠ 用自动全局返回键的页面**不要再调这个**，否则会出两个返回键。
 */
lv_obj_t *ui_back_button_create(lv_obj_t *parent);

/*
 * 把返回键摆到标题栏左侧：**圆心落在导航栏中线**（UI_TITLEBAR_MID，与标题、页签、
 * 右上角动作按钮同一条线），上下各悬出 UI_BACK_BTN_DIP
 * （几何见 ui_metrics.h 的 UI_BACK_BTN_*）。
 *
 * 为什么要有这个函数：`lv_obj_align` 的偏移是相对**父容器内容区**（扣掉 padding）
 * 算的，全局键的父容器是 `lv_layer_top()`（无 padding）、页面里的父容器是
 * `as_make_screen()` 造的 screen（有 padding）—— 各页自己写偏移就会差一个 padding、
 * 返回钮对不齐。本函数自己把父容器 padding 扣掉，落点全局一致。
 *
 * ⚠ 按钮是挂在 `lv_layer_top()` 上的、z 序永远最高，**悬出来的下半截会盖住
 *   栏下第一块内容**：有控件的页面必须让开 `UI_BACK_BTN_DIP`（做法见
 *   music_view_local 的列表、as_page_shell 的 body、app_online 的 tabs）。
 */
void ui_back_button_place(lv_obj_t *btn);

/*
 * 临时收起/恢复**全局**返回钮（图库全屏看图用；页面自带的返回钮不受影响）。
 * ⚠ 成对使用：谁 suppress(true) 谁负责在退出时 suppress(false)。
 * 收起后左边缘的返回手势热区仍然有效。
 */
void ui_back_button_suppress(bool suppress);

/*
 * 让**当前页**接管返回钮的语义（页面内部还有一层、但不想压路由栈时用）。
 *
 * 典型场景：在线音乐的"歌单详情"是同一页里换内容（不是新路由），
 * 点返回键应当先回到歌单列表，而不是直接退出整个在线音乐页。
 *
 *   handler 返回 true  = "我处理了"，ui_core 不再 ui_router_pop()；
 *   handler 返回 false = 交回默认行为（退栈）；
 *   handler 传 NULL    = 取消接管（恢复默认）。
 *
 * ⚠ **谁设置谁负责清**：页面 DELETE / on_exit 里必须再调一次 NULL，
 *   否则用户退出这一页之后，别的页面的返回键仍然会先问一个已经失效的 handler。
 * ⚠ 只改**行为**，返回钮的外观/位置/按下动画一律不变（见上面的版面口径）。
 */
void ui_back_button_set_handler(bool (*handler)(void));

/* ------------------------------------------------------------------ 页面底色 */

/*
 * 页面 screen 的**默认深色底**（`UI_COLOR_BG`，不透明）。
 *
 * 每个页面建完 screen 就调它，**不要**自己写 `remove_style_all + bg_color` ——
 * `lv_obj_remove_style_all()` 之后 `bg_opa` 是 **0（透明）**，只设颜色等于一个像素
 * 都不画，栅格里留着**上一屏的画面**：从首页进设置看到首页壁纸、从播放器进设置
 * 看到播放器那张背景图（2026-10-21 真机就这么"串"过一次）。
 */
void ui_screen_apply_default_bg(lv_obj_t *scr);

/* ------------------------------------------------------------------ 状态栏
 *
 * 状态栏那四个图标（Wi-Fi / 蓝牙 / 输出通道 / microSD）**由状态栏自己的定时器**
 * 从 wifi_service / bt_service / storage_service 读状态后画出来
 * （见 ui_status_bar.cc 的文件头）。
 *
 * 时钟也一样（2026-11 改）：以前是首页的 1 s tick 往里塞文本，导致"不在首页时
 * 状态栏时钟是停的"。现在由状态栏每 500 ms 自己取系统时间，格式由
 * `wifi_service_clock_show_seconds()`（显示设置 → 时钟显秒）决定。
 */

/*
 * 状态栏整条显隐（图库的"单击全屏看图"用它把状态栏收起来）。
 * ⚠ 谁隐藏谁负责恢复：页面销毁/退出时必须再调一次 true，否则整个系统没状态栏。
 */
void ui_status_bar_set_visible(bool visible);

/* ------------------------------------------------------------------ 控制中心 */

void ui_cc_open(void);
void ui_cc_close(void);
void ui_cc_toggle(void);
bool ui_cc_is_open(void);

/* ------------------------------------------------------------------ 轻提示 */

void ui_toast(const char *text, uint32_t duration_ms);

/*
 * 立刻收掉当前 Toast（不等 duration 到点）。
 * 给"结束时刻由代码决定"的场景用（例如开机启动图淡出：见 main/splash.cc）。
 * 没有 Toast 显示时是空操作。
 */
void ui_toast_hide(void);

/* ------------------------------------------------------------------ 弹层动画
 *
 * 弹窗/菜单/Toast 这类"突然出现、突然消失"的东西统一走这两个函数做**渐入渐出**
 * （淡入淡出 + 面板轻微上滑；160 ms 进 / 140 ms 出，ease_out）。各 app 自己搭的
 * 浮层（例如 app_online 的长按菜单、添加/重命名浮层）建好后调 ui_popup_in()，
 * 关闭时用 ui_popup_out() **代替** lv_obj_delete*()。
 *
 *   scrim —— 全屏遮罩（可传 NULL：没有遮罩的浮层）
 *   panel —— 内容面板；上滑只作用于它，遮罩只淡入淡出
 *
 * ui_popup_out() 会在动画结束后自己删掉 scrim（没 scrim 就删 panel），
 * 所以调用方**不要再删一次**，也不要在动画期间访问这些对象。
 */
void ui_popup_in(lv_obj_t *scrim, lv_obj_t *panel);
void ui_popup_out(lv_obj_t *scrim, lv_obj_t *panel);

/* ------------------------------------------------------------------ 音量悬浮条
 *
 * 从屏幕右边缘滑入的竖向音量条，停在**屏幕靠右中间**（见 ui_volume_panel.cc）。
 * 顶部 = 100%、往上拖 = 变大，填充自下而上增长（拖动方向与填充方向一致）。
 * 显示后 2.5 秒无操作自动缩回；拖动即跟手调音量。
 */
void ui_volume_show(void);
void ui_volume_hide(void);

/*
 * 硬件 VOL± 按键步进音量（一格 = 5%）：按一次 +1 / -1。
 *
 * **可以在任意任务上下文调用**（例如 bsp 按键回调里）：这里只做一次原子
 * 累加，真正的音量下发与"弹出音量条"由 ui_volume_panel.cc 的 LVGL 定时器
 * 在 LVGL 任务上下文完成。息屏（LVGL 暂停）期间计数会夹紧，不会积压爆发。
 */
void ui_volume_key_step(int dir);

/* ------------------------------------------------------------------ 息屏（停刷新，音频继续） */

void ui_screen_off(void);
void ui_screen_on(void);
bool ui_screen_is_off(void);

#ifdef __cplusplus
}
#endif