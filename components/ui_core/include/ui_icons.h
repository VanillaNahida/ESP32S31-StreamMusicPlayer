/*
 * ui_icons.h —— 烘进固件的位图图标（由 PNG 生成，不要手改分毫）
 *
 * 生成方式：
 *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \
 *       assets/icons/netease.png --name netease_logo --size 96
 *
 * 为什么是"烘好的像素"而不是运行时解码 PNG：
 *   本工程 `LV_USE_LIBPNG` / `LV_USE_LODEPNG` / `LV_USE_TJPGD` **全是 n**
 *   （AGENTS.md 第 3 节要求的配置里没有它们），运行期没有 PNG 解码器可用；
 *   而图标是固定尺寸的小图，编译期解好、运行时零解码开销最划算。
 *
 * 用法：把 `&netease_logo` 填到 app_desc_t 的 `img` 字段（结构体最后一个成员），
 * 桌面就会用位图代替 symbol 文字（见 ui_home.cc 的图标块）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 网易云音乐图标，96x96 ARGB8888（来自 assets/icons/netease.png，282x282 原图缩放） */
LV_IMAGE_DECLARE(netease_logo);

#ifdef __cplusplus
}
#endif
