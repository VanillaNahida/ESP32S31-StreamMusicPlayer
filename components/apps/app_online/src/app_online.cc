/*
 * app_online.cc —— 网易云音乐 / QQ 音乐 APP 的**外壳**
 *
 * 需求对应：
 *   - 添加歌单 ID → 抓取歌单 → **二进制存到 SD**（都在 online_service 里）
 *   - 播放歌单里的在线歌曲：默认**流式播放**，封面/歌词缓存在 **PSRAM**
 *   - 中文拼音输入法搜索
 *   - 歌单/搜索的结果都交给 app_music 当"当前队列"
 *   - 界面照 assets/UIDesign/在线音乐主界面.svg：顶栏 + 四个页签（我的歌单 /
 *     我添加的歌单 / 搜索 / 账号）+ 渐变背景 + 右上角径向光晕
 *
 * 线程模型（照 app_music 的老规矩）：
 *   LVGL 任务只做 UI；HTTP 全在 online_work 任务里做，结果经 200 ms 轮询搬到界面。
 *
 * ★ 版面是**绝对坐标版**（设计稿 800x480）：screen 不留 padding，标题/加号/页签/
 *   宿主全部按设计稿的像素位置摆；换方向编译时靠 UI_PX_W/UI_PX_H 按比例缩放。
 */
#include "app_online.h"

#include "ui_app_registry.h"
#include "ui_ba.h"         /* ui_ba_confirm_close()：二次确认框挂在 layer_top，退页面要收 */
#include "ui_core.h"
#include "ui_icons.h"      /* &netease_logo（桌面图标位图，由 PNG 生成） */
#include "ui_metrics.h"

#include "online_service.h"

#include "ao_common.h"
#include "ao_pl_cover.h"
#include "ao_provider.h"
#include "ao_model.h"
#include "ao_overlay.h"
#include "ao_pages.h"
#include "ao_result.h"
#include "ao_selftest.h"
#include "ao_style.h"
#include "ao_work.h"

static const char *TAG = AO_TAG;

#define D_X(n) UI_PX_W(n)
#define D_Y(n) UI_PX_H(n)

/* ---------------------------------------------------------------- 页面 */

static void online_on_delete(lv_event_t *e)
{
    (void)e;
    ao_work_set_page_alive(false);   /* 工作线程据此判断"这份结果还有没有人要" */
    ao_result_poll_stop();
    ao_modal_close();
    ao_collect_close();   /* 收藏到歌单的浮层同样挂在 layer_top 上，退页面要收 */
    ao_pl_menu_close();
    ao_qr_close();       /* 二维码浮层挂在 lv_layer_top 上，不随页面销毁，必须显式收掉 */
    /* 二次确认框同样挂在 lv_layer_top 上：退页面时它必须一起消失 */
    ui_ba_confirm_close();
    /* 拼音键盘和自带键盘挂在 lv_layer_top 上，**不归本页所有**：只收不删 */
    ao_ime_hide();

    /* ★ 返回键的接管必须跟着清掉：否则退出本 APP 之后，
     *   别的页面的返回键还会先问一个已经失效的 handler。
     * ⚠ 这里**不要**再调 ao_pages_set_detail_mode()：screen 的 DELETE 事件是在
     *   子对象已经销毁之后才派发的（LVGL 先删子对象），那时 tabs/title 都是野指针。 */
    ui_back_button_set_handler(NULL);

    /* 歌单详情的封面：页面没了就整块还给 PSRAM */
    ao_pl_cover_release_all();

    ao_pages_clear();    /* 骨架：页面根 / 四个 tab / 宿主 / 四个页容器 */
    ao_acct_clear();     /* 各页控件随 screen 一起销毁，必须清指针 */
    ao_pl_clear();
    ao_srch_clear();
    ao_account_clear();
    ao_detail_close_all();

    /* 结果槽位里可能还压着一份"工作线程刚发布、本页定时器还没搬走"的结果 */
    ao_work_discard_pending();
    /* 被替换下来的旧队列：交给播放器托管 */
    ao_queue_drop_stale();
    ao_session_end();
}

/*
 * 全局返回钮的接管（见 ui_core.h 的 ui_back_button_set_handler）：
 * 歌单详情是"同一页里换内容"，没有压路由栈 —— 这时返回键必须先回歌单列表，
 * 而不是把整个在线音乐 APP 退掉。
 */
static bool online_back_handler(void)
{
    if (!ao_pages_in_detail()) {
        return false;               /* 列表态：交回默认行为（退栈） */
    }
    const int page = ao_pages_current();
    if (page == 0) {
        ao_acct_leave_detail();
    } else if (page == 1) {
        ao_pl_leave_detail();
    }
    return true;
}

static lv_obj_t *page_online(void)
{
    ao_work_set_page_alive(true);   /* 先置位：后面任何请求发布结果时都算"有人要" */
    ao_session_begin(lv_tick_get());

    lv_obj_t *scr = lv_obj_create(NULL);
    /* 设计稿的背景：斜向三段渐变（#132536 → #0E1722 → #0A0F16） */
    ao_style_screen_bg(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);   /* 绝对坐标版式 */
    ao_pages_set_root(scr);

    /* 右上角径向光晕（设计稿 cx=80% cy=15% r=55% 25% 不透明）：先建 = 最底层 */
    ao_style_glow(scr, 80, 15, 64);

    /* 标题（返回键由 ui_core 统一叠在顶层；标题从 UI_HEADER_TEXT_X 起排，给按钮让位） */
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(title, ao_provider_title());
    /* screen 的 pad 是 0（绝对坐标版式），所以这里要自己补上标题栏的顶边
     * UI_TITLEBAR_Y，否则标题会跑到状态栏里（见 ui_metrics.h 的版面说明） */
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, UI_HEADER_TEXT_X,
                 UI_TITLEBAR_Y + ui_titlebar_text_dy());
    ao_pages_set_chrome(title);

    /*
     * 右上角的「+」（添加歌单 ID 浮层）已按需求**移除**（2026-11 用户："去掉右上角的添加歌单
     * 按钮，现在它是没有什么用了"）——歌单都从「我的歌单」里按账号同步下来，这个入口只剩
     * 手工录 ID 一条用途。要加回来：在这里重建按钮 + 挂 ao_modal_open(OREQ_ADD)。
     */

    /*
     * 页签行：设计稿 (54,76)，四段文字 + 会跟着当前页走的 3 px 强调色下划线。
     * 页签排布由 ao_pages_build_tabs() 负责（文字宽随语言变，不能写死坐标）。
     */
    lv_obj_t *tabs = lv_obj_create(scr);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_pos(tabs, D_X(54), D_Y(76));
    lv_obj_set_size(tabs, D_X(692), D_Y(36));
    lv_obj_set_style_pad_top(tabs, D_Y(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(tabs, D_X(26), LV_PART_MAIN);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    ao_pages_build_tabs(tabs);

    /* ---- 四页的宿主（y=118 起，详情模式会自己上移到 88） ---- */
    lv_obj_t *host = ao_pages_build_host(scr);

    /* ================= 第 0 页：我的歌单 ================= */
    ao_pages_set_page(0, ao_acct_build(host));
    /* ================= 第 1 页：我添加的歌单 ================= */
    ao_pages_set_page(1, ao_pl_build(host));
    /* ================= 第 2 页：搜索 ================= */
    ao_pages_set_page(2, ao_srch_build(host));
    /* ================= 第 3 页：账号（新增） ================= */
    ao_pages_set_page(3, ao_account_build(host));

    /*
     * ★ 首次定位必须在**整屏跑过一次 layout 之后**：页宽取自 lv_obj_get_width(s_host)，
     *   而 s_host 的宽度是父级的百分比算出来的 —— 没 layout 时宽是 0，几页会叠一起。
     */
    lv_obj_update_layout(scr);
    ao_srch_reset_view();
    ao_page_show(0, false);
    ao_pl_rebuild();
    ao_srch_rebuild();
    ao_account_rebuild();

    lv_obj_add_event_cb(scr, online_on_delete, LV_EVENT_DELETE, NULL);
    ui_back_button_set_handler(online_back_handler);
    ao_result_poll_start();
    if (ao_is_qq()) {
        /* QQ 的"收藏到歌单"藏在长按里，第一次进来提示一次，否则没人发现 */
        ui_toast("长按歌曲可收藏到歌单", 2400);
    }
    ESP_LOGI(TAG, "%s page created (%u playlist(s))", ao_provider_title(),
             (unsigned)online_playlist_count());
    return scr;
}

/* ---------------------------------------------------------------- 注册 */

/*
 * 两个桌面入口、**同一份页面代码**：
 *   "online"  网易云音乐（order 20，保持原来的位置）
 *   "qqmusic" QQ 音乐（order 21，紧挨着网易云）
 */
static lv_obj_t *page_netease(void)
{
    ao_provider_set(AO_PROV_NETEASE);
    return page_online();
}

static lv_obj_t *page_qqmusic(void)
{
    ao_provider_set(AO_PROV_QQ);
    return page_online();
}

void app_online_register_apps(void)
{
    static const app_desc_t online = {
        "online", "网易云音乐", LV_SYMBOL_WIFI, 0x3E9E86,
        page_netease, NULL, NULL, 20, 0,
        &netease_logo,          /* 桌面图标用网易云的 PNG（gen_icon.py 烘成位图） */
    };
    ui_app_register(&online);

    static const app_desc_t qqmusic = {
        "qqmusic", "QQ音乐", LV_SYMBOL_AUDIO, 0x2BB673,
        page_qqmusic, NULL, NULL, 21, 0,
        &qqmusic_logo,          /* assets/icons/qqmusic.png 烘成的位图 */
    };
    ui_app_register(&qqmusic);

    /* 请求队列（深度 4）+ online_work 任务（栈 16 KB，优先级 3） */
    ao_work_init();
#if ONLINE_SELFTEST
    ao_selftest_start();
#endif
}
