idf_component_register(
    SRCS
        "bsp.cc"
        "bsp_display.cc"
        "bsp_storage.cc"
        "bsp_audio.cc"
        "bsp_input.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        lvgl
        esp_board_manager
        esp_lvgl_adapter
        esp_codec_dev
        led_strip
        button
)

# esp_board_manager / esp_lvgl_adapter 的配置宏使用部分聚合初始化（见 main/CMakeLists.txt）
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)