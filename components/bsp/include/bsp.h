/*
 * bsp.h —— 板级支持层对外接口
 *
 * 职责只有三件事（见 开发指南 5.1）：
 *   1. 集中登记引脚（bsp_pinmap.h，仅供文档/日志）
 *   2. 薄封装 esp_board_manager 的初始化结果，对外暴露统一句柄
 *   3. 暴露板级能力探测（SD 是否挂载、背光、功放使能）
 *
 * 本层**不做业务**，也不手写任何寄存器序列。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_codec_dev.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 生命周期 */

/* NVS + esp_board_manager 全局初始化（幂等） */
esp_err_t bsp_init(void);

/* ---------------------------------------------------------------- 显示/触摸 */

/* 把 RGB 面板注册进 LVGL（esp_lvgl_adapter），失败返回非 0 */
esp_err_t bsp_display_register_lvgl(lv_display_t **out_disp);

/* 把 GT1151 触摸注册为 LVGL 输入设备（没有触摸时不报错） */
esp_err_t bsp_touch_register_lvgl(lv_display_t *disp, lv_indev_t **out_indev);

/* 显示尺寸（横屏下即 800 x 480；旋转后由 LVGL 报告） */
int bsp_display_width(void);
int bsp_display_height(void);

/*
 * 背光。**本板官方板级配置未定义背光设备**，无法真正调光；
 * 这里只记录目标值供 UI 显示，并返回 ESP_ERR_NOT_SUPPORTED。
 * 息屏请走 esp_lv_adapter_pause()/resume()（停刷新）而不是关背光。
 */
esp_err_t bsp_backlight_set(int percent);
int bsp_backlight_get(void);

/* ---------------------------------------------------------------- 存储 */

/* 挂载板载 microSD（4-bit SDMMC + SD_CTRL 使能）；未插卡返回错误但不崩 */
esp_err_t bsp_sdcard_mount(void);
void bsp_sdcard_unmount(void);
bool bsp_sdcard_is_mounted(void);
const char *bsp_sdcard_mount_point(void);
/* 容量（KB）；未挂载返回 0 */
esp_err_t bsp_sdcard_capacity_kb(uint32_t *total_kb, uint32_t *free_kb);

/*
 * SD 卡的**物理信息**（"设置 → 存储"详情页用；容量之外的那些）。
 * 取不到的字段保持 0 / 空串，不报错 —— UI 少显示一行即可。
 *
 * 各字段来源（与 IDF 的 sdmmc_card_print_info() 同一套解码，别自己发明）：
 *   card_bytes = csd.capacity × csd.sector_size，type 由 ocr 的 CCS/S18R 位判定，
 *   数据线宽度取 ssr.cur_bus_width（**当前**宽度，不是卡支持的最大值）。
 */
typedef struct {
    uint64_t card_bytes;    /* 卡容量（CSD） */
    int      freq_khz;      /* 实际工作频率 */
    int      max_freq_khz;  /* 卡支持的最高频率 */
    int      bus_width;     /* 当前数据线位宽：1 / 4 / 8 */
    bool     ddr;           /* 是否 DDR 模式 */
    char     type[20];      /* SDSC / SDHC / SDHC/SDXC (UHS-I) / MMC / SDIO */
    char     name[16];      /* CID 里的产品名（尾部空格已去掉） */
    uint32_t serial;        /* CID 序列号 */
} bsp_sd_card_info_t;

/* 未挂载返回 ESP_ERR_INVALID_STATE；拿到卡句柄失败返回其他错误码 */
esp_err_t bsp_sdcard_card_info(bsp_sd_card_info_t *out);

/* ---------------------------------------------------------------- 音频 */

/* 初始化 ES8389 DAC 通路（保持静音） */
esp_err_t bsp_audio_init(void);
/* 底层 esp_codec_dev 句柄（供 GMF out 回调写 PCM；未初始化返回 NULL） */
esp_codec_dev_handle_t bsp_audio_codec(void);
/* 以指定格式打开输出（播放前必须调用；本板 I2S 固定 48k / 16bit / 2ch TDM） */
esp_err_t bsp_audio_open_output(uint32_t rate_hz, uint8_t channels, uint8_t bits);
/* 功放 NS4150B 使能（GPIO7，高电平有效） */
esp_err_t bsp_audio_pa_enable(bool on);
/* 主音量 0~100，走 ES8389 数字音量寄存器 */
esp_err_t bsp_audio_set_volume(int percent);
int bsp_audio_get_volume(void);
esp_err_t bsp_audio_set_mute(bool mute);
/* 采样率（Hz），切换音源时需重设 */
esp_err_t bsp_audio_set_sample_rate(uint32_t sample_rate_hz);

/* ---------------------------------------------------------------- 指示 */

/* WS2812 RGB LED（GPIO37）。percent 用于亮度缩放 */
esp_err_t bsp_led_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_led_off(void);

/* ---------------------------------------------------------------- 按键
 *
 * 板载四键共用 GPIO42 的电阻分压（ESP32-S31 ADC1 ch0），由板级
 * adc_button_group 设备（官方 iot_button）统一采样、去抖与长按判定。
 *
 * 档位与键名的对应关系**以原理图为准**（esp_boards 的 board_devices.yaml
 * 把 label 按电压升序写成了 SET/MODE/VOLUME_DOWN/VOLUME_UP，与原理图
 * 正好把 SET 和 VOL+ 弄反了）：
 *
 *   SW6  R82 1.3K  → 3.3 × 1.3/(10+1.3)  = 0.38 V → VOL+   （音量 +）
 *   SW5  R81 3.3K  → 3.3 × 3.3/(10+3.3)  = 0.82 V → VOL-   （音量 -）
 *   SW4  R80 6.8K  → 3.3 × 6.8/(10+6.8)  = 1.34 V → MODE
 *   SW3  R79 13K   → 3.3 × 13/(10+13)    = 1.87 V → SET
 *
 * 回调里的"按下"是**立即**的（PRESS_DOWN），"长按"按 KEY_HOLD 节奏重复触发，
 * 方便实现"按一下一格、长按连续调"。
 */
typedef enum {
    BSP_KEY_VOL_UP = 0,   /* SW6  380 mV  音量 + */
    BSP_KEY_VOL_DOWN,     /* SW5  820 mV  音量 - */
    BSP_KEY_MODE,         /* SW4 1340 mV  （暂未绑定功能） */
    BSP_KEY_SET,          /* SW3 1870 mV  （暂未绑定功能） */
    BSP_KEY_MAX,
} bsp_key_t;

typedef enum {
    BSP_KEY_EVT_PRESS = 0,   /* 按下（短按一次） */
    BSP_KEY_EVT_REPEAT,      /* 长按持续（已节流，约 8 次/秒） */
    BSP_KEY_EVT_RELEASE,     /* 抬起 */
} bsp_key_evt_t;

typedef void (*bsp_key_cb_t)(bsp_key_t key, bsp_key_evt_t evt, void *user);

/*
 * 初始化 ADC 分压按键并注册回调（幂等）。
 *
 * ⚠ 回调运行在 iot_button 的 esp_timer 周期任务上下文，**必须极短**，
 *   而且**不能碰 LVGL**；要更新界面就置标志 / 投递计数，由 LVGL 任务消费
 *   （音量条就是这么做的，见 ui_volume_key_step）。
 */
esp_err_t bsp_button_init(bsp_key_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif