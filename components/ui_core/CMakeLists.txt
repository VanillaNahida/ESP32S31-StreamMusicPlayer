idf_component_register(
    SRCS
        "ui_metrics.cc"
        "ui_spring.cc"
        "ui_app_registry.cc"
        "ui_router.cc"
        "ui_core.cc"
        "ui_status_bar.cc"
        "ui_home.cc"
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
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)