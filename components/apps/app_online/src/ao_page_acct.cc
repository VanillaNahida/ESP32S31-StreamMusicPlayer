/*
 * ao_page_acct.cc —— 第 0 页「我的歌单」（网易云账号页）
 *
 * 未登录 = 一屏提示 + 「扫码登录」；已登录 = 「每日推荐」+ 账号歌单（我创建的/收藏）。
 * 点进去复用"同页换内容"的详情模式（和 metin 歌单详情同一套做法，少一层跨页路由）。
 *
 * 账号歌单详情是**懒加载 + 分页**：打开时按 trackCount 一次性 calloc（指针从此不变），
 * 只取当前页需要的那一段（见 ao_model.cc 的 ao_lazy_*）。
 *
 * 时间一律由本文件取好再传给模型（模型不 include LVGL）：`lv_tick_get()`。
 */
#include <string.h>

#include "lvgl.h"

#include "online_service.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_common.h"
#include "ao_account.h"
#include "ao_model.h"
#include "ao_pages.h"
#include "ao_overlay.h"
#include "ao_provider.h"
#include "ao_work.h"

/* 控件（页面容器由 ao_pages 持有；返回行挂在页容器上，故这里也持有） */
static lv_obj_t *s_acct_back;          /* 详情页的"‹ 返回"行（**固定**在列表上方） */
static lv_obj_t *s_acct_hint;          /* 顶部说明行 */
static lv_obj_t *s_acct_list;          /* 行容器（每日推荐 / 歌单行 / 歌曲行） */

void ao_acct_clear(void)
{
    /* 第 0 页（账号页）的控件随 screen 一起销毁：必须清指针，
     * 否则残留的悬垂指针一旦被 rebuild 碰到就是崩溃 */
    s_acct_hint = NULL;
    s_acct_list = NULL;
    s_acct_back = NULL;         /* 返回行挂在页容器上，随 screen 一起销毁 */
}

lv_obj_t *ao_acct_list(void)
{
    return s_acct_list;
}

static void acct_back_clicked(lv_event_t *e)
{
    (void)e;
    ao_collect_ctx_clear();
    ao_acct_detail_exit();
    ao_acct_rebuild();
}

/* 一次性推荐列表（每日推荐 / QQ 每日30首 / 私人雷达）：kind 见 ao_account.h 的 AO_FEED_* */
static void acct_feed_clicked(lv_event_t *e)
{
    const int kind = (int)(intptr_t)lv_event_get_user_data(e);
    ao_collect_ctx_clear();       /* 推荐列表不是可编辑歌单 */
    /*
     * 先把"当前队列属于谁"抹掉：请求在途的那 1~2 秒里屏幕上是"载入中"，
     * 而不是上一份列表（2026-11 用户报"两家的每日推荐/私人雷达串了"）。
     * 只清身份、不释放数组 —— 正在播的那首不受影响。
     */
    ao_queue_key_reset();
    ao_acct_feed_enter(kind);
    ao_detail_mark_request(lv_tick_get());   /* 超时兜底计时（见 ao_detail_wait_rows） */
    ao_acct_rebuild();                       /* 先把"载入中"铺上，再发请求 */
    ao_list_scroll_top(s_acct_list);
    /* 一次一整份（30~33 首），不做懒加载 */
    ao_work_submit(OREQ_DAILY, kind == AO_FEED_QQ_DAILY ? "guess"
                            : kind == AO_FEED_QQ_RADAR ? "radar" : NULL);
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
    ao_detail_mark_request(lv_tick_get());   /* 超时兜底计时（见 ao_detail_wait_rows） */
    /*
     * ★ 懒加载：先按"歌单总首数"把缓冲分配好（指针从此不变），**只取第一页**；
     *   其余等用户翻页时按需取（见 ao_lazy_fetch_if_needed）。
     *   歌单几百首也照样秒开 —— 大头（600 × 2.3 KB ≈ 1.4 MB 的 JSON）不再一次拉。
     */
    if (!ao_lazy_begin(p->id, (size_t)p->track_count)) {
        ui_toast("内存不足，打不开这个歌单", 2000);
        ao_acct_detail_close();
        ao_acct_rebuild();
        return;
    }
    ao_acct_rebuild();                   /* 先铺"载入中" */
    ao_list_scroll_top(s_acct_list);     /* 新详情从第一首开始显示 */
    ao_lazy_fetch_if_needed(lv_tick_get());   /* 再取第一页 */
}

static void acct_login_clicked(lv_event_t *e)
{
    (void)e;
    ao_qr_open();                        /* 和"我添加的歌单"页顶部那个入口是同一套浮层 */
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
     *      只清掉失败标记重新补页，别把前面几页再下一遍。
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

/*
 * 重新拉当前账号歌单详情的**第一页**（删除成功后用）。
 *
 * 直接复用"点开歌单"那条路：ao_lazy_begin 会把队列换成空壳（旧的交给播放器接管，
 * 见 ao_model.cc 的 ao_queue_drop_stale），所以正在播的那首不会被打断。
 */
void ao_acct_detail_refresh(void)
{
    if (!ao_acct_detail() || !s_acct_list) {
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

void ao_acct_rebuild(void)
{
    if (!s_acct_list || !s_acct_hint) {
        return;
    }
    lv_obj_clean(s_acct_list);
    /* 返回行只在详情页出现（它是页容器的固定行，不是列表行，见 ao_make_back_label） */
    if (s_acct_back) {
        lv_obj_add_flag(s_acct_back, LV_OBJ_FLAG_HIDDEN);
    }
    /*
     * ★ "当前判定未登录"时再读一次 SD 上的 cookie：开机后 SD 可能晚挂上，
     *   那一次读不到并不代表没登录过（见 online_netease_cookie_reload 的说明）。
     *   不这么做，重启后进这一页会一直停在"登录后显示你的歌单"，歌单自然也拉不出来。
     */
    if (!ao_logged_in()) {
        ao_login_state_reload();
    }
    const bool in = ao_logged_in();
    ao_login_name_sync();
    if (!in) {
        ao_acct_logged_out_view();
    }

    /* ---------------- 详情：每日推荐 / 某个账号歌单的歌曲 ---------------- */
    if (ao_acct_detail()) {
        /* 返回行固定在上方（不随列表滚动）；列表里**不再**放返回行 */
        if (s_acct_back) {
            lv_obj_remove_flag(s_acct_back, LV_OBJ_FLAG_HIDDEN);
        }

        const size_t pls_n = ao_acct_pls_count();
        const int sel = ao_acct_sel();
        /*
         * 头部信息：账号歌单是**懒加载**的，所以要说清"已经载入多少 / 总共多少 /
         * 第几页" —— 用户看到"已载入 100/637 首"就知道后面还能翻。
         */
        if (ao_acct_daily()) {
            static const char *const kFeedName[] = { "每日推荐", "每日30首", "私人雷达" };
            int kind = ao_acct_feed();
            if (kind < 0 || kind > AO_FEED_QQ_RADAR) {
                kind = AO_FEED_NETEASE_DAILY;
            }
            lv_label_set_text_fmt(s_acct_hint, "%s · %u 首", kFeedName[kind],
                                  (unsigned)ao_queue_count());
        } else if (sel >= 0 && (size_t)sel < pls_n) {
            const online_acct_playlist_t *p = ao_acct_playlists(NULL) + sel;
            if (ao_acct_lazy()) {
                const int pg = ao_acct_page_count();
                const int page = ao_acct_page();
                if (ao_songs_total()) {
                    lv_label_set_text_fmt(s_acct_hint,
                                          "%.32s · 已载入 %u/%u 首 · 第 %d/%d 页", p->name,
                                          (unsigned)ao_queue_count(),
                                          (unsigned)ao_songs_total(),
                                          page + 1, pg);
                } else {
                    /* 总首数未知（服务端没给 trackCount）：只报"已载入多少" */
                    lv_label_set_text_fmt(s_acct_hint, "%.32s · 已载入 %u 首 · 第 %d/%d 页",
                                          p->name, (unsigned)ao_queue_count(), page + 1, pg);
                }
            } else {
                lv_label_set_text_fmt(s_acct_hint, "%.32s · %d 首", p->name, p->track_count);
            }
        }

        /*
         * ★ 只画"这一份歌确实属于当前详情"的时候（ao_detail_key_matches）。
         *   否则点开第二个歌单时会把**上一个歌单的歌**先顶上来，
         *   用户看到的、点到的都是上一个歌单的歌（"挨个点开都是同样的"）。
         */
        if (ao_detail_key_matches()) {
            const size_t shown = ao_queue_count();            /* 已载入的**连续前缀** */
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
            if (rows > 0) {
                ao_fill_song_rows_range(s_acct_list, first, rows);
            }
            /*
             * 这一页**还没取满**、而且**不确认已经到歌单末尾** → 转圈 + 按需补页。
             * ⚠ 判据不能写成"总数 > 已画的"，因为总数可能未知（trackCount 缺失）——
             *   那样一首都不会去取，列表永远空着。到末尾由这两条确认：
             *   总首数已知且已载入到位，或者缓冲已经装满。
             */
            const bool at_end = (ao_songs_total() > 0)
                                    ? (shown >= ao_songs_total())
                                    : (shown >= ao_queue_cap());
            if (ao_acct_lazy() && rows < ONLINE_PAGE_ROWS && !at_end) {
                if (ao_fetch_fail()) {
                    lv_obj_t *h = lv_label_create(s_acct_list);
                    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
                    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
                    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(h, LV_PCT(100));
                    const char *why = online_last_error();
                    lv_label_set_text_fmt(h, "这一页没取到：%.40s", why ? why : "网络问题");
                    lv_obj_t *retry = ao_make_row(s_acct_list, LV_SYMBOL_REFRESH, 0x3E9E86,
                                                  "重试", NULL);
                    lv_obj_add_event_cb(retry, acct_reload_clicked, LV_EVENT_CLICKED, NULL);
                } else {
                    ao_loading_rows(s_acct_list, "载入中…");
                    ao_lazy_fetch_if_needed(lv_tick_get());   /* 需要哪一段就取哪一段 */
                }
            }
            ao_page_rows(s_acct_list, PG_ACCT, page, pages);
        } else if (ao_queue_owner() == OWNER_ACCT) {
            ao_detail_wait_rows(s_acct_list, acct_reload_clicked);   /* 转圈（超时给重试） */
        } else {
            /* s_songs 里是别处（搜索/歌单）的结果：给个重新载入的入口，
             * 别把界面卡在转圈上 */
            lv_obj_t *reload = ao_make_row(s_acct_list, LV_SYMBOL_REFRESH, 0x3E9E86,
                                           "重新载入", NULL);
            lv_obj_add_event_cb(reload, acct_reload_clicked, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    /* ---------------- 未登录：提示 + 扫码登录 ---------------- */
    if (!in) {
        lv_label_set_text_fmt(s_acct_hint, "%s账号", ao_provider_account_name());
        lv_obj_t *box = lv_obj_create(s_acct_list);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, UI_PX_H(30), LV_PART_MAIN);
        lv_obj_set_style_pad_row(box, UI_PX_H(14), LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *t1 = lv_label_create(box);
        lv_obj_set_style_text_font(t1, UI_FONT_TITLE, LV_PART_MAIN);
        lv_obj_set_style_text_color(t1, UI_COLOR_FG, LV_PART_MAIN);
        lv_label_set_text(t1, "登录后显示你的歌单");

        lv_obj_t *t2 = lv_label_create(box);
        lv_obj_set_style_text_font(t2, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(t2, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        /* 两家能拿到的东西不一样：QQ 没有"每日推荐"入口，换成「我喜欢」 */
        lv_label_set_text(t2, ao_is_qq()
                                  ? "用手机 QQ 扫码，\n即可获取你的歌单与「我喜欢」"
                                  : "用网易云音乐 App 扫码，\n即可获取你的账号歌单与每日推荐");

        lv_obj_t *btn = lv_button_create(box);
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, UI_PX_H(10), LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, UI_PX_H(10), LV_PART_MAIN);
        lv_obj_t *bl = lv_label_create(btn);
        lv_obj_set_style_text_font(bl, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_label_set_text(bl, ao_is_qq() ? "扫码登录 QQ 音乐" : "扫码登录网易云");
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, acct_login_clicked, LV_EVENT_CLICKED, NULL);
        return;
    }

    /* ---------------- 已登录：每日推荐 + 账号歌单 ---------------- */
    ao_acct_kick_if_needed(lv_tick_get());
    ao_acct_nick_kick_if_needed(lv_tick_get());

    const char *name = ao_login_name();
    const size_t pls_n = ao_acct_pls_count();
    /* 头部一行：昵称（NVS 里那份先用着）+ 已经拿到几个歌单 */
    const char *fallback = ao_is_qq() ? "QQ 音乐账号" : "网易云账号";
    if (ao_acct_pls_busy() && pls_n == 0) {
        lv_label_set_text_fmt(s_acct_hint, "%s · 正在加载歌单…", name[0] ? name : fallback);
    } else {
        lv_label_set_text_fmt(s_acct_hint, "%s · 歌单 %u 个", name[0] ? name : fallback,
                              (unsigned)pls_n);
    }

    /* 网络没通时给一句明确的话（不是"正在加载"转到天荒地老） */
    if (!online_has_network() && pls_n == 0) {
        lv_obj_t *h = lv_label_create(s_acct_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_text(h, "Wi-Fi 未连接：进「设置」连上网络后这里会自动加载歌单");
        return;
    }

    /*
     * 一次性推荐列表：
     *   网易云 → 每日推荐（/recommend/songs）
     *   QQ    → 每日30首（猜你喜欢连拿 6 次）+ 私人雷达（雷达 3 页）
     * 两家都是一次请求拿整份，点进去走同一条"OWner=ACCT + DETAIL_KEY_DAILY"的路径。
     */
    if (!ao_is_qq()) {
        lv_obj_t *dr = ao_make_row(s_acct_list, LV_SYMBOL_BELL, 0xE8A33D, "每日推荐",
                                   "根据你的口味每天更新");
        lv_obj_add_event_cb(dr, acct_feed_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)AO_FEED_NETEASE_DAILY);
    } else {
        lv_obj_t *daily = ao_make_row(s_acct_list, LV_SYMBOL_BELL, 0xE8A33D, "每日30首",
                                      "每天为你挑 30 首");
        lv_obj_add_event_cb(daily, acct_feed_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)AO_FEED_QQ_DAILY);
        lv_obj_t *radar = ao_make_row(s_acct_list, LV_SYMBOL_GPS, 0x6C5CE7, "私人雷达",
                                      "按你的口味滚动推荐");
        lv_obj_add_event_cb(radar, acct_feed_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)AO_FEED_QQ_RADAR);
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
        lv_obj_t *row = ao_make_row(s_acct_list,
                                    fav ? LV_SYMBOL_AUDIO : LV_SYMBOL_LIST,
                                    fav ? 0xE8475C : 0x3E9E86, pls[i].name, sub);
        lv_obj_add_event_cb(row, acct_pl_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* 列表还没到：转圈（用户要求："加载歌单这里也要转圈 + 加载出来再显示"） */
    if (ao_acct_pls_busy() && pls_n == 0) {
        ao_loading_rows(s_acct_list, "正在加载歌单…");
        return;
    }
    if (pls_n == 0) {
        lv_obj_t *h = lv_label_create(s_acct_list);
        lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(h, "还没取到歌单（检查 API 地址是否正确、网络是否通）");
    }
}

lv_obj_t *ao_acct_build(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page, 0, 0);

    /* ★ 返回行必须是页容器的**第一个**子对象（flex 列的第一行），这样它固定在
     *   列表上方、不随歌曲列表滚动（用户 2026-10 报"返回按钮跟着滑动"）。 */
    s_acct_back = ao_make_back_label(page, LV_SYMBOL_LEFT " 返回", acct_back_clicked);

    s_acct_hint = lv_label_create(page);
    lv_obj_set_style_text_font(s_acct_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_acct_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_acct_hint, LV_PCT(100));
    lv_label_set_long_mode(s_acct_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_acct_hint, "");

    s_acct_list = lv_obj_create(page);
    lv_obj_remove_style_all(s_acct_list);
    lv_obj_set_width(s_acct_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_acct_list, 1);
    lv_obj_set_flex_flow(s_acct_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_acct_list, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_acct_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_acct_list, LV_SCROLLBAR_MODE_AUTO);
    return page;
}
