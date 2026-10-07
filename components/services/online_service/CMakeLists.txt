idf_component_register(
    SRCS "online_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        mbedtls
        cjson
        nvs_flash
        fatfs
        esp_timer
        wifi_service
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
