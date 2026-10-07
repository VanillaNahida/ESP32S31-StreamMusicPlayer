idf_component_register(
    SRCS "bt_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        esp_event
        esp_bt_audio
        esp_audio_codec
        esp_asrc
        nvs_flash
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
