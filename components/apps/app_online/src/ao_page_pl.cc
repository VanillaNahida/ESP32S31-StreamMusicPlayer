/*
 * ao_page_pl.cc —— 第 1 页「我添加的歌单」（列表 + 歌单详情 + 长按菜单）
 *
 * 同页换内容：先是歌单列表；点进某个歌单后，**同一页**变成"歌单详情"（歌曲列表），
 * 顶部给一行 "‹ 歌单列表" 返回。为什么不做成压栈的第二页：单页内切换状态更少、
 * 也不用把"加载结果该进哪个列表"跨页路由（那个最容易出竞态）。
 *
 * ★★ 长按菜单的两条机制**绝不能简化**（见 docs/refactor/04_app_online.md R5/R6）：
 *   1) `pl_row_clicked` 挂的是 **LV_EVENT_SHORT_CLICKED**，不是 CLICKED ——
 *      LVGL 松手时无条件补发 CLICKED，长按之后也会补，用 CLICKED 的话"长按既弹菜单
 *      又进歌单页"。SHORT_CLICKED 只在 indev_proc_short_click() 里发，永远不跟长按；
 *   2) 长按回调**只记录目标 + lv_async_call**，菜单本体等手指抬起来再建
 *      （按压期间建的遮罩会被 re-hit-test 命中，松手的 CLICKED 会把菜单自己关掉）。
 */
#include "lvgl.h"

#include "online_service.h"
#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "ao_common.h"
#include "ao_model.h"
#include "ao_overlay.h"
#include "ao_pages.h"
#include "ao_work.h"

static lv_obj_t *s_pl_list;      /* 行容器（歌单行 / 歌曲行） */
static lv_obj_t *s_pl_hint;      /* 顶部说明行 */
static lv_obj_t *s_pl_back;      /* 详情页的"‹ 歌单列表"行（**固定**在列表上方） */

void ao_pl_clear(void)
{
    s_pl_list = NULL;
    s_pl_hint = NULL;
    s_pl_back = NULL;
}

lv_obj_t *ao_pl_list(void)
{
    return s_pl_list;
}

static void pl_back_clicked(lv_event_t *e)
{
    (void)e;
    ao_pl_detail_exit();
    ao_pl_rebuild();
}

/* 详情页里的"重新载入这个歌单"：当前选中的歌单再读/抓一次（详见重建函数里的说明） */
static void pl_reload_clicked(lv_event_t *e)
{
    (void)e;
    const char *id = (ao_pl_sel() >= 0) ? online_playlist_id((size_t)ao_pl_sel()) : NULL;
    if (!id) {
        return;
    }
    ao_detail_mark_request(lv_tick_get());
    ao_pl_rebuild();               /* 先把转圈画上，再发请求 */
    ao_list_scroll_top(s_pl_list);
    ao_work_submit(OREQ_LOAD, id);
}

/*
 * 长按菜单的遮罩与"关闭"（完整定义在下面，这里先给声明：上面这个点击处理要用）
 */
static lv_obj_t *s_menu;
void ao_pl_menu_close(void);

/*
 * 打开歌单详情。**挂在 LV_EVENT_SHORT_CLICKED 上，不是 CLICKED** —— 这一点是整个
 * 长按菜单能用的前提，别再改回 CLICKED：
 *
 *   LVGL 的松手处理（src/indev/lv_indev.c 的 indev_proc_release）是
 *       if(indev->pointer.pressed) {
 *           if(indev->long_pr_sent == 0) indev_proc_short_click(indev);  ← 只有"短按"才进
 *           send_event(LV_EVENT_CLICKED, indev_act);                     ← 无条件发！
 *       }
 *   而 LV_EVENT_SHORT_CLICKED 是在 indev_proc_short_click() **里面**发的（1603 行），
 *   所以它**永远不会跟在一次长按后面**。
 *
 * 用 CLICKED 的话：长按弹菜单的同时还会补一个 CLICKED 落到这一行 → 菜单底下又被
 * 换成歌单详情；曾经用"时间标志"去挡，但那依赖"松手事件"和"异步建菜单"的先后顺序，
 * 实测挡不住（用户报：长按既弹菜单又进歌单页、界面卡死）。
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
    ao_pl_detail_enter(idx);       /* sel=idx, detail=true, page=0（新歌单从第 1 页看起） */
    if (s_pl_hint) {
        lv_label_set_text(s_pl_hint, "载入中…");
    }
    ao_detail_mark_request(lv_tick_get());   /* 超时兜底计时（见 ao_detail_wait_rows） */
    ao_pl_rebuild();               /* 先画出"载入中"的空壳 */
    ao_list_scroll_top(s_pl_list); /* 新详情从第一首开始显示 */
    ao_work_submit(OREQ_LOAD, id);
}

/* 长按歌单行 → 重命名 / 更新 / 删除 菜单（s_menu 的声明在上面，点击处理要用） */
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
        /* 渐出（下滑 + 缩小 + 淡出）：动画结束由 ui_popup_out 自己删 */
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
        ao_pl_detail_drop();
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
 *
 * ★ 为什么不能在长按回调里直接建（这是"菜单点不动、还点到下层布局"的根因）：
 *   长按那一刻手指还按在屏幕上，而 LVGL 在按压期间会**不断重新命中手指下的对象**
 *   （lv_indev.c 的 pointer_search_obj，注释写着 "Without `LV_OBJ_FLAG_PRESS_LOCK`
 *   new widget can be found while pressing"）。歌单行没有 PRESS_LOCK，所以
 *   全屏遮罩一建出来就顶在手指底下 → 下一轮 act_obj 变成遮罩；
 *   而 indev_proc_release 里**长按之后照样补发 CLICKED**（它只跳过 short_click）
 *   → 松手那个 CLICKED 落在遮罩上 → "点空白关掉"把刚弹出的菜单自己删掉，
 *   之后所有点击自然都落到下层页面（= "点到了底部的布局"）。
 *   顺带还有个更危险的：万一手指正压在某个按钮上，松手会当场触发它（最坏是删歌单）。
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
    /*
     * ★ 防泄漏：菜单已经在了就直接返回。
     * 万一有两个"建菜单"的请求挤在一起（长按事件重复/异步重入），第二块全屏遮罩
     * 会把 s_menu 覆盖掉 —— 旧那块没人删、还盖在最上面吃掉所有点击，
     * 现象就是"界面卡死怎么点都没反应"。宁可少弹一次，也不能留下孤儿遮罩。
     */
    if (s_menu) {
        return;
    }
    s_menu_tick = lv_tick_get();

    /* 碧蓝档案风格白卡片 + 三条选项行（左侧色条区分动作，删除用红） */
    const char *nm = online_playlist_name((size_t)idx);
    s_menu_dlg = ui_ba_open((nm && nm[0]) ? nm : "歌单", 56, LV_ALIGN_CENTER, 0);
    s_menu = s_menu_dlg.scrim;
    lv_obj_add_event_cb(s_menu, pl_menu_deleted, LV_EVENT_DELETE, NULL);
    /* 点遮罩空白处关掉（带 MENU_GRACE_MS 保险） */
    lv_obj_add_event_cb(s_menu, pl_menu_scrim_clicked, LV_EVENT_CLICKED, NULL);
    /* 点卡片自己的空白处不该关掉菜单 */
    lv_obj_add_event_cb(s_menu_dlg.card, pl_menu_panel_clicked, LV_EVENT_CLICKED, NULL);

    ui_ba_hint(s_menu_dlg.body, "选择要对这个歌单做的操作");
    ui_ba_opt_row(s_menu_dlg.body, "重命名歌单", BA_BLUE, pl_menu_btn, (void *)(intptr_t)0);
    ui_ba_opt_row(s_menu_dlg.body, "更新（重新抓取）", 0x3E9E86, pl_menu_btn, (void *)(intptr_t)1);
    ui_ba_opt_row(s_menu_dlg.body, "删除歌单", BA_DANGER, pl_menu_btn, (void *)(intptr_t)2);
}

void ao_pl_rebuild(void)
{
    if (!s_pl_list) {
        return;
    }
    lv_obj_clean(s_pl_list);
    /* 返回行只在详情页出现（它是页容器的固定行，不是列表行，见 ao_make_back_label） */
    if (s_pl_back) {
        lv_obj_add_flag(s_pl_back, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 详情：某个歌单的歌曲列表 ---- */
    if (ao_pl_detail()) {
        if (s_pl_back) {
            lv_obj_remove_flag(s_pl_back, LV_OBJ_FLAG_HIDDEN);
        }
        const int sel = ao_pl_sel();
        const char *id = (sel >= 0) ? online_playlist_id((size_t)sel) : NULL;
        const char *nm = (sel >= 0) ? online_playlist_name((size_t)sel) : NULL;
        /*
         * ★ 关键：只有"内存里这份 s_songs 确实属于**当前这个歌单**"才画它。
         *   原先只判 `s_owner == OWNER_PL && s_song_n > 0` —— 点进第二个歌单时
         *   s_owner 还是 OWNER_PL、s_song_n 也 > 0（装的是**上一个歌单**的歌），
         *   于是列表先把上一个歌单的歌顶上来，用户看到、点到的全是它
         *   （"播放列表的歌曲我挨个点开了怎么都是同样的"就是这么来的）。
         *   现在对不上就画转圈：加载成功（ao_queue_install 把 s_cur_pl_id 换成这个
         *   歌单）才会显示歌单内容 —— 即"点进去先转圈，加载好了再显示歌单"。
         */
        const bool own = ao_detail_owns_songs();
        const size_t song_n = ao_queue_count();
        if (s_pl_hint) {
            char buf[96];
            const char *title = (nm && nm[0]) ? nm : (id ? id : "歌单");
            if (own) {
                /* meting 歌单是一次抓齐的（落在 SD 上的二进制），所以只有"分页显示" */
                snprintf(buf, sizeof(buf), "%s · 共 %u 首 · 第 %d/%d 页", title,
                         (unsigned)song_n, ao_pl_page() + 1, ao_page_count(song_n));
            } else {
                snprintf(buf, sizeof(buf), "%s", title);
            }
            lv_label_set_text(s_pl_hint, buf);
        }
        /* 返回行是页容器上的固定行（s_pl_back，见 ao_make_back_label），
         * **不能**再往 s_pl_list 里塞一个 —— 那样会跟着歌曲列表滚走 */

        if (own) {
            /* ★ 分页显示：一页最多 100 行（600 首就是 3000+ 个 LVGL 对象，
             *   全建出来滚动会明显掉帧，同本地列表 §12.2 的做法） */
            const int page = ao_page_clamp(ao_pl_page(), song_n);
            ao_pl_page_set(page);
            const size_t first = ao_page_first(page);
            size_t rows = (song_n > first) ? (song_n - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            ao_fill_song_rows_range(s_pl_list, first, rows);
            ao_page_rows(s_pl_list, PG_PL, page, ao_page_count(song_n));
        } else if (ao_queue_owner() == OWNER_SRCH || ao_queue_owner() == OWNER_ACCT) {
            /* s_songs 里是别人（搜索/账号歌单）的结果：给一个"重新载入"按钮，
             * 点了才发请求（自动重发容易和结果回调互相触发） */
            lv_obj_t *reload = ao_make_row(s_pl_list, LV_SYMBOL_REFRESH, 0x3E9E86,
                                           "重新载入这个歌单", NULL);
            lv_obj_add_event_cb(reload, pl_reload_clicked, LV_EVENT_CLICKED, NULL);
        } else {
            ao_detail_wait_rows(s_pl_list, pl_reload_clicked);   /* 转圈（超时给重试） */
        }
        return;
    }

    /* ---- 歌单列表 ---- */
    const size_t n = online_playlist_count();
    if (s_pl_hint) {
        char buf[64];
        snprintf(buf, sizeof(buf), "共 %u 个歌单（长按可重命名/更新/删除）", (unsigned)n);
        lv_label_set_text(s_pl_hint, buf);
    }
    /* 顶部先放"网易云账号"状态行（扫码登录 / 已登录+退出登录） */
    ao_acct_row_build(s_pl_list);
    if (n == 0) {
        lv_obj_t *h = lv_label_create(s_pl_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_text(h, online_has_network()
                                ? "还没有歌单：点右上角「+」添加网易云歌单 ID\n（也可以切到「歌曲搜索」直接找歌）"
                                : "Wi-Fi 未连接：进「设置」连上网络再添加歌单");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *id = online_playlist_id(i);
        const char *nm = online_playlist_name(i);
        lv_obj_t *row = ao_make_row(s_pl_list, LV_SYMBOL_LIST, 0x5B6FB5,
                                    (nm && nm[0]) ? nm : (id ? id : "歌单"),
                                    (nm && nm[0]) ? id : NULL);
        lv_obj_add_event_cb(row, pl_row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        /* 长按 = 重命名/更新/删除（需求："长按歌单项可重命名、更新、删除歌单"） */
        lv_obj_add_event_cb(row, pl_row_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

lv_obj_t *ao_pl_build(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page, 0, 0);

    /* 同上：返回行固定在列表上方，不跟着歌曲列表滚 */
    s_pl_back = ao_make_back_label(page, LV_SYMBOL_LEFT " 歌单列表", pl_back_clicked);

    s_pl_hint = lv_label_create(page);
    lv_obj_set_style_text_font(s_pl_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pl_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_pl_hint, LV_PCT(100));
    lv_label_set_long_mode(s_pl_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_pl_hint, "");

    s_pl_list = lv_obj_create(page);
    lv_obj_remove_style_all(s_pl_list);
    lv_obj_set_width(s_pl_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_pl_list, 1);
    lv_obj_set_style_pad_right(s_pl_list, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_pl_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_pl_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_pl_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_pl_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_pl_list, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_pl_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_pl_list, UI_PX_W(6), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_pl_list, UI_PX_H(3), LV_PART_SCROLLBAR);
    return page;
}
