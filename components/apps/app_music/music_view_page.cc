/*
 * music_view_page.cc —— 播放器页的**布局骨架**（视图层）
 */
#include "music_view_player.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "audio_service.h"   /* 建页时读一次"是不是在播"用于按钮图标（纯取数） */
#include "online_service.h"  /* 建页时从在线队列取标题/歌手 */
#include "storage_service.h" /* 建页时从曲目表取标题/歌手 */
#include "ui_core.h"        /* ui_screen_apply_default_bg（页面底色：连 opa 一起设） */
#include "ui_icons.h"        /* heart_on / heart_off（红心位图，tools/icons 烘出来的） */
#include "ui_metrics.h"

#include "music_cover.h"
#include "music_menu.h"      /* 播放器设置弹窗（FFT 开关 + 歌曲信息入口） */
#include "music_player.h"
#include "music_player_bg.h"
#include "music_spectrum.h"  /* 进度条上方的 FFT 频谱柱 */
#include "music_state.h"
#include "music_view_internal.h"
#include "music_view_lyric.h"
#include "music_view_output.h"

static const char *TAG = "app_music";

/* ---------------------------------------------------------------- 设计稿坐标
 *
 * 一律写设计稿里的参考像素，由 UI_PX_W/UI_PX_H 换算成实际像素。
 * 本板就是 800x480，所以数值上等于原值；换方向编译时按比例缩放。
 */
#define D_X(n) UI_PX_W(n)
#define D_Y(n) UI_PX_H(n)

#define COVER_X       D_X(64)
#define COVER_Y       D_Y(76)
#define COVER_W       D_X(210)
#define COVER_H       D_Y(210)
#define COVER_R       D_Y(18)
/*
 * 内置默认封面（assets/default_black_vinyl_record.png）的两个尺寸 —— **别再合成一个宏**。
 *
 *   COVER_BMP_SIDE  = 烘进 flash 的那张位图**本身的**边长，是**数据布局**的一部分：
 *                     header.w/h/stride 与"alpha 平面从 w*h*2 开始"都按它算。
 *                     改它必须重烘 .bin（assets/cover/photos.txt + gen_photo.py），
 *                     否则每行按错的跨距去读 → 整张图越读越偏，看着就是"斜切/错位"
 *                     （2026-11-24 踩过：把显示尺寸宏改小 3 px 就中招）。
 *   COVER_DEF_SIDE  = 显示边长（卡片 210，这里留边距）。它**只影响绘制**：
 *                     小于位图时由 lv_image 的 CONTAIN 等比缩到方框里，随便调。
 *
 * 188 这个数不是拍的：位图是"源图 488x459 补透明边凑成 488x488 后缩到 188"，
 * 唱片在源图里直径 ≈449 px，所以屏上直径 ≈449*188/488 ≈ 173 px —— 与上一版
 * （176 方图居中裁）的 172 px 一致：**大小/位置不变，但左右不再被裁**。
 */
#define COVER_BMP_SIDE  188
#define COVER_DEF_SIDE  188

/*
 * 占位卡片的底色：**浅粉灰**（#EDE9EB，用户定的一版；唱片四周的透明区就透出它）。
 * 沿革：设计稿是斜向蓝渐变 → 试过深黑 #0A0A0C → 现在是浅粉灰（当"唱片套"底色）。
 */
#define C_COVER_BG     lv_color_hex(0xEDE9EB)   /* #EDE9EB */

#define TITLE_X       D_X(64)
#define TITLE_Y       D_Y(306)
#define ARTIST_Y      D_Y(336)
#define TEXT_W        D_X(256)         /* 封面下方两行的可用宽度（歌词列从 336 起） */

#define LYRIC_X       D_X(336)
#define LYRIC_Y       D_Y(76)
#define LYRIC_W       D_X(400)
#define LYRIC_H       D_Y(252)         /* 中心 = 202，正好是设计稿当前行的位置 */

#define SPEC_Y        D_Y(344)         /* 频谱柱（可选功能）：夹在歌词与进度条之间 */
#define SPEC_H        D_Y(38)

#define SEEK_X        D_X(64)
#define SEEK_Y        D_Y(371)         /* 抓取区（含上下各 12 px 余量） */
#define SEEK_W        D_X(672)
#define SEEK_H        D_Y(30)
#define BAR_REL_Y     D_Y(13)          /* 条本体在抓取区里的 y（5 px 高，中心 15.5） */
#define BAR_H         D_Y(5)
#define KNOB_D        D_Y(12)          /* 设计稿 r=6 的白圆点 */
#define KNOB_REL_Y    D_Y(9)

#define TIMES_Y       D_Y(397)
/*
 * 控制排图标中心线：434 → **422**（2026-11 用户："下方控制按钮水平位置太靠屏幕下边界，
 * 请往上调整一点"）。44 px 命中区的下沿因此从 456 抬到 444，离屏底留出 36 px，
 * 与上方时间行（397..417）也仍然不重叠（控制排的 x 从 119 起，时间只占两头）。
 */
#define CTL_Y         D_Y(422)
#define CTL_BTN       D_Y(44)          /* 图标键命中区 */
#define PLAY_BTN      D_Y(48)          /* 中央播放键直径 */

/* 控制排各键的中心 x（照抄设计稿） */
#define CTL_INFO_X    D_X(141)
#define CTL_MODE_X    D_X(235)
#define CTL_PREV_X    D_X(325)
#define CTL_PLAY_X    D_X(400)
#define CTL_NEXT_X    D_X(472)
#define CTL_LIKE_X    D_X(561)
#define CTL_VOL_X     D_X(641)
#define CTL_OUT_X     D_X(688)

/* 设计稿的白色与几档不透明度（控制排两端是 65% 白） */
#define C_WHITE       lv_color_hex(0xFFFFFF)

/* 渠道胶囊的平台色见 music_view_player.h 的 MUSIC_SRC_COLOR_*（两处共用一份） */

/* 控制排最左边那个按钮（原来的 ⓘ"歌曲信息"）→ 打开"播放器设置"弹窗。
 * 需求（2026-11 用户）：点开是一个设置面板，里面放 FFT 开关与"歌曲信息"入口。 */
static void info_btn_clicked(lv_event_t *e)
{
    (void)e;
    music_menu_open();
}

/* ================================================================ 左栏：封面 */

/*
 * 内置默认封面（构建时烘进 flash 的 RGB565A8 像素，见 app_music/CMakeLists.txt 的
 * target_add_binary_data + assets/cover/photos.txt）。
 * 用带 alpha 的格式是为了保留透明：那张唱片四周是空的，透出卡片底色（现在是浅粉灰，
 * 见 C_COVER_BG）才像"唱片"，不透的话会是一个白色方块。选 rgb565a8 而不是 argb8888
 * 是省 1/4 flash（每像素 3 字节 vs 4，app 槽只剩 ~50 KB 余量）。
 */
extern "C" const uint8_t _binary_player_default_cover_bin_start[];
extern "C" const uint8_t _binary_player_default_cover_bin_end[];

static const lv_image_dsc_t *player_default_cover_dsc(void)
{
    static lv_image_dsc_t dsc;
    static bool init;
    if (!init) {
        init = true;
        const size_t len = (size_t)(_binary_player_default_cover_bin_end -
                                    _binary_player_default_cover_bin_start);
        const size_t want = (size_t)COVER_BMP_SIDE * COVER_BMP_SIDE * 3u;  /* RGB565A8 */
        if (len != want) {
            /* 这不是"能不能显示"的问题：w/h/stride 与数据对不上 = 每行错位（斜切）。
             * 真出现就是有人改了 COVER_BMP_SIDE 却没重烘 .bin。 */
            ESP_LOGE(TAG, "默认封面位图尺寸不符：%u 字节，期望 %u（%dx%d RGB565A8）——"
                          "改 COVER_BMP_SIDE 前先重烘 assets/cover/*.bin",
                     (unsigned)len, (unsigned)want, COVER_BMP_SIDE, COVER_BMP_SIDE);
        }
        dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        /* rgb565a8 = 先 RGB565 平面（stride = w*2）再 A8 平面 —— LVGL 的约定见
         * lv_draw_sw_img.c 里 RGB565A8 分支的"mask_buf 紧跟 RGB 平面"注释。 */
        dsc.header.cf     = LV_COLOR_FORMAT_RGB565A8;
        dsc.header.w      = COVER_BMP_SIDE;
        dsc.header.h      = COVER_BMP_SIDE;
        dsc.header.stride = COVER_BMP_SIDE * 2u;
        dsc.data_size = (uint32_t)len;
        dsc.data = _binary_player_default_cover_bin_start;
    }
    return &dsc;
}

static void build_cover_view(lv_obj_t *parent)
{
    lv_obj_t *cover = lv_obj_create(parent);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, COVER_W, COVER_H);
    lv_obj_set_pos(cover, COVER_X, COVER_Y);
    lv_obj_set_style_radius(cover, COVER_R, LV_PART_MAIN);
    /* 占位底色 = 浅粉灰（见 C_COVER_BG）。有真封面时整张铺满，这层看不见；
     * 没有真封面时它就是黑胶唱片四周的底色（唱片位图四周是透明的）。 */
    lv_obj_set_style_bg_color(cover, C_COVER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    /*
     * ★ 圆角靠 clip_corner 裁**子对象**：lv_image 自己身上的 radius 样式不会裁图
     *   （LVGL 只按对象矩形贴图，圆角是背景/边框的事）。真封面和内置默认封面都是
     *   它的子对象，于是两者拿到的是**同一半径、同一条裁切路径** ——
     *   用户要求的"一模一样的圆角效果"就这么保证的（2026-11）。
     */
    lv_obj_set_style_clip_corner(cover, true, LV_PART_MAIN);
    /* 投影：设计稿 feDropShadow dy=12 stdDev=14 / 45% —— LVGL 的 shadow_width 是
     * 模糊直径，所以取 2×14。阴影只在重绘时算，封面是静态对象，不吃每帧开销。 */
    lv_obj_set_style_shadow_width(cover, D_Y(28), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(cover, D_Y(12), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(cover, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(cover, LV_OPA_40, LV_PART_MAIN);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);   /* 点击交给外层：整页点了展开歌词 */

    /* 默认占位封面：内置黑胶唱片（没有专辑封面时显示它）。
     * 显示边长 = COVER_DEF_SIDE（比卡片小一截，四周留出浅粉灰底），居中摆放；
     * 与位图尺寸解耦：位图是 COVER_BMP_SIDE，用 CONTAIN 等比缩进这个方框。 */
    lv_obj_t *def = lv_image_create(cover);
    lv_obj_set_size(def, COVER_DEF_SIDE, COVER_DEF_SIDE);
    lv_obj_center(def);
    lv_image_set_src(def, player_default_cover_dsc());
    lv_image_set_inner_align(def, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(def, LV_OBJ_FLAG_CLICKABLE);

    /* 真封面：RGB565，按 COVER 等比铺满（裁掉多余） */
    lv_obj_t *img = lv_image_create(cover);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    /*
     * 封面左下角那个**很小的半透明圆角胶囊**：渠道指示。
     *
     * 需求（2026-11 用户）："歌曲专辑封面上不要有任何文字，仅允许歌曲专辑封面左下角有
     * 一个很小的圆形半透明胶囊标签的渠道指示：本地播放 / QQ音乐 / 网易云，每个胶囊
     * 按不同平台的主题色显示对应的颜色"。
     *   - 文案与底色由 music_view_player_apply_src_badge() 按当前音源刷（幂等）；
     *   - 尺寸 LV_SIZE_CONTENT + 圆角 LV_RADIUS_CIRCLE = 胶囊；白字 16 px 是项目里
     *     最小的中文字号，配上 8/2 的内边距就是"很小"的那一档。
     */
    lv_obj_t *badge = lv_obj_create(cover);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* "尽可能小"：16 px 位图字体行高 20，左右只留 6 px —— 整个胶囊 20 px 高 */
    lv_obj_set_style_pad_hor(badge, D_X(6), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(badge, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, lv_color_hex(MUSIC_SRC_COLOR_LOCAL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_50, LV_PART_MAIN);   /* 半透明 */
    /* 12 px 内缩：正好落在圆角裁切线内侧，胶囊不会被切掉一角 */
    lv_obj_align(badge, LV_ALIGN_BOTTOM_LEFT, D_X(12), -D_Y(12));
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *b_label = lv_label_create(badge);
    lv_obj_set_style_text_font(b_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(b_label, C_WHITE, LV_PART_MAIN);
    lv_label_set_text(b_label, "本地播放");
    lv_obj_center(b_label);

    music_view_player_bind_src_badge(badge);
    music_view_player_bind_cover(cover, img, def);
}

/* ================================================================ 右栏：歌词区 */

static void build_lyric_view(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, LYRIC_X, LYRIC_Y);
    lv_obj_set_size(box, LYRIC_W, LYRIC_H);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    /*
     * 提示行：[转圈] [文字]，整体在框里居中。
     * 需求（2026-10）：缓冲时只写一行"缓冲中…"看着像卡死，要**转圈 + 速度**。
     */
    lv_obj_t *hint_row = lv_obj_create(box);
    lv_obj_remove_style_all(hint_row);
    lv_obj_set_size(hint_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hint_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint_row, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_remove_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hint_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(hint_row);

    /* 转圈：lv_spinner 自带无限循环动画，不用自己起 timer */
    lv_obj_t *spin = lv_spinner_create(hint_row);
    lv_obj_set_size(spin, UI_PX_H(22), UI_PX_H(22));
    lv_obj_set_style_arc_width(spin, UI_PX_H(3), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, UI_PX_H(3), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);   /* 只有缓冲时才转 */

    lv_obj_t *hint = lv_label_create(hint_row);
    lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_text_opa(hint, LV_OPA_50, LV_PART_MAIN);
    lv_label_set_text(hint, "轻点封面显示歌词");

    /*
     * 歌词列：整列用 translate_y 平移让当前行居中（逐行滚动会抖），
     * 左对齐（设计稿 text-anchor="start"，x=336）。
     */
    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, D_Y(14), LV_PART_MAIN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    /* 歌词那一组控件归 music_view_lyric.cc，只把指针交给它 */
    music_view_lyric_bind(box, hint, hint_row, spin, col);
    music_view_player_lyric_rebuild();
    music_view_player_lyric_apply_mode();
}

/* ================================================================ 底部：进度 + 控制排 */

static void build_seek(lv_obj_t *parent)
{
    /*
     * 抓取区：自己按触点 x 算比例，松手才真定位（见 music_view_player.cc）。
     * 高度 30 = 条 5 + 上下余量：设计稿那个白圆点 r=6 会越出条体，得留够空间。
     */
    lv_obj_t *seek_hit = lv_obj_create(parent);
    lv_obj_remove_style_all(seek_hit);
    lv_obj_set_pos(seek_hit, SEEK_X, SEEK_Y);
    lv_obj_set_size(seek_hit, SEEK_W, SEEK_H);
    lv_obj_set_style_bg_opa(seek_hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(seek_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seek_hit, LV_OBJ_FLAG_CLICKABLE);
    /* 手指横向滑出抓取区也不断手势（与边缘热区同理） */
    lv_obj_add_flag(seek_hit, LV_OBJ_FLAG_PRESS_LOCK);

    /* 条本体：设计稿 h=5、r=2.5，轨道白 16%、已播白 100% */
    lv_obj_t *bar = lv_bar_create(seek_hit);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);   /* 触摸都交给抓取区 */
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, BAR_H);
    lv_obj_set_pos(bar, 0, BAR_REL_Y);
    lv_bar_set_range(bar, 0, 1000);                   /* 千分比，与时长解耦 */
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_INDICATOR);

    /* 设计稿那粒白圆点：位置由 tick 按千分比摆（见 music_view_player_set_progress） */
    lv_obj_t *knob = lv_obj_create(seek_hit);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, KNOB_D, KNOB_D);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(knob, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(knob, 0, KNOB_REL_Y);

    music_view_player_bind_seek(seek_hit);

    /*
     * 时间行：左"已播" / 右"总时长"，设计稿 12 px 白 55%。
     * 用一个整宽的行容器 + SPACE_BETWEEN 保证右端严格贴到进度条右沿（x=736），
     * 而不用"按文字宽度反算 x"（时间文本一变宽度就要重排）。
     */
    lv_obj_t *times = lv_obj_create(parent);
    lv_obj_remove_style_all(times);
    lv_obj_set_pos(times, SEEK_X, TIMES_Y);
    lv_obj_set_size(times, SEEK_W, D_Y(20));
    lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(times, LV_OBJ_FLAG_SCROLLABLE);
    /* 别让这个透明容器吃掉进度条下沿那 4 px 的触摸 */
    lv_obj_remove_flag(times, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(times, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cur = lv_label_create(times);
    lv_obj_set_style_text_font(cur, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cur, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_text_opa(cur, LV_OPA_50, LV_PART_MAIN);

    lv_obj_t *tot = lv_label_create(times);
    lv_obj_set_style_text_font(tot, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(tot, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_text_opa(tot, LV_OPA_50, LV_PART_MAIN);
    lv_label_set_text(tot, "--:--");

    /* 两个时间标签、进度条本体与圆点交给 owner，再写初值：
     * 开机恢复的曲目这里直接显示记忆到的进度，而不是 0:00。 */
    music_view_player_bind_times(cur, tot, bar, knob);
}

/* 把 44x44 的图标键摆到"中心 (cx, cy)"（设计稿给的就是中心坐标） */
static void place_ctl(lv_obj_t *btn, int cx, int cy)
{
    if (!btn) {
        return;
    }
    const int w = (int)lv_obj_get_width(btn);
    const int h = (int)lv_obj_get_height(btn);
    lv_obj_set_pos(btn, cx - w / 2, cy - h / 2);
}

static void build_controls(lv_obj_t *parent)
{
    const lv_opa_t dim  = LV_OPA_60;   /* 设计稿两端图标：65% 白 */
    const lv_opa_t bright = LV_OPA_90; /* 上一首/下一首：90% 白 */

    /* ① 歌曲信息 / 播放器设置（设计稿最左边那个"三行列表"图标） */
    lv_obj_t *info_btn = music_view_make_icon_btn(parent, LV_SYMBOL_LIST, NULL,
                                                 LV_EVENT_CLICKED, C_WHITE, dim);
    lv_obj_add_event_cb(info_btn, info_btn_clicked, LV_EVENT_CLICKED, NULL);
    place_ctl(info_btn, CTL_INFO_X, CTL_Y);

    /* ② 循环模式：点击在 列表循环/单曲循环/随机循环 之间切换（图标 + Toast） */
    lv_obj_t *mode_btn = music_view_make_icon_btn(parent, LV_SYMBOL_LOOP,
                                                  music_view_player_on_mode_click,
                                                  LV_EVENT_CLICKED, C_WHITE, dim);
    place_ctl(mode_btn, CTL_MODE_X, CTL_Y);
    music_view_player_bind_mode_btn(mode_btn);

    /* 单曲循环角标：LOOP 图标右上角的 "1"（字体里没有 repeat-one 字形） */
    lv_obj_t *badge1 = lv_label_create(mode_btn);
    lv_obj_set_style_text_font(badge1, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(badge1, C_WHITE, LV_PART_MAIN);
    lv_label_set_text(badge1, "1");
    lv_obj_align(badge1, LV_ALIGN_TOP_RIGHT, -UI_PX_W(4), 0);
    music_view_player_bind_mode_badge(badge1);
    music_view_player_apply_play_mode();

    /* ③ 上一首 / ④ 播放暂停 / ⑤ 下一首 */
    lv_obj_t *prev_btn = music_view_make_icon_btn(parent, LV_SYMBOL_PREV, NULL,
                                                  LV_EVENT_CLICKED, C_WHITE, bright);
    place_ctl(prev_btn, CTL_PREV_X, CTL_Y);

    lv_obj_t *play_btn = music_view_make_play_btn(parent);
    lv_label_set_text(lv_obj_get_child(play_btn, 0),
                      music_state()->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    place_ctl(play_btn, CTL_PLAY_X, CTL_Y);

    lv_obj_t *next_btn = music_view_make_icon_btn(parent, LV_SYMBOL_NEXT, NULL,
                                                  LV_EVENT_CLICKED, C_WHITE, bright);
    place_ctl(next_btn, CTL_NEXT_X, CTL_Y);

    music_view_player_wire_transport(prev_btn, play_btn, next_btn);

    /* ⑥ 红心"我喜欢"：只显示图标（点按 = 收藏/取消），只对在线曲目可见。
     * 图标 26 px 位图（assets/icons/heart*.svg 烘的），运行期只换 recolor。 */
    lv_obj_t *like_btn = lv_obj_create(parent);
    lv_obj_remove_style_all(like_btn);
    lv_obj_set_size(like_btn, CTL_BTN, CTL_BTN);
    lv_obj_set_style_bg_opa(like_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(like_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(like_btn, LV_OBJ_FLAG_CLICKABLE);
    place_ctl(like_btn, CTL_LIKE_X, CTL_Y);

    lv_obj_t *like_img = lv_image_create(like_btn);
    lv_image_set_src(like_img, &heart_off);
    lv_obj_center(like_img);
    lv_obj_set_style_image_recolor(like_img, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(like_img, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(like_img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *like_spin = lv_spinner_create(like_btn);
    lv_obj_set_size(like_spin, UI_PX_H(24), UI_PX_H(24));
    lv_obj_set_style_arc_width(like_spin, UI_PX_H(3), LV_PART_MAIN);
    lv_obj_set_style_arc_width(like_spin, UI_PX_H(3), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(like_spin, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(like_spin, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_remove_flag(like_spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(like_spin, LV_OBJ_FLAG_HIDDEN);   /* 只在请求期间转 */
    lv_obj_center(like_spin);

    music_view_player_bind_like(like_btn, like_img, like_spin);

    /* ⑦ 音量：从右边缘滑出竖向音量条 */
    lv_obj_t *vol_btn = music_view_make_icon_btn(parent, LV_SYMBOL_VOLUME_MAX,
                                                 music_view_output_on_volume,
                                                 LV_EVENT_CLICKED, C_WHITE, dim);
    place_ctl(vol_btn, CTL_VOL_X, CTL_Y);

    /*
     * ⑧ 输出设备选择：图标跟着当前输出走（扬声器/蓝牙/静音）。
     *   点   = 打开选择弹窗
     *   长按 = 直接跳到"设置 → 蓝牙"（要连耳机时少点两下）
     * ★ 短按用 SHORT_CLICKED 的理由见 music_view_output.h。
     */
    lv_obj_t *out_btn = music_view_output_make_btn(parent);
    place_ctl(out_btn, CTL_OUT_X, CTL_Y);
}

/* ================================================================ 整页 */

lv_obj_t *music_view_player_build(void)
{
    const music_state_t *st = music_state();

    lv_obj_t *scr = lv_obj_create(NULL);
    /* 底色不透明（启动图/背景图还没贴好时也不能露出上一屏），背景图随后贴上来 */
    ui_screen_apply_default_bg(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /*
     * 本页是**绝对坐标版式**（照设计稿排），所以屏幕不再留 padding：
     * 位置一律按 800x480 参考坐标写，经 D_X/D_Y 换算。
     */
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    /*
     * 背景图（assets/player_background.jpg）：图是开机后台解好的，这里只是贴一下；
     * 万一还没解完，解码任务会在解好后自己贴到这个 screen 上（见 music_player_bg.cc）。
     * 必须在 pad 之后调 —— 它只改 bg_image 样式，不动 pad。
     */
    music_player_bg_attach(scr);

    /* 整块可点 → 展开/收起歌词（点封面、点歌名都算） */
    lv_obj_t *left = lv_obj_create(scr);
    lv_obj_remove_style_all(left);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_size(left, LYRIC_X, D_Y(365));   /* 左栏：封面 + 歌名/歌手那一块 */
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(left, music_view_player_on_cover_click, LV_EVENT_CLICKED, NULL);
    build_cover_view(left);

    /*
     * 歌名 / 歌手（封面下方）：
     *   设计稿 22 px 粗体 + 14 px 58%。
     *   22 px 用项目自带的 22 px 中文字体（ui_metrics.h 的 UI_FONT_TITLE_22，
     *   生僻字自动落到 16 px），字体由 set_now_playing() 统一设置，这里只建控件。
     */
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_pos(title, COVER_X, TITLE_Y);
    lv_obj_set_width(title, TEXT_W);
    lv_obj_set_style_text_font(title, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_WHITE, LV_PART_MAIN);
    /*
     * 歌名：**不换行、过长就横向跑马灯**（2026-11 用户）。
     *   - 宽度固定成 TEXT_W（封面下方那一条），LVGL 只在文本超宽时才起动画；
     *   - 换曲换成短歌名时 lv_label_set_text 会重算尺寸、动画自动停（lv_label.c 里
     *     hor_anim 为 false 就 lv_anim_delete）。
     */
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_t *artist = lv_label_create(scr);
    lv_obj_set_pos(artist, COVER_X, ARTIST_Y);
    lv_obj_set_width(artist, TEXT_W);
    lv_obj_set_style_text_font(artist, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(artist, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_text_opa(artist, LV_OPA_60, LV_PART_MAIN);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);

    /* ★ 必须登记进控件表：换曲时 set_now_playing() 要靠它刷新
     * （漏绑的表现是"首次进页面正常、之后换曲标题不动"）。 */
    music_view_player_bind_now_playing(title, artist);

    if (st->on_mode && st->on_songs && st->index >= 0 && (size_t)st->index < st->on_count) {
        const online_song_t *song = &((const online_song_t *)st->on_songs)[st->index];
        music_view_player_set_now_playing(song->title, song->artist);   /* 在线曲目 */
    } else {
        const track_t *t = (st->index >= 0) ? storage_service_track((size_t)st->index) : NULL;
        if (st->index >= 0) {
            music_view_player_set_now_playing(t ? t->title : "未在播放", t ? t->artist : NULL);
        } else {
            /* 还没选过歌：标题给提示，歌手单独一句引导（见下） */
            music_view_player_set_now_playing("未在播放", NULL);
            lv_label_set_text(artist, "请在「本地音乐」里选一首");
        }
    }

    /* 歌词区：封面右侧，垂直中心对齐设计稿当前行（y=202） */
    build_lyric_view(scr);
    if (music_view_player_lyric_expanded()) {
        music_view_player_lyric_tick();
    }

    /*
     * FFT 频谱柱（可选功能，设置弹窗里开关）：设计稿没有这一块，
     * 所以只在"歌词区与进度条之间那 38 px"里画，关掉时整块隐藏。
     * ★ 必须建在 seek 之前 —— 右栏是绝对坐标，但对象顺序仍是 z 序。
     */
    lv_obj_t *spec = lv_obj_create(scr);
    lv_obj_remove_style_all(spec);
    lv_obj_set_pos(spec, SEEK_X, SPEC_Y);
    lv_obj_set_size(spec, SEEK_W, SPEC_H);
    lv_obj_remove_flag(spec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spec, LV_OBJ_FLAG_CLICKABLE);
    music_spectrum_attach(spec);

    build_seek(scr);
    build_controls(scr);

    lv_obj_add_event_cb(scr, music_view_player_page_delete_cb, LV_EVENT_DELETE, NULL);

    music_view_player_build_prepare();
    /* 立刻把主题色贴上去（否则要等第一个 500 ms tick 才不"发琥珀色"） */
    music_view_player_accent_apply();

    /*
     * 进播放器页时的封面：
     *   1) 已经有解好的封面（PSRAM 里留着，见音乐页销毁处的说明）→ **直接贴回来**；
     *   2) 还没有（开机恢复的曲目：start_track_at 排了队但没走起播流程）→ 补排一次解码。
     */
    if (music_cover_have_pixels()) {
        music_view_player_cover_reattach();
    } else if (st->index >= 0 && !music_state()->cover_busy) {
        music_cover_request(st->index);
    }

    ESP_LOGI(TAG, "player page created (%s)",
             music_state_status_text(st, audio_service_is_playing()));
    return scr;
}
