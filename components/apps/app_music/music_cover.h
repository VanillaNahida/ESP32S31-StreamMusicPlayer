/*
 * music_cover.h —— 封面异步解码（工作线程）+ 封面像素/描述符的**唯一 owner**
 *
 * 从 app_music.cc 的"封面"一段整体搬出来（原 122–144、1872–1968、2376–2425）。
 *
 * ★ 产权规则（拆开后必须仍然只有一处 free，否则就是 double free / 泄漏）：
 *   - **像素**（music_state.cover_pixels，PSRAM 里的 RGB565）与**堆上的
 *     lv_image_dsc_t**（music_state.cover_dsc）只由本模块释放/替换；
 *   - 页面销毁时**故意不释放**（离开播放器页再回来要能直接贴回封面）；
 *   - 在线歌曲的封面字节（music_state.on_cover_data）**所有权转移**进解码请求，
 *     由工作线程自己 free —— 所以"释放"只可能发生在一处。
 *
 * ★ 线程纪律：cover_task 只读文件/解内存缓冲 + 写 music_state 的产出字段，
 *   **绝不碰 LVGL**；结果由 tick（LVGL 任务）经 music_cover_pump() 搬上屏。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 当前是否已经有解好的封面像素 + 描述符（进播放器页时"直接贴回来"的判断） */
bool music_cover_have_pixels(void);

/* 现有描述符（lv_image_dsc_t*，直接喂 lv_image_set_src）；没有则 NULL。
 * 返回 void* 是为了让视图层不必在这里 include lvgl.h。 */
void *music_cover_dsc(void);
uint8_t *music_cover_pixels(void);

/* 当前封面是不是"在线内存封面"（决定换曲时要不要连带丢弃在线缓冲） */
bool music_cover_source_is_online(void);

/* 排一次解码请求（-1 = 不排）。真正的起飞在 music_cover_kick()。 */
int  music_cover_want(void);
void music_cover_request(int idx);

/*
 * 起播 `idx` 前的问询：屏上那张封面是不是**就是它**？
 *
 * 返回 false（= 不用重来）时，调用方**不要**调 cover_show(NULL) / request / kick ——
 * 原地留着就行；返回 true 才照旧走"释放 + 重新请求"。
 *
 * 为什么需要：同一首**重起播**（在线地址过期重连、加载中点了播放导致重来一次）
 * 也走"起播"路径。若无脑释放再请求，在线封面的内存缓冲是一次性消费的
 * （起播时已转移/释放），重来就再也拿不到图 → **封面永久为空**
 * （2026-10 用户报的"加载时点播放，封面丢失"）。
 */
bool music_cover_needs_reload(int idx);

/* 有请求且空闲就起解码任务（必须在 LVGL 任务上下文调用）。
 * 起不来（没内存 / 任务创建失败）时静默返回，占位图兜底。 */
void music_cover_kick(void);

/* 丢掉没人认领的解码结果（换曲太快、页面已退出等） */
void music_cover_reap(void);

/*
 * 把封面放到视图上：px 为 NULL 时回到占位色块。必须在 LVGL 任务里调用。
 *
 * 描述符**每张封面新分配一块**（而不是复用同一块改内容）：lv_image 只认这个
 * 指针，LVGL 的图片缓存也是按指针做键的，复用同一块会让它拿到旧内容；
 * 换指针还能避开 LVGL 的内部缓存 API（lv_image_cache_drop 不在公开头文件里）。
 *
 * ★ 顺序不可改（历史上在这里 hard fault 过）：**先摘掉旧 src**（避免 LVGL 还在画
 *   一块即将被 free 的像素）→ 再 free 旧描述符 → 最后 free 旧像素。
 *
 * @param set_src      视图提供的"把描述符交给 lv_image（NULL = 摘掉）"回调
 * @param set_visible  视图提供的"显示/隐藏真封面"回调
 * @param user         透传给两个回调
 */
typedef void (*music_cover_set_src_cb)(void *user, const void *dsc);
typedef void (*music_cover_set_visible_cb)(void *user, bool visible);

void music_cover_place(uint8_t *px, int w, int h,
                       music_cover_set_src_cb set_src,
                       music_cover_set_visible_cb set_visible,
                       void *user);

typedef enum {
    MUSIC_COVER_PUMP_EMPTY = 0,   /* 工作线程还没产出 */
    MUSIC_COVER_PUMP_STALE,       /* 产出了，但下标已经不是当前这首 —— 已释放 */
    MUSIC_COVER_PUMP_OK,          /* 产出了且匹配 —— 交给调用方 place */
} music_cover_pump_t;

/*
 * 取走工作线程的产出（LVGL 任务，由 tick 调）：
 *   - 下标 == keep_idx → 返回 OK，像素通过 px/w/h 交给调用方（调用方负责 place）；
 *   - 下标不符 → 直接释放，返回 STALE（换曲太快时上一首的封面不该上屏）。
 */
music_cover_pump_t music_cover_pump(int keep_idx, uint8_t **px, int *w, int *h);

#ifdef __cplusplus
}
#endif
