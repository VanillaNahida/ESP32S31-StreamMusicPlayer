/*
 * mm_cover.h —— 封面提取编排 + 独立图文件 / 内存缓冲入口
 *
 * 实现见 mm_cover.cc。像素缓冲的所有权契约与拆分前完全一致：
 * 成功时 *pixels 由 mm_image.cc 用 **jpeg_calloc_align** 分配（16 字节对齐），
 * 调用方必须用 mm_cover_free()（内部 jpeg_free_align）释放 —— 换分配器就是堆损坏。
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
 * 把一段图片字节拷成调用方所有的一块内存（与 FLAC 封面共用的最小工具）。
 * 成功返回 ESP_OK 并填 out/out_len/is_png，失败返回 ESP_ERR_NO_MEM。
 * 放在本模块是为了让 mm_id3.cc 与 flac_cover() 共用一份，不重复实现。
 * （原 media_meta.cc 的 static copy_image()）
 */
esp_err_t mm_copy_image(uint8_t **out, size_t *out_len, bool *is_png,
                        const uint8_t *src, size_t len, bool png);

/* 音频内嵌封面（MP3 = ID3v2、FLAC = PICTURE）→ RGB565（原 media_meta_cover_rgb565） */
esp_err_t mm_cover_from_audio(const char *audio_path, int max_dim,
                              uint8_t **pixels, int *out_w, int *out_h);

/* 独立封面图文件（JPEG/PNG，按魔数分派）→ RGB565（原 media_meta_image_file_rgb565） */
esp_err_t mm_cover_from_file(const char *img_path, int max_dim,
                             uint8_t **pixels, int *out_w, int *out_h);

/* 内存缓冲里的封面图 → RGB565（原 media_meta_image_mem_rgb565；只读不持有 data） */
esp_err_t mm_cover_from_mem(const uint8_t *data, size_t len, int max_dim,
                            uint8_t **pixels, int *out_w, int *out_h);

/* 释放上面的像素缓冲（传 NULL 安全）——必须与分配用的 JPEG 对齐分配器配对 */
void mm_cover_free(uint8_t *pixels);

#ifdef __cplusplus
}
#endif
