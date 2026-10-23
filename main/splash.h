/*
 * splash.h —— 开机启动图（assets/init.jpg）
 *
 * 时序（见 main.cc）：
 *   1. app_main 一开始就 boot_splash_decode_start()：后台任务把这张 800x480 的
 *      JPEG 软解成 RGB565（PSRAM，~770 KB），**与后面的初始化并行**；
 *   2. **显示链路一就绪（esp_lv_adapter_start 之后立刻）** 就调
 *      boot_splash_early_show()：建一个**不依赖 ui_core 的独立 screen**（背景色 +
 *      启动图 + 底部状态行"正在初始化…"），这是用户看到的第一帧；
 *      之后每一步慢初始化（存储卡 / 播放服务 / 界面）都先
 *      boot_splash_set_status("正在…") 把话说清楚；
 *   3. ui_core_init() 建好全局层之后 boot_splash_show() 把启动图挪到
 *      lv_layer_top() 的浮层上（早期 screen 顺带收掉），再补一条 Toast；
 *   4. 后台初始化（WiFi / 蓝牙）结束时调 boot_splash_init_done()，
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
 * **第一帧**：在 esp_lv_adapter_start() 之后立刻调用（要持 LVGL 锁或在 LVGL 任务里）。
 * 建一个独立 screen（不是 layer_top 浮层，因为 ui_core 还没建）：背景色 + 启动图
 * （解好就贴，没解好由轮询补）+ 底部状态行。可重复调用，第二次无效。
 *
 * 之所以要有它：板子从 bsp_init 到 ui_core_init 之间有一串**慢初始化**
 * （SD 挂载 / 播放服务 / 曲库服务），以前全在这串跑完之后才出画面 —— 用户看到
 * 的就是"黑屏好几秒"。现在先出画面，再把慢活放到画面后面做。
 */
void boot_splash_early_show(void);

/*
 * 更新早期屏底部那行状态文字（"正在挂载存储卡…" 之类）。
 * 任意任务上下文可调：内部自己拿/放 LVGL 锁；早期屏已经收掉时是空操作。
 */
void boot_splash_set_status(const char *text);

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
