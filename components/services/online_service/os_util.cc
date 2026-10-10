/*
 * os_util.cc —— 纯逻辑工具的实现（无 ESP-IDF 依赖，可 host 编译单测）
 *
 * 全部逐字搬自原 online_service.cc，只改函数名（加 os_ 前缀）以便跨文件共享：
 *   str_copy      234–252
 *   id_from_url   261–271
 *   url_encode   1046–1062
 *   ext_from_sniff 1164–1183
 * 注释一律跟着函数搬（这些注释记的是"为什么这么写"，删了坑会复发）。
 */
#include "internal/os_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void os_str_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;                       /* 不要截在多字节字符中间 */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 从 meting 的 url 字段里抠出歌曲 id：...&id=123456 */
uint32_t os_id_from_url(const char *url)
{
    if (!url) {
        return 0;
    }
    const char *p = strstr(url, "id=");
    if (!p) {
        return 0;
    }
    return (uint32_t)strtoul(p + 3, NULL, 10);
}

/* UTF-8 关键词 → 百分号编码（只放行 RFC3986 的 unreserved 字符）。
 * 中文关键词会被编码成 %E5%91%A8...，服务器那边按 UTF-8 解。 */
void os_url_encode(const char *src, char *dst, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < cap; p++) {
        const unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

/* 按魔数定扩展名（音频缓存文件的后缀决定 ASP/播放器用哪个解码器） */
void os_ext_from_sniff(const uint8_t *sniff, char *ext, size_t cap)
{
    const char *e = ".mp3";                       /* 默认按 mp3 处理 */
    if (sniff) {
        if (memcmp(sniff, "fLaC", 4) == 0) {
            e = ".flac";
        } else if (memcmp(sniff, "OggS", 4) == 0) {
            e = ".ogg";
        } else if (memcmp(sniff, "RIFF", 4) == 0) {
            e = ".wav";
        } else if (memcmp(sniff + 4, "ftyp", 4) == 0) {
            e = ".m4a";
        } else if (memcmp(sniff, "ID3", 3) == 0 || (sniff[0] == 0xFF && (sniff[1] & 0xE0) == 0xE0)) {
            e = ".mp3";
        } else if (memcmp(sniff, "ADIF", 4) == 0 || (sniff[0] == 0xFF && (sniff[1] & 0xF0) == 0xF0)) {
            e = ".aac";
        }
    }
    snprintf(ext, cap, "%s", e);
}
