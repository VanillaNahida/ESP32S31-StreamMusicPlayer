idf_component_register(
    SRCS "wifi_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
