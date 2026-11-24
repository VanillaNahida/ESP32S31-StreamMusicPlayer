/*
 * ao_page_pl.cc —— 第 1 页「我添加的歌单」（列表 + 歌单详情 + 长按菜单）
 *
 * 版式见 ao_view.h（列表 / 详情两种），这里只负责"填什么内容"：
 *   列表：online_playlist_count() 个歌单 → ao_style_playlist_row()
 *   详情：ao_queue_songs() 里的歌 → ao_style_song_row()，左卡片显示歌单名 + 首曲封面
 *
 * ★★ 长按菜单的两条机制**绝不能简化**（见 docs/refactor/04_app_online.md R5/R6）：
 *   1) 点行进详情挂的是 **LV_EVENT_SHORT_CLICKED**，不是 CLICKED ——
 *      LVGL 松手时无条件补发 CLICKED，用 CLICKED 的话"长按既弹菜单又进歌单页"；
 *   2) 长按回调**只记录目标 + lv_async_call**，菜单本体等手指抬起来再建。
 *
 * ★ 返回：详情模式不再自己画"‹ 返回"行 —— 全局返回钮经 ui_back_button_set_handler()
 *   接管（见 app_online.cc 的 online_back_handler），按返回先回列表。
 */
#include "lvgl.h"

#include "app_music.h"
#include "online_service.h"
#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_common.h"
#include "ao_model.h"
#include "ao_overlay.h"
#include "ao_pages.h"
#include "ao_provider.h"
#include "ao_style.h"
#include "ao_view.h"
#include "ao_work.h"

static ao_view_t s_view;

void ao_pl_clear(void)
{
    ao_view_clear(&s_view);
}

lv_obj_t *ao_pl_list(void)
{
    return ao_view_list(&s_view);
}

/* "播放全部"：把当前这份队列从第 0 首开始放 */
static void pl_play_all_clicked(lv_event_t *e)
{
    (void)e;
    if (!ao_detail_owns_songs() || ao_queue_count() == 0) {
        ui_toast("歌单还没载入", 1500);
        return;
    }
    ao_queue_drop_stale();
    app_music_play_online(ao_queue_songs(), ao_queue_count(), 0);
    app_music_open_player();
}

/* 详情页里的"重新载入这个歌单" */
static void pl_reload_clicked(lv_event_t *e)
{
    (void)e;
    const char *id = (ao_pl_sel() >= 0) ? online_playlist_id((size_t)ao_pl_sel()) : NULL;
    if (!id) {
        return;
    }
    ao_detail_mark_request(lv_tick_get());
    ao_pl_rebuild();               /* 先把转圈画上，再发请求 */
    ao_list_scroll_top(ao_view_list(&s_view));
    ao_work_submit(OREQ_LOAD, id);
}

/*
 * 长按菜单的遮罩与"关闭"（完整定义在下面，这里先给声明：上面这个点击处理要用）
 */
static lv_obj_t *s_menu;
void ao_pl_menu_close(void);

/*
 * 打开歌单详情（SHORT_CLICKED，理由见文件头）。
 */
static void pl_row_clicked(lv_event_t *e)
{
    /* 兜底：菜单开着的时候不允许换页（正常路径下遮罩会把点击吃掉） */
    if (s_menu) {
        ao_pl_menu_close();
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const char *id = online_playlist_id((size_t)idx);
    if (!id) {
        return;
    }
    /* 按 ID 添加的歌单没有 dirid（加/删歌要用目录 ID）→ 清掉收藏上下文 */
    ao_collect_ctx_clear();
    ao_pl_detail_enter(idx);       /* sel=idx, detail=true, page=0 */
    ao_detail_mark_request(lv_tick_get());   /* 超时兜底计时（见 ao_detail_wait_rows） */
    ao_pl_rebuild();               /* 先画出"载入中"的空壳 */
    ao_list_scroll_top(ao_view_list(&s_view));
    ao_work_submit(OREQ_LOAD, id);
}

/* 返回键 / 列表态切换：退出详情（app_online 的返回键 handler 会调） */
void ao_pl_leave_detail(void)
{
    if (!ao_pl_detail()) { 
        return;
    }
    ao_pl_detail_exit();
    ao_pl_rebuild();
}

/* 长按歌单行 → 重命名 / 更新 / 删除 菜单 */
static int       s_menu_idx = -1;
/* 重命名目标（ao_pl_menu_close() 会清 s_menu_idx，所以先存到这份里） */
static int       s_rename_idx = -1;
/* 已经排了"等手指抬起再弹菜单"的异步调用（防重复排） */
static bool      s_menu_pending;
/* 菜单弹出的时刻：刚弹出的极短时间里忽略遮罩上的"点空白关闭" */
static uint32_t  s_menu_tick;

#define MENU_GRACE_MS  200      /* 这段时间内遮罩上的 CLICKED 不算"点空白关闭" */

/* 长按菜单的 BA 对话框句柄（s_menu 是它的 scrim，兼作"菜单开着吗"的标志） */
static ui_ba_dialog_t s_menu_dlg;
/* 待删除的歌单：确认框弹出期间菜单已经关了，s_menu_idx 会被清 —— 自己存一份 */
static int       s_del_idx = -1;
static char      s_del_id[ONLINE_ID_MAX];

static void pl_menu_build(int idx);
static void pl_menu_deferred(void *arg);

int  ao_pl_rename_idx(void)        { return s_rename_idx; }
void ao_pl_rename_idx_set(int idx) { s_rename_idx = idx; }

void ao_pl_menu_close(void)
{
    s_menu_pending = false;
    if (s_menu) {
        /* 渐出（下滑 + 缩小 + 淡出）：动画结束由 ui_ba_close 自己删 */
        ui_ba_close(&s_menu_dlg);
        s_menu = NULL;
    }
    s_menu_idx = -1;
}

static void pl_menu_deleted(lv_event_t *e)
{
    (void)e;
    s_menu = NULL;
    s_menu_idx = -1;
}

/* 「删除歌单」二次确认通过后真正下单（菜单那时已经关了） */
static void pl_menu_del_confirmed(void *user)
{
    (void)user;
    const int idx = s_del_idx;
    s_del_idx = -1;
    if (idx < 0 || !s_del_id[0]) {
        return;
    }
    if (ao_pl_sel() == idx) {
        ao_pl_leave_detail();      /* 删的是眼前这个歌单：先退回列表 */
    }
    ui_toast("已删除该歌单", 1500);
    ao_work_submit(OREQ_REMOVE, s_del_id);
}

static void pl_menu_btn(lv_event_t *e)
{
    const int action = (int)(intptr_t)lv_event_get_user_data(e);   /* 0=重命名 1=更新 2=删除 */
    const int idx = s_menu_idx;
    const char *id = (idx >= 0) ? online_playlist_id((size_t)idx) : NULL;
    if (!id) {
        ao_pl_menu_close();
        return;
    }
    char id_copy[ONLINE_ID_MAX];
    snprintf(id_copy, sizeof(id_copy), "%s", id);   /* 删掉菜单后 id 指针可能失效 */
    ao_pl_menu_close();

    if (action == 0) {
        /* ao_pl_menu_close() 会把 s_menu_idx 清掉：先把目标歌单存进 s_rename_idx */
        ao_pl_rename_idx_set(idx);
        ao_modal_open(MODAL_RENAME);
        return;
    }
    if (action == 1) {
        if (!online_has_network()) {
            ui_toast("Wi-Fi 未连接", 1500);
            return;
        }
        ui_toast("正在更新歌单，请稍后…", 1500);
        ao_work_submit(OREQ_ADD, id_copy);   /* ADD 对已存在的 ID 就是"重新抓取刷新" */
        return;
    }

    /* ★ 删除是不可逆的（歌单要从服务器重新抓才有）：先弹二次确认 */
    s_del_idx = idx;
    snprintf(s_del_id, sizeof(s_del_id), "%s", id_copy);
    ui_ba_confirm_ex("删 除 歌 单", "确认删除这个歌单吗？\n删除后需要重新添加才能找回。",
                     "删除", "取消", true, pl_menu_del_confirmed, NULL);
}

/*
 * 长按歌单行：**只记录目标并排一个异步调用**，菜单本体等手指抬起来再建。
 * （原因见文件头第 2 条：按压期间建出来的全屏遮罩会被同一次松手关掉。）
 */
static void pl_row_long_pressed(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= online_playlist_count() || s_menu || s_menu_pending) {
        return;
    }
    s_menu_idx     = idx;
    s_menu_pending = true;
    lv_async_call(pl_menu_deferred, NULL);
}

/* 还按着就下一轮再看 —— 宁可晚一点弹，也不能让"长按那次按压"碰到菜单 */
static void pl_menu_deferred(void *arg)
{
    (void)arg;
    if (!s_menu_pending) {
        return;
    }
    const lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lv_async_call(pl_menu_deferred, NULL);
        return;
    }
    const int idx = s_menu_idx;
    s_menu_pending = false;
    if (idx >= 0 && (size_t)idx < online_playlist_count()) {
        pl_menu_build(idx);
    }
}

/* 点遮罩空白处关掉菜单；刚弹出的极短时间里不认（那一下只可能是长按的余波） */
static void pl_menu_scrim_clicked(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_menu_tick) < MENU_GRACE_MS) {
        return;
    }
    ao_pl_menu_close();
}

/* 点菜单自己的空白处不该关掉菜单（CLICKED 会从子对象冒泡上来） */
static void pl_menu_panel_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

/* 真正建菜单（只在手指离开屏幕后被调用） */
static void pl_menu_build(int idx)
{
    /* ★ 防泄漏：菜单已经在了就直接返回（否则留下孤儿遮罩 = 界面卡死） */
    if (s_menu) {
        return;
    }
    s_menu_tick = lv_tick_get();

    /* 碧蓝档案风格白卡片 + 三条选项行（左侧色条区分动作，删除用红） */
    const char *nm = online_playlist_name((size_t)idx);
    s_menu_dlg = ui_ba_open((nm && nm[0]) ? nm : "歌单", 56, LV_ALIGN_CENTER, 0);
    s_menu = s_menu_dlg.scrim;
    lv_obj_add_event_cb(s_menu, pl_menu_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(s_menu, pl_menu_scrim_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_menu_dlg.card, pl_menu_panel_clicked, LV_EVENT_CLICKED, NULL);

    ui_ba_hint(s_menu_dlg.body, "选择要对这个歌单做的操作");
    ui_ba_opt_row(s_menu_dlg.body, "重命名歌单", BA_BLUE, pl_menu_btn, (void *)(intptr_t)0);
    ui_ba_opt_row(s_menu_dlg.body, "更新（重新抓取）", 0x3E9E86, pl_menu_btn, (void *)(intptr_t)1);
    ui_ba_opt_row(s_menu_dlg.body, "删除歌单", BA_DANGER, pl_menu_btn, (void *)(intptr_t)2);
}

void ao_pl_rebuild(void)
{
    lv_obj_t *list = ao_view_list(&s_view);
    if (!list) {
        return;
    }
    lv_obj_clean(list);

    /* ---------------- 详情：某个歌单的歌曲列表 ---------------- */
    if (ao_pl_detail()) {
        const int sel = ao_pl_sel();
        const char *id = (sel >= 0) ? online_playlist_id((size_t)sel) : NULL;
        const char *nm = (sel >= 0) ? online_playlist_name((size_t)sel) : NULL;
        const bool own = ao_detail_owns_songs();
        const size_t song_n = ao_queue_count();
        const char *title = (nm && nm[0]) ? nm : (id ? id : "歌单");

        /*
         * 副标题写"这个歌单是什么"（几首 · 第几页）：设计稿这里是一行说明文字，
         * 几百首的歌单靠它告诉用户还能往后翻。
         */
        char sub[80];
        if (own) {
            const int pages = ao_page_count(song_n);
            const int page = ao_page_clamp(ao_pl_page(), song_n);
            snprintf(sub, sizeof(sub), "%u 首 · 第 %d/%d 页", (unsigned)song_n, page + 1, pages);
        } else {
            /* 副标题是"这个歌单是什么"，加载中也要说人话（轮换文案只给转圈用） */
            snprintf(sub, sizeof(sub), "正在载入曲目…");
        }
        ao_view_detail_mode(&s_view, title, sub);
        ao_pages_sync_detail(1, true);

        if (own) {
            const int page = ao_page_clamp(ao_pl_page(), song_n);
            ao_pl_page_set(page);
            const size_t first = ao_page_first(page);
            size_t rows = (song_n > first) ? (song_n - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            online_song_t *songs = ao_queue_songs();
            for (size_t i = 0; i < rows; i++) {
                const size_t idx = first + i;
                lv_obj_t *row = ao_style_song_row(list, (int)idx + 1, songs[idx].title,
                                                 songs[idx].artist,
                                                 (idx == 0),            /* 设计稿首行高亮 */
                                                 (i + 1 < rows));       /* 最后一行不画分割线 */
                ao_song_row_wire(row, idx);
            }
            ao_page_rows_flat(list, PG_PL, page, ao_page_count(song_n));
            /* 左卡片：歌单名 + 首曲封面（异步抓；解好后回调自己贴） */
            ao_view_set_cover(&s_view, (song_n > 0) ? &songs[0] : NULL);
        } else if (ao_queue_owner() == OWNER_SRCH || ao_queue_owner() == OWNER_ACCT) {
            /* s_songs 里是别人（搜索/账号歌单）的结果：给一个"重新载入"按钮 */
            lv_obj_t *reload = ao_style_text_row(list, "重新载入这个歌单");
            lv_obj_add_event_cb(reload, pl_reload_clicked, LV_EVENT_CLICKED, NULL);
        } else {
            ao_detail_wait_rows(list, pl_reload_clicked);   /* 转圈（超时给重试） */
        }
        return;
    }

    /* ---------------- 列表：所有"我添加的歌单" ---------------- */
    ao_pages_sync_detail(1, false);
    const size_t n = online_playlist_count();
    char hint[96];
    snprintf(hint, sizeof(hint), "歌单 · %u 个（长按可重命名 / 更新 / 删除）", (unsigned)n);
    ao_view_list_mode(&s_view, hint);

    if (n == 0) {
        lv_obj_t *h = lv_label_create(list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(92));
        lv_obj_set_style_pad_left(h, UI_PX_W(40), LV_PART_MAIN);
        char buf2[192];
        /* 右上角的「+」已经按需求移除（2026-11），文案里不要再提它 */
        snprintf(buf2, sizeof(buf2),
                 online_has_network()
                     ? "还没有歌单：切到「我的歌单」按%s账号同步，或直接进「搜索」找歌"
                     : "Wi-Fi 未连接：进「设置」连上网络后这里会自动加载",
                 ao_provider_account_name());
        lv_label_set_text(h, buf2);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *id = online_playlist_id(i);
        const char *nm = online_playlist_name(i);
        /* QQ 的歌单键带 'q' 前缀（与网易云同号不同单），显示时把前缀藏掉 */
        const char *shown_id = (id && id[0] == 'q' && id[1] >= '0' && id[1] <= '9') ? id + 1 : id;
        /* 设计稿的"高亮首行"用在列表第一项上（与在线音乐主界面.svg 一致） */
        lv_obj_t *row = ao_style_playlist_row(list,
                                              (nm && nm[0]) ? nm : (shown_id ? shown_id : "歌单"),
                                              shown_id, (i == 0), (i + 1 < n));
        lv_obj_add_event_cb(row, pl_row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, pl_row_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

lv_obj_t *ao_pl_build(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page, 0, 0);

    ao_view_init(&s_view, page);
    lv_obj_add_event_cb(ao_view_play_all(&s_view), pl_play_all_clicked, LV_EVENT_CLICKED, NULL);
    return page;
}
