/*
 * ao_overlay.cc —— **所有浮层 + 两个键盘 + 账号状态行**（原来分在 ao_qr_login / ao_modal）
 *
 * 需求：
 *   - 用网易云音乐 App 扫码，拿到账号歌单与每日推荐（二维码浮层 + 账号状态行）；
 *   - 添加/重命名歌单、拼音搜索（浮层 + lvgl_pinyin_ime 键盘 + LVGL 自带键盘）。
 *
 * ★★ 六条不能动的纪律：
 *   1) **浮层一律从点击回调里打开**（不是在按压期间建）：按压期间建的遮罩会被同一轮
 *      re-hit-test 命中，松手那一下无条件发的 CLICKED 会把刚弹出的浮层自己关掉
 *      —— AGENTS §14.6 记的坑，别在新弹层上重犯。
 *   2) `s_qr_px`（媒体像素）与 `s_qr_dsc`（图片描述符）**只在 ao_qr_close() 释放**。
 *   3) 拼音键盘/自带键盘挂在 **lv_layer_top()** 上、**不归任何浮层所有**：只建一次、
 *      全局复用。销毁页面时只 ao_ime_hide()，绝不 delete。
 *   4) 建拼音键盘前必须先把容器塞进一个**绝对像素宽度**并 `lv_obj_update_layout()`：
 *      组件的按键尺寸是 create() 时按父对象宽度算一次的，容器还没跑 layout 时宽度是 0
 *      → 组件退化成"按 320 px 算"，键位只有 ~23 px（"键盘太小、不好按"）。
 *   5) 每次显示键盘都要 `lv_obj_move_foreground()` —— 每次开浮层都会新建一个 scrim
 *      （也是 layer_top 的子对象、排在后面），不提到最前就会被遮罩压住（看得到点不动）。
 *   6) 二维码状态机（800/801/802/803 怎么走、登录成功后复位什么）在 ao_result.cc，
 *      本文件只负责"画"。
 */
#include "ao_overlay.h"

#include <stdlib.h>

#include "esp_log.h"

#include "media_meta.h"
#include "online_service.h"
#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "voice_service.h"       /* 语音输入宿主：WS + 麦克风接上这把键盘 */

#include "ao_account.h"
#include "ao_model.h"
#include "ao_pages.h"
#include "ao_work.h"

static const char *TAG = AO_TAG;

#define QR_POLL_MS      1500           /* 轮询间隔：服务端语义是"扫码/确认"事件，1.5 s 够灵敏 */
#define QR_IMG_MAX_DIM  240            /* 二维码显示尺寸（要能被相机清晰识别，别缩太小） */

static lv_obj_t     *s_qr_scrim;       /* 全屏遮罩（浮层根） */
static ui_ba_dialog_t s_qr_dlg;        /* BA 风格对话框句柄（与 s_qr_scrim 同生共死） */
static lv_obj_t     *s_qr_img;         /* 二维码图片 */
static lv_image_dsc_t *s_qr_dsc;       /* 图片描述符（堆上，随浮层释放） */
static uint8_t      *s_qr_px;          /* 解码后的 RGB565（media_meta_cover_free 释放） */
static lv_obj_t     *s_qr_status;      /* 状态行：等待扫码 / 已扫码待确认 / 已失效… */
static lv_timer_t   *s_qr_timer;       /* 轮询定时器 */
static char          s_qr_key[64];     /* 当前 unikey */

void ao_qr_set_status(const char *txt)
{
    if (s_qr_status) {
        lv_label_set_text(s_qr_status, txt ? txt : "");
    }
}

void ao_qr_stop_poll(void)
{
    if (s_qr_timer) {
        lv_timer_delete(s_qr_timer);
        s_qr_timer = NULL;
    }
}

/* 二维码 PNG → RGB565 → 贴到浮层（复用 media_meta 的解码器，呼叫方传入的 PNG 由本函数释放） */
static void qr_show_png(uint8_t *png, size_t len)
{
    uint8_t *px = NULL;
    int w = 0;
    int h = 0;
    const esp_err_t err = media_meta_image_mem_rgb565(png, len, QR_IMG_MAX_DIM, &px, &w, &h);
    free(png);
    if (err != ESP_OK || !px) {
        ESP_LOGW(TAG, "qr png decode failed: %s", esp_err_to_name(err));
        ao_qr_set_status("二维码解析失败，点「刷新」重试");
        return;
    }

    if (s_qr_px) {
        media_meta_cover_free(s_qr_px);
        s_qr_px = NULL;
    }
    if (s_qr_dsc) {
        free(s_qr_dsc);
        s_qr_dsc = NULL;
    }
    lv_image_dsc_t *dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
    if (!dsc) {
        media_meta_cover_free(px);
        return;
    }
    s_qr_px = px;
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = (uint32_t)w;
    dsc->header.h      = (uint32_t)h;
    dsc->header.stride = (uint32_t)w * 2u;
    dsc->data_size     = (uint32_t)w * 2u * (uint32_t)h;
    dsc->data          = px;
    s_qr_dsc = dsc;
    if (s_qr_img) {
        lv_image_set_src(s_qr_img, dsc);
    }
    ESP_LOGI(TAG, "qr image %dx%d", w, h);
}

static void qr_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_qr_key[0]) {
        ao_work_submit(OREQ_QR_POLL, s_qr_key);
    }
}

void ao_qr_close(void)
{
    ao_qr_stop_poll();
    s_qr_key[0] = '\0';
    /* 下滑 + 缩小 + 淡出（ui_ba），动画结束由 ui_popup_out 自己删掉整块浮层 */
    ui_ba_close(&s_qr_dlg);
    s_qr_scrim = NULL;
    s_qr_img = NULL;
    s_qr_status = NULL;
    if (s_qr_dsc) {
        free(s_qr_dsc);
        s_qr_dsc = NULL;
    }
    if (s_qr_px) {
        media_meta_cover_free(s_qr_px);
        s_qr_px = NULL;
    }
}

void ao_qr_show_key(const char *key, uint8_t *png, size_t png_len)
{
    snprintf(s_qr_key, sizeof(s_qr_key), "%s", key ? key : "");
    qr_show_png(png, png_len);        /* 内部 free(png) */
    ao_qr_set_status("请用网易云音乐 App 扫码");
    if (!s_qr_timer) {
        s_qr_timer = lv_timer_create(qr_poll_cb, QR_POLL_MS, NULL);
    }
}

static void qr_cancel_clicked(lv_event_t *e)
{
    (void)e;
    ao_qr_close();
}

static void qr_refresh_clicked(lv_event_t *e)
{
    (void)e;
    ao_qr_stop_poll();
    s_qr_key[0] = '\0';
    ao_qr_set_status("正在获取二维码…");
    ao_work_submit(OREQ_QR_KEY, NULL);
}

void ao_qr_open(void)
{
    if (s_qr_scrim) {
        return;                          /* 已经开着：绝不留第二块遮罩（孤儿遮罩会吃掉所有点击） */
    }

    /* 碧蓝档案风格白卡片：标题 + 黄色下划线 + 右上角 ✕（见 ui_ba.h） */
    s_qr_dlg = ui_ba_open("扫码登录", 46, LV_ALIGN_CENTER, 0);
    s_qr_scrim = s_qr_dlg.scrim;
    /* ✕ / 点空白 都走同一个关闭路径（要顺手停轮询、释放二维码内存） */
    ui_ba_on_close(&s_qr_dlg, [](void *user) { (void)user; ao_qr_close(); }, NULL);
    ui_ba_scrim_dismissible(&s_qr_dlg, true);

    ui_ba_hint(s_qr_dlg.body, "用网易云音乐 App 扫码，即可获取你的账号歌单与每日推荐");

    s_qr_img = lv_image_create(s_qr_dlg.body);
    lv_obj_set_size(s_qr_img, UI_PX_H(QR_IMG_MAX_DIM), UI_PX_H(QR_IMG_MAX_DIM));
    /* 图还没到时给个占位底色：面板高度不跳变，用户也知道"正在出图" */
    lv_obj_set_style_bg_color(s_qr_img, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_qr_img, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_qr_img, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_qr_img, UI_PX_H(6), LV_PART_MAIN);

    s_qr_status = lv_label_create(s_qr_dlg.body);
    lv_obj_set_style_text_font(s_qr_status, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_qr_status, lv_color_hex(BA_TEXT_BODY), LV_PART_MAIN);
    lv_label_set_text(s_qr_status, "正在获取二维码…");

    ui_ba_btn(s_qr_dlg.btn_row, "刷新二维码", false, qr_refresh_clicked, NULL);
    ui_ba_btn(s_qr_dlg.btn_row, "取消", true, qr_cancel_clicked, NULL);

    ao_work_submit(OREQ_QR_KEY, NULL);   /* 打开即取二维码（工作线程里两步合并） */
}

static void login_clicked(lv_event_t *e)
{
    (void)e;
    ao_qr_open();
}

/* 「退出登录」的二次确认：清 cookie 是不可逆的（要重新扫码），必须问一次 */
static void logout_confirmed(void *user)
{
    (void)user;
    ao_work_submit(OREQ_LOGOUT, NULL);
}

static void logout_clicked(lv_event_t *e)
{
    (void)e;
    ui_ba_confirm_ex("退 出 登 录", "确认退出网易云账号吗？\n退出后需要重新扫码登录。",
                     "退出", "取消", true, logout_confirmed, NULL);
}

/* 账号状态行：未登录 = 「扫码登录」入口；已登录 = 昵称 + 「退出登录」 */
void ao_acct_row_build(lv_obj_t *col)
{
    const bool in = online_netease_logged_in();
    /* 昵称先用 NVS 里存的那份（重启后不用等 /user/account 往返） */
    ao_login_name_sync();
    if (in) {
        ao_acct_nick_kick_if_needed(lv_tick_get());  /* 还没有昵称就取一次；失败会节流重试 */
    }

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const char *name = ao_login_name();
    lv_obj_t *txt = lv_label_create(row);
    lv_obj_set_style_text_font(txt, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, in ? UI_COLOR_ACCENT : UI_COLOR_FG, LV_PART_MAIN);
    if (in) {
        lv_label_set_text_fmt(txt, "已登录：%.24s", name[0] ? name : "网易云账号");
    } else {
        lv_label_set_text(txt, "未登录网易云");
    }
    lv_obj_align(txt, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *btn = lv_label_create(row);
    lv_obj_set_style_text_font(btn, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(btn, in ? "退出登录" : "扫码登录");
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, UI_PX_H(12));
    lv_obj_add_event_cb(btn, in ? logout_clicked : login_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* ================================================================ 浮层与键盘 */

/*
 * 拼音键盘的目标高度（fit height，见 lvgl_pinyin_ime 的 lv_pinyin_ime_set_fit_height）。
 * 组件按"候选条 + 可见行数"反推出行高，让整块键盘正好占半屏 —— 按键会随父容器宽度
 * 自适应缩放。
 */
#define IME_FIT_HEIGHT    UI_PX_H(240)   /* UI_H / 2：键盘恰好半屏 */

static lv_obj_t *s_modal;        /* 当前浮层（BA 对话框的 scrim） */
static ui_ba_dialog_t s_modal_dlg;   /* BA 对话框句柄（与 s_modal 同生共死） */
static lv_obj_t *s_modal_ta;     /* 浮层里的输入框 */
static lv_obj_t *s_ime;          /* 拼音键盘（懒建，全局复用） */
static lv_obj_t *s_ime_box;      /* 拼音键盘的容器（宽度决定按键大小） */
static lv_obj_t *s_kb;           /* 歌单 ID 用的普通键盘（LVGL 自带，带数字） */
static int       s_modal_kind;   /* OREQ_ADD / MODAL_RENAME */

static void modal_submit(lv_event_t *e);

void ao_modal_close(void)
{
    /*
     * ★★ 2026-10 真机崩溃根因修复：**必须在浮层真的被删掉之前把键盘解绑**。
     *
     * LVGL 的 `lv_keyboard_set_textarea()` 只存指针（`keyboard->ta = ta`），
     * **不**注册 textarea 的 DELETE 处理，而且在**下一次**设置时会先去动旧对象：
     *
     *     void lv_keyboard_set_textarea(obj, ta) {
     *         if (keyboard->ta) lv_obj_remove_state(keyboard->ta, LV_STATE_FOCUSED);  // ← 写旧 textarea
     *         keyboard->ta = ta;
     *     }
     *
     * 而 `s_kb` 是**全局复用**的：浮层关了只把键盘藏起来、指针没清 →
     * 下次开浮层时那一次 `set_textarea()` 就是**往已释放的对象写状态位**，
     * 踩坏 LVGL 的对象树。真机现象：**连续开/关"添加歌单"浮层**后，
     * 渲染阶段 `Load address misaligned` in `lv_color_over32`（故障地址 0x9）。
     *
     * 这里趁 textarea 还活着先解绑（`set_textarea(kb, NULL)` 只会去 remove 旧对象的
     * FOCUSED 状态，此刻它是合法的）。**顺序不能颠倒**：先解绑，再藏键盘。
     * 拼音键盘同样处理 —— `lv_pinyin_ime_attach(ime, NULL)` 是允许的，
     * 它内部只把 `ctx->ta` 置空（组件自己也不会在 textarea 删除时清理）。
     */
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ime) {
        lv_pinyin_ime_attach(s_ime, NULL);
        lv_pinyin_ime_hide(s_ime);
    }
    if (s_modal) {
        /* 渐出（下滑 + 缩小 + 淡出）：动画结束由 ui_popup_out 自己删。
         * 本函数可能在键盘事件回调里被调，所以这里只"开始淡出"，不阻塞。 */
        ui_ba_close(&s_modal_dlg);
        s_modal = NULL;
    }
    s_modal_ta = NULL;
    s_modal_kind = OREQ_NONE;
}

static void modal_submit(lv_event_t *e)
{
    (void)e;
    if (!s_modal_ta || !s_modal) {
        return;
    }
    const char *txt = lv_textarea_get_text(s_modal_ta);

    /* 重命名浮层：就地改名字，不走网络任务 */
    if (s_modal_kind == MODAL_RENAME) {
        if (ao_pl_rename_idx() >= 0) {
            online_playlist_rename((size_t)ao_pl_rename_idx(), txt ? txt : "");
            ui_toast("重命名成功", 1200);
            ao_pl_rebuild();
        }
        ao_pl_rename_idx_set(-1);
        ao_modal_close();
        return;
    }

    if (!txt || !txt[0]) {
        ui_toast("请输入内容", 1200);
        return;
    }
    if (!online_has_network()) {
        ui_toast("Wi-Fi 未连接", 1500);
        return;
    }
    const int kind = s_modal_kind;
    ao_work_submit(kind, txt);
    ao_modal_close();
}

/*
 * 浮层销毁（异步删除或页面切走时都会来）：
 * 键盘挂在 lv_layer_top 上，不归浮层所有，所以这里只需要把指针清掉。
 *
 * ★ 这里同样要解绑（第二道保险）：`ao_modal_close()` 已经解过一次，正常路径走到这里
 *   键盘指针已经是 NULL，`set_textarea(NULL)` 是空操作。留着是因为**万一**有别的路径
 *   直接删了浮层（没走 close），下一次开浮层就会踩已释放的 textarea。
 *   ⚠ 之所以在这里做是安全的：LVGL 的 `lv_obj_delete()` 是"**先给对象自己发
 *     LV_EVENT_DELETE、之后才删子对象**"（`lv_obj_tree.c` 的 `obj_delete_core()`），
 *     所以此刻 textarea 还活着，remove_state 不会踩空。
 */
static void modal_on_delete(lv_event_t *e)
{
    (void)e;
    s_modal = NULL;
    s_modal_ta = NULL;
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ime) {
        lv_pinyin_ime_attach(s_ime, NULL);
        lv_pinyin_ime_hide(s_ime);
    }
}

/*
 * 拼音键盘上的对勾 / 回车 → 组件在"没有未上屏拼音"时发 ready 事件。
 * 语义按当前上下文分派：
 *   - 重命名浮层开着 → 等于"确定"（提交重命名）
 *   - 否则（搜索页）  → 输入框非空就开始搜索，空则收起键盘
 */
static void ime_ready_cb(lv_event_t *e)
{
    if (s_modal && s_modal_kind == MODAL_RENAME) {
        modal_submit(e);
        return;
    }
    ao_srch_ime_ready(e);
}

/* 收起拼音键盘与自带键盘（搜索页失焦、搜索开始时都要收） */
void ao_ime_hide(void)
{
    if (s_ime) {
        lv_pinyin_ime_hide(s_ime);
    }
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ao_ime_open(void)
{
    /* 注意看的是**键盘对象**的隐藏位，不是容器：容器是 SIZE_CONTENT，
     * 键盘一隐藏它就塌成 0 高，但"开没开"要看键盘自己。 */
    return s_ime && !lv_obj_has_flag(s_ime, LV_OBJ_FLAG_HIDDEN);
}

/*
 * 键盘的按键尺寸是 lv_pinyin_ime_create() 里按**父对象宽度**算一次的
 * （IME_UI_UNIT_PX = 可用宽度 / 12）。刚建出来的容器还没跑过 layout，
 * lv_obj_get_width() 是 0，组件会退化成"按 320 px 算" → 键盘特别小。
 * 所以这里先把容器塞进一个显式宽度、并强制跑一次 layout，再建键盘。
 */
static void modal_build_keyboards(lv_obj_t *ta, bool search)
{
    if (search) {
        if (s_kb) {
            lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        }
        if (!s_ime) {
            s_ime_box = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(s_ime_box);
            /*
             * ★ 宽度必须给**绝对像素**：组件的按键尺寸是 create() 时按父对象宽度
             * 算一次的（IME_UI_UNIT_PX = 可用宽度 / 12）。给百分比 + 刚建出来的
             * 对象还没跑 layout，lv_obj_get_width() 会返回 0 → 组件退化成"按 320
             * 算"，键位只有 ~23 px，就是"键盘太小、不好按"的原因。
             * 给满屏宽 → 约 63 px/键，整块约占半屏。
             */
            lv_obj_set_width(s_ime_box,
                             lv_display_get_horizontal_resolution(NULL) - 2 * UI_PX_W(8));
            lv_obj_set_height(s_ime_box, LV_SIZE_CONTENT);
            lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(2));
            lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_update_layout(s_ime_box);      /* ★ 让宽度立刻生效（见上面注释） */
            /*
             * ★ 目标高度必须在 create() 之前设：组件按"fit 高度 ÷ 可见行数"反推出
             * 行高，键盘正好占半屏（IME_FIT_HEIGHT = UI_H/2）。行高随父容器宽度
             * 自适应（按键 flex grow），模式/面板切换时会按新行数重新分摊。
             */
            lv_pinyin_ime_set_fit_height(IME_FIT_HEIGHT);
            s_ime = lv_pinyin_ime_create(s_ime_box);
            if (!s_ime) {
                ESP_LOGW(TAG, "pinyin ime create failed (dict 分区没烧？)");
                ui_toast("拼音输入法不可用。", 2500);
            } else {
                /*对勾 / 回车（无未上屏拼音时触发）→ 搜索或提交重命名 */
                lv_obj_add_event_cb(s_ime, ime_ready_cb, lv_pinyin_ime_event_ready(), NULL);
            }
        }
        if (s_ime) {
            lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(4));
            lv_pinyin_ime_attach(s_ime, ta);
    /*
     * ★ 语音输入：把宿主（WebSocket + 板载麦克风）接到这把键盘上。
     *   必须在 attach(ime, ta) **之后** —— 组件要先有会话，我们再补 host 回调；
     *   识别期间会自动暂停播放（见 voice_service.cc 的 on_voice_state）。
     */
    voice_service_attach_ime(s_ime);
            lv_pinyin_ime_show(s_ime);
            /*
             * ★ 键盘挂在 lv_layer_top() 上、只建一次；而**每次开浮层都会新建一个
             * scrim**（同样是 layer_top 的子对象，排在后面）—— 于是第二次开始
             * 打开搜索，遮罩就压在键盘之上，键盘看得到却点不动。
             * 把键盘提到最后一个子对象，保证它永远在遮罩之上。
             */
            lv_obj_move_foreground(s_ime_box);
        }
        return;
    }

    /* 歌单 ID：纯数字就够 —— 键盘开成**数字键盘**（LVGL 自带的 NUMBER 布局：
     * 0-9 + 退格 + ✓，见 kb_map_num），并且输入层也只接受数字。
     * 两道都做：键盘解决"按键太大/找不着数字"，accepted_chars 解决"接上实体键盘
     * 或切到字母布局时仍能打进字母"（那些键按了也不会进文本）。
     * ⚠ 数字布局里那个 ⌨ 键会让键盘切回字母布局，所以输入层的过滤不能省。 */
    if (s_ime) {
        lv_pinyin_ime_attach(s_ime, NULL);
        lv_pinyin_ime_hide(s_ime);
    }
    if (!s_kb) {
        s_kb = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(s_kb, LV_PCT(96), LV_PCT(46));
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(4));
        lv_obj_set_style_text_font(s_kb, UI_FONT_TEXT, LV_PART_ITEMS);
        /* 自带键盘的 OK / 关闭键：直接走"确定"与"取消" */
        lv_obj_add_event_cb(s_kb, modal_submit, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_kb, [](lv_event_t *e) { (void)e; ao_modal_close(); },
                            LV_EVENT_CANCEL, NULL);
    }
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);   /* ★ 打开就是数字键盘 */
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    /* 同上：歌单 ID 键盘也可能被后来新建的 scrim 压住 */
    lv_obj_move_foreground(s_kb);
}

/* 给某个输入框挂上拼音键盘并显示（键盘只建一次，挂在 lv_layer_top 上） */
void ao_ime_show_for(lv_obj_t *ta)
{
    if (ta) {
        modal_build_keyboards(ta, true);
    }
}

/* 建浮层：BA 白卡片（标题 + 输入框 + 提示 + 取消/确定）+（可选拼音键盘） */
void ao_modal_open(int kind)
{
    if (s_modal) {
        return;
    }
    s_modal_kind = kind;
    const bool search = (kind == OREQ_SEARCH);
    const bool rename = (kind == MODAL_RENAME);

    /* 卡片停在上半屏：下半屏留给拼音键盘（IME_FIT_HEIGHT = UI_H/2） */
    s_modal_dlg = ui_ba_open(rename ? "重命名歌单" : (search ? "搜索歌曲" : "添加歌单"),
                             62, LV_ALIGN_TOP_MID, UI_PX_H(8));
    s_modal = s_modal_dlg.scrim;
    lv_obj_add_event_cb(s_modal, modal_on_delete, LV_EVENT_DELETE, NULL);
    ui_ba_on_close(&s_modal_dlg, [](void *user) { (void)user; ao_modal_close(); }, NULL);

    lv_obj_t *ta = ui_ba_input(s_modal_dlg.body,
                               rename ? "输入新的歌单名（可留空恢复显示 ID）"
                                      : (search ? "输入歌名或歌手（可用拼音键盘）"
                                                : "请输入歌单 ID"),
                               NULL);
    s_modal_ta = ta;
    /* 歌单 ID 只能输数字（用户要求）：输入层直接挡掉非数字字符。
     * 键盘侧还有一道 `LV_KEYBOARD_MODE_NUMBER`（见 modal_build_keyboards）。 */
    if (!rename && !search) {
        lv_textarea_set_accepted_chars(ta, "0123456789");
    }
    /* 重命名：把当前名字（没有就填 ID）预填进去，用户改两个字就行 */
    if (rename && ao_pl_rename_idx() >= 0) {
        const char *nm = online_playlist_name((size_t)ao_pl_rename_idx());
        const char *id = online_playlist_id((size_t)ao_pl_rename_idx());
        lv_textarea_set_text(ta, (nm && nm[0]) ? nm : (id ? id : ""));
    }

    ui_ba_hint(s_modal_dlg.body, "※最多可以输入 60 个字。");

    ui_ba_btn(s_modal_dlg.btn_row, "取消", false,
              [](lv_event_t *e) { (void)e; ao_modal_close(); }, NULL);
    ui_ba_btn(s_modal_dlg.btn_row, rename ? "保存" : (search ? "搜索" : "添加"),
              true, modal_submit, NULL);

    /* 重命名可能起中文名 → 也要拼音键盘；只有"歌单 ID"用数字键盘 */
    modal_build_keyboards(ta, search || rename);
}
