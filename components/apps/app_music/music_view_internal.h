/*
 * music_view_internal.h —— 视图层内部共享：播放器页控件指针的小结构 + 本地页分页常量
 *
 * ★ 为什么控件指针要装进**一个结构体**：
 *   `tick`（500 ms）与 `media_cmd_tick`（100 ms）两个定时器是**开机常驻、永不销毁**的，
 *   而播放器页会被路由 pop 掉 —— 页面销毁时必须把它们会碰到的**每一个**指针清掉，
 *   漏一个就是"定时器写已销毁对象"（概率性 hard fault，只在"退页→再进"复现）。
 *   装进结构体后收尾就是一次 memset，不可能漏。这就是原 app_music.cc 里那 17 行
 *   逐个置 NULL 的等价形式（且更不容易漏）。
 *
 * ⚠ 本头文件只给视图层的 .cc 用（含音乐 APP 自己的视图模块），不要让业务模块 include。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *title;         /* 歌名（整宽滚动 label） */
    lv_obj_t *artist;        /* 歌手 */
    lv_obj_t *seek;          /* lv_bar：进度条本体（纯显示，触摸由抓取区管） */
    lv_obj_t *seek_hit;      /* 进度条抓取区：自己算比例，松手才真定位 */
    lv_obj_t *cur;           /* 已播时间 */
    lv_obj_t *tot;           /* 总时长 */
    lv_obj_t *btn;           /* 播放/暂停按钮 */
    lv_obj_t *cover_box;     /* 左栏封面容器 */
    lv_obj_t *cover_img;     /* 真封面（没解出来时隐藏） */
    lv_obj_t *src_badge;     /* 来源药丸（在线播放 / 本地播放） */
    int       src_badge_state;   /* 上次画的状态（-1 = 还没画过） */
    lv_obj_t *mode_btn;      /* 模式按钮里的图标 label */
    lv_obj_t *mode_badge;    /* 单曲循环的 "1" 角标 */
    bool      seeking;       /* 手指正在拖进度条：期间别让 tick 覆盖它 */
} music_player_ctrls_t;

/* 播放器页控件表（页面不在时全部为 NULL）。
 * ⚠ 歌词那一组控件不在这里 —— 它们归 music_view_lyric.cc 自己持有（见该头文件）。 */
music_player_ctrls_t *music_player_ctrls(void);

/* 本地页分页常量 */
#define LOCAL_PAGE_ROWS   120
#define LOCAL_ROOT        "/sdcard"

/* local_add_row 的 sel 取值：>=0 是条目下标，负数是这几种动作行 */
#define LOCAL_SEL_NONE    (-1)   /* 提示行，不可点 */
#define LOCAL_SEL_PARENT  (-2)   /* ".." 上一级 */
#define LOCAL_SEL_PREV    (-3)   /* 上一页 */
#define LOCAL_SEL_NEXT    (-4)   /* 下一页 */

#ifdef __cplusplus
}
#endif
