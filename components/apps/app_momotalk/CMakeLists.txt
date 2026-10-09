idf_component_register(
    SRCS "app_momotalk.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_core
        lvgl_pinyin_ime
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
