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
 *   16 px 主字体 1.01 MB + jp1 4.2 KB + jp2 0.7 KB + dict16 762 KB ≈ 1.79 MB
 *   22 px 大标题（ui_font_sc22，GB2312 第一级）1.02 MB
 *   合计约 2.81 MB（app 槽 12.25 MB，见 partitions.csv 的沿革）。
 *   （改 bpp / 字符集前先看 tools/font/gen_lvgl_font.py 顶部的说明：app 分区很紧。）
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 主字体 + 日文 fallback 链的可写包装。用法与普通 lv_font_t 完全一样。 */
extern lv_font_t ui_font_text;
/* 生僻字 + extra_chars.txt（2bpp）。挂在 ui_font_text 的 fallback **链尾**：
 * 常用字走 sc16（4bpp，质量不变），生僻字自动落到这份 —— 见 ui_font.c 的链条说明。
 * 单独一份的原因：LVGL 紧凑字形描述符的 bitmap_index 只有 20 位 ⇒ 单份字形数据 ≤1MB ✗。*/
extern lv_font_t ui_font_dict16;

/*
 * 22 px 大标题字体（2026-11 UI 重构稿：播放器歌名 / 歌词当前行 / 歌单名 / 账号昵称）。
 *
 * 只有 **GB2312 第一级汉字（3755）+ 区 1~9 符号标点 + ASCII**，4 bpp ——
 * 全量 GB2312 在 22 px 下要 1.7 MB，app 槽放不下（见 docs/ui-refactor-2026-11.md）。
 * fallback 指向 ui_font_text：第一级之外的字落到 16 px **同一个字**，
 * 只是小一号、不会变方框 —— 这是"敢只收第一级"的前提。
 *
 * ⚠ 与 ui_font_text 一样，**必须先 ui_font_init()** 再给任何标签用。
 */
extern lv_font_t ui_font_title22;

/* 建好 fallback 链。幂等；必须在创建任何标签之前调用一次。 */
void ui_font_init(void);

#ifdef __cplusplus
}
#endif
