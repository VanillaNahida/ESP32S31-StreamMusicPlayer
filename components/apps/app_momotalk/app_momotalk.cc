/*
 * app_momotalk.cc —— MomoTalk 聊天页（碧蓝档案里那个聊天软件）
 *
 * 需求与实现对应（2026-10 用户提）：
 *   1. 桌面图标用 assets/MomoTalk.png          → ui_icons.c 里的 momotalk_logo
 *   2. 页面照 assets/MomoTalk.html             → 粉色标题栏 + 左对方/右我方气泡
 *   3. 右上角那个 SVG 的 ✕ 保留                → 用 prepare_sources.ps1 把那段内联
 *      SVG 重绘成 PNG，再烘成 momotalk_close（见 ui_icons.h）
 *   4. 左上角图标用 assets/MomoIcon.png        → 它其实是 ICO，转成 momotalk_icon
 *   5. 左侧发送人默认头像用 assets/default.png → 它其实是 JPEG，转成 momotalk_avatar
 *      （居中裁方 + 圆形裁切显示）；**我方没有头像**（与 HTML 一致）
 *   6. 下方输入框，位置跟着输入法往上走      → 页面是 flex 列，键盘一开就把
 *      screen 的 pad_bottom 顶到键盘高度，flex 自己把输入条抬到键盘上方
 *   7. 输入框样式 [ 点击输入 ] [ 发送 ]        → 浅灰圆角输入框 + 蓝色"发送"按钮
 *   8. 发出去的消息由对方回一句"收到消息：xxx" → 延时 420ms 的 echo（初期测试用）
 *
 * 层级与返回：
 *   本页**自带**返回钮（右上角 ✕），所以描述符里带 APP_FLAG_NO_BACK，
 *   由路由决定不叠那个全局返回键（否则右上/左上会各有一个返回入口）。
 *
 * 字体：页面用的中文都在 GB2312 内（工具校验过），走 UI_FONT_TEXT 就不会缺字。
 */
#include "app_momotalk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_app_registry.h"
#include "ui_icons.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "voice_service.h"       /* 语音输入宿主：WS + 麦克风接上这把键盘 */

static const char *TAG = "app_momotalk";

/* ------------------------------------------------------------------ 尺寸
 *
 * 全部相对 UI_W / UI_H，不写死 800/480（工程规范：UI 里不准出现分辨率字面量）。
 * 缩放基准：HTML 是按 1080 宽的手机画的大字号（气泡正文 32px），本机 UI 字体是
 * 16px，所以整体按约 0.5 倍取尺寸。
 */
#define MT_BAR_H        UI_PX_H(50)      /* 粉色标题栏高 */
#define MT_BAR_LOGO     UI_PX_H(32)      /* 标题栏左侧圆形图标（= momotalk_icon 烘制尺寸） */
#define MT_CLOSE_TOUCH  UI_MIN_TOUCH     /* 右上角 ✕ 的可点区域（位图尺寸由烘制尺寸决定 = momotalk_close） */
#define MT_INPUTBAR_H   UI_PX_H(52)      /* 底部输入条高 */
#define MT_TA_H         UI_PX_H(36)      /* 输入框高 */
#define MT_SEND_W       UI_PX_W(76)      /* "发送"按钮宽 */
#define MT_AVATAR       UI_PX_H(44)      /* 头像直径（= momotalk_avatar 烘制尺寸） */
/*
 * 气泡小尖角。
 *
 * ★ 尖角对象的高度 = MT_TAIL_OFF + MT_TAIL_H，三角**画在对象内部**、
 *   从 y1+MT_TAIL_OFF 开始，而不是靠 translate_y 把整个对象往下挪 ——
 *   靠 translate 的话"对象的实际位置"和"draw 回调里 lv_obj_get_coords()
 *   拿到的位置"是不是同一个值，取决于 LVGL 把 translate 记在坐标里还是在
 *   绘制时叠加；一旦两者不一致，画出来的三角就比气泡高出一截、
 *   正好扎进气泡的圆角里（现象就是"三角形没贴合"）。
 *   把偏移烘进对象高度 + 画点里，这条依赖就没了。
 *
 * ★ MT_TAIL_OFF 取**气泡圆角半径**（MT_BUBBLE_RADIUS）：圆角那一段气泡的左边缘
 *   是往里收的，尖角竖边只有落在圆角**以下**的直边上才严丝合缝。
 */
#define MT_TAIL_W       UI_PX_W(7)       /* 气泡小尖角宽 */
#define MT_TAIL_H       UI_PX_H(14)      /* 气泡小尖角高（竖边长度） */
#define MT_TAIL_OFF     UI_PX_H(8)       /* 尖角顶端相对气泡顶部的下沉（= 圆角半径） */
#define MT_TAIL_OVL     UI_PX_W(2)       /* 竖边往气泡里压进去这么多，盖掉 AA 缝 */
#define MT_BUBBLE_RADIUS UI_PX_H(8)      /* 气泡圆角（HTML 是 16px，半屏缩放后 8） */
#define MT_BUBBLE_PAD_X UI_PX_W(12)
#define MT_BUBBLE_PAD_Y UI_PX_H(8)
#define MT_BUBBLE_MAX_W UI_PCT_X(58)     /* 气泡最大宽度（HTML 是 620/1080 ≈ 57%） */
#define MT_IME_H        UI_PX_H(240)     /* 拼音键盘高度（与 app_online 的 IME_FIT_HEIGHT 一致） */
/* 键盘弹出/收起时"布局上移"的动画时长（ms）。太快像硬跳、太慢打字会等，
 * 220 ms + ease_out 接近 iOS/Android 聊天页的手感。 */
#define MT_PAD_ANIM_MS  220

/* ------------------------------------------------------------------ 配色（照 MomoTalk.html） */
#define MT_C_PINK        0xFD8AA1     /* 标题栏主题粉 */
#define MT_C_OTHER       0x4C5D71     /* 对方气泡：rgb(76,93,113) */
#define MT_C_MINE        0x4B8ACB     /* 我方气泡 / 发送按钮：rgb(75,138,203) */
#define MT_C_NAME        0x2B2B2B     /* 对方名字 */
#define MT_C_BUBBLE_FG   0xFFFFFF
#define MT_C_PAGE_BG     0xFFFFFF
#define MT_C_INPUT_BG    0xF2F3F5
#define MT_C_INPUT_FG    0x2B2B2B
#define MT_C_INPUT_DIM   0x9AA3AD     /* 占位文字"点击输入" */
#define MT_C_LINE        0xE5E7EB     /* 输入条上沿的分隔线 */

/* ------------------------------------------------------------------ 消息存储
 *
 * 静态数组、不 malloc：这是初期测试用的 echo 对话，条数封顶；上限到了就丢最旧的
 * 一条（整体前移），界面同步重建。文本按 UTF-8 字节截断（中文一个字 3 字节），
 * 截断时保证不切在半个字上（见 mt_text_copy）。
 */
#define MT_TEXT_MAX 192
#define MT_MSG_MAX  40

typedef struct {
    bool mine;
    char text[MT_TEXT_MAX];
} mt_msg_t;

static mt_msg_t s_msgs[MT_MSG_MAX];
static size_t   s_msg_n;

/* 对方的名字（照 MomoTalk.html 的示例） */
#define MT_PEER_NAME "百合园 圣娅"

/* ------------------------------------------------------------------ 页面状态 */
static lv_obj_t *s_scr;        /* 页面 screen */
static lv_obj_t *s_list;       /* 消息列表（可滚动） */
static lv_obj_t *s_ta;         /* 输入框 */
static lv_obj_t *s_send_btn;   /* "发送"按钮 */
static lv_obj_t *s_ime;        /* 拼音键盘（懒建，全局复用一份） */
static lv_obj_t *s_ime_box;    /* 键盘的容器（宽度决定按键大小） */
static lv_timer_t *s_echo_timer;
static char      s_echo_text[MT_TEXT_MAX];
static bool      s_page_alive;

/* 前向声明 */
static void mt_push(bool mine, const char *text);
static void mt_scroll_bottom(void);
static void mt_ime_hide(void);

/* ------------------------------------------------------------------ 小工具 */

/* 按 UTF-8 边界拷贝，避免把中文切成半个字（LONG_WRAP 会把半个字画成方框） */
static void mt_text_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
        /* 往回退到 UTF-8 起始字节（最高两位不是 10 的那个字节） */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ------------------------------------------------------------------ 气泡尖角
 *
 * HTML 用 CSS 的 border 三角（.bubble::before/::after）：一个竖直边贴在气泡上、
 * 另一头是个尖。这里同样用一个三角形画出来。
 *
 * 贴合的三个要点（少一个就会看到缝或者"扎进圆角里"）：
 *   1. 尖角对象与气泡之间**不留列间距**（brow 的 pad_column = 0），
 *      所以竖边就落在气泡的边缘上，不会中间空 1 px；
 *   2. 竖边再往气泡里压 MT_TAIL_OVL，盖掉抗锯齿留下的那圈半透明缝；
 *   3. 三角从对象顶部下沉 MT_TAIL_OFF（= 气泡圆角半径）开始画 ——
 *      圆角那一段气泡边缘是圆弧，尖角必须落在下面的直边上。
 */
static void mt_tail_draw(lv_event_t *e)
{
    lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!o || !layer) {
        return;
    }
    const bool left = ((intptr_t)lv_event_get_user_data(e) != 0);

    lv_area_t c;
    lv_obj_get_coords(o, &c);
    if (lv_area_get_width(&c) < 2 || lv_area_get_height(&c) < MT_TAIL_OFF + 4) {
        return;
    }

    const int32_t yt = c.y1 + MT_TAIL_OFF;          /* 竖边顶端 */
    const int32_t yb = yt + MT_TAIL_H;              /* 竖边底端 */

    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.opa   = LV_OPA_COVER;
    d.color = lv_color_hex(left ? MT_C_OTHER : MT_C_MINE);
    if (left) {
        /* 贴对方气泡的**左**边：竖边在右（压进气泡 MT_TAIL_OVL），尖朝左 */
        d.p[0].x = c.x2 + MT_TAIL_OVL; d.p[0].y = yt;
        d.p[1].x = c.x2 + MT_TAIL_OVL; d.p[1].y = yb + 1;
        d.p[2].x = c.x1;               d.p[2].y = (yt + yb) / 2;
    } else {
        /* 贴我方气泡的**右**边：竖边在左（压进气泡 MT_TAIL_OVL），尖朝右 */
        d.p[0].x = c.x1 - MT_TAIL_OVL; d.p[0].y = yt;
        d.p[1].x = c.x1 - MT_TAIL_OVL; d.p[1].y = yb + 1;
        d.p[2].x = c.x2;               d.p[2].y = (yt + yb) / 2;
    }
    lv_draw_triangle(layer, &d);
}

static lv_obj_t *mt_tail_create(lv_obj_t *parent, bool attach_left)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    /* 高度留出"下沉 + 竖边"两段：三角画在对象内部，不靠 translate 位移 */
    lv_obj_set_size(t, MT_TAIL_W, MT_TAIL_OFF + MT_TAIL_H + 1);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t, mt_tail_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)(attach_left ? 1 : 0));
    return t;
}

/* ------------------------------------------------------------------ 气泡 */

static lv_obj_t *mt_bubble_create(lv_obj_t *parent, bool mine, const char *text)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, lv_color_hex(mine ? MT_C_MINE : MT_C_OTHER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, MT_BUBBLE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, MT_BUBBLE_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(b, MT_BUBBLE_PAD_Y, LV_PART_MAIN);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);   /* 让点击落到列表上（收键盘用） */

    /*
     * 气泡要"短的贴身、长的换行"，但 LVGL 的 LV_SIZE_CONTENT 不会换行、
     * 给死宽度又会让短消息撑成一大条。所以先按最大宽度量一次换行后的包围盒，
     * 再把 label 的宽度定成量出来的宽度 —— 换行点与刚才那次测量一致，
     * 父容器 LV_SIZE_CONTENT 自然就贴着文字。
     */
    const int32_t max_text_w = MT_BUBBLE_MAX_W - 2 * MT_BUBBLE_PAD_X;
    lv_point_t sz = { 0, 0 };
    lv_text_get_size(&sz, text, UI_FONT_TEXT, 0, UI_PX_H(2), max_text_w, LV_TEXT_FLAG_NONE);
    if (sz.x < 1) {
        sz.x = 1;
    }

    lv_obj_t *lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(MT_C_BUBBLE_FG), LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, sz.x + 2);
    lv_label_set_text(lbl, text);
    return b;
}

/* ------------------------------------------------------------------ 一行消息 */

static lv_obj_t *mt_row_create(bool mine, const char *text)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* 我方整行靠右，对方靠左（HTML 里是 flex-direction: row-reverse） */
    lv_obj_set_flex_align(row, mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, UI_PX_W(8), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* 同上：让点击落到列表 */

    if (mine) {
        /* 我方：只有气泡 + 右侧尖角，**没有头像**（需求原话） */
        lv_obj_t *brow = lv_obj_create(row);
        lv_obj_remove_style_all(brow);
        lv_obj_set_size(brow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(brow, 0, LV_PART_MAIN);   /* 尖角要贴着气泡，不能留缝 */
        lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(brow, LV_OBJ_FLAG_CLICKABLE);
        mt_bubble_create(brow, true, text);
        mt_tail_create(brow, false);          /* 尖角在气泡右边 */
        return row;
    }

    /* 对方：圆头像 +（名字 / 气泡）一列 */
    lv_obj_t *av = lv_obj_create(row);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, MT_AVATAR, MT_AVATAR);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(av, lv_color_hex(0xD9E6F2), LV_PART_MAIN);   /* HTML 的占位底色 */
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, LV_PART_MAIN);
    /* ★ 圆形裁切：clip_corner 会把子对象压到圆角里，头像是方的也能显示成圆的 */
    lv_obj_set_style_clip_corner(av, true, LV_PART_MAIN);
    lv_obj_set_style_pad_all(av, 0, LV_PART_MAIN);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *im = lv_image_create(av);
    lv_image_set_src(im, &momotalk_avatar);     /* 尺寸已烘成 MT_AVATAR，别让 LVGL 缩放 */
    lv_obj_center(im);
    lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, UI_PX_H(2), LV_PART_MAIN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(col);
    lv_obj_set_style_text_font(name, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(MT_C_NAME), LV_PART_MAIN);
    lv_label_set_text(name, MT_PEER_NAME);

    lv_obj_t *brow = lv_obj_create(col);
    lv_obj_remove_style_all(brow);
    lv_obj_set_size(brow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(brow, 0, LV_PART_MAIN);   /* 尖角要贴着气泡，不能留缝 */
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_CLICKABLE);
    mt_tail_create(brow, true);                  /* 尖角在气泡左边 */
    mt_bubble_create(brow, false, text);
    return row;
}

/* ------------------------------------------------------------------ 新消息入场动画
 *
 * 需求："新气泡滑入 + 淡入"，120 ms。
 *
 * 只对**新追加**的那一条做（`mt_push`）。进页面重建整个列表（`mt_rebuild`）时不做 ——
 * 40 条一起淡入看着像"页面在闪"，而不是"来了条消息"。
 *
 * 做法：两个并行动画（LVGL 一个动画只驱动一个属性，同 var 不同 exec_cb 可以共存）：
 *   - `translate_y`：+MT_ANIM_RISE px → 0（从下方"升"到位，聊天列表的自然方向）
 *   - `opa`：0 → COVER（容器的不透明度会**带着子对象一起**淡入）
 *
 * ⚠ 三个细节：
 *   1. **先把起始值写死再起动画**：不写的话，从"创建对象"到"动画第一次执行"之间
 *      会有一帧是全不透明的，看起来是"闪一下再淡入"。
 *   2. 结束值就是默认值（0 / COVER），动画跑完不留任何视觉残留。
 *   3. 对象中途被删（换页 / 列表重建）时 LVGL 会自动删掉挂在它身上的动画，不用自己收尾。
 *   想改成"我方从右、对方从左"的横向滑入，把 translate_y 换成 translate_x
 *   并按 mine 给正负号即可。
 */
#define MT_ANIM_MS     120
#define MT_ANIM_RISE   UI_PX_H(10)

static void mt_anim_translate_y(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, LV_PART_MAIN);
}

static void mt_anim_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void mt_row_animate_in(lv_obj_t *row)
{
    if (row == NULL) {
        return;
    }
    /* 先落起始状态（见上面第 1 条），再起两条并行动画 */
    lv_obj_set_style_translate_y(row, MT_ANIM_RISE, LV_PART_MAIN);
    lv_obj_set_style_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, row);
    lv_anim_set_exec_cb(&a, mt_anim_translate_y);
    lv_anim_set_values(&a, MT_ANIM_RISE, 0);
    lv_anim_set_duration(&a, MT_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, row);
    lv_anim_set_exec_cb(&a, mt_anim_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, MT_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void mt_scroll_bottom(void)
{
    if (!s_list) {
        return;
    }
    /* 内容刚变过：先跑一次布局，否则算出来的底部位置还是旧的 */
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_OFF);
}

/* 往列表里追加一条（同时记进历史） */
static void mt_push(bool mine, const char *text)
{
    if (!s_page_alive || !s_list || !text || !text[0]) {
        return;
    }
    if (s_msg_n >= MT_MSG_MAX) {
        /* 满了：丢掉最旧的一条，整体前移（测试功能，不做滚动窗口） */
        memmove(&s_msgs[0], &s_msgs[1], sizeof(s_msgs[0]) * (MT_MSG_MAX - 1));
        s_msg_n = MT_MSG_MAX - 1;
    }
    s_msgs[s_msg_n].mine = mine;
    mt_text_copy(s_msgs[s_msg_n].text, sizeof(s_msgs[s_msg_n].text), text);
    s_msg_n++;

    lv_obj_t *row = mt_row_create(mine, s_msgs[s_msg_n - 1].text);
    /* 先贴到底、再起入场动画：`translate_y` 是绘制期变换，不参与 flex 布局，
     * 所以滚动位置按"动画跑到终点的样子"算，动画期间不会抖。 */
    mt_scroll_bottom();
    mt_row_animate_in(row);
}

/* 重画整个列表（进页面时用；消息数封顶 40，重建一次开销很小） */
static void mt_rebuild(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_msg_n; i++) {
        mt_row_create(s_msgs[i].mine, s_msgs[i].text);
    }
    mt_scroll_bottom();
}

/* ------------------------------------------------------------------ 输入法
 *
 * 与 app_online 的搜索页同一套（lvgl_pinyin_ime，键盘只建一次、挂在 lv_layer_top）；
 * 差别只有一处：键盘一开就把 screen 的 pad_bottom 顶到键盘高度，
 * flex 自动把输入条抬到键盘正上方 —— 打字时输入框和最新消息都看得见。
 *
 * ★ 2026-10：这个"顶上去"改成**动画**（原来一步到位，观感是"啪"地跳一下）。
 *   动画只改 screen 的 pad_bottom：
 *     - flex 每帧重算 → 消息区逐渐变矮、输入条与最新消息平滑上移；
 *     - 每一帧都把消息列表贴到底（聊天页在键盘弹起时的标准手感：最新一条
 *       始终贴着输入框，而不是被顶出可视区）；
 *     - 收键盘是反向动画，结束后再贴一次底。
 *   为什么不用 LVGL 的 style transition：transition 只在"状态/局部样式属性变化"
 *   时触发，时机和 `lv_obj_update_layout` 不好配合，也不能保证每帧重贴底；
 *   直接 `lv_anim` 改样式属性更直白，快速"开→关"打断也只是删掉旧动画。
 */

/** 动画每帧：改 pad_bottom → 重算布局 → 贴底 */
static void mt_pad_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_pad_bottom((lv_obj_t *)var, v, LV_PART_MAIN);
    /*
     * ⚠ 必须 `lv_obj_update_layout(s_scr)`（父对象），不能只更新列表：
     *   决定列表高度的是 screen 的 pad/flex，父对象不重排的话滚动上限还是旧的，
     *   贴底就会停在"半中间"（原实现里也专门写了这一条）。
     */
    lv_obj_update_layout(s_scr);
    if (s_list) {
        lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/** 动画跑完：再贴一次底，兜住最后一帧取整带来的几像素差 */
static void mt_pad_anim_done(lv_anim_t *a)
{
    (void)a;
    if (s_scr) {
        lv_obj_update_layout(s_scr);
    }
    if (s_list) {
        lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void mt_ime_apply_offset(bool open)
{
    if (!s_scr) {
        return;
    }
    const int32_t target = open ? (MT_IME_H + UI_PX_H(6)) : 0;
    const int32_t from = lv_obj_get_style_pad_bottom(s_scr, LV_PART_MAIN);

    /* 打断上一次（用户可能飞快地"开→关"）：同 var+exec_cb 的动画只留一个 */
    lv_anim_delete(s_scr, mt_pad_anim_cb);

    if (from == target) {
        mt_pad_anim_done(NULL);         /* 已经在目标位置：只把底贴好 */
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_scr);
    lv_anim_set_exec_cb(&a, mt_pad_anim_cb);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_duration(&a, MT_PAD_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, mt_pad_anim_done);
    lv_anim_start(&a);
}

static void mt_ime_hide(void)
{
    if (s_ime) {
        lv_pinyin_ime_hide(s_ime);
    }
    mt_ime_apply_offset(false);
}

static void mt_send(void);

/* 拼音键盘上的对勾 / 回车：没有未上屏拼音时组件发这个事件 → 直接发送 */
static void mt_ime_ready(lv_event_t *e)
{
    (void)e;
    mt_send();
}

static void mt_ime_show(void)
{
    if (!s_ta) {
        return;
    }
    if (!s_ime) {
        s_ime_box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_ime_box);
        /*
         * ★ 宽度必须给**绝对像素**：组件的按键尺寸是 create() 时按父对象宽度
         *   算一次的（可用宽度 / 12）。给百分比 + 刚建出来的对象还没跑 layout，
         *   lv_obj_get_width() 返回 0 → 组件退化成"按 320 px 算"，键位特别小。
         *   所以先给满屏宽、强制跑一次 layout，再建键盘（同 app_online）。
         */
        lv_obj_set_width(s_ime_box,
                         lv_display_get_horizontal_resolution(NULL) - 2 * UI_PX_W(8));
        lv_obj_set_height(s_ime_box, LV_SIZE_CONTENT);
        lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(2));
        lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_update_layout(s_ime_box);

        lv_pinyin_ime_set_fit_height(MT_IME_H);
        s_ime = lv_pinyin_ime_create(s_ime_box);
        if (!s_ime) {
            ESP_LOGW(TAG, "pinyin ime create failed (dict 分区烧了吗？)");
            ui_toast("拼音输入法不可用", 2500);
            lv_obj_delete(s_ime_box);
            s_ime_box = NULL;
            return;
        }
        lv_obj_add_event_cb(s_ime, mt_ime_ready, lv_pinyin_ime_event_ready(), NULL);
    }
    lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(2));
    lv_pinyin_ime_attach(s_ime, s_ta);
    /* ★ 语音输入宿主（WebSocket + 板载麦克风）；必须在 attach 之后，见 voice_service.cc */
    voice_service_attach_ime(s_ime);
    lv_pinyin_ime_show(s_ime);
    /* 键盘挂在 lv_layer_top 上、只建一次：提到最前，保证它压在页面之上 */
    lv_obj_move_foreground(s_ime_box);
    mt_ime_apply_offset(true);
}

/* ------------------------------------------------------------------ 交互 */

static void mt_send(void)
{
    if (!s_ta) {
        return;
    }
    const char *txt = lv_textarea_get_text(s_ta);
    if (!txt || !txt[0]) {
        ui_toast("先输入点什么吧", 1200);
        return;
    }

    char msg[MT_TEXT_MAX];
    mt_text_copy(msg, sizeof(msg), txt);

    mt_push(true, msg);
    lv_textarea_set_text(s_ta, "");

    /* 对方回一句"收到消息：xxx"（初期测试用的 echo） */
    snprintf(s_echo_text, sizeof(s_echo_text), "收到消息：%.*s",
             MT_TEXT_MAX - 32, msg);
    if (s_echo_timer) {
        lv_timer_delete(s_echo_timer);
    }

    ESP_LOGI(TAG, "发送的原始消息: %s", msg);

    /* 延时一下，先让用户看到自己那条再冒出回复 */
    s_echo_timer = lv_timer_create([](lv_timer_t *t) {
        (void)t;
        s_echo_timer = NULL;
        mt_push(false, s_echo_text);
    }, 420, NULL);
    lv_timer_set_repeat_count(s_echo_timer, 1);
}

static void mt_send_clicked(lv_event_t *e)
{
    (void)e;
    mt_send();
}

static void mt_ta_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        mt_ime_show();
    } else if (code == LV_EVENT_CLICKED) {
        /* 点输入框任何位置都保证键盘是开的（有时焦点已经在框里，不会再发 FOCUSED） */
        mt_ime_show();
    }
    /*
     * ★ 绝不在 LV_EVENT_DEFOCUSED 里收键盘：点键盘按键会把焦点转到按键对象上，
     *   输入框随即收到 DEFOCUSED —— 在那儿收键盘就是"按第一个键键盘就没了"
     *   （app_online 踩过这个坑）。收键盘只有两个入口：点消息区 / 离开页面。
     */
}

static void mt_list_clicked(lv_event_t *e)
{
    (void)e;
    mt_ime_hide();
}

static void mt_close_clicked(lv_event_t *e)
{
    (void)e;
    mt_ime_hide();
    ui_router_pop();
}

/* ------------------------------------------------------------------ 页面 */

static lv_obj_t *mt_build_titlebar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), MT_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(MT_C_PINK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, UI_PX_W(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左上角圆形小图标（HTML: .logo 50x50 + border-radius:50%） */
    lv_obj_t *logo_box = lv_obj_create(bar);
    lv_obj_remove_style_all(logo_box);
    lv_obj_set_size(logo_box, MT_BAR_LOGO, MT_BAR_LOGO);
    lv_obj_set_style_radius(logo_box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(logo_box, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(logo_box, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(logo_box, LV_OPA_20, LV_PART_MAIN);
    lv_obj_remove_flag(logo_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(logo_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *logo = lv_image_create(logo_box);
    lv_image_set_src(logo, &momotalk_icon);
    lv_obj_center(logo);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(title, 2, LV_PART_MAIN);
    lv_label_set_text(title, "MomoTalk");

    /* 撑开中间，把 ✕ 顶到最右（HTML 的 margin-left:auto） */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    /* 右上角退出 ✕：位图就是 MomoTalk.html 里那段内联 SVG 转出来的，
     * 可点区域比图大一圈（MT_CLOSE_TOUCH），和 HTML 里 44px 的 .close 一个意思 */
    lv_obj_t *close = lv_obj_create(bar);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, MT_CLOSE_TOUCH, MT_CLOSE_TOUCH);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close, UI_PX_W(4));
    lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close, mt_close_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_image_create(close);
    lv_image_set_src(x, &momotalk_close);
    lv_obj_center(x);
    lv_obj_remove_flag(x, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static lv_obj_t *mt_build_inputbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), MT_INPUTBAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(MT_C_PAGE_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, UI_PX_W(8), LV_PART_MAIN);
    /* 上沿一条细线，把输入区和消息区分开 */
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(MT_C_LINE), LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* [ 点击输入 ] —— 点击后弹拼音键盘 */
    s_ta = lv_textarea_create(bar);
    lv_obj_set_height(s_ta, MT_TA_H);
    lv_obj_set_flex_grow(s_ta, 1);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(MT_C_INPUT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ta, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ta, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_ta, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_ta, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(MT_C_INPUT_FG), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(MT_C_INPUT_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(MT_C_MINE), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, MT_TEXT_MAX - 1);
    lv_textarea_set_placeholder_text(s_ta, "点击输入");
    lv_obj_add_event_cb(s_ta, mt_ta_event, LV_EVENT_ALL, NULL);

    /* [ 发送 ] —— 蓝色，和我方气泡同色 */
    s_send_btn = lv_obj_create(bar);
    lv_obj_remove_style_all(s_send_btn);
    lv_obj_set_size(s_send_btn, MT_SEND_W, MT_TA_H);
    lv_obj_set_style_bg_color(s_send_btn, lv_color_hex(MT_C_MINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_send_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_send_btn, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_send_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_send_btn, mt_send_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_send_btn);
    lv_obj_set_style_text_font(sl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(sl, 1, LV_PART_MAIN);
    lv_label_set_text(sl, "发送");
    lv_obj_center(sl);
    return bar;
}

static void mt_on_delete(lv_event_t *e)
{
    (void)e;
    s_page_alive = false;
    /* 布局上移动画可能还在跑：先删掉，否则动画回调会去碰已销毁的 screen/列表 */
    if (s_scr) {
        lv_anim_delete(s_scr, mt_pad_anim_cb);
    }
    if (s_echo_timer) {
        lv_timer_delete(s_echo_timer);
        s_echo_timer = NULL;
    }
    /* 键盘挂在 lv_layer_top 上、不归本页所有：只收起来，不删（下次进页面复用，
     * 删了要重新加载词库，卡一秒）。
     * ★ 但**必须解绑**：组件只是把 textarea 指针存进 ctx（`ctx->ta`），
     *   而本页的输入框会随页面销毁 —— 不解绑就留下一个悬垂指针
     *   （同 app_online 那个"添加歌单浮层连续开关 → 渲染崩"的根因，见 ao_overlay.cc）。
     *   这里趁 textarea 还活着解绑（页面 DELETE 回调在子对象被删之前触发）。 */
    if (s_ime) {
        lv_pinyin_ime_attach(s_ime, NULL);
        lv_pinyin_ime_hide(s_ime);
    }
    s_scr = NULL;
    s_list = NULL;
    s_ta = NULL;
    s_send_btn = NULL;
}

static lv_obj_t *page_momotalk(void)
{
    s_page_alive = true;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(MT_C_PAGE_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
    /* 状态栏（常驻、在 lv_layer_top 上）高 UI_STATUSBAR_H：让开它，别把标题栏压在下面 */
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H, LV_PART_MAIN);
    /* 键盘弹出时这里会被顶到键盘高度，输入条就跟着抬到键盘上面（见 mt_ime_apply_offset） */
    lv_obj_set_style_pad_bottom(scr, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_scr = scr;

    mt_build_titlebar(scr);

    /* 消息列表：占满中间，竖向滚动；点空白处收键盘 */
    s_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_list, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_list, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, UI_PX_H(14), LV_PART_MAIN);   /* HTML 的 gap:56px → 半屏缩放 */
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_list, mt_list_clicked, LV_EVENT_CLICKED, NULL);

    mt_build_inputbar(scr);

    /* 首次进来塞两条示例（和 MomoTalk.html 里的两段占位文本一致） */
    if (s_msg_n == 0) {
        mt_text_copy(s_msgs[0].text, sizeof(s_msgs[0].text), "测试文本");
        s_msgs[0].mine = false;
        mt_text_copy(s_msgs[1].text, sizeof(s_msgs[1].text), "文本测试");
        s_msgs[1].mine = true;
        s_msg_n = 2;
    }
    mt_rebuild();

    lv_obj_add_event_cb(scr, mt_on_delete, LV_EVENT_DELETE, NULL);
    ESP_LOGI(TAG, "momotalk page created (%u message(s))", (unsigned)s_msg_n);
    return scr;
}

/* ------------------------------------------------------------------ 注册 */

void app_momotalk_register_apps(void)
{
    static const app_desc_t momotalk = {
        "momotalk", "MomoTalk", LV_SYMBOL_ENVELOPE, 0xFD8AA1,
        page_momotalk, NULL, NULL,
        25,                       /* 排在"网易云音乐"(20) 与"本地音乐"(30) 之间 */
        APP_FLAG_NO_BACK,         /* 自带右上角 ✕ 退出：别再叠一个全局返回键 */
        &momotalk_logo,           /* 桌面图标用 assets/MomoTalk.png 烘的位图 */
    };
    ui_app_register(&momotalk);
}
