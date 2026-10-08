idf_component_register(
    SRCS "cxx_heap.cc" "main.cc" "ime_dict_stub.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        lvgl
        esp_lvgl_adapter
        bsp
        ui_core
        audio_service
        app_music
        app_settings
        app_online
        wifi_service
        bt_service
        online_service
)

# esp_lvgl_adapter / esp_board_manager 的配置宏使用部分聚合初始化，
# C++ 编译时会触发 -Wmissing-field-initializers（IDF 默认按 error 处理）。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)