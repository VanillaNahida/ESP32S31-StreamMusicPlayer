/*
 * app_gallery.h —— 图库 App（浏览 SD 卡里的 jpg/jpeg/png/gif）
 *
 * 用法（main.cc，与 app_music_register_apps 同一步）：
 *     app_gallery_register();
 * 之后桌面会出现"图库"图标（位图 gallery_logo，见 assets/icons/icons.txt）。
 *
 * 依赖：ui_core（图标/度量/路由）、media_meta（JPEG/PNG 解码）、
 *       lvgl（lv_gif 播放 GIF、lv_image 缩放）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册到桌面（只登记描述符；页面在用户点击时才建） */
void app_gallery_register(void);

/* 页面构建入口（app_desc_t.create 用；也可以被别的页面直接 push） */
lv_obj_t *gallery_page_build(void);

/*
 * 打开某一张图片（压栈）。给文件管理用：点图片行就调它。
 *
 * 行为：图库页打开后**定位到这张**（列表就绪后按路径找下标），左右滑可以接着看
 * 同一张卡上的其它图；返回回到原来那个目录（目录与页码都不变，因为图库是压栈的）。
 * 路径找不到（被删了 / 在扫描深度之外）就退回第 0 张，不会打不开页面。
 */
void gallery_open_path(const char *path);

#ifdef __cplusplus
}
#endif
