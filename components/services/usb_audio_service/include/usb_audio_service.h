/*
 * usb_audio_service.h —— USB 声卡（UAC 1.0 Speaker）输出
 *
 * 需求（2026-10）：
 *   1. 能把音频输出到 USB 声卡；
 *   2. **该选项只在插着声卡时出现**且可切换；
 *   3. 播放中声卡被意外拔出 → **立刻**切回内置（板载）输出。
 *
 * 分工（层级纪律，见 AGENTS §6）：
 *   本组件只负责 USB 侧：装 Host/UAC 驱动、检测插拔、把 PCM 写进 UAC 端点。
 *   **不碰 LVGL、不改路由** —— 插拔通过状态回调告告诉 audio_service，由它决定切不切。
 *
 * 为什么是 UAC 1.0 / 48 kHz / 16-bit / 2ch：
 *   - 目标设备（0C76:1710 USB PnP Audio Device）只支持 **48 kHz**、UAC 1.0、2 声道；
 *   - 本工程输出级已经恒为 **48 kHz/16bit/2ch**（esp_asrc 统一），所以**不需要任何
 *     额外重采样**：同一块 PCM 直接写 USB —— 192 B/ms 与音频时间轴口径完全一致；
 *   - 选 16 bit 而不是 24 bit：省带宽、省 CPU（设备两档都支持）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 状态回调。**在 USB/UAC 驱动任务里被调用**：不要在里面做重活、不要碰 LVGL
 * （要弹 Toast 请交给 UI 自己的定时器）。
 * ready=true：声卡已打开且流已启动，可以切过去；ready=false：拔出/传输失败。
 */
typedef void (*usb_audio_state_cb_t)(bool ready, void *user);

/*
 * 装 USB Host + UAC 类驱动并开始后台检测。幂等。
 * 建议在**首帧之后**调用（见 main.cc）：它会装 USB PHY、起两个任务，
 * 放在开机关键路径上会白白拖慢第一帧。
 */
esp_err_t usb_audio_init(void);

/* 声卡已就绪（已打开 + 流已启动），可以推 PCM / 可以切到这个输出 */
bool usb_audio_is_ready(void);

/* 设备名（产品名 UTF-8；没连上或没名字时返回 "USB 声卡"）。LVGL 任务里读是安全的。 */
const char *usb_audio_device_name(void);

/*
 * 推一块 48 kHz/16-bit/2ch PCM（内部按 10 ms 分块喂，任意大小都行）。
 *
 * 返回值语义（**调用方必须区分**）：
 *   ESP_OK               —— 整块都进了 UAC 环形缓冲；
 *   ESP_ERR_INVALID_STATE —— **设备真的不在了**（接口不再 ACTIVE）。调用方应当
 *                            立刻回落板载；本组件内部也已经把 ready 清 0 并通知上层；
 *   其它（ESP_FAIL 等）  —— 只是这一块没写进去（典型是 UAC 环形缓冲满），
 *                            **不代表拔出**，丢掉这块继续放就行。
 */
esp_err_t usb_audio_write(const void *pcm, size_t bytes, uint32_t timeout_ms);

/* 设备侧音量（UAC 的 volume control，0~100）。没连上时只是记下来。 */
void usb_audio_set_volume(int percent);

/* 注册状态回调（只保留一个；传 NULL 取消） */
void usb_audio_set_state_cb(usb_audio_state_cb_t cb, void *user);

/*
 * 给**用户看的一句话**（插入 / 拔出 / 不支持）。与状态回调分开的原因：
 *   状态回调回答"现在能不能往 USB 推 PCM"（决定路由）；
 *   提示回调负责"告诉用户发生了什么"（包括"插上了但用不了"这种 ready 一直是 false 的情况）。
 * 同样在 USB 驱动任务里调用：**不要在里面碰 LVGL**，把字符串存下来交给 UI 定时器。
 */
typedef void (*usb_audio_notice_cb_t)(const char *msg, void *user);
void usb_audio_set_notice_cb(usb_audio_notice_cb_t cb, void *user);

/*
 * 当前实际使用的位深（16 / 24 / 32）；未起流返回 0。
 * 选择策略是**自动判断**：设备有 24-bit alt 就用 24（见 open_speaker），
 * 没有就回退到设备自己的默认（16，再不行 32）。UI 用它显示"24bit"。
 */
int  usb_audio_bit_depth(void);

#ifdef __cplusplus
}
#endif
