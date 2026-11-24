/*
 * ui_font.h —— UI 文本字体（fonts 分区 + mmap + fallback 链）
 *
 * 字体放在哪
 * ----------
 * 五个预渲染的 LVGL 位图字体**不再编进 app**，而是放在独立的 fonts 分区
 * （partitions.csv，data 类型 0x41），运行时由 ui_font.c 用
 * esp_partition_mmap() 映射后直接使用：
 *
 *     sc16    GB2312 + ASCII + LVGL 图标   4 bpp   ← 中文/UI 主字体
 *     sc22    GB2312 第一级 + 符号 + ASCII 4 bpp   ← 22 px 大标题
 *     jp1     JIS 第 1 水準 + 日文记号     4 bpp   ← 常用日文汉字
 *     jp2     JIS 第 2 水準                2 bpp   ← 生僻日文汉字
 *     dict16  词典生僻字 + extra_chars     2 bpp   ← 链尾补充
 *
 * 字形位图 / glyph_dsc 都指在 flash 映射区（不复制到 RAM），只有 cmap 表
 * 在 RAM 里重建（几 KB）。字体镜像由 tools/font/pack_fonts.py 从
 * lv_font_conv 生成的 .c 打包，idf.py flash 会自动写 fonts 分区
 * （见 components/ui_font/CMakeLists.txt）。
 *
 * 为什么还要 fallback 链
 * ----------------------
 * GB2312 是简体中文字符集，"鎖""転"这类**日文汉字**的码位根本不在里面
 * （中文对应的是 锁/转，码位不同），日文曲名会缺字成方框。LVGL 9 的
 * lv_font_t.fallback 支持"本字体没有就去备用字体找，可递归"；但字体结构体
 * 在映射区里是只读的，fallback 字段不能就地改 —— 所以 ui_font.c 把每个
 * 字体复制成一份**可写的 lv_font_t**（几十字节，共享同一份 dsc/位图）再串链：
 *
 *     sc16(常用中文,4bpp) → jp1(常用日文,4bpp) → jp2(生僻日文,2bpp)
 *         → dict16(生僻字+补充表,2bpp)
 *
 *     sc22(22px 标题) → sc16 链（第一级之外的字落到 16 px 同一个字，
 *                              只是小一号、不会变方框）
 *
 * 用法
 * ----
 *   - UI 代码一律用 ui_metrics.h 的 UI_FONT_TEXT / TITLE / SMALL / TITLE_22；
 *   - **必须先 ui_font_init()**（ui_core_init 的第一件事）再创建任何标签 ——
 *     在那之前 ui_font_text 还是 LVGL 默认字体（或全零），拿它排版会不对；
 *   - fonts 分区没烧 / 校验失败时，ui_font_init() 会退回 LVGL 默认字体
 *     （中文会变方框，但不会崩），并把原因打到串口。
 *
 * 体积账（实测）
 * -------------
 *   16 px 主字体 1.01 MB + jp1 4.1 KB + jp2 0.6 KB + dict16 762 KB ≈ 1.78 MB
 *   22 px 大标题（ui_font_sc22，GB2312 第一级）1.02 MB
 *   镜像合计 2.73 MB（fonts 分区 2.75 MB，见 partitions.csv）。
 *   app 槽因此从 12.31 MB 缩到 9.56 MB；固件实测 9.31 MB，仍余约 261 KB。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 主字体 + 日文 fallback 链的可写包装。用法与普通 lv_font_t 完全一样。 */
extern lv_font_t ui_font_text;

/*
 * 22 px 大标题字体（播放器歌名 / 歌词当前行 / 歌单名 / 账号昵称）。
 *
 * 只有 **GB2312 第一级汉字（3755）+ 区 1~9 符号标点 + ASCII**，4 bpp ——
 * 全量 GB2312 在 22 px 下要 1.7 MB，放不进 fonts 分区（2.75 MB 要装五个字体）。
 * fallback 指向 ui_font_text：第一级之外的字落到 16 px **同一个字**，
 * 只是小一号、不会变方框 —— 这是"敢只收第一级"的前提。
 *
 * ⚠ 与 ui_font_text 一样，**必须先 ui_font_init()** 再给任何标签用。
 */
extern lv_font_t ui_font_title22;

/* 建好 fonts 分区映射 + fallback 链。幂等；必须在创建任何标签之前调用一次。 */
void ui_font_init(void);

/* 字体是否从 fonts 分区加载成功（false = 正在用 LVGL 默认字体兜底）。 */
bool ui_font_is_ready(void);

#ifdef __cplusplus
}
#endif
