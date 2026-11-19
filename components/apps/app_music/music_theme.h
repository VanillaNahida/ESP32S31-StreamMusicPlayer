/*
 * music_theme.h —— 播放器页"跟随封面"的主题色（强调色）
 *
 * 需求（2026-11）：进度条与控制排按钮的主题色**跟着封面走**，没有封面时用
 * 默认的**蔚蓝色**（不是 UI_COLOR_ACCENT 那个琥珀色 —— 那个是全局强调色，
 * 播放器页整屏都靠封面取色，不跟它走）。
 *
 * 分工：
 *   - **解码线程**（music_cover.cc 的 cover_task）：解出 RGB565 后调
 *     music_theme_rgb_from_cover()，把结果写进 music_state.cover_accent；
 *   - **LVGL 任务**：只读 music_theme_accent_rgb() / music_theme_dim_rgb()，
 *     由 music_view_player_accent_apply() 分发到各控件（有缓存，颜色没变不重设）。
 *
 * 为什么不在 UI 侧现算：240x240 逐像素统计有几十微秒到几百微秒级开销，
 * 放在解码线程里做完全不影响出声与刷新。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 没有封面（或封面是纯黑白灰）时的默认主题色：亮蔚蓝 */
#define MUSIC_THEME_DEFAULT_RGB 0x4A9EFF

/*
 * 从 RGB565 像素里挑一个"能当强调色"的主色（0xRRGGBB）。
 *
 * 做法：HSV 24 段色相直方图（按饱和度×亮度加权），丢掉近黑/近白/低饱和像素，
 * 取权重最高那一段的加权均值，最后把 S/V 规整到暗背景上好看的范围。
 * 全图都没有可用颜色（纯黑白封面）时返回 **0** —— 由调用方回落到默认蔚蓝。
 *
 * 纯函数、不碰 LVGL、不开辟内存，可在工作线程调用。
 */
uint32_t music_theme_rgb_from_cover(const uint16_t *px, int w, int h);

/* 当前主题色（0xRRGGBB）：没有封面时是 MUSIC_THEME_DEFAULT_RGB */
uint32_t music_theme_accent_rgb(void);

/* 次要控件的底色（主题色混进面板灰）：控制排里除播放键以外的圆按钮用它 */
uint32_t music_theme_dim_rgb(void);

#ifdef __cplusplus
}
#endif
