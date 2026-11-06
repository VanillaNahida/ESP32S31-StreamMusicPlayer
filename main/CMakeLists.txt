idf_component_register(
    SRCS "cxx_heap.cc" "main.cc" "splash.cc" "ime_dict_stub.c" "console_cmds.cc"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        lvgl
        esp_lvgl_adapter
        esp_timer
        console          # 串口诊断命令（songinfo / songscan / playstat）
        bsp
        ui_core
        media_meta
        audio_service
        usb_audio_service   # 首帧之后起 USB 声卡检测（UAC 1.0）
        voice_service
        app_music
        app_settings
        app_online
        app_momotalk
        app_gallery
        wifi_service
        bt_service
        online_service
)

# ---------------------------------------------------------------------------
# 开机启动图：assets/init.jpg（800x480 JPEG，约 355 KB）
#
# 用 BINARY 嵌入（**不要** TEXT：JPEG 数据里随便一个 0x00 都会被当成字符串结尾，
# 而 BINARY 不会补结尾 0）。符号名用 RENAME_TO 固定住，于是 splash.cc 里引用的
# 就是 _binary_boot_splash_jpg_start/_end，跟文件名怎么改都无关。
#
# 文件不存在时只警告、不报错，但 splash.cc 会因缺符号而链接失败 —— 那是有意的：
# 缺了启动图应该当场发现，而不是静静地少一个功能。真要彻底去掉启动图，
# 就同时删掉 main/splash.cc 与本段。
# ---------------------------------------------------------------------------
set(BOOT_SPLASH_JPG "${CMAKE_SOURCE_DIR}/assets/init.jpg")
if(EXISTS "${BOOT_SPLASH_JPG}")
    target_add_binary_data(${COMPONENT_LIB} "${BOOT_SPLASH_JPG}" BINARY
                           RENAME_TO "boot_splash_jpg")
    message(STATUS "main: 开机启动图 <- assets/init.jpg")
else()
    message(WARNING "main: 找不到 assets/init.jpg（见 main/splash.cc）")
endif()

# esp_lvgl_adapter / esp_board_manager 的配置宏使用部分聚合初始化，
# C++ 编译时会触发 -Wmissing-field-initializers（IDF 默认按 error 处理）。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
