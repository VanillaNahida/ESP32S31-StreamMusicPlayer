/*
 * bsp.cc —— 板级支持层公共部分
 *
 * 设备采用**惰性初始化**（与官方 display_lvgl / play_sdcard_music 示例一致）：
 * 不调用 esp_board_manager_init() 一次性拉起全部设备，而是各自按需
 * esp_board_manager_init_device_by_name()，避免把本项目用不到的摄像头等
 * 设备也初始化一遍（耗时且会刷错误日志）。
 */
#include "bsp.h"
#include "bsp_internal.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "bsp";

static bool s_nvs_ready;

esp_err_t bsp_init(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_nvs_ready = true;
    ESP_LOGI(TAG, "board: %s (%s) v%s",
             g_esp_board_info.name, g_esp_board_info.chip, g_esp_board_info.version);
    return ESP_OK;
}

/* 内部使用：按需初始化一个板级设备（引用计数，重复调用安全） */
esp_err_t bsp_device_ensure(const char *name)
{
    if (!esp_board_manager_check_name(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_board_manager_init_device_by_name(name);
}