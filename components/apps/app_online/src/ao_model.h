/*
 * ao_model.h —— app_online 的**歌单队列 / 分页 / 懒加载模型**（纯逻辑，不依赖 LVGL）
 *
 * 搬出来的都是"跟界面无关、可以 host 侧单测"的东西：
 *   - 队列所有权状态机（songs / stale / 身份 Key，最敏感，见风险 R2/R7/R8）
 *   - 分页数学（page_first / page_count / page_clamp / acct_page_count）
 *   - 详情身份 Key（detail_*，账号页那半份在 ao_account.h）
 *   - 账号歌单懒加载游标（begin / append / fetch_if_needed / handle_page）
 *   - 页面进入 / 销毁时的一次性状态复位
 *
 * 两条硬约束（改的时候先读这里）：
 *   1) **不许 include lvgl / ui_*** —— 时间一律由调用方用参数传进来（`uint32_t now`，
 *      调用点写 `lv_tick_get()`），本模块自己绝不调 lv_tick_get()。
 *   2) **绝不 free s_songs** —— 播放器可能还存着它的指针；被换下来的旧队列只挪进
 *      s_stale，由 ao_queue_drop_stale() → app_music_online_detach() 交给播放器托管。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "ao_common.h"

/* s_songs 是谁的结果：歌单详情 / 搜索结果 / 账号歌单（两份结果共用一个数组，
 * 避免双份所有权带来的重复释放；谁的结果谁显示，另一个页面显示自己的内容） */
enum { OWNER_PL = 0, OWNER_SRCH = 1, OWNER_ACCT = 2 };

/* ---------------------------------------------------------------- 队列所有权 */

online_song_t *ao_queue_songs(void);      /* 当前队列（播放器也持有这个指针） */
size_t         ao_queue_count(void);      /* 已载入条数 */
size_t         ao_queue_cap(void);        /* 槽位数（懒加载缓冲容量） */
int            ao_queue_owner(void);
const char    *ao_queue_key(void);        /* s_cur_pl_id */

/*
 * 换掉当前队列 s_songs。
 * ★ 老的那份**绝不在这里 free**（2026-10 修 use-after-free）：播放器可能还存着它的
 *   指针（用户点歌之后又打开别的歌单时，app_music 的 s_on_songs 仍指着它），
 *   直接 free 会让上/下一首、自动续播读到已释放内存。统一走 s_stale →
 *   ao_queue_drop_stale() → app_music_online_detach()：播放器还在用就由它接管
 *   （它换队列/退回本地播放时自己放），没用就立刻放掉。
 */
void ao_queue_replace(online_song_t *buf, size_t cap, size_t count,
                      const char *key, int owner);
/* 安装一份**整份**队列（meting 歌单 / 搜索 / 每日推荐的结果） */
void ao_queue_install(online_song_t *songs, size_t count, const char *pl_id, int owner);
/* 回收"被替换下来的旧队列"（s_stale），所有权交给 app_music */
void ao_queue_drop_stale(void);
/* OREQ_ADD 成功后：内存里那份过期了，把身份标记清掉（下次点进这个歌单会重新读） */
void ao_queue_key_clear_if(const char *key);

/* ---------------------------------------------------------------- 分页数学 */

size_t ao_page_first(int page);                 /* 第 page 页第一首的绝对下标 */
int    ao_page_count(size_t n);                 /* 空列表也算 1 页 */
int    ao_page_clamp(int page, size_t n);
int    ao_acct_page_count(void);                /* 账号歌单页数（总数未知时多给一页） */

int    ao_pl_page(void);     void ao_pl_page_set(int page);
int    ao_srch_page(void);   void ao_srch_page_set(int page);

/* ---------------------------------------------------------------- 详情身份 Key */

/* 详情页要的那份 key（歌单 ID / 每日推荐标记 / 空串 = 没有明确目标）。
 * 账号页那半份判断委托给 ao_account.cc 的 ao_acct_detail_want_key()。 */
const char *ao_detail_want_key(void);
bool        ao_detail_key_matches(void);   /* 缓冲就是"这个详情要的那一份"（空壳也算） */
bool        ao_detail_owns_songs(void);    /* 是这一份，**而且已经有歌可画** */
void        ao_detail_mark_request(uint32_t now);
uint32_t    ao_detail_tick(void);

/* 我添加的歌单：进/退详情 */
bool ao_pl_detail(void);
int  ao_pl_sel(void);
void ao_pl_detail_enter(int idx);   /* detail=true, sel=idx, page=0 */
void ao_pl_detail_exit(void);       /* detail=false, sel=-1, page=0 */
void ao_pl_detail_drop(void);       /* detail=false, sel=-1（不动页码） */

/* ---------------------------------------------------------------- 账号歌单懒加载 */

bool ao_acct_lazy(void);
size_t ao_songs_total(void);
bool ao_fetch_fail(void);
/* 详情页"重试"：清掉失败标记，允许重新补页 */
void ao_fetch_fail_clear(void);

/* 打开一个账号歌单：按总首数一次性 calloc（指针从此不变），置懒加载态 */
bool ao_lazy_begin(const char *pl_id, size_t total);
/* 把一页追加到已载入末尾（非连续则丢弃并告警） */
void ao_lazy_append(size_t off, const online_song_t *page, size_t n);
/* 懒加载的唯一入口：当前页缺歌时按 ACCT_FETCH_MAX 补一段（内部 order 下单） */
void ao_lazy_fetch_if_needed(uint32_t now);

/* OREQ_ACCT_SONGS 的结果处理（纯状态迁移；日志按原样打） */
typedef enum {
    AO_PAGE_NONE = 0,     /* 结果过期/不属于本歌单：丢弃 */
    AO_PAGE_APPENDED,     /* 追加成功 */
    AO_PAGE_ENDED,        /* 到歌单末尾（空页） */
    AO_PAGE_FAILED,       /* 这一页没取到 */
} ao_page_outcome_t;

ao_page_outcome_t ao_lazy_handle_page(int off, int want, esp_err_t err, const char *key,
                                      const online_song_t *page, size_t n);

/* ---------------------------------------------------------------- 页面生命周期 */

/* page_online() 开头的一次性复位（详情计时起点 / 退避 / 页码 / 在途标志） */
void ao_session_begin(uint32_t now);
/* online_on_delete()：清未消费状态（槽位与 s_stale 由调用方按原顺序处理） */
void ao_session_end(void);
/* online_on_delete()：两个详情标记一起清 */
void ao_detail_close_all(void);
