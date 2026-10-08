/*
 * bsp_display.cc —— RGB LCD + GT1151 触摸，经 esp_lvgl_adapter 接入 LVGL
 *
 * 时序、面板参数、framebuffer（PSRAM）与触摸配置全部由 esp_board_manager 的
 * esp32_s31_korvo_1 板级配置提供，本文件不写任何 LCD 寄存器序列。
 */
#include "bsp.h"
#include "bsp_internal.h"
#include "bsp_pinmap.h"

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "bsp_disp";

static lv_display_t *s_disp;
static lv_indev_t   *s_indev;
static int           s_backlight_pct = 80;   /* 仅记录，见 bsp_backlight_set */

/* 由板级 lcd_cfg 推导旋转（与官方 display_lvgl 示例一致） */
static esp_lv_adapter_rotation_t lcd_rotation(const dev_display_lcd_config_t *c)
{
    if (c->swap_xy) {
        return c->mirror_x ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_270;
    }
    return (c->mirror_x || c->mirror_y) ? ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
}

/* 由 framebuffer 数量推导 tear-avoid 模式（本板 num_fbs = 1 → NONE） */
static esp_lv_adapter_tear_avoid_mode_t lcd_tear_mode(uint8_t num_fbs)
{
    if (num_fbs >= 3) return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    if (num_fbs == 2) return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
}

esp_err_t bsp_display_register_lvgl(lv_display_t **out_disp)
{
    esp_err_t ret = bsp_device_ensure(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init display_lcd failed: %s", esp_err_to_name(ret));
        return ret;
    }

    void *lcd_handle = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (ret != ESP_OK || !lcd_handle) {
        ESP_LOGE(TAG, "get display_lcd handle failed: %s", esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    dev_display_lcd_config_t *lcd_cfg = NULL;
    ret = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&lcd_cfg);
    if (ret != ESP_OK || !lcd_cfg) {
        ESP_LOGE(TAG, "get display_lcd config failed: %s", esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    dev_display_lcd_handles_t *h = (dev_display_lcd_handles_t *)lcd_handle;

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        h->panel_handle, h->io_handle, lcd_cfg->lcd_width, lcd_cfg->lcd_height,
        lcd_rotation(lcd_cfg));
    disp_cfg.tear_avoid_mode = lcd_tear_mode(lcd_cfg->sub_cfg.rgb.panel_config.num_fbs);

    /*
     * 绘制缓冲（LVGL 画一"条"，再 flush 给面板）。
     *
     * 默认宏给的是 buffer_height = 50 且 use_psram = false：
     *   - 480 行 ÷ 50 = 一帧要 **10 次 flush**，每次 80 KB。滑屏动画每帧都是整屏
     *     重绘，这 10 次调用的固定开销（每次都要按区域做 cache 回写 + 提交面板
     *     传输）是实打实的白给；
     *   - 80 KB 还占的是**内部 RAM** —— 本工程内部 RAM 只有 ~300 KB，WiFi/蓝牙/
     *     音频都在抢。
     *
     * 改成 120 行 × 800 × 2 B = 192 KB，一帧 4 条；放 PSRAM（16 MB，不紧张）。
     * 依据：esp_lvgl_adapter README「缓冲调优」——提高 buffer_height 减少 flush
     * 次数、提升吞吐量，代价只是 RAM。
     *
     * 想回退/微调就改这两个数：48~120 都合理（越小越省 RAM、flush 越多）。
     */
    disp_cfg.profile.buffer_height = 120;
    disp_cfg.profile.use_psram     = true;

    /*
     * 打开 PPA（Pixel Processing Accelerator，带 2D-DMA）硬件加速。
     *
     * ESP32-S31 有 PPA（SOC_PPA_SUPPORTED / SOC_DMA2D_SUPPORTED 均为 1），
     * esp_lvgl_adapter 明确把 esp32s31 列为 PPA 目标平台：置位后它会为 RGB565
     * 目标注册自定义 blend/fill 处理器，把**填充与 alpha 混合**从 CPU 挪到 PPA，
     * CPU 只做几何与命令解析（官方说明：显著降低 CPU 占用，FPS 提升不明显）。
     *
     * 三个前提都已满足，改动时注意别破坏：
     *   1) LV_DRAW_SW_DRAW_UNIT_CNT 必须为 1（sdkconfig.defaults 已锁定）；
     *   2) 绘制缓冲要按 cache line 对齐 —— 适配器用
     *      esp_cache_get_alignment(MALLOC_CAP_SPIRAM) 分配，本就满足；
     *      万一不满足，PPA 处理器会自己退回软件路径，不会画错；
     *   3) 面板 framebuffer 数量仍是板级配置的 1（PPA 只加速"绘制"这一段，
     *      不改变 LCD 外设的 RGB 连续扫描通路 —— 那条路本来就是 DMA）。
     */
    disp_cfg.profile.enable_ppa_accel = true;

    ESP_LOGI(TAG, "display %ux%u rotation=%d tear=%d fb_num=%u ppa=%d",
             (unsigned)lcd_cfg->lcd_width, (unsigned)lcd_cfg->lcd_height,
             (int)lcd_rotation(lcd_cfg), (int)disp_cfg.tear_avoid_mode,
             (unsigned)lcd_cfg->sub_cfg.rgb.panel_config.num_fbs,
             (int)disp_cfg.profile.enable_ppa_accel);

    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "esp_lv_adapter_register_display failed");
        return ESP_FAIL;
    }
    if (out_disp) {
        *out_disp = s_disp;
    }
    return ESP_OK;
}

esp_err_t bsp_touch_register_lvgl(lv_display_t *disp, lv_indev_t **out_indev)
{
    if (!disp) {
        disp = s_disp;
    }
    if (!disp) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH)) {
        ESP_LOGW(TAG, "board has no lcd_touch device");
        return ESP_OK;   /* 无触摸不是致命错误 */
    }

    esp_err_t ret = bsp_device_ensure(ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "init lcd_touch failed: %s (继续，无触摸)", esp_err_to_name(ret));
        return ret;
    }

    void *touch_handle = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle);
    if (ret != ESP_OK || !touch_handle) {
        ESP_LOGW(TAG, "get lcd_touch handle failed: %s", esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    dev_lcd_touch_handles_t *th = (dev_lcd_touch_handles_t *)touch_handle;
    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, th->touch_handle);

    s_indev = esp_lv_adapter_register_touch(&touch_cfg);
    if (!s_indev) {
        ESP_LOGW(TAG, "esp_lv_adapter_register_touch failed");
        return ESP_FAIL;
    }
    if (out_indev) {
        *out_indev = s_indev;
    }
    ESP_LOGI(TAG, "touch registered");
    return ESP_OK;
}

int bsp_display_width(void)
{
    return (int)lv_display_get_horizontal_resolution(s_disp);
}

int bsp_display_height(void)
{
    return (int)lv_display_get_vertical_resolution(s_disp);
}

esp_err_t bsp_backlight_set(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_backlight_pct = percent;

#if BSP_BACKLIGHT_UNAVAILABLE
    /* 官方 esp32_s31_korvo_1 板级配置里没有 lcd_brightness 设备，硬件上也没有
     * 软件可控的背光脚，因此这里无法真正调光。息屏请用 esp_lv_adapter_pause()。 */
    return ESP_ERR_NOT_SUPPORTED;
#else
    return ESP_OK;
#endif
}

int bsp_backlight_get(void)
{
    return s_backlight_pct;
}