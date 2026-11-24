# UI 位图字体（预渲染），运行时从 fonts 分区 mmap 加载。
#
# 五个 .c 是 lv_font_conv 的生成物，**不要手改**；重新生成：
#   tools/font/venv/Scripts/python.exe tools/font/gen_subset.py   （先切字符集）
#   python tools/font/gen_lvgl_font.py                            （再出字体 .c）
#
# ⚠ 从 2026-11 起这些 .c **不再参与编译**：它们是 pack_fonts.py 的输入，
#   字形数据最终进 fonts 分区（partitions.csv），app 里只留加载器。
#   只编 ui_font.c（mmap 加载 + fallback 链）。
#
# 字体镜像 fonts.bin 由下面的自定义命令生成到 build/，并随 idf.py flash
# 自动写到 fonts 分区（esptool_py_flash_to_partition）。
idf_component_register(
    SRCS
        "ui_font.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        esp_partition
        heap
        esp_rom
)

# ---------------------------------------------------------------------------
# fonts 分区镜像
#
# 输入是 lv_font_conv 生成的五个 .c（保留在仓库里，便于从 C 源重新打包）；
# 输出 build/fonts.bin，格式见 tools/font/pack_fonts.py 顶部。
# 改了字体 .c / 打包脚本 / sdkconfig（LARGE 布局）都会自动重打。
# ---------------------------------------------------------------------------
set(UI_FONT_C_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ui_font_sc16.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui_font_sc22.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui_font_jp1_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui_font_jp2_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui_font_dict16.c"
)
set(UI_FONT_PACK_SCRIPT "${CMAKE_SOURCE_DIR}/tools/font/pack_fonts.py")
set(UI_FONT_BIN "${CMAKE_BINARY_DIR}/fonts.bin")

idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT "${UI_FONT_BIN}"
    COMMAND "${python}" "${UI_FONT_PACK_SCRIPT}" --out "${UI_FONT_BIN}" --verify
    DEPENDS ${UI_FONT_C_SOURCES} "${UI_FONT_PACK_SCRIPT}" "${CMAKE_SOURCE_DIR}/sdkconfig"
    COMMENT "打包 UI 位图字体 -> fonts.bin（fonts 分区）"
    VERBATIM
)
add_custom_target(ui_font_bin ALL DEPENDS "${UI_FONT_BIN}")
add_dependencies(${COMPONENT_LIB} ui_font_bin)

if(TARGET flash)
    esptool_py_flash_to_partition(flash "fonts" "${UI_FONT_BIN}")
    add_dependencies(flash ui_font_bin)
else()
    message(WARNING
        "ui_font: 没有 flash 目标，fonts.bin 不会自动烧写；"
        "请手动执行 idf.py flash 或 esptool.py write_flash <fonts 分区偏移> ${UI_FONT_BIN}")
endif()
