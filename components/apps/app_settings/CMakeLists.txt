idf_component_register(
    SRCS "app_settings.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_core
        bsp
        audio_service
        storage_service
        wifi_service
        bt_service
        esp_app_format
        esp_hw_support      # esp_chip_info / esp_mac / esp_clk（"关于本机"页）
        heap                # heap_caps_*：SRAM / PSRAM 用量
        spi_flash           # esp_flash_get_size：Flash 总容量
        esp_partition       # 运行分区的地址 / 大小
        app_update          # esp_ota_get_running_partition
        bootloader_support  # esp_image_get_metadata：镜像长度 → 算"Flash 可用"
        esp_timer           # 运行时长
        esp_driver_tsens    # 芯片温度（temperature_sensor）
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
