idf_component_register(
    SRCS "play_state.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
