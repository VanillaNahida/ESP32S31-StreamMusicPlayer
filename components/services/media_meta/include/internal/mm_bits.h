/*
 * mm_bits.h —— 字节序 / ID3 synchsafe / MIME 判定的**纯函数**
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）。
 * 本头文件与 mm_bits.cc **只用 libc，不 include 任何 ESP-IDF 头**，
 * 可以直接拿到 host 上单独编译跑单测（蓝图 §6.3 的"字节序 / synchsafe"用例）。
 *
 * 每个读取函数都要求调用方保证指针后至少有对应字节数 —— 与拆分前一致：
 * 调用点在读之前都已经做过长度检查，这里不重复判。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 大端 32 位 */
uint32_t mm_be32(const uint8_t *p);
/* 大端 24 位（FLAC 块长度） */
uint32_t mm_be24(const uint8_t *p);
/* ID3v2 的 7-bit synchsafe 长度 */
uint32_t mm_synchsafe32(const uint8_t *p);
/* 小端 16 位（WAV 用） */
uint32_t mm_le16(const uint8_t *p);
/* 小端 32 位（WAV 用） */
uint32_t mm_le32(const uint8_t *p);

/* 只看 image/png 前缀（大小写不敏感）——**仅用于日志/回退提示**：
 * 解码一律按文件魔数分派（见 mm_image.cc），APIC 的 MIME 字段常常是错的。 */
bool mm_mime_is_png(const char *mime);

#ifdef __cplusplus
}
#endif
