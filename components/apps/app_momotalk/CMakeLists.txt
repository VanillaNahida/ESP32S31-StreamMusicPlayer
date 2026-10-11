idf_component_register(
    SRCS "app_momotalk.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        voice_service   # 语音输入宿主（WS + 麦克风）
        lvgl
        ui_core
        lvgl_pinyin_ime
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
