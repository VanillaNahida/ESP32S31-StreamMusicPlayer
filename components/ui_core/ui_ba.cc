/*
 * ui_ba.cc —— 碧蓝档案（Blue Archive）风格弹窗工具箱
 *
 * 视觉参照 assets/blue_archive_ui_replica.html。这里只做"外壳 + 控件"，
 * 打开/关闭动画复用 ui_core 的 ui_popup_in()/ui_popup_out()（见 ui_core.cc）。
 *
 * 两个实现要点值得记一笔：
 *
 * 1. **角落的浅蓝几何装饰用 lv_draw_triangle 现画**，不摆旋转/斜切过的方块。
 *    LVGL 的对象变换是"先渲染进 ARGB8888 图层、再整体变换"，一张 400x300 的
 *    卡片每帧要多占 ~470 KB 图层缓冲；而软渲染下卡片在进场动画期间是**逐帧
 *    重绘**的 —— 这个代价在 ESP32-S31 上不值当。三角形是直接画进当前图层的，
 *    只有几十行掩码计算。
 *
 * 2. **按钮的"斜切"（CSS 的 skewX(-14deg)）不能用 transform_skew_x**。
 *    lv_draw_sw.c 的 evaluate() 里写着 "not support skew"：软渲染与 PPA 都
 *    直接拒收带 skew 的图层任务 —— 真用了会看到按钮**整个不画出来**。
 *    这里改成拿两个三角形拼一个平行四边形：左半边与右半边共用同一个 VER
 *    渐变，按行取色，所以中间那条对角线接缝看不出来。
 */
#include "ui_ba.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ 尺寸 */

#define BA_CARD_RADIUS   UI_PX_H(16)
#define BA_BTN_W         UI_PX_W(144)
#define BA_BTN_H         UI_PX_H(44)
#define BA_BTN_SKEW      UI_PX_W(11)     /* 平行四边形上下边的水平错位 */
#define BA_PAD_X         UI_PX_W(16)
#define BA_PAD_Y         UI_PX_H(12)

/* 按钮样式 */
#define BA_STYLE_CANCEL  0
#define BA_STYLE_CONFIRM 1
#define BA_STYLE_DANGER  2

/* ------------------------------------------------------------------ 内部上下文
 *
 * 挂在 scrim 的 user_data 上（DELETE 时释放）：关闭回调、点空白关闭开关。
 * 这样 ui_ba_close_scrim() 只需要一个 scrim 指针就能还原全部行为。
 */
typedef struct {
    lv_obj_t *card;
    void (*on_close)(void *user);
    void *on_close_user;
    bool  dismissible;
    /*
     * 已经在"退场动画"里了。关闭必须**幂等**：同一个浮层可能被连点两次 ✕、
     * 或者"点空白"与调用方自己的 close 挤在一起 —— 第二次 ui_popup_out()
     * 会在同一个对象上再排一条动画和一个 lv_obj_delete_async。
     * （LVGL 自己也有 is_deleting + 取消重复 async 的兜底，但两条动画互相
     *   抢 translate_y 会看到"抖一下"，索性在这里挡住。）
     */
    bool  closing;
} ba_ctx_t;

/* "复原后才派发"的回调挂载（实现在文件后半的"按下反馈"一节）。
 * 对外的 `ui_ba_press_feedback()` 声明在 ui_ba.h 里（返回钮也用它）。 */

static ba_ctx_t *ba_ctx_of(lv_obj_t *scrim)
{
    return scrim ? (ba_ctx_t *)lv_obj_get_user_data(scrim) : NULL;
}

static void ba_ctx_free(lv_event_t *e)
{
    ba_ctx_t *c = (ba_ctx_t *)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    if (c) {
        free(c);
    }
}

/* ------------------------------------------------------------------ 三角形 / 平行四边形 */

static void ba_tri(lv_layer_t *layer, lv_point_precise_t p0, lv_point_precise_t p1,
                   lv_point_precise_t p2, lv_grad_dir_t dir,
                   uint32_t c0, lv_opa_t o0, uint32_t c1, lv_opa_t o1, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0] = p0;
    d.p[1] = p1;
    d.p[2] = p2;
    d.opa  = opa;
    /* 只支持 VER / HOR（LV_DRAW_SW_COMPLEX_GRADIENTS 没开，线性/径向/锥形不可用） */
    d.grad.dir            = dir;
    d.grad.stops[0].color = lv_color_hex(c0);
    d.grad.stops[0].opa   = o0;
    d.grad.stops[0].frac  = 0;
    d.grad.stops[1].color = lv_color_hex(c1);
    d.grad.stops[1].opa   = o1;
    d.grad.stops[1].frac  = 255;
    d.grad.stops_count    = 2;
    lv_draw_triangle(layer, &d);
}

/* 一块纯色/竖向渐变的平行四边形：上边右移 sk、下边左移 sk（视觉上右倾） */
static void ba_para(lv_layer_t *layer, const lv_area_t *a, int32_t sk,
                    uint32_t top, uint32_t bottom)
{
    const int32_t x1 = a->x1;
    const int32_t y1 = a->y1;
    const int32_t x2 = a->x2;
    const int32_t y2 = a->y2;
    if (lv_area_get_width(a) < sk * 2 + 4 || lv_area_get_height(a) < 4) {
        return;
    }
    const lv_point_precise_t pa = { x1 + sk, y1 };   /* 左上 */
    const lv_point_precise_t pb = { x2,      y1 };   /* 右上 */
    const lv_point_precise_t pc = { x2 - sk, y2 };   /* 右下 */
    const lv_point_precise_t pd = { x1,      y2 };   /* 左下 */
    ba_tri(layer, pa, pb, pc, LV_GRAD_DIR_VER, top, LV_OPA_COVER, bottom, LV_OPA_COVER, LV_OPA_COVER);
    ba_tri(layer, pa, pc, pd, LV_GRAD_DIR_VER, top, LV_OPA_COVER, bottom, LV_OPA_COVER, LV_OPA_COVER);
}

typedef struct {
    uint32_t top;
    uint32_t bottom;
    uint32_t edge;
} ba_para_t;

static void ba_para_deleted(lv_event_t *e)
{
    void *p = lv_event_get_user_data(e);
    if (p) {
        free(p);
    }
}

static void ba_para_draw(lv_event_t *e)
{
    const ba_para_t *p = (const ba_para_t *)lv_event_get_user_data(e);
    lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!p || !o || !layer) {
        return;
    }
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    if (lv_area_get_width(&c) < BA_BTN_SKEW * 2 + 6 || lv_area_get_height(&c) < 8) {
        return;
    }
    /* 外圈 = 描边色，内芯缩进 2 px 盖上去 */
    ba_para(layer, &c, BA_BTN_SKEW, p->edge, p->edge);

    lv_area_t in = c;
    in.x1 += 2;
    in.y1 += 2;
    in.x2 -= 2;
    in.y2 -= 2;
    ba_para(layer, &in, BA_BTN_SKEW, p->top, p->bottom);
}

/* ------------------------------------------------------------------ 角落装饰 */

/*
 * 角落的直角三角形：直角边贴着卡片的两条边，斜边朝卡片中心，
 * 沿 horiz 方向（或竖直方向）从 color 渐隐到透明。
 *   br = true  → 取右下角；否则左上角
 */
static void ba_deco_corner(lv_layer_t *layer, const lv_area_t *a, uint32_t color,
                           lv_opa_t opa, bool br, bool horiz)
{
    lv_point_precise_t p0, p1, p2;
    if (!br) {
        p0 = (lv_point_precise_t){ a->x1, a->y1 };
        p1 = (lv_point_precise_t){ a->x2, a->y1 };
        p2 = (lv_point_precise_t){ a->x1, a->y2 };
    } else {
        p0 = (lv_point_precise_t){ a->x2, a->y2 };
        p1 = (lv_point_precise_t){ a->x1, a->y2 };
        p2 = (lv_point_precise_t){ a->x2, a->y1 };
    }
    /* 渐变的两端：贴着角落那侧是 color，斜边那侧透明 */
    if (!br) {
        ba_tri(layer, p0, p1, p2, horiz ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER,
               color, opa, color, LV_OPA_TRANSP, LV_OPA_COVER);
    } else {
        ba_tri(layer, p0, p1, p2, horiz ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER,
               color, LV_OPA_TRANSP, color, opa, LV_OPA_COVER);
    }
}

static void ba_deco_draw(lv_event_t *e)
{
    lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!o || !layer) {
        return;
    }
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    const int32_t w = lv_area_get_width(&c);
    const int32_t h = lv_area_get_height(&c);
    if (w < 40 || h < 40) {
        return;
    }

    lv_area_t a;
    /* 左上：横向 + 纵向两块叠出"斜向高光" */
    a = (lv_area_t){ c.x1, c.y1, c.x1 + w * 62 / 100, c.y1 + h * 72 / 100 };
    ba_deco_corner(layer, &a, BA_BLUE_PALE, 80, false, true);
    a = (lv_area_t){ c.x1, c.y1, c.x1 + w * 44 / 100, c.y1 + h * 84 / 100 };
    ba_deco_corner(layer, &a, BA_BLUE_PALE, 70, false, false);

    /* 右下：同色系稍深（HTML 里的 B9E6FF） */
    a = (lv_area_t){ c.x2 - w * 66 / 100, c.y2 - h * 62 / 100, c.x2, c.y2 };
    ba_deco_corner(layer, &a, 0xB9E6FF, 95, true, true);
    a = (lv_area_t){ c.x2 - w * 42 / 100, c.y2 - h * 78 / 100, c.x2, c.y2 };
    ba_deco_corner(layer, &a, 0xB9E6FF, 85, true, false);
}

/* ------------------------------------------------------------------ 卡片外壳 */

/*
 * 点遮罩空白处关闭。语义与右上角 ✕ 完全一致：**先走调用方的关闭回调**
 * （qr_close() / out_picker_close() / pw_close()…），没有回调才自己关。
 * 不能直接 ui_ba_close_scrim()：那样调用方自己的状态（定时器、指针、缓存的
 * 图片）就没人清了 —— 例如二维码浮层会留下一个还在轮询的定时器。
 * 另外只有"点在遮罩自己身上"才算点空白：从子对象冒泡上来的不算
 * （lv_obj 默认不带 EVENT_BUBBLE，这里再兜一道）。
 */
static void ba_scrim_clicked(lv_event_t *e)
{
    lv_obj_t *scrim = (lv_obj_t *)lv_event_get_current_target(e);
    if (lv_event_get_target(e) != scrim) {
        return;
    }
    ba_ctx_t *c = ba_ctx_of(scrim);
    if (!c || !c->dismissible) {
        return;
    }
    if (c->on_close) {
        c->on_close(c->on_close_user);
        return;
    }
    ui_ba_close_scrim(scrim);
}

static void ba_close_clicked(lv_event_t *e)
{
    lv_obj_t *scrim = (lv_obj_t *)lv_event_get_user_data(e);
    ba_ctx_t *c = ba_ctx_of(scrim);
    if (c && c->on_close) {
        c->on_close(c->on_close_user);
        return;
    }
    ui_ba_close_scrim(scrim);
}

ui_ba_dialog_t ui_ba_open_in(lv_obj_t *parent, const char *title, int width_pct,
                             lv_align_t align, int y_ofs)
{
    ui_ba_dialog_t d;
    memset(&d, 0, sizeof(d));

    if (!parent) {
        parent = lv_layer_top();
    }
    if (width_pct < 20) {
        width_pct = 20;
    } else if (width_pct > 96) {
        width_pct = 96;
    }

    lv_obj_t *scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(BA_SCRIM), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, ba_scrim_clicked, LV_EVENT_CLICKED, NULL);

    ba_ctx_t *ctx = (ba_ctx_t *)calloc(1, sizeof(ba_ctx_t));
    if (ctx) {
        lv_obj_set_user_data(scrim, ctx);
        lv_obj_add_event_cb(scrim, ba_ctx_free, LV_EVENT_DELETE, NULL);
    }
    d.scrim = scrim;

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, UI_PCT_X(width_pct));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(BA_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, BA_CARD_RADIUS, LV_PART_MAIN);
    /* 投影：卡片浮在压暗的背景上才有 BA 那种"贴纸"感。
     * ⚠ 本工程 LV_DRAW_SW_SHADOW_CACHE_SIZE=0（阴影不缓存、每帧重算），
     *   所以别把 shadow_width 调大 —— 进场动画期间卡片是逐帧重绘的。 */
    lv_obj_set_style_shadow_width(card, UI_PX_H(26), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(card, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, align, 0, y_ofs);
    if (ctx) {
        ctx->card = card;
    }
    d.card = card;

    /* 装饰层：铺满卡片、不参与 flex、不吃点击。
     * 必须是**第一个**子对象 —— 子对象按创建顺序绘制，这样它压在白底之上、
     * 文字与按钮之下。
     *
     * ⚠ 用 FLOATING 而不是 IGNORE_LAYOUT：卡片是 LV_SIZE_CONTENT 高，而这个
     *   装饰层是 100% 高 —— IGNORE_LAYOUT 只让它不参与 flex，**仍然**算进
     *   calc_content_height()（那里只跳过 HIDDEN / FLOATING），于是"卡片多高
     *   取决于装饰层多高、装饰层多高又取决于卡片多高"就成了自反馈；
     *   lv_obj_update_layout() 的 `while(scr->scr_layout_inv)` 没有次数上限，
     *   一旦来回震荡就是 LVGL 任务空转、看门狗超时。FLOATING 会被布局和
     *   内容尺寸计算一起跳过，正是"覆盖层"该有的语义。 */
    lv_obj_t *deco = lv_obj_create(card);
    lv_obj_remove_style_all(deco);
    lv_obj_set_size(deco, LV_PCT(100), LV_PCT(100));
    lv_obj_align(deco, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(deco, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(deco, ba_deco_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* ---------------- 标题栏 ---------------- */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(head, BA_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_top(head, BA_PAD_Y, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(head, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_set_style_pad_row(head, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    if (title && title[0]) {
        lv_obj_t *ttl = lv_label_create(head);
        lv_obj_set_style_text_font(ttl, UI_FONT_TITLE, LV_PART_MAIN);
        lv_obj_set_style_text_color(ttl, lv_color_hex(BA_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(ttl, 2, LV_PART_MAIN);
        lv_label_set_text(ttl, title);
        d.title = ttl;

        /* BA 标志：标题下面那根黄色小横杠 */
        lv_obj_t *ul = lv_obj_create(head);
        lv_obj_remove_style_all(ul);
        lv_obj_set_size(ul, UI_PX_W(38), UI_PX_H(4));
        lv_obj_set_style_bg_color(ul, lv_color_hex(BA_YELLOW), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ul, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ul, UI_PX_H(2), LV_PART_MAIN);
        lv_obj_remove_flag(ul, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 右上角关闭 X：绝对定位（不参与 flex，也不算进卡片的内容高度） */
    lv_obj_t *close = lv_obj_create(card);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, UI_PX_H(34), UI_PX_H(34));
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -UI_PX_W(6), UI_PX_H(6));
    lv_obj_add_flag(close, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close, UI_PX_H(6));
    lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *xl = lv_label_create(close);
    lv_obj_set_style_text_font(xl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(xl, lv_color_hex(BA_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);
    /* 右上角 ✕ 也走同一套按下反馈（它不走 ui_ba_btn，得自己挂） */
    ui_ba_press_feedback(close, ba_close_clicked, scrim);

    /* ---------------- 内容区 ---------------- */
    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(body, BA_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(body, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    d.body = body;

    /* ---------------- 底部按钮行 ---------------- */
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, BA_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, BA_PAD_Y, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, UI_PX_W(22), LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    d.btn_row = row;

    ui_popup_in(scrim, card);
    return d;
}

ui_ba_dialog_t ui_ba_open(const char *title, int width_pct, lv_align_t align, int y_ofs)
{
    return ui_ba_open_in(lv_layer_top(), title, width_pct, align, y_ofs);
}

void ui_ba_close(ui_ba_dialog_t *d)
{
    if (!d) {
        return;
    }
    if (d->scrim) {
        ui_ba_close_scrim(d->scrim);
    }
    memset(d, 0, sizeof(*d));
}

void ui_ba_close_scrim(lv_obj_t *scrim)
{
    if (!scrim) {
        return;
    }
    ba_ctx_t *c = ba_ctx_of(scrim);
    if (c) {
        if (c->closing) {
            return;                 /* 幂等：退场动画只跑一条 */
        }
        c->closing = true;
    }
    ui_popup_out(scrim, lv_obj_get_child(scrim, 0));
}

void ui_ba_scrim_dismissible(ui_ba_dialog_t *d, bool on)
{
    if (!d || !d->scrim) {
        return;
    }
    ba_ctx_t *c = ba_ctx_of(d->scrim);
    if (c) {
        c->dismissible = on;
    }
}

void ui_ba_on_close(ui_ba_dialog_t *d, void (*cb)(void *user), void *user)
{
    if (!d || !d->scrim) {
        return;
    }
    ba_ctx_t *c = ba_ctx_of(d->scrim);
    if (c) {
        c->on_close      = cb;
        c->on_close_user = user;
    }
}

/* ------------------------------------------------------------------ 按下反馈
 *
 * 需求（2026-10 用户）：BA 按钮**按住时渐进式缩小**，松手后**先复原、再执行事件**。
 * 2026-10 第二次需求：**标题栏的返回钮也要一模一样的手感** —— 于是这一套提成
 * 对外入口 `ui_ba_press_feedback()`（声明在 ui_ba.h），弹窗按钮 / 选项行 / ✕ /
 * 返回钮全走它，常量只有这一份（见 ui_core.cc 的 ui_back_button_create）。
 *
 * 实现要点（三条都别改，改了就变形）：
 *   1. 缩放用 `transform_scale_x/y`（LVGL 的单位是 **1/256**，256 = 原尺寸）。
 *      它是**绘制期变换**、不参与布局 —— 所以按钮缩小不会把旁边的按钮挤动，
 *      也不会触发整卡片重排（用宽高做动画就会）。
 *      代价：变换期间 LVGL 会把该对象（含子树）先渲染进一层 ARGB8888 图层，
 *      72 px 的按钮 ≈ 20 KB/帧 —— 只在 120+150 ms 的动画里发生，可以接受。
 *      ⚠ 别改成 `lv_image_set_scale()`：那个只能作用在 **lv_image 对象**上，
 *      对 lv_button 调用等于把 `lv_image_t` 的字段写到一块 `lv_obj_t` 大小的
 *      内存上（本工程 `LV_USE_ASSERT_OBJ=n`，不会被拦下）—— 越界写堆内存，
 *      而且视觉上什么都不会发生（返回钮"按了没反应"就是这么来的）。
 *   2. **用户回调不再挂 CLICKED**，改挂一个自定义事件码：
 *      真实 CLICKED 在松手**那一瞬间**就来，用它直接执行就等于"先执行、后复原"，
 *      与需求相反。所以：PRESSED → 缩小；RELEASED → 放大，动画**跑完**再派发自定义
 *      事件；PRESS_LOST（手指滑出按钮）→ 只复原、不派发。
 *      自定义事件派发时 `lv_event_get_target()/_get_user_data()` 都是正常的，
 *      所以各调用点的写法（含用 user_data 传参的，如 WiFi 密码浮层）不用改。
 *   3. 起动画前先 `lv_anim_delete(var, exec_cb)`：连点/快速点两次不会叠加，
 *      而且**从当前实际缩放值**起步（中途打断也不会跳一下）。
 */
#define BA_SCALE_NORM   256      /* 100% */
#define BA_SCALE_PRESS  150      /* 按住时 ≈92% */
#define BA_PRESS_MS     400
#define BA_RELEASE_MS   400

static lv_event_code_t s_ba_click_evt;   /* 懒注册：= "动画跑完的那次点击" */
static bool            s_ba_click_reg;

static void ba_btn_scale_anim(void *var, int32_t v)
{
    lv_obj_t *b = (lv_obj_t *)var;
    lv_obj_set_style_transform_scale_x(b, v, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(b, v, LV_PART_MAIN);
}

static void ba_btn_restore_done(lv_anim_t *a)
{
    if ((uintptr_t)lv_anim_get_user_data(a) != 0 && s_ba_click_reg) {
        lv_obj_send_event((lv_obj_t *)a->var, s_ba_click_evt, NULL);
    }
}

static void ba_btn_scale_to(lv_obj_t *b, int32_t to, uint32_t ms, bool fire)
{
    const int32_t from = lv_obj_get_style_transform_scale_x(b, LV_PART_MAIN);
    if (from == to && !fire) {
        return;
    }
    lv_anim_delete(b, ba_btn_scale_anim);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, b);
    lv_anim_set_exec_cb(&a, ba_btn_scale_anim);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, (void *)(uintptr_t)(fire ? 1u : 0u));
    lv_anim_set_completed_cb(&a, ba_btn_restore_done);
    lv_anim_start(&a);
}

static void ba_btn_pressed_cb(lv_event_t *e)
{
    ba_btn_scale_to((lv_obj_t *)lv_event_get_current_target(e), BA_SCALE_PRESS,
                    BA_PRESS_MS, false);
}

static void ba_btn_released_cb(lv_event_t *e)
{
    ba_btn_scale_to((lv_obj_t *)lv_event_get_current_target(e), BA_SCALE_NORM,
                    BA_RELEASE_MS, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

/* 登记用户回调：挂到自定义事件码上（见上面第 2 条），动画完成后由我们派发 */
static void ba_attach_click(lv_obj_t *obj, lv_event_cb_t cb, void *user)
{
    if (!obj || !cb) {
        return;
    }
    if (!s_ba_click_reg) {
        s_ba_click_evt = (lv_event_code_t)lv_event_register_id();
        s_ba_click_reg = true;
    }
    lv_obj_add_event_cb(obj, cb, s_ba_click_evt, user);
}

/*
 * ★ 对外入口：给任意可点对象挂上"按下缩小 + 复原后才回调"。
 *   弹窗按钮 / 选项行 / ✕ / **标题栏返回钮**（ui_core.cc）全走这一份实现 ——
 *   常量、时长、缓动、事件顺序只有一处，改手感就改这里，各处一定一致。
 *   on_click 传 NULL 就是"只要按下反馈、不要回调"。
 */
void ui_ba_press_feedback(lv_obj_t *obj, lv_event_cb_t on_click, void *user)
{
    if (!obj) {
        return;
    }
    /* 缩放的枢轴放正中：LVGL 默认也是中心，这里写明，免得以后被别处改掉 */
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), LV_PART_MAIN);
    /* 起始缩放**显式**写成 100%：别依赖默认值 —— `ba_btn_scale_to()` 会读当前值当动画
     * 起点，默认值若不是 256，第一次按下的动画就会从错误的比例开始（跳一下）。 */
    lv_obj_set_style_transform_scale_x(obj, BA_SCALE_NORM, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(obj, BA_SCALE_NORM, LV_PART_MAIN);
    lv_obj_add_event_cb(obj, ba_btn_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, ba_btn_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, ba_btn_released_cb, LV_EVENT_PRESS_LOST, NULL);
    ba_attach_click(obj, on_click, user);
}

/* ------------------------------------------------------------------ 卡片内控件 */

lv_obj_t *ui_ba_btn_styled(lv_obj_t *row, const char *text, int style,
                           lv_event_cb_t cb, void *user)
{
    if (!row) {
        return NULL;
    }
    lv_obj_t *wrap = lv_obj_create(row);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_obj_create(wrap);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, BA_BTN_W, BA_BTN_H);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, UI_PX_H(4));
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    ba_para_t *p = (ba_para_t *)calloc(1, sizeof(ba_para_t));
    if (p) {
        if (style == BA_STYLE_CONFIRM) {
            p->top    = BA_BLUE_LIGHT;
            p->bottom = BA_BLUE;
            p->edge   = BA_BLUE_EDGE;
        } else if (style == BA_STYLE_DANGER) {
            p->top    = 0xF87171;
            p->bottom = 0xDC2626;
            p->edge   = 0xFCA5A5;
        } else {
            p->top    = BA_CANCEL_A;
            p->bottom = BA_CANCEL_B;
            p->edge   = BA_CARD;
        }
        lv_obj_add_event_cb(btn, ba_para_draw, LV_EVENT_DRAW_MAIN, p);
        lv_obj_add_event_cb(btn, ba_para_deleted, LV_EVENT_DELETE, p);
    } else {
        /* 兜底：拿不到那点内存就别让按钮整个消失，退回普通的圆角渐变矩形 */
        const uint32_t top = (style == BA_STYLE_CONFIRM) ? BA_BLUE_LIGHT
                                                         : (style == BA_STYLE_DANGER ? 0xF87171 : BA_CANCEL_A);
        const uint32_t bot = (style == BA_STYLE_CONFIRM) ? BA_BLUE
                                                         : (style == BA_STYLE_DANGER ? 0xDC2626 : BA_CANCEL_B);
        lv_obj_set_style_bg_color(btn, lv_color_hex(top), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(btn, lv_color_hex(bot), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, UI_PX_H(6), LV_PART_MAIN);
    }

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(style == BA_STYLE_CANCEL ? BA_CANCEL_FG : 0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
    lv_label_set_text(l, text ? text : "");
    lv_obj_center(l);

    /*
     * 按钮下面原来还画一个 "Enter" / "Esc" 键位徽章（BA 网页稿的键盘提示）。
     * 本机是触摸 + 6 个 ADC 按键，没有键盘，这两个字纯属误导，已去掉。
     */

    /* 按下反馈 + "复原后才派发"见文件后半"按下反馈"一节（返回钮共用同一套） */
    ui_ba_press_feedback(btn, cb, user);
    return btn;
}

lv_obj_t *ui_ba_btn(lv_obj_t *row, const char *text, bool primary,
                    lv_event_cb_t cb, void *user)
{
    return ui_ba_btn_styled(row, text, primary ? BA_STYLE_CONFIRM : BA_STYLE_CANCEL, cb, user);
}

lv_obj_t *ui_ba_opt_row(lv_obj_t *col, const char *text, uint32_t accent,
                        lv_event_cb_t cb, void *user)
{
    if (!col) {
        return NULL;
    }
    lv_obj_t *r = lv_obj_create(col);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), UI_PX_H(46));
    lv_obj_set_style_bg_color(r, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(r, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(r, UI_PX_W(16), LV_PART_MAIN);
    lv_obj_set_style_border_width(r, UI_PX_W(4), LV_PART_MAIN);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_color(r, lv_color_hex(accent ? accent : BA_BLUE), LV_PART_MAIN);
    lv_obj_set_style_border_opa(r, accent ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(r);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(BA_TEXT_BODY), LV_PART_MAIN);
    lv_label_set_text(l, text ? text : "");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    /* 选项行同样给按下反馈 */
    ui_ba_press_feedback(r, cb, user);
    return r;
}

lv_obj_t *ui_ba_input(lv_obj_t *parent, const char *placeholder, const char *text)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, UI_PX_H(46));
    lv_obj_set_style_text_font(ta, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_hex(BA_INPUT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ta, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ta, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, UI_PX_H(2), LV_PART_MAIN);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_hex(BA_INPUT_LINE), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(BA_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(BA_TEXT_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, lv_color_hex(BA_BLUE), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 60);
    if (placeholder && placeholder[0]) {
        lv_textarea_set_placeholder_text(ta, placeholder);
    }
    if (text && text[0]) {
        lv_textarea_set_text(ta, text);
    }
    return ta;
}

lv_obj_t *ui_ba_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(BA_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_ba_text(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(BA_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_pad_ver(l, UI_PX_H(4), LV_PART_MAIN);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* ------------------------------------------------------------------ 二次确认 */

static struct {
    ui_ba_dialog_t dlg;
    void (*ok)(void *user);
    void *user;
} s_cfm;

/*
 * 待弹出的确认请求。**必须拷一份参数**：调用方常常传栈上的临时缓冲
 * （例如"删除「xxx」这条记录？"是就地 snprintf 出来的），而这个请求要
 * 等到手指抬起来才真正使用它们。
 */
static struct {
    char  title[32];
    char  msg[192];
    char  ok_text[16];
    char  cancel_text[16];
    bool  danger;
    void (*on_ok)(void *user);
    void *user;
    bool  pending;
} s_req;

static void cfm_deleted(lv_event_t *e)
{
    lv_obj_t *scrim = (lv_obj_t *)lv_event_get_target(e);
    if (s_cfm.dlg.scrim == scrim) {
        memset(&s_cfm, 0, sizeof(s_cfm));
    }
}

static void cfm_ok_clicked(lv_event_t *e)
{
    (void)e;
    void (*cb)(void *) = s_cfm.ok;
    void *user = s_cfm.user;
    ui_ba_close(&s_cfm.dlg);        /* 先把框收掉，再执行动作（动作可能再弹框） */
    if (cb) {
        cb(user);
    }
}

static void cfm_cancel_clicked(lv_event_t *e)
{
    (void)e;
    ui_ba_close(&s_cfm.dlg);
}

void ui_ba_confirm_close(void)
{
    s_req.pending = false;          /* 还没弹出来的那一次也一起取消 */
    ui_ba_close(&s_cfm.dlg);
}

bool ui_ba_confirm_is_open(void)
{
    return s_cfm.dlg.scrim != NULL || s_req.pending;
}

/* 真正把确认框建出来（只在手指离开屏幕之后被调，见 ui_ba_confirm_ex） */
static void cfm_build_from_req(void)
{
    s_cfm.dlg = ui_ba_open(s_req.title[0] ? s_req.title : "通 知", 54, LV_ALIGN_CENTER, 0);
    s_cfm.ok   = s_req.on_ok;
    s_cfm.user = s_req.user;

    lv_obj_add_event_cb(s_cfm.dlg.scrim, cfm_deleted, LV_EVENT_DELETE, NULL);
    ui_ba_scrim_dismissible(&s_cfm.dlg, true);   /* 点空白 = 取消 */

    ui_ba_text(s_cfm.dlg.body, s_req.msg);

    ui_ba_btn_styled(s_cfm.dlg.btn_row, s_req.cancel_text[0] ? s_req.cancel_text : "取消",
                     BA_STYLE_CANCEL, cfm_cancel_clicked, NULL);
    ui_ba_btn_styled(s_cfm.dlg.btn_row, s_req.ok_text[0] ? s_req.ok_text : "确认",
                     s_req.danger ? BA_STYLE_DANGER : BA_STYLE_CONFIRM, cfm_ok_clicked, NULL);
}

/*
 * ★ 二次确认**不能**在按压期间建浮层 —— 这是 AGENTS §14.6 记的那个坑：
 *   LVGL 在按压期间会不断重新命中手指下的对象（lv_indev.c 的 pointer_search_obj，
 *   行对象没有 PRESS_LOCK 就会被换掉），而松手时 indev_proc_release() 会
 *   **无条件**补发一个 CLICKED（只跳过 short_click）。于是"长按列表行 → 弹确认框"
 *   这条路上，那个补发的 CLICKED 会落到刚建好的确认框上：
 *     - 落在遮罩上 → "点空白关闭"把确认框自己关掉（用户看到一闪而过）；
 *     - 落在按钮上 → **当场执行**那个动作，最坏是没确认就把记录删了。
 *   所以这里和 app_online 的长按菜单用同一招：先把请求参数存下来，
 *   用 lv_async_call 每轮查一次手指状态，**等手指抬起来**再真正建浮层。
 *   从 CLICKED 里调用的场合手指已经抬起，这个循环一轮就过，没有额外延迟。
 */
static void cfm_open_deferred(void *arg)
{
    (void)arg;
    if (!s_req.pending) {
        return;
    }
    const lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lv_async_call(cfm_open_deferred, NULL);   /* 还按着：下一轮再看 */
        return;
    }
    s_req.pending = false;
    cfm_build_from_req();
}

void ui_ba_confirm_ex(const char *title, const char *msg, const char *ok_text,
                      const char *cancel_text, bool danger,
                      void (*on_ok)(void *user), void *user)
{
    ui_ba_close(&s_cfm.dlg);        /* 同一时间只留一个（已经弹出来的立刻收掉） */

    /* 参数要**拷一份**：msg 常常是调用方栈上的临时缓冲，而这里要延后到松手才用 */
    snprintf(s_req.title, sizeof(s_req.title), "%s", (title && title[0]) ? title : "通 知");
    snprintf(s_req.msg, sizeof(s_req.msg), "%s", msg ? msg : "");
    snprintf(s_req.ok_text, sizeof(s_req.ok_text), "%s", (ok_text && ok_text[0]) ? ok_text : "确认");
    snprintf(s_req.cancel_text, sizeof(s_req.cancel_text), "%s",
             (cancel_text && cancel_text[0]) ? cancel_text : "取消");
    s_req.danger = danger;
    s_req.on_ok  = on_ok;
    s_req.user   = user;

    if (s_req.pending) {
        return;                     /* 已经排了一次，新的参数直接覆盖，别排第二条 */
    }
    s_req.pending = true;
    lv_async_call(cfm_open_deferred, NULL);
}

void ui_ba_confirm(const char *title, const char *msg, const char *ok_text,
                   void (*on_ok)(void *user), void *user)
{
    ui_ba_confirm_ex(title, msg, ok_text, "取消", false, on_ok, user);
}
