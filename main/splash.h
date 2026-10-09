/*
 * splash.h —— 开机启动图（assets/init.jpg）
 *
 * 时序（见 main.cc）：
 *   1. app_main 一开始就 boot_splash_decode_start()：后台任务把这张 800x480 的
 *      JPEG 软解成 RGB565（PSRAM，~770 KB），**与后面的初始化并行**，等屏幕起来时
 *      图通常已经就绪；
 *   2. 显示链路 ready（esp_lv_adapter_start）且 ui_core_init() 建好全局层之后，
 *      boot_splash_show() 在 lv_layer_top() 上盖一张全屏图 + 一条 Toast
 *      "正在初始化..."；
 *   3. 后台初始化（WiFi / 蓝牙）结束时调 boot_splash_init_done()，
 *      启动图淡出 + 上滑，露出已经建好的首页。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 起后台解码任务（越早调用越好；可重复调用，第二次无效）。任意任务上下文。 */
void boot_splash_decode_start(void);

/*
 * 盖全屏启动图 + Toast "正在初始化..."。
 * ⚠ 必须在 LVGL 任务上下文（或 esp_lv_adapter_lock() 保护下）调用，
 *   且要在 ui_core_init() **之后** —— Toast 对象是 ui_core_init() 建的。
 */
void boot_splash_show(void);

/* 后台初始化完成（任意任务可调；只置一个标志，LVGL 侧的定时器会来消费） */
void boot_splash_init_done(void);

/* 启动图是否还盖在屏幕上（调试/自检用） */
bool boot_splash_active(void);

#ifdef __cplusplus
}
#endif
