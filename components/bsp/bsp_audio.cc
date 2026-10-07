/*
 * bsp_audio.cc —— ES8389 DAC 通路 + NS4150B 功放使能
 *
 * 寄存器序列（含 TDM 格式、无 MCLK 配置）全部由 esp_board_manager 的
 * audio_dac 设备通过 esp_codec_dev 完成，本文件不写寄存器。
 *
 * 板上只有左右扬声器（无 3.5 mm 座）；蓝牙输出走片内经典蓝牙 A2DP，
 * 由 services/bt_manager 负责，切到蓝牙时必须把 PA 拉低避免双出声。
 */
#include "bsp.h"
#include "bsp_internal.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "bsp_audio";

#define AUDIO_DAC_DEV  ESP_BOARD_DEVICE_NAME_AUDIO_DAC   /* "audio_dac" */
#define PA_PERIPH_NAME "gpio_pa_control"

static esp_codec_dev_handle_t s_dac;
static int s_volume = 55;
static bool s_muted = true;
static int16_t s_pa_gpio = -1;

static void pa_resolve(void)
{
    if (s_pa_gpio >= 0) {
        return;
    }
    void *h = NULL;
    if (esp_board_manager_get_periph_handle(PA_PERIPH_NAME, &h) == ESP_OK && h) {
        s_pa_gpio = (int16_t)((periph_gpio_handle_t *)h)->gpio_num;
        ESP_LOGI(TAG, "PA_CTRL on GPIO%d", s_pa_gpio);
    } else {
        ESP_LOGW(TAG, "PA peripheral %s not found", PA_PERIPH_NAME);
    }
}

esp_err_t bsp_audio_init(void)
{
    if (s_dac) {
        return ESP_OK;
    }

    esp_err_t ret = bsp_device_ensure(AUDIO_DAC_DEV);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init %s failed: %s", AUDIO_DAC_DEV, esp_err_to_name(ret));
        return ret;
    }

    void *handle = NULL;
    ret = esp_board_manager_get_device_handle(AUDIO_DAC_DEV, &handle);
    if (ret != ESP_OK || !handle) {
        ESP_LOGE(TAG, "get %s handle failed: %s", AUDIO_DAC_DEV, esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    s_dac = ((dev_audio_codec_handles_t *)handle)->codec_dev;
    if (!s_dac) {
        ESP_LOGE(TAG, "codec_dev is NULL");
        return ESP_FAIL;
    }

    pa_resolve();

    /* 上电先保持静音并关功放，避免"哒"声 */
    esp_codec_dev_set_out_mute(s_dac, true);
    bsp_audio_pa_enable(false);
    ESP_LOGI(TAG, "ES8389 DAC ready (muted)");
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec(void)
{
    return s_dac;
}

esp_err_t bsp_audio_open_output(uint32_t rate_hz, uint8_t channels, uint8_t bits)
{
    if (!s_dac) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = channels,
        .sample_rate = rate_hz,
    };
    esp_err_t ret = esp_codec_dev_open(s_dac, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s (%u Hz %uch %ubit)",
                 esp_err_to_name(ret), (unsigned)rate_hz, channels, bits);
        return ret;
    }
    ESP_LOGI(TAG, "codec opened: %u Hz %uch %ubit", (unsigned)rate_hz, channels, bits);
    /* 打开后按当前静音状态恢复 */
    return esp_codec_dev_set_out_mute(s_dac, s_muted);
}

esp_err_t bsp_audio_pa_enable(bool on)
{
    if (s_pa_gpio < 0) {
        pa_resolve();
    }
    if (s_pa_gpio < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 板级 active_level = 1：高电平使能 */
    return gpio_set_level((gpio_num_t)s_pa_gpio, on ? 1 : 0);
}

esp_err_t bsp_audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;

    if (!s_dac) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 主音量走 ES8389 数字音量寄存器，软件增益只做补充 */
    return esp_codec_dev_set_out_vol(s_dac, percent);
}

int bsp_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    s_muted = mute;
    if (!s_dac) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_set_out_mute(s_dac, mute);
}

esp_err_t bsp_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_dac) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate_hz,
    };
    /* 改采样率前后都要静音再恢复，见 开发指南 6.6 */
    esp_err_t ret = esp_codec_dev_set_out_mute(s_dac, true);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_open(s_dac, &fs);
    }
    if (ret == ESP_OK && !s_muted) {
        ret = esp_codec_dev_set_out_mute(s_dac, false);
    }
    return ret;
}