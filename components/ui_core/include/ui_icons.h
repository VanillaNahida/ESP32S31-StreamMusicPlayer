/*
 * ui_icons.h —— 烘进固件的位图图标（由 PNG/SVG 生成，不要手改分毫）
 *
 * 生成方式（清单在 assets/icons/icons.txt，一次生成全部）：
 *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \
 *       --manifest assets/icons/icons.txt -o components/ui_core/ui_icons.c
 *
 * 想肉眼看一眼烘出来长什么样（另存 PNG，不参与构建）：
 *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \
 *       --manifest assets/icons/icons.txt --preview build/icons_preview
 *
 * 单个图标也可以临时生成（只写这一个，会覆盖整个 ui_icons.c）：
 *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \
 *       assets/icons/netease.png --name netease_logo --size 96
 *
 * ⚠ 有三个素材的**扩展名撒谎**，gen_icon.py 解不了，先跑
 *   tools/icons/prepare_sources.ps1 转成标准 PNG：
 *     assets/default.png   其实是 JPEG（404x456）→ momotalk_avatar.png
 *     assets/MomoIcon.png  其实是 ICO（64x64）  → momotalk_icon.png
 *     MomoTalk.html 里右上角那个 ✕ 是内联 SVG → momotalk_close.png
 *
 * 为什么是"烘好的像素"而不是运行时解码 PNG：
 *   本工程 `LV_USE_LIBPNG` / `LV_USE_LODEPNG` / `LV_USE_TJPGD` **全是 n**
 *   （AGENTS.md 第 8 节要求的配置里没有它们），运行期没有 PNG 解码器可用；
 *   而图标是固定尺寸的小图，编译期解好、运行时零解码开销最划算。
 *
 * 用法：把 `&netease_logo` 填到 app_desc_t 的 `img` 字段（结构体最后一个成员），
 * 桌面就会用位图代替 symbol 文字（见 ui_home.cc 的图标块）；页面里直接
 * `lv_image_set_src(img, &momotalk_icon)` 即可，**尺寸已按显示尺寸烘好**，
 * 不要再让 LVGL 运行期缩放（那是每帧都要重算的）。
 *
 * ⚠ 状态栏那批（wifi_s1..sd_warn）是**单色**位图（SVG 按白色烘），显示颜色由
 *   `lv_obj_set_style_image_recolor()` + `image_recolor_opa = LV_OPA_COVER`
 *   决定（见 ui_status_bar.cc 的 icon_set()）。别把它们当彩色图用。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 桌面 APP 图标 */
/* 网易云音乐，96x96 ARGB8888（来自 assets/icons/netease.png，282x282 原图缩放） */
LV_IMAGE_DECLARE(netease_logo);
/* MomoTalk，96x96 ARGB8888（来自 assets/MomoTalk.png，525x525 原图缩放） */
LV_IMAGE_DECLARE(momotalk_logo);

/* MomoTalk 页面 */
/* 标题栏左侧圆形小图标，32x32（来自 assets/MomoIcon.png——那个文件其实是 ICO） */
LV_IMAGE_DECLARE(momotalk_icon);
/* 对方头像，44x44（来自 assets/default.png——那个文件其实是 JPEG，已居中裁方） */
LV_IMAGE_DECLARE(momotalk_avatar);
/* 右上角退出 ✕，40x40（来自 MomoTalk.html 那段内联 SVG，按 4 倍重绘后缩小） */
LV_IMAGE_DECLARE(momotalk_close);

/* ------------------------------------------------------------------ 状态栏（22x22）
 *
 * 边长必须与 ui_metrics.h 的 UI_STATUS_ICON 一致（22）：烘好的位图**就是**显示
 * 尺寸，LVGL 不再做运行期缩放。改尺寸要同时改 icons.txt 并重新生成。
 */
/* Wi-Fi 信号 1~4 格（已连接时按 RSSI 选） */
LV_IMAGE_DECLARE(wifi_s1);
LV_IMAGE_DECLARE(wifi_s2);
LV_IMAGE_DECLARE(wifi_s3);
LV_IMAGE_DECLARE(wifi_s4);
/* Wi-Fi 开着但没连上（断开 / 连接中 / 失败） */
LV_IMAGE_DECLARE(wifi_off);
/* 蓝牙：空闲 / 搜索中 / 已连接 */
LV_IMAGE_DECLARE(bt_idle);
LV_IMAGE_DECLARE(bt_searching);
LV_IMAGE_DECLARE(bt_connected);
/* microSD：可用 / 异常（未插卡时状态栏不显示任何 SD 图标） */
LV_IMAGE_DECLARE(sd_ok);
LV_IMAGE_DECLARE(sd_warn);

#ifdef __cplusplus
}
#endif
