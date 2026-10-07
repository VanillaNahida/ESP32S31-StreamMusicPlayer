idf_component_register(
    SRCS "app_settings.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_core
        bsp
        audio_service
        storage_service
        wifi_service
        bt_service
        esp_app_format
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
