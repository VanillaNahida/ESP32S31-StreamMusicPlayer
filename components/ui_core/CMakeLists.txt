idf_component_register(
    SRCS
        "ui_metrics.cc"
        "ui_spring.cc"
        "ui_app_registry.cc"
        "ui_router.cc"
        "ui_core.cc"
        "ui_ba.cc"          # 碧蓝档案风格弹窗工具箱（见 include/ui_ba.h）
        "ui_status_bar.cc"
        "ui_home.cc"
        "ui_home_bg.cc"
        "ui_control_center.cc"
        "ui_volume_panel.cc"
        "ui_gesture.cc"
        "ui_icons.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_font
        esp_lvgl_adapter
        esp_board_manager
        esp_timer
        bsp
        audio_service
        media_meta
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------
# 首页背景图：assets/background.jpg（800x480 JPEG，约 347 KB）
#
# 与启动图（main/CMakeLists.txt）同一套做法：BINARY 嵌入（**不要** TEXT ——
# JPEG 数据里随便一个 0x00 都会被当成字符串结尾），RENAME_TO 把符号名固定成
# _binary_home_bg_jpg_start/_end，于是 ui_home_bg.cc 里的引用与文件名无关。
#
# 资源归 ui_core 所有是有意的：画首页的组件自己带着首页的图，
# 换壁纸只需替换 assets/background.jpg，不用碰 main。
# 文件不存在时只警告，链接期会因缺符号而失败（缺资源应当场发现）。
# ---------------------------------------------------------------------------
set(HOME_BG_JPG "${CMAKE_SOURCE_DIR}/assets/background.jpg")
if(EXISTS "${HOME_BG_JPG}")
    target_add_binary_data(${COMPONENT_LIB} "${HOME_BG_JPG}" BINARY
                           RENAME_TO "home_bg_jpg")
    message(STATUS "ui_core: 首页背景图 <- assets/background.jpg")
else()
    message(WARNING "ui_core: 找不到 assets/background.jpg（见 ui_home_bg.cc）")
endif()
