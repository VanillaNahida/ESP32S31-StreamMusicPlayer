/*
 * ao_view.h —— 在线音乐**歌单页的两种版式**（列表 / 详情），两个页面共用一份
 *
 * 第 0 页「我的歌单」和第 1 页「我添加的歌单」长得一模一样：
 *
 *   列表模式（在线音乐主界面.svg）：            详情模式（歌单曲目详情页.svg）：
 *     "歌单 · 14 个"                             ┌───────┐  01 曲名        歌手
 *     [●] 每日推荐        每天更新               │ 封面   │  ────────────────
 *     ─────────────────────────                  │ 184x184│  02 曲名        歌手
 *     [ ] 香草味…         645 首                 └───────┘  ────────────────
 *                                                歌单名                     …
 *                                                副标题
 *                                                (▶ 播放全部)
 *
 * 两者的清单**是同一个对象**（只是换位置/换宽），所以翻页/滚动状态不丢。
 * 页面只负责"往 ao_view_list() 里填行"，版式切换由本模块做。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *page;        /* 页容器（s_host 的子对象，由页面自己建） */
    lv_obj_t *hint;        /* 列表模式顶部那行小字 */
    lv_obj_t *list;        /* 清单（两种模式共用） */
    lv_obj_t *left;        /* 详情模式左列（封面卡片 + 歌单名 + 播放全部） */
    lv_obj_t *cover;       /* 封面卡片本体（圆角 + clip_corner，两个封面图都被它裁） */
    lv_obj_t *cover_img;   /* 真封面（第一首歌的封面；没解好时隐藏） */
    lv_obj_t *cover_def;   /* **默认封面**（assets/defalut_cover.png 烘的位图；有真封面时隐藏） */
    lv_obj_t *name;        /* 卡片下方歌单名（封面上不再写字，见 ao_view.cc） */
    lv_obj_t *desc;        /* 卡片下方副标题（"N 首 · 第 x/y 页"，跟着歌单名自适应下移） */
    lv_obj_t *play_all;    /* "播放全部"胶囊 */
    bool      detail;      /* 当前是不是详情模式 */
    int       want_id;     /* 左卡片想要的封面属于哪首歌（0 = 没有） */
} ao_view_t;

/* 建两种版式要用到的全部控件（刚建好是列表模式、左列隐藏） */
void ao_view_init(ao_view_t *v, lv_obj_t *page);
/* 页面销毁：清指针（并让封面回调不再指向它） */
void ao_view_clear(ao_view_t *v);

/* 清单容器：页面往这里填行（两种模式都是它） */
lv_obj_t *ao_view_list(ao_view_t *v);

/* 切到列表模式：显示 hint，清单铺满整个宽度 */
void ao_view_list_mode(ao_view_t *v, const char *hint_text);

/* 切到详情模式：藏 hint，显示左列，清单靠右 */
void ao_view_detail_mode(ao_view_t *v, const char *name, const char *desc);

/*
 * 选"左卡片显示哪首歌的封面"（约定传歌单第一首；「我喜欢的音乐」传**最新收藏**那首，
 * 见 ao_page_acct.cc 的 acct_fav_cover_song）。
 * 内部会排一次异步抓取；解好之后经 ao_pl_cover 的回调自动贴到 cover_img。
 */
void ao_view_set_cover(ao_view_t *v, const online_song_t *first);

/* 把当前可用的封面贴上去（没解好 / 不匹配就亮出内置的默认封面） */
void ao_view_cover_apply(ao_view_t *v);

/* "播放全部"胶囊对象（页面自己挂 CLICKED 回调） */
lv_obj_t *ao_view_play_all(ao_view_t *v);

#ifdef __cplusplus
}
#endif
