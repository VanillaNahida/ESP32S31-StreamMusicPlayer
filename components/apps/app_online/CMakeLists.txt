idf_component_register(
    SRCS "app_online.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_core
        app_music
        audio_service
        online_service
        media_meta
        wifi_service
        lvgl_pinyin_ime
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
