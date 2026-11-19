/*
 * mm_mem.h —— media_meta 里"大块临时缓冲"的统一分配入口
 *
 * 为什么需要它（2026-11，真机踩过）：
 * 封面 / 歌词 / 探测缓冲动辄几十 KB 到几 MB，而这些代码原来一律用裸 `malloc()`。
 * 在 ESP-IDF 里裸 `malloc` 走的是**内部 RAM**（默认堆），而本工程内部 RAM 在播
 * 高解析音频时只剩 ~26 KB（`play stats: mem int 26 KB … dma 1`）—— 一次 200 KB 的
 * APIC/FLAC PICTURE 拷贝、或者 mm_probe 那 32 KB 的首帧探测，就足以把内部堆（以及
 * 其中"可 DMA"的那部分）挤干，紧接着 SD 驱动 `allocate_dma_buf` 拿不到 512 B，
 * `sdmmc_read_blocks failed (0x101)` → 切歌时"解码崩溃 / 封面歌词都没有"。
 *
 * 所以：**大块一律先要 PSRAM**，拿不到才退回内部 RAM（PSRAM 也满的极端情况）。
 * 释放端继续用 `free()` 即可 —— heap_caps_* 分配的内存与 malloc 是同一个堆。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 大块临时缓冲：PSRAM 优先，失败退回内部 RAM；两者都失败返回 NULL。 */
void *mm_alloc_big(size_t size);

#ifdef __cplusplus
}
#endif
