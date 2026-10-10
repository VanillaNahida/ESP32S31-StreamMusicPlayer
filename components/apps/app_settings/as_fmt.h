/*
 * as_fmt.h —— 设置页的**纯逻辑**格式化（不依赖 LVGL，可 host 单测）
 *
 * 只做"数值 → 人看的字符串"这一件事，整数运算、不碰浮点
 * （本工程 newlib 是 nano 配置，printf 不支持 %f；而且 -Werror 下
 *   `%f` 还牵扯 newlib 的 float printf 配置，改完就得重配 sdkconfig）。
 *
 * 原来这些函数散在 app_settings.cc 的"存储 / 关于本机"两段里，
 * 拆出来是为了让它们能被独立编译/单测（见 docs/refactor/06_app_settings.md §6.3）。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KB → 人看的字符串（"12.3 GB" / "345.6 MB" / "78 KB"）。
 * 整数运算，不碰浮点。out/cap 为空时直接返回。
 */
void as_fmt_size_kb(uint32_t kb, char *out, size_t cap);

/*
 * 字节 → 人看的字符串（"1.25 GB" / "12.3 MB" / "345 KB" / "78 B"）。
 * 整数运算，不用 %f（-Werror 下还牵扯 newlib 的 float printf 配置，
 * 别为了好看引进来）。out/cap 为空时直接返回。
 */
void as_fmt_bytes(uint64_t bytes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
