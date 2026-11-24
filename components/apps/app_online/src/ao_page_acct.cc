/*
 * ao_page_acct.cc —— 第 0 页「我的歌单」（账号歌单 + 每日推荐）
 *
 * 版式见 ao_view.h（列表 / 详情两种），这里只负责"填什么内容"：
 *   列表：每日推荐（QQ 是每日30首 + 私人雷达）+ 账号歌单
 *   详情：每日推荐/某个账号歌单的歌曲（懒加载 + 分页），左卡片是歌单名 + 首曲封面
 *
 * 未登录时不再在本页塞"扫码登录"的大块占位 —— 登录/退出统一收到第 3 页「账号」，
 * 这里只留一句提示 + 一个"去账号页"的入口（重构指南：账号单独一页）。
 *
 * 账号歌单详情是**懒加载 + 分页**：打开时按 trackCount 一次性 calloc（指针从此不变），
 * 只取当前页需要的那一段（见 ao_model.cc 的 ao_lazy_*）。
 *
 * 时间一律由本文件取好再传给模型（模型不 include LVGL）：`lv_tick_get()`。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "app_music.h"
#include "online_service.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_common.h"
#include "ao_account.h"
#include "ao_model.h"
#include "ao_pages.h"
#include "ao_overlay.h"
#include "ao_provider.h"
#include "ao_style.h"
#include "ao_view.h"
#include "ao_work.h"

static ao_view_t s_view;

/* ================================================================ 每日推荐缓存
 *
 * QQ 的 /qq/recommend/daily 在服务端是"串 6 次猜你喜欢"拼出来的（见 ncm-api 的
 * qq-provider.md §5.4）—— 每次请求都是新的 30 首，所以"每次进来都是新歌单"。
 * 用户要的是"今天的 30 首"，于是这里按日期落盘：同一天命中、换天失效。
 *
 * ⚠ 缓存只对**在线歌单**有意义的两种推荐生效（每日推荐 / 每日30首）；
 *   私人雷达是"滚动推荐"，故意不缓存。
 */
#define DAILY_CACHE_PREFIX "qq30_"

/* 当天的 yyyyMMdd；时钟没校时返回 0（那种情况下不缓存，否则 1970 那份会永远命中） */
static uint32_t today_ymd(void)
{
    const time_t now = time(NULL);
    if (now < 1700000000) {          /* 2023-11 之前 = 还没对上时间 */
        return 0;
    }
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) {
        return 0;
    }
    const int y = tmv.tm_year + 1900;
    if (y < 2024 || y > 2100) {
        return 0;
    }
    return (uint32_t)(y * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

/* 缓存 key：日期 + 前缀（长度 ≤ 24，见 online_service 的 ONLINE_ID_MAX） */
static void daily_cache_key(int feed, char *out, size_t cap)
{
    out[0] = '\0';
    if (feed != AO_FEED_QQ_DAILY) {
        return;                      /* 只缓存 QQ 每日30首（网易云服务端按天稳定） */
    }
    const uint32_t d = today_ymd();
    if (d == 0) {
        return;
    }
    snprintf(out, cap, DAILY_CACHE_PREFIX "%08u", (unsigned)d);
}

bool ao_daily_cache_take(int feed)
{
    char key[24];
    daily_cache_key(feed, key, sizeof(key));
    if (!key[0]) {
        return false;
    }
    online_song_t *songs = NULL;
    size_t n = 0;
    if (online_feed_cache_load(key, &songs, &n) != ESP_OK || !songs || n == 0) {
        free(songs);
        return false;
    }
    ao_acct_feed_set(feed);
    ao_daily_loaded();
    ao_queue_install(songs, n, DETAIL_KEY_QQ_DAILY, OWNER_ACCT);
    ESP_LOGI(AO_TAG, "daily cache hit: %s (%u songs)", key, (unsigned)n);
    return true;
}

void ao_daily_cache_store(int feed, const online_song_t *songs, size_t n)
{
    char key[24];
    daily_cache_key(feed, key, sizeof(key));
    if (!key[0] || !songs || n == 0) {
        return;
    }
    if (online_feed_cache_save(key, songs, n) == ESP_OK) {
        ESP_LOGI(AO_TAG, "daily cached: %s (%u songs)", key, (unsigned)n);
    }
}

void ao_daily_cache_clear(void)
{
    char key[24];
    daily_cache_key(AO_FEED_QQ_DAILY, key, sizeof(key));
    if (key[0]) {
        online_feed_cache_forget(key);
        ESP_LOGI(AO_TAG, "daily cache cleared: %s", key);
    }
}

void ao_acct_clear(void)
{
    /* 第 0 页的控件随 screen 一起销毁：必须清指针，
     * 否则残留的悬垂指针一旦被 rebuild 碰到就是崩溃 */
    ao_view_clear(&s_view);
}

lv_obj_t *ao_acct_list(void)
{
    return ao_view_list(&s_view);
}

/* "播放全部"：从队列第 0 首开始放 */
static void acct_play_all_clicked(lv_event_t *e)
{
    (void)e;
    if (ao_queue_count() == 0) {
        ui_toast("歌单还没载入", 1500);
        return;
    }
    ao_queue_drop_stale();
    app_music_play_online(ao_queue_songs(), ao_queue_count(), 0);
    app_music_open_player();
}

/* 去第 3 页登录 */
static void acct_goto_login(lv_event_t *e)
{
    (void)e;
    ao_page_show(3, true);
}

/* 一次性推荐列表（每日推荐 / QQ 每日30首 / 私人雷达）：kind 见 ao_account.h 的 AO_FEED_* */
static void acct_feed_clicked(lv_event_t *e)
{
    const int kind = (int)(intptr_t)lv_event_get_user_data(e);
    ao_collect_ctx_clear();       /* 推荐列表不是可编辑歌单 */
    /*
     * 先把"当前队列属于谁"抹掉：请求在途的那 1~2 秒里屏幕上是"载入中"，
     * 而不是上一份列表（2026-11 用户报"两家的每日推荐/私人雷达串了"）。
     */
    ao_queue_key_reset();
    ao_acct_feed_enter(kind);
    ao_detail_mark_request(lv_tick_get());   /* 超时兜底计时（见 ao_detail_wait_rows） */

    /*
     * ★ 先看"今天有没有抓过"：QQ 的每日30首在服务端是随机的，不缓存的话
     *   同一个入口反复进出会一直换歌单（用户 2026-11 反馈）。
     */
    if (ao_daily_cache_take(kind)) {
        ao_acct_rebuild();
        ao_list_scroll_top(ao_view_list(&s_view));
        ui_toast("今天这一份（本地缓存）", 1500);
        return;
    }

    ao_acct_rebuild();                       /* 先把"载入中"铺上，再发请求 */
    ao_list_scroll_top(ao_view_list(&s_view));
    /* 一次一整份（30~33 首），不做懒加载 */
    ao_work_submit(OREQ_DAILY, kind == AO_FEED_QQ_DAILY ? "guess"
                            : kind == AO_FEED_QQ_RADAR ? "radar" : NULL);
}

/*
 * 这个账号歌单是不是「我喜欢的音乐」？
 *
 *   QQ   ：服务层给的那条特殊项，id 是 ONLINE_QQ_FAV_KEY 哨兵；
 *   网易云：该歌单的 playlist id 就是自己的 uid；
 *   另外两家都叫「我喜欢的音乐」，名字兜一次底。
 */
static bool acct_is_fav_playlist(const online_acct_playlist_t *p)
{
    if (!p) {
        return false;
    }
    if (strcmp(p->id, ONLINE_QQ_FAV_KEY) == 0) {
        return true;
    }
    const char *uid = online_netease_uid();
    if (uid && uid[0] && strcmp(p->id, uid) == 0) {
        return true;
    }
    return strstr(p->name, "我喜欢") != NULL;
}

/*
 * 「我喜欢的音乐」卡片封面要用的那首歌：**最近收藏的那一首**（见
 * online_last_liked_song_set，播放器页点亮红心时记下来的）。
 * 不是它 / 没有记录 / 音源对不上（这台机器现在登的是另一家）→ 返回 NULL，
 * 调用方退回"歌单第一首"的老口径。
 */
static const online_song_t *acct_fav_cover_song(const online_acct_playlist_t *p)
{
    static online_song_t s_song;
    if (!acct_is_fav_playlist(p)) {
        return NULL;
    }
    if (!online_last_liked_song_get(&s_song)) {
        return NULL;
    }
    const uint8_t want = ao_is_qq() ? ONLINE_SRC_QQ : ONLINE_SRC_NETEASE;
    if (s_song.src != want || !s_song.cover_url[0]) {
        return NULL;
    }
    return &s_song;
}

static void acct_pl_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= ao_acct_pls_count()) {
        return;
    }
    const online_acct_playlist_t *p = ao_acct_playlists(NULL) + idx;
    /* 记住"当前列表是哪个歌单"：长按 → 收藏浮层里的「删除」要用它的 dirid */
    ao_collect_ctx_set(p->dirid, p->name);
    if (p->track_count == 0) {
        /* 空歌单（QQ 的「我喜欢的音乐」在没收藏时就是 0 首）：
         * 不要进详情 —— 懒加载会按 0 首分配缓冲、再去要一页，然后一直转圈。 */
        ui_toast("这个歌单还没有歌", 1600);
        return;
    }
    ao_acct_pl_enter(idx);
    ao_detail_mark_request(lv_tick_get());   /* 超时兜底计时 */
    /*
     * ★ 懒加载：先按"歌单总首数"把缓冲分配好（指针从此不变），**只取第一页**；
     *   其余等用户翻页时按需取（见 ao_lazy_fetch_if_needed）。
     */
    if (!ao_lazy_begin(p->id, (size_t)p->track_count)) {
        ui_toast("内存不足，打不开这个歌单", 2000);
        ao_acct_detail_close();
        ao_acct_rebuild();
        return;
    }
    ao_acct_rebuild();                   /* 先铺"载入中" */
    ao_list_scroll_top(ao_view_list(&s_view));
    ao_lazy_fetch_if_needed(lv_tick_get());   /* 再取第一页 */
}

/* 详情页里"重新载入"：当前详情（每日推荐 / 某个账号歌单）再取一次 */
static void acct_reload_clicked(lv_event_t *e)
{
    (void)e;
    ao_fetch_fail_clear();
    ao_detail_mark_request(lv_tick_get());
    if (ao_acct_daily()) {
        ao_acct_rebuild();                   /* 先把转圈画上 */
        ao_work_submit(OREQ_DAILY, NULL);
        return;
    }
    const size_t n = ao_acct_pls_count();
    const int sel = ao_acct_sel();
    if (sel < 0 || (size_t)sel >= n) {
        return;
    }
    /*
     * 两种情况分开处理：
     *   1) 缓冲里还是**别的东西**（别的歌单 / 搜索结果）→ 重建懒加载缓冲；
     *   2) 只是"这一页没取到"（缓冲就是本歌单、已经载入若干首）→ 保留已载入的，
     *      只清掉失败标记重新补页。
     */
    if (!ao_detail_key_matches()) {
        const online_acct_playlist_t *p = ao_acct_playlists(NULL) + sel;
        if (!ao_lazy_begin(p->id, (size_t)p->track_count)) {
            ui_toast("内存不足，打不开这个歌单", 2000);
            return;
        }
    }
    ao_acct_rebuild();
    ao_lazy_fetch_if_needed(lv_tick_get());
}

/* 返回键：退出详情，回到账号歌单列表（app_online 的返回键 handler 会调） */
void ao_acct_leave_detail(void)
{
    if (!ao_acct_detail()) {
        return;
    }
    ao_collect_ctx_clear();
    ao_acct_detail_exit();
    ao_acct_rebuild();
}

/*
 * 重新拉当前账号歌单详情的**第一页**（删除成功后用）。
 * 直接复用"点开歌单"那条路：ao_lazy_begin 会把队列换成空壳（旧的交给播放器接管）。
 */
void ao_acct_detail_refresh(void)
{
    if (!ao_acct_detail() || !ao_view_list(&s_view)) {
        return;
    }
    const int sel = ao_acct_sel();
    const size_t n = ao_acct_pls_count();
    if (sel < 0 || (size_t)sel >= n) {
        return;                     /* 每日推荐没有懒加载缓冲 */
    }
    const online_acct_playlist_t *p = ao_acct_playlists(NULL) + sel;
    if (!ao_lazy_begin(p->id, (size_t)p->track_count)) {
        return;
    }
    ao_acct_page_set(0);
    ao_acct_rebuild();
    ao_lazy_fetch_if_needed(lv_tick_get());
}

/* 列表模式下的一行歌单（设计稿：首行高亮 + 青点） */
static lv_obj_t *acct_make_pl_row(lv_obj_t *list, const char *title, const char *sub,
                                  bool first, bool divider, lv_event_cb_t cb, int idx)
{
    lv_obj_t *row = ao_style_playlist_row(list, title, sub, first, divider);
    lv_obj_add_event_cb(row, cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx);
    return row;
}

void ao_acct_rebuild(void)
{
    lv_obj_t *list = ao_view_list(&s_view);
    if (!list) {
        return;
    }
    lv_obj_clean(list);

    /*
     * ★ "当前判定未登录"时再读一次 SD 上的 cookie：开机后 SD 可能晚挂上，
     *   那一次读不到并不代表没登录过（见 online_netease_cookie_reload 的说明）。
     */
    if (!ao_logged_in()) {
        ao_login_state_reload();
    }
    const bool in = ao_logged_in();
    ao_login_name_sync();

    /* ---------------- 详情：每日推荐 / 某个账号歌单的歌曲 ---------------- */
    if (ao_acct_detail()) {
        const size_t pls_n = ao_acct_pls_count();
        const int sel = ao_acct_sel();

        /* 头部：标题 + 副标题（歌单名 / 已载入多少 / 第几页） */
        char title[96];
        char sub[96];
        if (ao_acct_daily()) {
            static const char *const kFeedName[] = { "每日推荐", "每日30首", "私人雷达" };
            int kind = ao_acct_feed();
            if (kind < 0 || kind > AO_FEED_QQ_RADAR) {
                kind = AO_FEED_NETEASE_DAILY;
            }
            snprintf(title, sizeof(title), "%s", kFeedName[kind]);
            snprintf(sub, sizeof(sub), "%u 首 · 每天更新", (unsigned)ao_queue_count());
        } else if (sel >= 0 && (size_t)sel < pls_n) {
            const online_acct_playlist_t *p = ao_acct_playlists(NULL) + sel;
            snprintf(title, sizeof(title), "%.48s", p->name);
            const int pg = ao_acct_lazy() ? ao_acct_page_count() : ao_page_count(ao_queue_count());
            int page = ao_acct_page();
            if (page >= pg) {
                page = pg - 1;
            }
            if (page < 0) {
                page = 0;
            }
            if (ao_acct_lazy()) {
                if (ao_songs_total()) {
                    snprintf(sub, sizeof(sub), "已载入 %u/%u 首 · 第 %d/%d 页",
                             (unsigned)ao_queue_count(), (unsigned)ao_songs_total(),
                             page + 1, pg);
                } else {
                    snprintf(sub, sizeof(sub), "已载入 %u 首 · 第 %d/%d 页",
                             (unsigned)ao_queue_count(), page + 1, pg);
                }
            } else {
                snprintf(sub, sizeof(sub), "%d 首", p->track_count);
            }
        } else {
            snprintf(title, sizeof(title), "歌单");
            sub[0] = '\0';
        }
        ao_view_detail_mode(&s_view, title, sub);
        ao_pages_sync_detail(0, true);

        /*
         * ★ 只画"这一份歌确实属于当前详情"的时候（ao_detail_key_matches）。
         *   否则点开第二个歌单时会把**上一个歌单的歌**先顶上来。
         */
        if (ao_detail_key_matches()) {
            const size_t shown = ao_queue_count();
            const int pages = ao_acct_lazy() ? ao_acct_page_count() : ao_page_count(shown);
            int page = ao_acct_page();
            if (page >= pages) {
                page = pages - 1;
            }
            if (page < 0) {
                page = 0;
            }
            ao_acct_page_set(page);
            const size_t first = ao_page_first(page);
            size_t rows = (shown > first) ? (shown - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            online_song_t *songs = ao_queue_songs();
            /*
             * 封面：默认是"歌单第一首"；但如果这个歌单是「我喜欢的音乐」，
             * 换成**最近收藏的那一首**（用户 2026-11 的要求）。
             */
            const online_song_t *fav_cover =
                (sel >= 0 && (size_t)sel < pls_n) ? acct_fav_cover_song(&ao_acct_playlists(NULL)[sel])
                                                  : NULL;
            if (rows > 0) {
                for (size_t i = 0; i < rows; i++) {
                    const size_t idx = first + i;
                    lv_obj_t *row = ao_style_song_row(list, (int)idx + 1, songs[idx].title,
                                                      songs[idx].artist, (idx == 0),
                                                      (i + 1 < rows));
                    ao_song_row_wire(row, idx);
                }
                ao_view_set_cover(&s_view, fav_cover ? fav_cover : &songs[0]);
            }
            /*
             * 这一页**还没取满**、而且**不确认已经到歌单末尾** → 转圈 + 按需补页。
             * ⚠ 判据不能写成"总数 > 已画的"，因为总数可能未知（trackCount 缺失）。
             */
            const bool at_end = (ao_songs_total() > 0) ? (shown >= ao_songs_total())
                                                       : (shown >= ao_queue_cap());
            if (ao_acct_lazy() && rows < ONLINE_PAGE_ROWS && !at_end) {
                if (ao_fetch_fail()) {
                    lv_obj_t *h = lv_label_create(list);
                    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
                    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
                    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(h, LV_PCT(100));
                    const char *why = online_last_error();
                    lv_label_set_text_fmt(h, "这一页没取到：%.40s", why ? why : "网络问题");
                    lv_obj_t *retry = ao_style_text_row(list, "重试");
                    lv_obj_add_event_cb(retry, acct_reload_clicked, LV_EVENT_CLICKED, NULL);
                } else {
                    ao_loading_rows(list, NULL);   /* 用轮换的加载文案 */
                    ao_lazy_fetch_if_needed(lv_tick_get());   /* 需要哪一段就取哪一段 */
                }
            }
            ao_page_rows_flat(list, PG_ACCT, page, pages);
        } else if (ao_queue_owner() == OWNER_ACCT) {
            ao_detail_wait_rows(list, acct_reload_clicked);   /* 转圈（超时给重试） */
        } else {
            lv_obj_t *reload = ao_style_text_row(list, "重新载入");
            lv_obj_add_event_cb(reload, acct_reload_clicked, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    /* ---------------- 列表模式 ---------------- */
    ao_pages_sync_detail(0, false);

    /* 未登录：一句提示 + 一个去账号页的入口（登录本身在第 3 页） */
    if (!in) {
        ao_view_list_mode(&s_view, "登录后显示你的歌单");
        lv_obj_t *row = ao_style_playlist_row(list, "去「账号」页扫码登录",
                                             "同步歌单 / 我喜欢 / 每日推荐", true, false);
        lv_obj_add_event_cb(row, acct_goto_login, LV_EVENT_CLICKED, NULL);
        return;
    }

    ao_acct_kick_if_needed(lv_tick_get());
    ao_acct_nick_kick_if_needed(lv_tick_get());

    const size_t pls_n = ao_acct_pls_count();
    char hint[96];
    if (ao_acct_pls_busy() && pls_n == 0) {
        snprintf(hint, sizeof(hint), "歌单 · 正在加载…");
    } else {
        snprintf(hint, sizeof(hint), "歌单 · %u 个", (unsigned)pls_n);
    }
    ao_view_list_mode(&s_view, hint);

    /* 网络没通时给一句明确的话（不是"正在加载"转到天荒地老） */
    if (!online_has_network() && pls_n == 0) {
        lv_obj_t *h = lv_label_create(list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(92));
        lv_label_set_text(h, "Wi-Fi 未连接：进「设置」连上网络后这里会自动加载歌单");
        return;
    }

    /*
     * 一次性推荐列表：
     *   网易云 → 每日推荐（/recommend/songs）
     *   QQ    → 每日30首（猜你喜欢连拿 6 次）+ 私人雷达（雷达 3 页）
     */
    bool first_used = false;
    if (!ao_is_qq()) {
        lv_obj_t *dr = acct_make_pl_row(list, "每日推荐", "根据你的口味每天更新",
                                        true, true, acct_feed_clicked,
                                        AO_FEED_NETEASE_DAILY);
        (void)dr;
        first_used = true;
    } else {
        acct_make_pl_row(list, "每日30首", "每天为你挑 30 首", true, true,
                         acct_feed_clicked, AO_FEED_QQ_DAILY);
        acct_make_pl_row(list, "私人雷达", "按你的口味滚动推荐", false, true,
                         acct_feed_clicked, AO_FEED_QQ_RADAR);
        first_used = true;
    }

    const online_acct_playlist_t *pls = ao_acct_playlists(NULL);
    for (size_t i = 0; i < pls_n; i++) {
        char sub[64];
        /* QQ 的「我喜欢的音乐」是服务层插进来的第一条（dirid=201） */
        const bool fav = ao_is_qq() && strcmp(pls[i].id, ONLINE_QQ_FAV_KEY) == 0;
        if (fav) {
            snprintf(sub, sizeof(sub), "%d 首", pls[i].track_count);
        } else {
            snprintf(sub, sizeof(sub), "%s · %d 首",
                     pls[i].self_created ? "我创建的" : "收藏", pls[i].track_count);
        }
        /* 设计稿的首行高亮给"列表第一条"：有推荐行时给推荐行，否则给第一个歌单 */
        const bool first = !first_used && (i == 0);
        const bool divider = (i + 1 < pls_n);
        acct_make_pl_row(list, pls[i].name, sub, first, divider, acct_pl_clicked, (int)i);
    }

    /* 列表还没到：转圈（用户要求："加载歌单这里也要转圈 + 加载出来再显示"） */
    if (ao_acct_pls_busy() && pls_n == 0) {
        ao_loading_rows(list, NULL);      /* 用轮换的加载文案 */
        return;
    }
    if (pls_n == 0) {
        lv_obj_t *h = lv_label_create(list);
        lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_pad_left(h, UI_PX_W(40), LV_PART_MAIN);
        lv_label_set_text(h, "还没取到歌单（检查 API 地址是否正确、网络是否通）");
    }
}

lv_obj_t *ao_acct_build(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page, 0, 0);

    ao_view_init(&s_view, page);
    lv_obj_add_event_cb(ao_view_play_all(&s_view), acct_play_all_clicked, LV_EVENT_CLICKED, NULL);
    return page;
}
