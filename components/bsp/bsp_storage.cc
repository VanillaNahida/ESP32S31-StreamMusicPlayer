/*
 * bsp_storage.cc —— 板载 microSD（4-bit SDMMC + SD_CTRL）
 *
 * 引脚、总线宽度、频率、挂载点与供电控制全部由板级配置 fs_sdcard 提供。
 * 注意：SD_CTRL(GPIO39) 是**低电平使能**卡供电，由板级的 sdcard_power_ctrl
 * 设备负责，本文件不直接拉 GPIO。
 */
#include "bsp.h"
#include "bsp_internal.h"

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"          /* sdmmc_card_t（bsp_sdcard_card_info 用） */
#include "sd_protocol_defs.h"   /* SD_OCR_SDHC_CAP / SD_OCR_S18_RA */
#include "esp_board_manager_includes.h"

static const char *TAG = "bsp_sd";

#define SD_DEV_NAME       ESP_BOARD_DEVICE_NAME_FS_SDCARD   /* "fs_sdcard" */
#define SD_DEFAULT_MOUNT  "/sdcard"

static bool s_mounted;
static char s_mount_point[32] = SD_DEFAULT_MOUNT;

esp_err_t bsp_sdcard_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (!esp_board_manager_check_name(SD_DEV_NAME)) {
        ESP_LOGW(TAG, "board has no %s device", SD_DEV_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = bsp_device_ensure(SD_DEV_NAME);
    if (ret != ESP_OK) {
        /* 没插卡是正常情况：不崩，本地音乐入口显示"未检测到 SD 卡" */
        ESP_LOGW(TAG, "sdcard mount failed: %s (未插卡或卡异常)", esp_err_to_name(ret));
        return ret;
    }

    void *handle = NULL;
    ret = esp_board_manager_get_device_handle(SD_DEV_NAME, &handle);
    if (ret != ESP_OK || !handle) {
        ESP_LOGE(TAG, "get %s handle failed: %s", SD_DEV_NAME, esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    dev_fs_fat_handle_t *fat = (dev_fs_fat_handle_t *)handle;
    if (fat->mount_point) {
        /* 工程规范 F9：IDF 开了 -Werror，字符串拷贝一律用 snprintf，不用 strncpy */
        snprintf(s_mount_point, sizeof(s_mount_point), "%s", fat->mount_point);
    }

    s_mounted = true;
    ESP_LOGI(TAG, "sdcard mounted at %s", s_mount_point);
    return ESP_OK;
}

void bsp_sdcard_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    ESP_LOGI(TAG, "unmounting sdcard");
    esp_board_manager_deinit_device_by_name(SD_DEV_NAME);
    s_mounted = false;
}

bool bsp_sdcard_is_mounted(void)
{
    return s_mounted;
}

const char *bsp_sdcard_mount_point(void)
{
    return s_mount_point;
}

esp_err_t bsp_sdcard_capacity_kb(uint32_t *total_kb, uint32_t *free_kb)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(s_mount_point, &total, &free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    if (total_kb) {
        *total_kb = (uint32_t)(total / 1024);
    }
    if (free_kb) {
        *free_kb = (uint32_t)(free_bytes / 1024);
    }
    return ESP_OK;
}

/*
 * 卡的物理信息（"设置 → 存储"详情页要显示的额外几行）。
 *
 * 解码口径**照抄 IDF 的 sdmmc_card_print_info()**（sdmmc/sdmmc_common.c:314-361），
 * 别自己发明：类型靠 ocr 的 CCS/S18R 位判，数据线宽度取 ssr.cur_bus_width
 * （那是**当前**宽度，不是卡支持的最大值）。
 */
esp_err_t bsp_sdcard_card_info(bsp_sd_card_info_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    void *handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(SD_DEV_NAME, &handle);
    if (ret != ESP_OK || !handle) {
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }
    const dev_fs_fat_handle_t *fat = (const dev_fs_fat_handle_t *)handle;
    const sdmmc_card_t *card = fat->card;
    if (!card) {
        return ESP_ERR_NOT_FOUND;
    }

    out->card_bytes   = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    out->freq_khz     = card->real_freq_khz;
    out->max_freq_khz = (int)card->max_freq_khz;
    out->bus_width    = card->ssr.cur_bus_width ? 4 : 1;
    out->ddr          = card->is_ddr != 0;

    if (card->is_mmc) {
        snprintf(out->type, sizeof(out->type), "MMC");
    } else if (card->is_sdio) {
        snprintf(out->type, sizeof(out->type), "SDIO");
    } else if ((card->ocr & SD_OCR_SDHC_CAP) == 0) {
        snprintf(out->type, sizeof(out->type), "SDSC");
    } else if (card->ocr & SD_OCR_S18_RA) {
        snprintf(out->type, sizeof(out->type), "SDHC/SDXC (UHS-I)");
    } else {
        snprintf(out->type, sizeof(out->type), "SDHC");
    }

    /* CID 里的产品名是 8 字节、**空格填充**的（不是 NUL 结尾），要自己截 + 去尾空格 */
    snprintf(out->name, sizeof(out->name), "%.8s", card->cid.name);
    for (int i = (int)strlen(out->name) - 1; i >= 0 && out->name[i] == ' '; i--) {
        out->name[i] = '\0';
    }
    out->serial = (uint32_t)card->cid.serial;
    return ESP_OK;
}