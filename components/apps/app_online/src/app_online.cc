/*
 * app_online.cc —— 网易云音乐（网易云歌单 / 搜索 / 播放）APP 的**外壳**
 *
 * 需求对应：
 *   - 添加网易云歌单 ID → 抓取歌单 → **二进制存到 SD**（都在 online_service 里）
 *   - 播放歌单里的在线歌曲：默认**流式播放**（音频边下边播，不落 SD），
 *     下载失败时回退"先下载到 SD 再播"；封面/歌词一律缓存在 **PSRAM**，
 *     不写 SD 卡（SD 读写是之前切歌卡顿的根因）
 *   - 中文拼音输入法搜索：搜索浮层用 lvgl_pinyin_ime 键盘，
 *     关键词走服务端的 type=search（关键词放 id=，内部做百分号编码，
 *     支持中文），没网时退化成扫本地已保存歌单
 *   - 歌单/搜索的结果都交给 app_music 当"当前队列"，上下曲一起工作
 *
 * 线程模型（照 app_music 的老规矩）：
 *   LVGL 任务只做 UI；HTTP（抓歌单/搜索/下载）全在 online_work 任务里做，
 *   结果写进一组静态槽位 + 一个"完成"标志，由 200 ms 的 LVGL 定时器搬到界面上。
 *
 * ★ 2026-10 拆分（见 docs/refactor/04_app_online.md）：本文件**只剩三件事** ——
 *   注册桌面 App、组装页面控件树（页面外壳）、页面销毁时按顺序收尾。
 *   其余按职责分到：ao_work（工作线程/协议）、ao_model（纯逻辑）、ao_pages（骨架/
 *   切页）、ao_page_acct/pl/srch（三页）、ao_qr_login（二维码）、ao_modal（浮层/
 *   键盘）、ao_widgets（通用控件）、ao_result（结果分发/轮询）、ao_selftest（自检）。
 */
#include "app_online.h"

#include "ui_app_registry.h"
#include "ui_ba.h"         /* ui_ba_confirm_close()：二次确认框挂在 layer_top，退页面要收 */
#include "ui_core.h"
#include "ui_icons.h"      /* &netease_logo（桌面图标位图，由 PNG 生成） */
#include "ui_metrics.h"

#include "online_service.h"

#include "ao_common.h"
#include "ao_model.h"
#include "ao_overlay.h"
#include "ao_pages.h"
#include "ao_result.h"
#include "ao_selftest.h"
#include "ao_work.h"

static const char *TAG = AO_TAG;

/* ---------------------------------------------------------------- 页面 */

static void online_on_delete(lv_event_t *e)
{
    (void)e;
    ao_work_set_page_alive(false);   /* 工作线程据此判断"这份结果还有没有人要" */
    ao_result_poll_stop();
    ao_modal_close();
    ao_pl_menu_close();
    ao_qr_close();       /* 二维码浮层挂在 lv_layer_top 上，不随页面销毁，必须显式收掉 */
    /* 二次确认框同样挂在 lv_layer_top 上：退页面时它必须一起消失，
     * 否则会跟着用户到别的页面去（而且回调还指着本页的状态） */
    ui_ba_confirm_close();
    /*
     * ★ 拼音键盘和自带键盘挂在 lv_layer_top 上，**不归本页所有**（只建一次、
     * 全局复用），所以这里只收起、不能删 —— 删了下次搜索就得重建（还要重新
     * 加载词库，卡一秒）。
     */
    ao_ime_hide();

    ao_pages_clear();    /* 骨架：页面根 / 三个 tab / 宿主 / 三个页容器 */
    ao_acct_clear();     /* 第 0 页控件（列表/提示/返回行）随 screen 一起销毁，
                          * 必须清指针，否则残留的悬垂指针一旦被 rebuild 碰到就是崩溃 */
    ao_pl_clear();
    ao_srch_clear();     /* 内含 s_srch_has_res = false */
    ao_detail_close_all();

    /* 结果槽位里可能还压着一份"工作线程刚发布、本页定时器还没搬走"的结果：一次清干净 */
    ao_work_discard_pending();
    /* 被替换下来的旧队列：交给播放器托管（它换队列时会自己放），别留在这儿 */
    ao_queue_drop_stale();
    /* 请求序号/重试节流 + 分页/懒加载状态复位：下次进来重新拉、从第 1 页开始 */
    ao_session_end();
}

/* 右上角那个「+」：加歌单浮层（搜索已经是下面的 tab，删除/更新在歌单行的长按菜单里） */
static void on_add_clicked(lv_event_t *e)
{
    (void)e;
    ao_modal_open(OREQ_ADD);
}

static lv_obj_t *page_online(void)
{
    ao_work_set_page_alive(true);   /* 先置位：后面任何请求发布结果时都算"有人要" */
    ao_session_begin(lv_tick_get());

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, UI_PX_H(6), LV_PART_MAIN);
    /* 纵向 flex：标题行 / tab 行 / 三页宿主（宿主 flex_grow 吃掉剩余高度）。
     * 为什么不用"绝对定位 + 给宿主留 pad_top"：那样宿主容器会整屏盖住标题行，
     * LVGL 的容器默认 clickable，会把 tab 的点击全吞掉。 */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    ao_pages_set_root(scr);

    /* 标题行（返回键由 ui_router/ui_core 统一叠在顶层，不再每页自己画） */
    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), UI_PX_H(40));
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(title, "网易云音乐");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, UI_HEADER_TEXT_X, 0);

    /* 右上角只剩"添加歌单"：搜索已经是下面的 tab，删除/更新在歌单行的长按菜单里 */
    lv_obj_t *add = lv_button_create(head);
    lv_obj_set_size(add, UI_PX_H(36), UI_PX_H(36));
    lv_obj_set_style_radius(add, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(add, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_add_event_cb(add, on_add_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(add);
    lv_obj_set_style_text_font(al, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(al, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_center(al);
    lv_obj_align(add, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- 顶部三个 tab：文字即标题，点一下切页（页签次序在 ao_pages.cc 里保证）---- */
    lv_obj_t *tabs = lv_obj_create(scr);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_PCT(100), UI_PX_H(42));
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, UI_PX_W(26), LV_PART_MAIN);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    ao_pages_build_tabs(tabs);

    /* ---- 三页的宿主：吃掉剩余高度，并把三页裁剪在可视范围内 ---- */
    lv_obj_t *host = ao_pages_build_host(scr);

    /* ================= 第 0 页：我的歌单（网易云账号） ================= */
    ao_pages_set_page(0, ao_acct_build(host));

    /* ================= 第 1 页：我添加的歌单 ================= */
    ao_pages_set_page(1, ao_pl_build(host));

    /* ================= 第 2 页：歌曲搜索 ================= */
    ao_pages_set_page(2, ao_srch_build(host));

    /*
     * ★ 首次定位必须在**整屏跑过一次 layout 之后**：页宽取自 lv_obj_get_width(s_host)，
     * 而 s_host 的宽度是父级 flex（flex_grow）算出来的 —— 只 update 它自己拿不到。
     * 没 layout 时宽是 0，三页会叠在一起。
     */
    lv_obj_update_layout(scr);
    ao_srch_reset_view();
    ao_page_show(0, false);
    ao_pl_rebuild();
    ao_srch_rebuild();

    lv_obj_add_event_cb(scr, online_on_delete, LV_EVENT_DELETE, NULL);
    ao_result_poll_start();
    ESP_LOGI(TAG, "online page created (%u playlist(s))", (unsigned)online_playlist_count());
    return scr;
}

/* ---------------------------------------------------------------- 注册 */

void app_online_register_apps(void)
{
    static const app_desc_t online = {
        "online", "网易云音乐", LV_SYMBOL_WIFI, 0x3E9E86,
        page_online, NULL, NULL, 20, 0,
        &netease_logo,          /* 桌面图标用网易云的 PNG（gen_icon.py 烘成位图） */
    };
    ui_app_register(&online);

    /* 请求队列（深度 4）+ online_work 任务（栈 16 KB，优先级 3） */
    ao_work_init();
#if ONLINE_SELFTEST
    ao_selftest_start();
#endif
}
