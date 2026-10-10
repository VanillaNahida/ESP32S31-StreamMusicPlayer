/*
 * as_fmt.cc —— 设置页的**纯逻辑**格式化（不依赖 LVGL、不依赖任何服务）
 *
 * 原 app_settings.cc：
 *   - storage_fmt_size_kb（1404–1418）→ as_fmt_size_kb
 *   - about_fmt_bytes    （1736–1752）→ as_fmt_bytes
 * 两个都是整数运算、不用 %f。函数体与注释**逐字搬迁**，只改了名字。
 */
#include "as_fmt.h"

#include <stdio.h>

/* 容量格式化（KB → "12.3 GB" / "345 MB" / "78 KB"）；原实现在 app_settings.cc 的"存储"一段 */
/* KB → 人看的字符串。整数运算，不碰浮点（-Werror 下 %f 还牵扯 newlib 配置） */
void as_fmt_size_kb(uint32_t kb, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    if (kb >= 1024u * 1024u) {                      /* ≥ 1 GB：保留 1 位小数 */
        const uint32_t t = (uint32_t)(((uint64_t)kb * 10u) / (1024u * 1024u));
        snprintf(out, cap, "%u.%u GB", (unsigned)(t / 10u), (unsigned)(t % 10u));
    } else if (kb >= 1024u) {                       /* ≥ 1 MB */
        const uint32_t t = (uint32_t)(((uint64_t)kb * 10u) / 1024u);
        snprintf(out, cap, "%u.%u MB", (unsigned)(t / 10u), (unsigned)(t % 10u));
    } else {
        snprintf(out, cap, "%u KB", (unsigned)kb);
    }
}

/*
 * 字节 → 人看的字符串。整数运算，不用 %f
 * （-Werror 下还牵扯 newlib 的 float printf 配置，别为了好看引进来）。
 */
void as_fmt_bytes(uint64_t bytes, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        const uint64_t t = (bytes * 100ULL) / (1024ULL * 1024ULL * 1024ULL);
        snprintf(out, cap, "%u.%02u GB", (unsigned)(t / 100ULL), (unsigned)(t % 100ULL));
    } else if (bytes >= 1024ULL * 1024ULL) {
        const uint64_t t = (bytes * 10ULL) / (1024ULL * 1024ULL);
        snprintf(out, cap, "%u.%u MB", (unsigned)(t / 10ULL), (unsigned)(t % 10ULL));
    } else if (bytes >= 1024ULL) {
        snprintf(out, cap, "%u KB", (unsigned)(bytes / 1024ULL));
    } else {
        snprintf(out, cap, "%u B", (unsigned)bytes);
    }
}
