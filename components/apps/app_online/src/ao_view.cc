/*
 * ao_view.cc —— 歌单页的两种版式（列表 / 详情）实现，见 ao_view.h 的结构图
 *
 * 坐标全部来自设计稿（800x480 参考像素）：
 *   列表模式：hint (54,16)；清单 (42,88) 704 宽
 *   详情模式：左列 (44,4) 184 宽；清单 (264,0) 496 宽
 *   左列内部：封面 184x184（圆角 18）、歌单名 y=216、副标题 y=238、播放全部 y=264
 */
#include "ao_view.h"

#include <string.h>

#include "ui_metrics.h"

#include "ao_pl_cover.h"
#include "ao_style.h"

#define D_X(n) UI_PX_W(n)
#define D_Y(n) UI_PX_H(n)

/* 清单在两种模式下的位置与尺寸（页容器内的坐标） */
#define LIST_X_LIST    D_X(42)
#define LIST_Y_LIST    D_Y(88)
#define LIST_W_LIST    D_X(704)
#define LIST_H_LIST    (UI_H - D_Y(206))

#define LIST_X_DETAIL  D_X(264)
#define LIST_Y_DETAIL  0
#define LIST_W_DETAIL  D_X(496)
#define LIST_H_DETAIL  (UI_H - D_Y(88))

#define LEFT_X         D_X(44)
#define LEFT_Y         D_Y(4)
#define LEFT_W         D_X(184)
/* 左列按 flex 竖排（封面 → 歌单名 → 副标题 → 播放全部），高度给足：
 * 歌单名换行 / 字号变大时，下面的东西整体下移，不会互相压住。
 * 底边 4+330=334 仍在屏幕内，也不会碰到别的东西（清单在右边）。 */
#define LEFT_H         D_Y(330)

#define COVER_SIDE     D_Y(184)
#define COVER_R        D_Y(18)

/*
 * 内置默认封面（assets/defalut_cover.png）的边长 = 显示尺寸。
 * ⚠ 必须与 COVER_SIDE 一致：位图是**按显示尺寸烘的**（assets/cover/photos.txt +
 *   tools/icons/gen_photo.py），运行期不做缩放。
 */
#define PL_DEF_COVER_SIDE  128

extern "C" const uint8_t _binary_pl_default_cover_bin_start[];
extern "C" const uint8_t _binary_pl_default_cover_bin_end[];

/* 默认封面的图像描述符：只建一次，data 直接指 flash 里的烘好像素（零解码、零拷贝） */
static const lv_image_dsc_t *pl_default_cover_dsc(void)
{
    static lv_image_dsc_t dsc;
    static bool init;
    if (!init) {
        init = true;
        dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        dsc.header.w      = PL_DEF_COVER_SIDE;
        dsc.header.h      = PL_DEF_COVER_SIDE;
        dsc.header.stride = PL_DEF_COVER_SIDE * 2u;
        dsc.data_size = (uint32_t)(_binary_pl_default_cover_bin_end -
                                   _binary_pl_default_cover_bin_start);
        dsc.data = _binary_pl_default_cover_bin_start;
    }
    return &dsc;
}

/* 同一时刻只有一个页面处于详情模式：封面解好后统一回到"当前那个视图" */
static ao_view_t *s_active;

static void view_cover_notify(void)
{
    if (s_active) {
        ao_view_cover_apply(s_active);
    }
}

void ao_view_cover_apply(ao_view_t *v)
{
    if (!v || !v->cover_img) {
        return;
    }
    const lv_image_dsc_t *dsc = ao_pl_cover_dsc();
    const bool ok = (dsc != NULL) && (v->want_id > 0) &&
                    (ao_pl_cover_current_id() == v->want_id);
    if (ok) {
        /* 先换指针再显示：旧图由 ao_pl_cover 释放，这里不能留悬垂 src */
        lv_image_set_src(v->cover_img, dsc);
        lv_obj_remove_flag(v->cover_img, LV_OBJ_FLAG_HIDDEN);
        if (v->cover_def) {
            lv_obj_add_flag(v->cover_def, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        /* 还没解好（或解的是别的歌）/ 解失败：亮出内置默认封面 */
        lv_image_set_src(v->cover_img, NULL);
        lv_obj_add_flag(v->cover_img, LV_OBJ_FLAG_HIDDEN);
        if (v->cover_def) {
            lv_obj_remove_flag(v->cover_def, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ao_view_set_cover(ao_view_t *v, const online_song_t *first)
{
    if (!v) {
        return;
    }
    v->want_id = (first && first->id) ? (int)first->id : -1;
    if (v->want_id > 0) {
        ao_pl_cover_want(first, lv_tick_get());
    }
    ao_view_cover_apply(v);
}

static void build_left(ao_view_t *v)
{
    lv_obj_t *left = lv_obj_create(v->page);
    lv_obj_remove_style_all(left);
    lv_obj_set_pos(left, LEFT_X, LEFT_Y);
    lv_obj_set_size(left, LEFT_W, LEFT_H);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(left, LV_OBJ_FLAG_HIDDEN);
    /*
     * 左列改成**竖排 flex**：封面 → 歌单名 → 副标题 → 播放全部。
     *
     * 需求（2026-11 用户）："封面上不需要写文字，文字放在封面下面，且页数和歌单标题
     * 不要挤在一起，要自适应位置"。
     *   - 以前歌单名/副标题是写死 y=216 / y=240 的绝对坐标，22 px 的歌单名行高 ~30 px，
     *     正好和副标题贴在一起；
     *   - 现在由 flex 排版 + 各自的 margin_top 控制间距：歌单名占多高，副标题就从它下面
     *     接着排，永远不会压在一起。整列在左侧 44~228、顶部 y=4 处。
     */
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(left, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(left, 0, LV_PART_MAIN);

    /* ---- 封面卡片：**上面一个字都没有**，只有图 ---- */
    lv_obj_t *cover = lv_obj_create(left);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, COVER_SIDE, COVER_SIDE);
    ao_style_cover_card(cover, COVER_R);
    /*
     * 圆角靠 clip_corner 裁子对象：lv_image 自己的 radius 样式**不会裁图**
     * （LVGL 只按对象矩形贴图，圆角是背景的事）。真封面与内置默认封面都是它的子对象，
     * 于是两者的圆角**完全一致**（同一半径、同一裁切路径）——见下面的两个 lv_image。
     */
    lv_obj_set_style_clip_corner(cover, true, LV_PART_MAIN);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);

    /* 默认封面（还没有第一首歌的封面时显示）：assets/defalut_cover.png 烘的位图 */
    lv_obj_t *def = lv_image_create(cover);
    lv_obj_set_size(def, LV_PCT(100), LV_PCT(100));
    lv_obj_center(def);
    lv_image_set_src(def, pl_default_cover_dsc());
    /* 位图是 128x128（省 flash），卡片是 184x184 → 让它拉满。
     * 这张图是纯色底 + 白色音符，放大 1.44 倍看不出差别；真封面仍然走 ALIGN_COVER。 */
    lv_image_set_inner_align(def, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(def, LV_OBJ_FLAG_CLICKABLE);

    /* 真封面：RGB565，按 COVER 等比铺满（裁掉多余） */
    lv_obj_t *img = lv_image_create(cover);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    /* ---- 卡片下方：歌单名 / 副标题 / 播放全部 ---- */
    lv_obj_t *nm = lv_label_create(left);
    lv_obj_set_style_text_font(nm, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(nm, LV_PCT(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_margin_top(nm, D_Y(10), LV_PART_MAIN);

    lv_obj_t *ds = lv_label_create(left);
    lv_obj_set_style_text_font(ds, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(ds, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(ds, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_width(ds, LV_PCT(100));
    lv_label_set_long_mode(ds, LV_LABEL_LONG_DOT);
    lv_obj_set_style_margin_top(ds, D_Y(2), LV_PART_MAIN);

    /* 播放全部：128x36 的胶囊（白 14%），左侧三角 + 文案，设计稿口径 */
    lv_obj_t *pill = lv_obj_create(left);
    lv_obj_remove_style_all(pill);
    lv_obj_set_style_margin_top(pill, D_Y(12), LV_PART_MAIN);
    lv_obj_set_size(pill, D_X(128), D_Y(36));
    lv_obj_set_style_radius(pill, D_Y(18), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, 36, LV_PART_MAIN);       /* 14% ≈ 36/255 */
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *tri = lv_label_create(pill);
    lv_obj_set_style_text_font(tri, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(tri, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(tri, LV_SYMBOL_PLAY);
    lv_obj_set_pos(tri, D_X(16), D_Y(8));

    lv_obj_t *pl = lv_label_create(pill);
    lv_obj_set_style_text_font(pl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(pl, "播放全部");
    lv_obj_set_pos(pl, D_X(36), D_Y(6));

    v->left      = left;
    v->cover     = cover;
    v->cover_img = img;
    v->cover_def = def;
    v->name      = nm;
    v->desc      = ds;
    v->play_all  = pill;
}

void ao_view_init(ao_view_t *v, lv_obj_t *page)
{
    if (!v || !page) {
        return;
    }
    memset(v, 0, sizeof(*v));
    v->page   = page;
    v->want_id = -1;

    /* 说明行："歌单 · 14 个"（12 px 白 45%） */
    v->hint = ao_style_caption(page, "");
    lv_obj_set_pos(v->hint, D_X(54), D_Y(16));
    lv_obj_set_width(v->hint, D_X(692));
    lv_label_set_long_mode(v->hint, LV_LABEL_LONG_DOT);

    /* 清单：扁平行（无卡片底），行距 0 —— 设计稿靠行距本身分隔 */
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, lv_color_hex(AO_C_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, D_X(4), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, D_X(2), LV_PART_SCROLLBAR);
    v->list = list;

    build_left(v);
    ao_view_list_mode(v, "");
    ao_pl_cover_set_notify(view_cover_notify);
    /* 长歌名跑马灯：手指划清单时停、停下 2.4 s 后自己恢复（见 ao_style.cc） */
    ao_style_marquee_attach(v->list);
}

void ao_view_clear(ao_view_t *v)
{
    if (!v) {
        return;
    }
    if (s_active == v) {
        s_active = NULL;
    }
    ao_pl_cover_hold(false);
    /* 跑马灯的 timer 和登记表要收掉（⚠ 不能碰控件：走到这里时子对象已经销毁） */
    ao_style_marquee_detach_all();
    memset(v, 0, sizeof(*v));
    v->want_id = -1;
}

lv_obj_t *ao_view_list(ao_view_t *v)
{
    return v ? v->list : NULL;
}

lv_obj_t *ao_view_play_all(ao_view_t *v)
{
    return v ? v->play_all : NULL;
}

void ao_view_list_mode(ao_view_t *v, const char *hint_text)
{
    if (!v || !v->list) {
        return;
    }
    v->detail = false;
    if (s_active == v) {
        s_active = NULL;
    }
    /* 回列表了：封面的 60 s 闲置计时从这一刻开始（详情页开着时是 hold 的） */
    ao_pl_cover_hold(false);
    if (v->left) {
        lv_obj_add_flag(v->left, LV_OBJ_FLAG_HIDDEN);
    }
    if (v->hint) {
        lv_obj_remove_flag(v->hint, LV_OBJ_FLAG_HIDDEN);
        if (hint_text) {
            lv_label_set_text(v->hint, hint_text);
        }
    }
    lv_obj_set_pos(v->list, LIST_X_LIST, LIST_Y_LIST);
    lv_obj_set_size(v->list, LIST_W_LIST, LIST_H_LIST);
}

void ao_view_detail_mode(ao_view_t *v, const char *name, const char *desc)
{
    if (!v || !v->list) {
        return;
    }
    v->detail = true;
    s_active = v;
    /* 详情页开着：封面别被闲置释放收走（见 ao_pl_cover_hold 的说明） */
    ao_pl_cover_hold(true);
    if (v->hint) {
        lv_obj_add_flag(v->hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (v->left) {
        lv_obj_remove_flag(v->left, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(v->list, LIST_X_DETAIL, LIST_Y_DETAIL);
    lv_obj_set_size(v->list, LIST_W_DETAIL, LIST_H_DETAIL);

    if (v->name && name) {
        lv_label_set_text(v->name, name);
        lv_obj_set_style_text_font(v->name, ao_style_title_font(name), LV_PART_MAIN);
    }
    if (v->desc) {
        lv_label_set_text(v->desc, desc ? desc : "");
    }
    /*
     * ⚠ 封面上**不再写任何文字**（2026-11 用户）：歌单名/页码都只在卡片下方那两行里。
     *   以前这里还会把同样的文字塞进封面占位卡的 ph_title / ph_sub，现在删掉了。
     */
}
