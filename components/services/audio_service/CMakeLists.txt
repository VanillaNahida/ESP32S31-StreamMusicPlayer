idf_component_register(
    SRCS "audio_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        bsp
        esp_audio_simple_player
        esp_codec_dev
        gmf_core
        media_meta
        bt_service
        play_state
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)