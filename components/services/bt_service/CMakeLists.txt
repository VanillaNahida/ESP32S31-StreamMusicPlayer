idf_component_register(
    SRCS
        "bt_service.cc"
        "bts_state.cc"
        "bts_name_cache.cc"
        "bts_settings.cc"
        "bts_audio.cc"
        "bts_feed.cc"
        "bts_events.cc"
        "bts_scan_pair.cc"
        "bts_conn.cc"
        "bts_volume.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include/internal"
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
