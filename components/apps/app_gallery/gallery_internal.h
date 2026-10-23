/*
 * gallery_internal.h —— 图库 App 的模块间契约（不进 INCLUDE_DIRS）
 *
 * 三个实现文件各管一段，共享这里的数据类型与接口：
 *   gallery_scan.cc    扫 SD 卡找图片（jpg/jpeg/png/gif），带大小与修改时间
 *   gallery_decode.cc  解码工作线程（core 0）：静图 -> RGB565，GIF -> 原始字节
 *   gallery_view.cc    查看器：显示 / 双指缩放 / 双击 / 拖动 / 左右滑 / 信息弹窗
 *   gallery_app.cc     页面骨架 + 当前索引 + 轮询解码结果 + 注册到桌面
 *
 * 线程纪律（与全工程一致）：
 *   - 解码在**工作线程**（core 0）跑，LVGL 任务只做"取结果 + 贴图"；
 *   - 像素缓冲的产权明确：take() 之后归 LVGL 侧，换图时由 view 释放。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAL_PATH_MAX   512
#define GAL_NAME_MAX   256     /* FATFS 单文件名最长 255 字节（CONFIG_FATFS_MAX_LFN） */
/* 一次最多列这么多张：几千张的卡也能用，只是多出来的不显示（不递归无限吃内存） */
#define GAL_MAX_ITEMS  600
/* 扫描下钻深度（/sdcard 算第 0 层）：与 storage_service 的曲库扫描同口径 */
#define GAL_SCAN_DEPTH 3

typedef enum {
    GAL_KIND_JPEG = 0,
    GAL_KIND_PNG,
    GAL_KIND_GIF,
    GAL_KIND_UNKNOWN,
} gal_kind_t;

typedef struct {
    char       path[GAL_PATH_MAX];   /* VFS 全路径，如 /sdcard/Pictures/a.jpg */
    char       name[GAL_NAME_MAX];   /* 文件名（不含目录），显示用 */
    uint32_t   size;                 /* 字节数 */
    uint32_t   mtime;                /* unix 秒（FAT 未记录时间时为 0） */
    gal_kind_t kind;
} gal_item_t;

/* ------------------------------------------------------------------ scan */

/* 按扩展名判类型（不看文件头：列表阶段没必要开文件，解码时会按魔数兜底） */
bool        gallery_kind_of(const char *name, gal_kind_t *out);
const char *gallery_kind_name(gal_kind_t k);

/*
 * 递归扫描 root（一般是 "/sdcard"）下的图片，按**文件名**排序后返回。
 * *out 是 malloc 的数组（最多 GAL_MAX_ITEMS 条），调用方 free()。
 * 返回条数（>=0），负数表示错误（没插卡 / 打不开目录）。
 */
int gallery_scan(gal_item_t **out, const char *root);

/* 扫描进度：已找到多少张（扫描线程写、UI 读）。UI 用它把"正在扫描…"变成
 * "正在扫描… 已找到 N 张"，避免扫大卡时屏幕一动不动像卡死。 */
int gallery_scan_progress(void);

/*
 * 最近一次静图解码失败的分类（0 = 没有失败）。
 * 为什么需要：`take()` 失败时只回一个空指针，UI 只能说"这张打不开" ——
 * 而真机上失败原因差别很大（文件超 12 MB / 其实是 WebP / 内存不足 / 数据损坏），
 * 提示里写清原因能省掉一轮来回。
 */
typedef enum {
    GAL_ERR_NONE        = 0,
    GAL_ERR_TOO_BIG     = 1,   /* 文件超过 12 MB，整份读不进 PSRAM */
    GAL_ERR_UNSUPPORTED = 2,   /* WebP / HEIF 等暂不支持 */
    GAL_ERR_NO_MEM      = 3,
    GAL_ERR_DECODE      = 4,   /* 数据损坏 / 解码器不认 */
} gal_err_t;

uint8_t gallery_decode_last_error(void);

/*
 * 只读文件头拿**原始分辨率**（不整张解码）。ⓘ 弹窗要显示它 ——
 * 解码器会按长边 > 1600 折半，拿解码后的尺寸当"分辨率"是错的。
 * 支持 JPEG(SOF0/1/2) / PNG(IHDR) / GIF(逻辑屏幕描述符)。成功返回 true。
 * ⚠ 同步读文件（几百字节~几 KB），只在**工作线程**里调，别在 LVGL 任务里调。
 */
bool gallery_image_size(const char *path, gal_kind_t kind, int *w, int *h);

/* ---------------------------------------------------------------- decode */

void gallery_decode_start(void);
void gallery_decode_stop(void);

/* 请求解码第 idx 张（item 会被拷贝，调用方不必保活） */
void gallery_decode_request(int idx, const gal_item_t *item);

/*
 * 取回结果：只有当"刚解完的正好是 idx"时返回 true 并**移交所有权**。
 *   is_gif == false：px / w / h 有效，用 media_meta_cover_free() 释放
 *   is_gif == true ：gif / gif_len 有效（原始 GIF 字节），用 free() 释放
 * 没拿到（还在解 / 解失败）返回 false。
 */
bool gallery_decode_take(int idx, bool *is_gif, uint8_t **px, int *w, int *h,
                         uint8_t **gif, size_t *gif_len, int *orig_w, int *orig_h,
                         uint32_t *size, uint32_t *mtime);

/* 丢弃没取走的结果（换图、退出页面时调） */
void gallery_decode_cancel(void);

/* ------------------------------------------------------------------ view */

/* 建查看器容器（铺满 parent 的内容区），返回容器对象 */
lv_obj_t *gallery_view_create(lv_obj_t *parent);

/* 换图：接管 px 的所有权（静图）/ gif 的所有权（GIF），并复位缩放与平移。
 * set_gif 返回是否装载成功；失败时**已经**把具体原因（太宽/太大/损坏）挂到提示行。 */
void gallery_view_set_still(uint8_t *px, int w, int h);
bool gallery_view_set_gif(uint8_t *gif, size_t len);

/* 居中提示（"载入中…" / "没有图片" / "这张打不开"），空串 = 不显示 */
void gallery_view_set_hint(const char *text);

/* 底部说明：文件名 + 序号 */
void gallery_view_set_caption(const char *text);

/* 当前图片对应的条目（ⓘ 弹窗要显示分辨率/大小/格式/修改日期） */
void gallery_view_set_item(const gal_item_t *item);

/* 原始分辨率（解码线程从文件头读出来的；0/0 = 未知则只显示解码后的尺寸） */
void gallery_view_set_orig_size(int w, int h);

/* 文件大小 / 修改时间（解码线程 stat 出来的；扫描阶段**不**取，见 gallery_scan.cc） */
void gallery_view_note_file_stat(uint32_t size, uint32_t mtime);

/* ⓘ：弹出详细信息 */
void gallery_view_show_info(void);

/* 左右滑翻页回调：dir = +1 下一张 / -1 上一张（只在"未放大"时触发） */
/* 翻页：dir = -1 上一张 / +1 下一张（左右箭头按钮触发，不再用滑动手势） */
typedef void (*gal_page_cb_t)(int dir, void *user);
void gallery_view_set_page_cb(gal_page_cb_t cb, void *user);

/* 只有 1 张图（或还没扫完）时把左右箭头藏起来 */
void gallery_view_set_pager_visible(bool visible);

/* 底部文件名/序号条显隐（全屏看图时收起） */
void gallery_view_set_caption_visible(bool visible);

/*
 * "单击"判定：LVGL 的 SHORT_CLICKED 会和双击放大冲突，所以单击先记个时间戳，
 * 由调用方的定时器来取（取走即清）。返回 true = 距离那次单击已超过延迟、且期间
 * 没有发生双击 —— 现在可以切全屏了。
 */
bool gallery_view_take_pending_tap(void);

/* 页面销毁时调：释放像素、停掉 GIF、清指针 */
void gallery_view_destroy(void);

#ifdef __cplusplus
}
#endif
