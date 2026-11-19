/*
 * music_spectrum.h —— 播放器页的音频可视化（FFT 频谱条）
 *
 * 需求（2026-11）：播放时在**进度条上方**显示一条音频可视化波形；
 * 设置弹窗里可以开/关（music_menu.cc）。
 *
 * 数据从哪来：audio_service 的 **PCM 监视回调**（audio_service_set_pcm_monitor）——
 * 输出任务把最终 48k/16bit/2ch 的 PCM 交给 write_out() 之后顺手喂给我们，
 * 我们在回调里只做"左右取平均 + 塞进 SPSC 环形缓冲"，绝不做 FFT。
 * FFT 由 LVGL 任务里的 40 ms lv_timer 做：**乐鑫官方 esp-dsp**（dsps_fft2r_fc32，
 * N=1024、Hann 窗 → 归一化 → log20 幅度 → 对数频率分带 → 线性映射柱高）。
 *
 * 线程纪律：
 *   - spectrum_pcm_cb() 跑在**输出任务**（prio 6）：只 memcpy，不调 LVGL、不调 audio_service_*；
 *   - 其余函数全部是 LVGL 任务上下文（attach/detach 在页面建/删里调）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * app 启动时调一次：读 NVS 里的开关 + 注册 PCM 监视回调（与页面是否打开无关）。
 * ★ 这里**不分配任何内存** —— 它跑在射频初始化之前，内部 RAM 要留给 Wi-Fi/蓝牙
 *   协议栈与 TLS 的 AES DMA（2026-11 真机踩过：频谱先吃掉 36 KB 内部 RAM，
 *   随后 esp-aes 申请不到 DMA 缓冲，HTTPS 写请求失败、账号歌单拉取失败）。
 */
void music_spectrum_init(void);

/* 建控件：在播放器页右侧列里、进度条**之前**调（列顺序 = 视觉顺序）。
 * 内存（环形缓冲 + FFT/窗/dsp 表）在这里才懒分配，环形缓冲优先 PSRAM。 */
void music_spectrum_attach(lv_obj_t *parent);
/* 页面销毁时调：删定时器、清指针，并把 FFT/窗占的内部 RAM 还给系统
 * （子控件随父对象一起销毁，**不要**手动删） */
void music_spectrum_detach(void);

bool music_spectrum_enabled(void);
/* 开/关：立即生效（隐藏/显示 + 暂停/恢复定时器）+ 落 NVS */
void music_spectrum_set_enabled(bool on);

/* 主题色变化时调（music_view_player_accent_apply）：柱子的底色跟着封面走 */
void music_spectrum_set_accent(uint32_t rgb);

#ifdef __cplusplus
}
#endif
