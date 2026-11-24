/*
 * ao_page_account.cc —— 第 3 页「账号」（assets/UIDesign/账号页面.svg）
 *
 * 页面职责很窄：**管理登录状态**。
 *   已登录：账号信息卡（头像/昵称/状态胶囊/UID+歌单数）+ 切换账号 / 退出登录
 *   未登录：同一张卡显示"未登录" + 一个"扫码登录"主按钮
 *
 * 为什么单独一页而不是塞进「我的歌单」：歌单页只关心内容，登录/退出属于账号管理；
 * 重构指南也要求"新增一个单独的账号页面，用来管理用户登录等"。
 *
 * ⚠ 所有按钮回调都**只发请求**，绝不在回调里重建页面 —— 重建会把正在派发事件的
 *   按钮删掉（LVGL 里就是 use-after-free）。结果回来时由 ao_result 调
 *   ao_account_rebuild()，那时才是安全的重建时机。
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "online_service.h"
#include "ui_core.h"       /* ui_toast */
#include "ui_metrics.h"

#include "ao_account.h"
#include "ao_common.h"
#include "ao_overlay.h"
#include "ao_pages.h"
#include "ao_provider.h"
#include "ao_style.h"
#include "ao_work.h"

#define D_X(n) UI_PX_W(n)
#define D_Y(n) UI_PX_H(n)

/* 设计稿坐标（相对页容器；页容器顶边 = 屏幕 y=118 那一行之下） */
#define CARD_X      D_X(54)
#define CARD_Y      D_Y(16)
#define CARD_W      D_X(692)
#define CARD_H      D_Y(148)

#define AVATAR_SIDE D_Y(84)
#define AVATAR_X    D_X(32)
#define AVATAR_Y    D_Y(32)

#define NAME_X      D_X(142)
#define NAME_Y      D_Y(42)

#define BTN_Y       D_Y(188)          /* 屏幕 y=306 */
#define BTN_W       D_X(334)
#define BTN_H       D_Y(48)
#define BTN1_X      D_X(54)
#define BTN2_X      D_X(412)          /* 54 + 358 */
#define HINT_Y      D_Y(262)          /* 屏幕 y=380 */

static lv_obj_t *s_page;

static void on_login_clicked(lv_event_t *e)
{
    (void)e;
    ao_qr_open();                     /* 扫码浮层（与添加歌单页同一套） */
}

static void on_logout_clicked(lv_event_t *e)
{
    (void)e;
    ui_toast(ao_is_qq() ? "正在退出 QQ 音乐…" : "正在退出网易云…", 1200);
    ao_work_submit(OREQ_LOGOUT, NULL);   /* 结果回来才重建（见文件头） */
}

/* 头像：渐变圆 + 白色人形轮廓（设计稿的默认头像） */
static void build_avatar(lv_obj_t *card)
{
    lv_obj_t *av = lv_obj_create(card);
    lv_obj_remove_style_all(av);
    lv_obj_set_pos(av, AVATAR_X, AVATAR_Y);
    lv_obj_set_size(av, AVATAR_SIDE, AVATAR_SIDE);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(av, lv_color_hex(0xA0B664), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(av, lv_color_hex(0x5B8C5A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(av, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(av, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(av, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(av, 55, LV_PART_MAIN);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = lv_obj_create(av);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, D_Y(28), D_Y(28));
    lv_obj_set_pos(head, D_Y(26), D_Y(16));
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(head, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(head, 230, LV_PART_MAIN);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_obj_create(av);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, D_Y(52), D_Y(30));
    lv_obj_set_pos(body, D_Y(14), D_Y(50));
    lv_obj_set_style_radius(body, D_Y(15), LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, 230, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_card(lv_obj_t *page, bool logged_in)
{
    lv_obj_t *card = ao_style_card(page, CARD_X, CARD_Y, CARD_W, CARD_H, D_Y(16));
    build_avatar(card);

    const char *nick = ao_login_name();
    const char *fallback = ao_is_qq() ? "QQ 音乐账号" : "网易云账号";

    lv_obj_t *nm = lv_label_create(card);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(nm, D_X(480));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_label_set_text(nm, logged_in ? ((nick && nick[0]) ? nick : fallback) : "未登录");
    lv_obj_set_style_text_font(nm, logged_in ? ao_style_title_font(lv_label_get_text(nm))
                                             : UI_FONT_TEXT,
                               LV_PART_MAIN);
    lv_obj_set_pos(nm, NAME_X, NAME_Y);

    /* 状态胶囊：64x22，圆角 11（设计稿） */
    lv_obj_t *pill = lv_obj_create(card);
    lv_obj_remove_style_all(pill);
    lv_obj_set_pos(pill, NAME_X, D_Y(76));
    /* 设计稿是 64x22 / 11 px 字；本工程的汉字只有 16 px 位图字体，
     * "已登录"三个字就要 48 px，所以胶囊加宽到 82（视觉比例与设计稿一致） */
    lv_obj_set_size(pill, D_X(82), D_Y(24));
    lv_obj_set_style_radius(pill, D_Y(11), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pill, lv_color_hex(logged_in ? AO_C_ACCENT : 0xFFFFFF),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, logged_in ? 40 : 26, LV_PART_MAIN);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *dot = lv_obj_create(pill);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, D_Y(7), D_Y(7));
    lv_obj_set_pos(dot, D_X(10), D_Y(7));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(logged_in ? AO_C_ACCENT : 0xB8C2CC),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pl = lv_label_create(pill);
    /* ⚠ 必须是中文字体：montserrat 里没有汉字，用它渲染"已登录"就是三个方框 */
    lv_obj_set_style_text_font(pl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(pl, lv_color_hex(logged_in ? AO_C_ACCENT : 0xB8C2CC),
                                LV_PART_MAIN);
    lv_label_set_text(pl, logged_in ? "已登录" : "未登录");
    lv_obj_set_pos(pl, D_X(20), D_Y(1));

    /* UID · 歌单数（设计稿 13 px 白 50%） */
    char sub[96];
    if (logged_in) {
        const char *uid = ao_is_qq() ? "" : online_netease_uid();
        if (uid && uid[0]) {
            snprintf(sub, sizeof(sub), "UID: %s · 歌单 %u 个", uid,
                     (unsigned)ao_acct_pls_count());
        } else {
            snprintf(sub, sizeof(sub), "歌单 %u 个", (unsigned)ao_acct_pls_count());
        }
    } else {
        snprintf(sub, sizeof(sub), "扫码后同步歌单 / 我喜欢 / 每日推荐");
    }
    lv_obj_t *uid = lv_label_create(card);
    lv_obj_set_style_text_font(uid, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(uid, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(uid, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_width(uid, D_X(420));
    lv_label_set_long_mode(uid, LV_LABEL_LONG_DOT);
    lv_label_set_text(uid, sub);
    lv_obj_set_pos(uid, D_X(NAME_X + 92), D_Y(78));
}

static void build_actions(lv_obj_t *page, bool logged_in)
{
    if (logged_in) {
        /* 次操作：切换账号 / 重新登录（白 8% 胶囊） */
        lv_obj_t *b1 = ao_style_pill(page, BTN_W, BTN_H, D_Y(14), "切换账号 / 重新登录",
                                     0xFFFFFF, 22, 0, LV_OPA_TRANSP, 0xFFFFFF);
        lv_obj_set_pos(b1, BTN1_X, BTN_Y);
        lv_obj_add_event_cb(b1, on_login_clicked, LV_EVENT_CLICKED, NULL);

        /* 警示操作：退出登录（珊瑚红 18% 底 + 45% 描边） */
        lv_obj_t *b2 = ao_style_pill(page, BTN_W, BTN_H, D_Y(14), "退出登录",
                                     AO_C_DANGER, 46, AO_C_DANGER, 115, 0xFF7A82);
        lv_obj_set_pos(b2, BTN2_X, BTN_Y);
        lv_obj_add_event_cb(b2, on_logout_clicked, LV_EVENT_CLICKED, NULL);
    } else {
        /* 未登录：一个主操作按钮（强调色实心，居中） */
        lv_obj_t *b = ao_style_pill(page, D_X(280), BTN_H, D_Y(14),
                                    ao_is_qq() ? "扫码登录 QQ 音乐" : "扫码登录网易云",
                                    AO_C_ACCENT, LV_OPA_COVER, 0, LV_OPA_TRANSP, 0x0B131D);
        lv_obj_set_pos(b, (UI_W - D_X(280)) / 2, BTN_Y + D_Y(12));
        lv_obj_add_event_cb(b, on_login_clicked, LV_EVENT_CLICKED, NULL);
    }

    /* 底部提示（设计稿 12 px 白 38%，整行居中） */
    lv_obj_t *h = lv_label_create(page);
    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(h, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(h, D_X(692));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_label_set_text(h, logged_in
                             ? "未登录状态下点击「登录」可通过扫码或 Cookie 同步个人歌单"
                             : "扫码登录后，这里会显示你的昵称、UID 与歌单数量");
    lv_obj_set_pos(h, CARD_X, HINT_Y);
}

void ao_account_rebuild(void)
{
    if (!s_page) {
        return;
    }
    lv_obj_clean(s_page);
    /*
     * "当前判定未登录"时再读一次 SD 上的 cookie：开机后 SD 可能晚挂上，
     * 那一次读不到并不代表没登录过（与 ao_page_acct 的做法一致）。
     */
    if (!ao_logged_in()) {
        ao_login_state_reload();
    }
    const bool in = ao_logged_in();
    build_card(s_page, in);
    build_actions(s_page, in);
}

lv_obj_t *ao_account_build(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page, 0, 0);
    s_page = page;
    ao_account_rebuild();
    return page;
}

void ao_account_clear(void)
{
    s_page = NULL;
}
