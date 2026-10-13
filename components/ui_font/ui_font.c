/*
 * ui_font.c —— 把"中文主字体 + 日文汉字补充字体"串成 fallback 链
 *
 * 背景与体积账见 include/ui_font.h 的注释。这里只强调两个易踩的点：
 *
 *   1) 生成的字体（ui_font_sc16 / ui_font_jp1_16 / ui_font_jp2_16）都是
 *      `const lv_font_t`，落在 .rodata（flash）里。`fallback` 是结构体字段，
 *      直接 `((lv_font_t *)&ui_font_sc16)->fallback = ...` 就是往只读 flash 写
 *      → Store access fault。所以这里复制出**可写副本**再改字段。
 *      副本只是几十字节的结构体，共享同一份 dsc/位图，几乎不花 RAM。
 *
 *   2) 链的顺序 = 查找顺序：主字体（中文 + 图标）→ jp1（常用日文，4bpp）
 *      → jp2（生僻日文，2bpp）。LVGL 递归地沿 fallback 找，找到就用，
 *      所以常用日文汉字走 4bpp 那份，只有生僻字才落到 2bpp。
 */
#include "ui_font.h"

LV_FONT_DECLARE(ui_font_sc16)
LV_FONT_DECLARE(ui_font_jp1_16)
LV_FONT_DECLARE(ui_font_jp2_16)

/* jp1 的可写副本：它自己要指向 jp2，所以也得能改（见注释 1） */
static lv_font_t s_jp1;
/* 主字体的可写副本：UI_FONT_* 全部指向它 */
lv_font_t ui_font_text;
// 链尾的"生僻字"字体副本（要改 fallback 指针，见文件头说明：不能改 flash 里的原件）
static lv_font_t s_jp2;
static lv_font_t s_dict;

void ui_font_init(void)
{
    /* 结构体拷贝：line_height / dsc / get_glyph_dsc 等原样带过来，
     * 位图与查找表仍是同一份（只读共享，不额外占 flash） */
    ui_font_text = ui_font_sc16;

    s_jp1 = ui_font_jp1_16;
    /*
 * 链条（2026-10 拆成四份，每份都压在 1 MB 的 20 位索引上限内 ✓）：
 *   sc16(常用中文,4bpp) → jp1(常用日文,4bpp) → jp2(生僻日文,2bpp) → dict16(生僻字+补充表,2bpp)
 */
s_jp2 = ui_font_jp2_16;
s_dict = ui_font_dict16;
s_jp2.fallback = &s_dict;
s_jp1.fallback = &s_jp2;

    ui_font_text.fallback = &s_jp1;
}
