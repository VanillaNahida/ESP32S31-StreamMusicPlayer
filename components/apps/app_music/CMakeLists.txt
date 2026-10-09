idf_component_register(
    SRCS "app_music.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_core
        bsp
        audio_service
        storage_service
        media_meta
        play_state
        bt_service
        online_service
        app_settings
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)