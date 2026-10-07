/*
 * ui_metrics.cc —— 横屏度量自检
 */
#include "ui_metrics.h"

#include "esp_log.h"

static const char *TAG = "ui_metrics";

void ui_metrics_selfcheck(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "no default display yet");
        return;
    }
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    if (w != UI_W || h != UI_H) {
        /* 旋转配置最容易踩的坑：编译期 UI_W/UI_H 与运行期分辨率不一致 */
        ESP_LOGW(TAG, "resolution mismatch: lvgl=%ldx%ld, UI_W/UI_H=%dx%d "
                      "(检查 CONFIG_APP_DISPLAY_ROTATION_*)",
                 (long)w, (long)h, UI_W, UI_H);
    } else {
        ESP_LOGI(TAG, "UI metrics ok: %dx%d (portrait=%d)", w, h, UI_IS_PORTRAIT);
    }
}