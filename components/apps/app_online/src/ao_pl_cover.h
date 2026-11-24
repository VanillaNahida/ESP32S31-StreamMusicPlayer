/*
 * ao_pl_cover.h —— 歌单详情左卡片的那张封面（异步抓取 + PSRAM 单槽缓存 + 闲置释放）
 *
 * 需求（重构指南）：
 *   "左边的封面自动加载位于歌单第一首歌的封面。尽可能多利用PSRAM。
 *    在资源使用后一段时间未使用则自动释放。"
 *
 * 做法（与 app_music 的 music_cover.cc 同一套线程纪律）：
 *   - **工作线程**负责 HTTP（online_song_cover_data，阻塞）+ 软解成 RGB565，写结果槽位；
 *     绝不碰 LVGL。
 *   - **LVGL 任务**（ao_result 的 200 ms 轮询 → ao_pl_cover_tick）取结果、建
 *     lv_image_dsc_t、贴到控件上；颜色/尺寸变化都在 LVGL 任务里发生。
 *   - 像素一律走 PSRAM（media_meta 的封面解码内部就优先 PSRAM）。
 *   - **闲置释放**：最后一次被显示之后 AO_PL_COVER_IDLE_MS 内没再被用到就整块释放
 *     （像素 + 描述符）；页面销毁时也会立刻释放。
 *
 * 单槽缓存的理由：同一时刻屏上只有一张歌单卡片；换歌单时旧图先留着（不闪白），
 * 新图到了再替换 —— 所以不需要多槽 LRU。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 闲置多久之后释放（毫秒）。取 60 s：翻回上一个歌单时通常还是热的，又不至于常驻 */
#define AO_PL_COVER_IDLE_MS  (60 * 1000)

/* 新图贴好之后的回调（LVGL 任务上下文）：页面在这里把 dsc 设给 lv_image */
typedef void (*ao_pl_cover_notify_t)(void);
void ao_pl_cover_set_notify(ao_pl_cover_notify_t cb);

/*
 * 选出"要显示哪首歌的封面"。
 *   song == NULL → 不需要封面（例如视频/歌单还没有数据）
 *   同一首不会重复请求；缓存命中时什么都不做（只是刷新 last_use）。
 */
void ao_pl_cover_want(const online_song_t *song, uint32_t now);

/* 当前可用的封面描述符（没解好/已被释放时返回 NULL） */
const lv_image_dsc_t *ao_pl_cover_dsc(void);

/* 正在显示的这张属于哪首歌（0 = 没有）；页面据此决定要不要换 src */
int ao_pl_cover_current_id(void);

/*
 * "现在屏上还在用它"——详情页打开期间必须 hold(true)：
 * 否则闲置计时会在用户盯着封面看的时候把图收走（页面上会突然变回占位渐变色）。
 * 退出详情（回列表）时 hold(false)，闲置计时从那一刻重新开始。
 */
void ao_pl_cover_hold(bool on);

/* 200 ms 轮询里调：取工作线程结果 → 上屏；按需拉起下一次；闲置则释放 */
void ao_pl_cover_tick(uint32_t now);

/* 页面销毁：作废在途请求 + 立刻释放缓存（下次进页面重新抓） */
void ao_pl_cover_release_all(void);

#ifdef __cplusplus
}
#endif
