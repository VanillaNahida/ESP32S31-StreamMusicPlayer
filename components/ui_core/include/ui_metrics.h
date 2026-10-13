/*
 * ui_metrics.h —— 横屏度量入口
 *
 * 纪律（开发指南 5.4）：**UI 代码里不准出现 800 / 480 字面量**，
 * 一律通过 UI_W / UI_H 与百分比。本文件是唯一允许出现参考分辨率的地方。
 *
 * 旋转是编译期开关（rot0 = 横屏默认）。90°/270° 时 LVGL 报告的宽高互换，
 * UI_W / UI_H 必须跟着走，所以下面所有派生尺寸都相对 UI_H / UI_W 计算而不是
 * 写死像素，换方向后能自动重排。
 */
#pragma once

#include "lvgl.h"

#if defined(CONFIG_APP_DISPLAY_ROTATION_90) || defined(CONFIG_APP_DISPLAY_ROTATION_270)
#define UI_W  (480)
#define UI_H  (800)
#define UI_IS_PORTRAIT (1)
#else
#define UI_W  (800)
#define UI_H  (480)
#define UI_IS_PORTRAIT (0)
#endif

/* 参考设计分辨率（仅本文件使用） */
#define UI_REF_W (800)
#define UI_REF_H (480)

/* 横向 / 纵向百分比 */
#define UI_PCT_X(p) ((p) * UI_W / 100)
#define UI_PCT_Y(p) ((p) * UI_H / 100)

/*
 * 关键尺寸：以参考横屏 800x480 为基准，按 UI_H / UI_W 等比缩放，
 * 保证旋转后比例关系不变。
 */
#define UI_PX_H(n) ((n) * UI_H / UI_REF_H)   /* 纵向按高度缩放 */
#define UI_PX_W(n) ((n) * UI_W / UI_REF_W)   /* 横向按宽度缩放 */

#define UI_STATUSBAR_H    UI_PX_H(32)    /* 状态栏高 */
#define UI_EDGE_HOTZONE   UI_PX_W(24)    /* 边缘手势热区（只有左边缘，见 ui_gesture.cc） */
#define UI_MIN_TOUCH      UI_PX_H(44)    /* 最小可点区域 */
#define UI_CC_PANEL_H     UI_PX_H(400)   /* 控制中心面板高 */
/*
 * 状态栏图标边长。
 *
 * ⚠ 这是**唯一一个不随 UI_H 缩放的尺寸**：位图是按这个 px 值烘进固件的
 *   （assets/icons/icons.txt 里那 11 条），而 LVGL 运行期缩放是每帧都要重算的
 *   （AGENTS.md 第 7 节）。改这里就必须同步改 icons.txt 并重新生成，
 *   否则图标会被拉伸 —— 所以刻意不给它套 UI_PX_H()。
 */
#define UI_STATUS_ICON    (22)
#define UI_LIST_ROW_H     UI_PX_H(64)    /* 列表行高 */
#define UI_LIST_COVER     UI_PX_H(48)    /* 列表封面 */
#define UI_MINI_PLAYER_H  UI_PX_H(56)    /* 迷你播放条高（首页已按需求移除，保留给后续页面复用） */
#define UI_HOME_ICON      UI_PX_H(96)    /* 首页图标 */
#define UI_HOME_CELL_W    UI_PX_W(130)   /* 首页格宽 */
#define UI_HOME_CELL_H    UI_PX_H(140)   /* 首页格高 */
#define UI_PAGE_PAD       UI_PX_W(8)     /* 页面安全区（不放可点元素） */

/*
 * 页面标题行的返回键。
 *
 * 左边缘 UI_EDGE_HOTZONE 是返回手势热区，它挂在 lv_layer_top 上、z 序高于页面，
 * 所以标题行左侧的按钮**不能贴 x=0**：贴边的话左半截被热区吃掉，按了没反应。
 * 按钮放在热区右侧，标题再让开按钮宽 + 间距。
 */
#define UI_BACK_BTN_X     UI_EDGE_HOTZONE
#define UI_BACK_BTN_SIZE  UI_PX_H(36)
#define UI_HEADER_TEXT_X  (UI_BACK_BTN_X + UI_BACK_BTN_SIZE + UI_PX_W(8))

/* 主题色（深色） */
#define UI_COLOR_BG          lv_color_hex(0x0B0B10)
#define UI_COLOR_BG_ALT      lv_color_hex(0x121218)
#define UI_COLOR_SURFACE     lv_color_hex(0x1B1B22)
#define UI_COLOR_PILL        lv_color_hex(0x2A2A33)
#define UI_COLOR_PILL_LIGHT  lv_color_hex(0xE6E6EA)
#define UI_COLOR_FG          lv_color_hex(0xF2F2F6)
#define UI_COLOR_FG_DIM      lv_color_hex(0x8C8C97)
#define UI_COLOR_ACCENT      lv_color_hex(0xF5A623)
#define UI_COLOR_ACCENT_ALT  lv_color_hex(0xE0863A)

/* 首页图标配色（对应设计稿） */
#define UI_COLOR_ICON_MUSIC   lv_color_hex(0xE8973A)
#define UI_COLOR_ICON_ONLINE  lv_color_hex(0x3E9E86)
#define UI_COLOR_ICON_LOCAL   lv_color_hex(0x5B6FB5)
#define UI_COLOR_ICON_CALENDAR lv_color_hex(0xC0584B)
#define UI_COLOR_ICON_BT      lv_color_hex(0x4A6FA5)
#define UI_COLOR_ICON_SETTINGS lv_color_hex(0x6E6E78)

/*
 * 状态栏图标配色（ui_status_bar.cc —— 单色位图 + image_recolor 上色）
 *
 * 为什么另开一组，不直接拿 ACCENT / FG_DIM 用：
 *   - **橙（ACCENT）留给"要注意的事"**：SD 读不出来（sd_warn）、音量条转弧。
 *     蓝牙"已连接"是**一切正常**的状态，也用橙会和警告撞色 —— 所以改成蓝色。
 *   - 蓝取 Material Blue 300（0x64B5F6），比首页图标那支 0x4A6FA5 **亮一档**：
 *     状态栏底色接近纯黑（0x08080C），偏深的蓝在那上面看着像"这格没点亮"。
 *   - **SD 可用用白**（与 Wi-Fi 已连接同色 FG）：sd_card 那个字形本身是**空心描边**，
 *     再配 FG_DIM(0x8C8C97) 会比旁边实心的 Wi-Fi / 蓝牙明显暗一档（用户反馈过）。
 */
#define UI_COLOR_STATUS_BT_LINK  lv_color_hex(0x64B5F6)

/*
 * 字体：
 *
 * UI 文本用 components/ui_font 里的位图字体（由 tools/font/gen_lvgl_font.py 生成，
 * 16 px 梦源黑体），一共三份、按 fallback 链串起来（见 ui_font.h）：
 *
 *   ui_font_sc16   GB2312 全集 + ASCII + LVGL 图标  4 bpp ← 中文/UI 主字体
 *   ui_font_jp1_16 JIS X 0208 第 1 水準 + 日文记号   4 bpp ← 常用日文汉字
 *   ui_font_jp2_16 JIS X 0208 第 2 水準              2 bpp ← 生僻日文汉字
 *
 * 为什么要有日文那两份：GB2312 是简体中文字符集，"鎖""転"这类**日文汉字**的码位
 * 不在里面（中文是 锁/转，码位不同），日文曲名会缺字成方框。
 * UI 代码只管用下面的宏 —— 指向的是 ui_font.c 里那个带 fallback 链的可写包装
 * `ui_font_text`，**但必须先 ui_font_init()**（ui_core_init 的第一件事）才能用。
 *
 * 为什么不用 LVGL 内置的 lv_font_source_han_sans_sc_16_cjk：
 *   它只收 "1338 most common CJK radicals"，GB2312 里其余汉字一律渲染成
 *   "缺字方框" —— 这是之前界面上成片方框的根因。
 *
 * 为什么 SMALL 也指向它：
 *   UI_FONT_SMALL 被用在状态栏、列表提示、控制中心小药丸等**含中文**的标签上，
 *   而 lv_font_montserrat_14 完全没有汉字，结果就是那些中文全变方框。
 *
 * 数字仍走蒙塞拉特，等宽数字（文泉驿微米黑）在 M5 接入。
 */
#include "ui_font.h"

#define UI_FONT_TEXT   (&ui_font_text)
#define UI_FONT_TITLE  (&ui_font_text)
#define UI_FONT_CLOCK  (&lv_font_montserrat_40)
#define UI_FONT_NUM    (&lv_font_montserrat_20)
#define UI_FONT_SMALL  (&ui_font_text)

#ifdef __cplusplus
extern "C" {
#endif

/* 创建页面后自检：LVGL 报告的分辨率与 UI_W 不一致时打警告（旋转配置最易踩的坑）*/
void ui_metrics_selfcheck(void);

#ifdef __cplusplus
}
#endif