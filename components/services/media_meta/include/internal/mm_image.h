/*
 * mm_image.h —— 封面图解码（JPEG / PNG）→ RGB565 + 盒式降采样
 *
 * 实现见 mm_image.cc。**唯一的对外出口是 mm_image_to_rgb565()**：
 * 按数据魔数分派解码器，输出 RGB565 little-endian、长边不超过 max_dim。
 *
 * ⚠ 输出缓冲由本模块用 **jpeg_calloc_align**（16 字节对齐）分配，
 *   必须用 mm_cover_free()（jpeg_free_align）释放 —— 换成别的分配器就是堆损坏。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 图片字节（JPEG 或 PNG，按魔数分派）→ RGB565 LE。
 * 成功时 *out_pixels 是 jpeg_calloc_align 分配的缓冲（调用方用 mm_cover_free 释放）。
 */
esp_err_t mm_image_to_rgb565(const uint8_t *data, size_t len, int max_dim,
                             uint8_t **out_pixels, int *out_w, int *out_h);

/*
 * **大图**版本（图库 App 用）：JPEG 走 esp_new_jpeg 解码期 scale，峰值内存≈输出。
 * 与上面那条路的区别见 mm_image.cc 的说明（手机照片整解会 OOM）。
 * 输出同样是 jpeg_calloc_align 分配，用 mm_cover_free() 释放。
 */
esp_err_t mm_photo_file_rgb565(const char *path, int max_dim,
                               uint8_t **out_pixels, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
