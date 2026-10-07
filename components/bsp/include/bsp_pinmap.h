/*
 * bsp_pinmap.h —— 板级引脚登记表（ESP32-S31-Korvo-1）
 *
 * 重要：这些宏**只用于文档与日志**。真正生效的引脚全部来自
 * esp_board_manager 的板级配置（components/gen_bmgr_codes/），业务代码
 * **不要**用这些宏去初始化外设，也不要在其它地方硬编码引脚。
 *
 * 数值出处：managed_components/espressif__esp_boards/esp32_s31_korvo_1/
 *           board_peripherals.yaml + board_devices.yaml
 * 与本文件不一致时以板级 YAML 为准。
 */
#pragma once

/* 音频 I2C（ES8389 控制口） */
#define BSP_I2C_SDA_GPIO      (0)
#define BSP_I2C_SCL_GPIO      (1)

/* 音频 I2S/TDM（ES8389 数据口）。板级 MCLK = -1（无 MCLK 引出） */
#define BSP_I2S_BCLK_GPIO     (3)
#define BSP_I2S_WS_GPIO       (4)
#define BSP_I2S_DOUT_GPIO     (5)
#define BSP_I2S_DIN_GPIO      (6)

/* 左右声道功放共用使能，高电平使能 */
#define BSP_PA_CTRL_GPIO      (7)

/* microSD（SDMMC 4-bit slot0）；SD_CTRL 低电平使能卡供电 */
#define BSP_SD_D0_GPIO        (20)
#define BSP_SD_D1_GPIO        (21)
#define BSP_SD_D2_GPIO        (22)
#define BSP_SD_D3_GPIO        (23)
#define BSP_SD_CLK_GPIO       (24)
#define BSP_SD_CMD_GPIO       (25)
#define BSP_SD_CTRL_GPIO      (39)

/* RGB LCD 控制信号（数据线 GPIO8~19 / GPIO33~36 由板级配置声明） */
#define BSP_LCD_PCLK_GPIO     (40)
#define BSP_LCD_DE_GPIO       (43)
#define BSP_LCD_HSYNC_GPIO    (44)
#define BSP_LCD_VSYNC_GPIO    (45)

/* 人机交互 */
#define BSP_ADC_BUTTON_GPIO   (42)   /* 4 键分压：SET/MODE/VOL-/VOL+ */
#define BSP_LED_WS2812_GPIO   (37)
#define BSP_BOOT_BUTTON_GPIO  (61)

/* 背光：官方板级配置**未定义背光设备**，本板软件无法调背光 */
#define BSP_BACKLIGHT_UNAVAILABLE  (1)