/*
 * mm_bits.cc —— 字节序 / synchsafe / MIME 判定（纯函数，无 IO、无硬件依赖）
 *
 * 从 media_meta.cc 拆出：be32/be24/synchsafe32（原 313–328）、
 * mime_is_png（原 330–344）、le16/le32（原 993–1000）。**纯搬迁**。
 *
 * 只用 libc —— 没有 esp_log / esp_err，是拆分后最容易 host 单测的一块。
 */
#include "internal/mm_bits.h"

/* ---------------------------------------------------------- 字节序小工具 */

uint32_t mm_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t mm_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* ID3v2 的长度字段是 7-bit synchsafe：每个字节只用低 7 位 */
uint32_t mm_synchsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

bool mm_mime_is_png(const char *mime)
{
    /* 只要前缀是 image/png（大小写不敏感）就算 PNG */
    static const char *k = "image/png";
    for (int i = 0; i < 9; i++) {
        char c = mime[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != k[i]) {
            return false;
        }
    }
    return true;
}

/* 小端读取（WAV 用） */
uint32_t mm_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

uint32_t mm_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
