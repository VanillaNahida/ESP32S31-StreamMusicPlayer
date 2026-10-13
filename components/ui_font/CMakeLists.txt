# 自带 UI 位图字体（梦源黑体 16px）。
#
# 三个 .c 都是生成物，**不要手改**；重新生成：
#   tools/font/venv/Scripts/python.exe tools/font/gen_subset.py      （先切字符集）
#   python tools/font/gen_lvgl_font.py                              （再出字体）
#   - ui_font_sc16.c   GB2312 全集 + ASCII + LVGL 图标（4 bpp，中文/UI 主字体）
#   - ui_font_jp1_16.c JIS 第 1 水準 + 日文记号（4 bpp，常用日文汉字）
#   - ui_font_jp2_16.c JIS 第 2 水準（2 bpp，生僻日文汉字）
# ui_font.c 是手写的：把三者串成 LVGL 的 fallback 链（见 include/ui_font.h）。
idf_component_register(
    SRCS
        "ui_font.c"
        "ui_font_sc16.c"
        "ui_font_jp1_16.c"
        "ui_font_jp2_16.c"
    "ui_font_dict16.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
)
