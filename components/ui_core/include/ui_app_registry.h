/*
 * ui_app_registry.h —— 桌面 APP 模块化注册（开发指南 5.8）
 *
 * 每个 APP 用 APP_DESC 描述符声明，由 ui_home 按 order 排序自动铺网格，
 * 超屏分页。要增删 APP 只需增删对应的描述符对象与 ui_core 里的注册调用，
 * 或用 Kconfig 开关裁剪固件。
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* APP 标志 */
#define APP_FLAG_HIDDEN      (1u << 0)   /* 不在桌面网格显示（仅内部可达） */
#define APP_FLAG_FULLSCREEN  (1u << 1)   /* 进入后隐藏迷你播放条 */
#define APP_FLAG_NO_BACK     (1u << 2)   /* 页面自带返回钮：路由不叠全局返回键 */

typedef struct app_desc {
    const char      *id;          /* 唯一 id，用于路由 */
    const char      *name;        /* 桌面显示名（中文） */
    const char      *symbol;      /* 图标：先用 LVGL 内置 symbol，避免 emoji */
    uint32_t         accent_hex;  /* 图标底色，0xRRGGBB（静态初始化不能直接写 lv_color_t） */
    lv_obj_t        *(*create)(void);  /* 建页；NULL 表示尚未实现 */
    void            (*on_enter)(void);
    void            (*on_exit)(void);
    uint8_t          order;       /* 排序，越小越靠前 */
    uint8_t          flags;
    /*
     * 位图图标（可选）：非 NULL 时桌面就用它代替 symbol 文字。
     *
     * 放在**结构体最后**是刻意的 —— 已有的那些位置初始化
     * （`{ "music", "音乐", LV_SYMBOL_AUDIO, 0xE8973A, ..., 10, 0 }`）少写这一个成员
     * 会被零初始化成 NULL，于是不用去动每一处描述符，只有真正要用位图的那个
     * 才在末尾补上 `&netease_logo`。
     *
     * 图像由 tools/icons/gen_icon.py 从 assets/icons 下的 PNG 烘成 C 数组
     * （ARGB8888，已解码；本工程没开 LVGL 的 PNG 解码器），声明见 ui_icons.h。
     */
    const lv_image_dsc_t *img;
} app_desc_t;

/* 注册一个 APP 描述符（在 ui_core_init 之前调用；内部只存指针） */
void ui_app_register(const app_desc_t *desc);

/* 清空注册表（恢复出厂/重启 UI 时用） */
void ui_app_reset(void);

/* 遍历：idx 从 0 开始，返回 NULL 表示结束。exclude_hidden 为真时跳过 HIDDEN */
const app_desc_t *ui_app_at(size_t idx, bool exclude_hidden);

/* 可见 APP 数量 */
size_t ui_app_count(bool exclude_hidden);

/* 按 id 查找 */
const app_desc_t *ui_app_find(const char *id);

#ifdef __cplusplus
}
#endif