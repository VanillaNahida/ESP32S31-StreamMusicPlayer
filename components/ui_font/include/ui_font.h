/*
 * ui_font.h —— UI 文本字体（中文主字体 + 日文汉字 fallback 链）
 *
 * 为什么需要这个包装
 * ------------------
 * 字体本身是 tools/font/gen_lvgl_font.py 生成的三个 **const** 位图字体：
 *
 *     ui_font_sc16    GB2312 + ASCII + LVGL 图标   4 bpp   ← 中文/UI 主字体
 *     ui_font_jp1_16  JIS 第 1 水準 + 日文记号     4 bpp   ← 常用日文汉字
 *     ui_font_jp2_16  JIS 第 2 水準                2 bpp   ← 生僻日文汉字
 *
 * GB2312 是简体中文字符集，"鎖""転"这类**日文汉字**的码位根本不在里面
 * （中文对应的是 锁/转，码位不同），所以日文曲名会缺字成方框。LVGL 9 的
 * `lv_font_t.fallback` 支持"本字体没有就去备用字体找，可递归"，但 `fallback`
 * 是字体结构体的一个字段 —— 而生成的字体在 `.rodata` 里，**不能运行期改写**
 * （写 flash 只读区会直接崩）。所以这里做一份**可写的副本**把链串起来。
 *
 * 用法
 * ----
 *   - UI 代码一律用 ui_metrics.h 的 UI_FONT_TEXT / TITLE / SMALL（都指向它）；
 *   - **必须先 ui_font_init()**（ui_core_init 的第一件事）再创建任何标签 ——
 *     在那之前 ui_font_text 还是全零，拿它渲染会崩。
 *
 * 体积账（实测常量数据）
 * ---------------------
 *   主字体 559.0 KB + jp1 92.0 KB + jp2 85.5 KB = 736.5 KB
 *   （改 bpp 前先看 tools/font/gen_lvgl_font.py 顶部的说明：app 分区很紧。）
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 主字体 + 日文 fallback 链的可写包装。用法与普通 lv_font_t 完全一样。 */
extern lv_font_t ui_font_text;

/* 建好 fallback 链。幂等；必须在创建任何标签之前调用一次。 */
void ui_font_init(void);

#ifdef __cplusplus
}
#endif
