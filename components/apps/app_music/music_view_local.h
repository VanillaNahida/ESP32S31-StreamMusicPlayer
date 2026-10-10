/*
 * music_view_local.h —— 本地音乐列表页（视图层）
 *
 * 搬迁自 app_music.cc 的 2878–2881、2939–3108、3265–3509（页面与渲染部分）。
 * 数据（目录条目 / 播放队列 / SD 代次）全部来自 music_local_source.h；
 * 点歌走 music_player_start_track()；进播放器页走 music_router_open_player()。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建整页（路由 page_desc_t.create 用）：page_local */
lv_obj_t *music_view_local_build(void);

#ifdef __cplusplus
}
#endif
