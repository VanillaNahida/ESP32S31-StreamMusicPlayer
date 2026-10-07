# 自带 UI 位图字体（梦源黑体 16px，GB2312 全集 + LVGL 图标）。
# ui_font_sc16.c 是生成物，不要手改；重新生成见 tools/font/gen_lvgl_font.py。
idf_component_register(
    SRCS "ui_font_sc16.c"
    REQUIRES lvgl
)