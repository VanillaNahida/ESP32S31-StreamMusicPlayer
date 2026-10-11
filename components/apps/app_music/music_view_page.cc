/*
 * music_view_page.cc —— 播放器页的**布局骨架**（视图层）
 *
 * 搬迁自 app_music.cc 的 2638–2876（page_player 的分栏/标题/歌词区/进度条/控制排）
 * 与 1837–1870（圆按钮工厂在 music_view_player.cc，这里只管摆放与事件注册）。
 *
 * 拆出来的理由：page_player 原本是 224 行的大函数，与"数值刷新/手势/歌词"混在一个文件里，
 * 拆分后本文件只做"建控件 + 把指针交给 owner 模块"，任何 tick 期的刷新都不在这里。
 *
 * ★ 事件注册的顺序与对象一个都没变（尤其：短按输出键必须 SHORT_CLICKED、
 *   进度条抓取区必须挂 PRESS_LOST、页面必须挂 DELETE）。
 */
#include "music_view_player.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "audio_service.h"   /* 建页时读一次"是不是在播"用于按钮图标（纯取数） */
#include "online_service.h"  /* 建页时从在线队列取标题/歌手 */
#include "storage_service.h" /* 建页时从曲目表取标题/歌手 */
#include "ui_metrics.h"

#include "music_cover.h"
#include "music_info.h"
#include "music_player.h"
#include "music_state.h"
#include "music_view_internal.h"
#include "music_view_lyric.h"
#include "music_view_output.h"

static const char *TAG = "app_music";

/* ⓘ（歌曲信息）按钮：只开弹窗，不做别的（弹窗内容见 music_info.cc） */
static void info_btn_clicked(lv_event_t *e)
{
    (void)e;
    music_info_open();
}

/* ------------------------------------------------------------------ 左栏：封面 */

static void build_cover_view(lv_obj_t *parent)
{
    lv_obj_t *cover = lv_obj_create(parent);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(cover, UI_PX_H(16), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cover, UI_COLOR_ICON_LOCAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);   /* 点击交给外层 left */

    /* 占位：还没解出封面时显示 */
    lv_obj_t *sym = lv_label_create(cover);
    lv_obj_set_style_text_font(sym, UI_FONT_CLOCK, LV_PART_MAIN);
    lv_obj_set_style_text_color(sym, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(sym, LV_SYMBOL_AUDIO);
    lv_obj_center(sym);

    /* 真封面：RGB565，按 COVER 等比铺满（裁掉多余），圆角与容器一致 */
    lv_obj_t *img = lv_image_create(cover);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_obj_set_style_radius(img, UI_PX_H(16), LV_PART_MAIN);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    music_view_player_bind_cover(cover, img);
}

/* ------------------------------------------------------------------ 右栏：歌词区 */

static void build_lyric_view(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);       /* 吃掉标题/歌手与控制排之间的剩余高度 */
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    /*
     * 提示行：[转圈] [文字]，整体在框里居中。
     *
     * 需求（2026-10）：缓冲时只写一行"缓冲中…"看着像卡死，要**转圈 + 速度**。
     * 用 flex 行把两个都装进去，比绝对定位省事：转圈藏起来时（"暂无歌词"等状态）
     * 文字自己就居中了，不需要额外调整。
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

    /* 转圈：lv_spinner 自带无限循环动画（LV_SPINNER_TYPE_SPINNING_ARC），不用自己起 timer。
     * 样式与 app_online 的"载入中"一致（暗底圈 + 强调色转弧）。 */
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
    lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(hint, "轻点封面显示歌词");

    /* 歌词列：整列用 translate_y 平移让当前行居中（逐行滚动会抖） */
    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    /* 歌词那一组控件归 music_view_lyric.cc，只把指针交给它 */
    music_view_lyric_bind(box, hint, hint_row, spin, col);
    music_view_player_lyric_rebuild();
    music_view_player_lyric_apply_mode();
}

/* ------------------------------------------------------------------ 整页 */

lv_obj_t *music_view_player_build(void)
{
    const music_state_t *st = music_state();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /*
     * 顺序要紧：lv_obj_set_style_pad_all() 展开成上下左右四个 pad，
     * 写在 pad_top 之后会把"给状态栏留的边距"覆盖掉 —— 之前就是这么写的，
     * 结果歌名/歌手钻到 32px 状态栏底下。必须先 pad_all，再补 pad_top。
     */
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PAGE_PAD, LV_PART_MAIN);

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PX_W(20), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 左栏：专辑封面（整块可点 → 在**封面右侧**展开/收起歌词） */
    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, UI_PX_H(240), UI_PX_H(240));
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(left, music_view_player_on_cover_click, LV_EVENT_CLICKED, NULL);

    build_cover_view(left);

    /* 右栏：信息 + 进度 + 控制 */
    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* 播放器页会被路由压栈打开（首页/本地/在线都会进这里）：返回键由
     * ui_core 的全局返回键自动叠在顶层，这里不再自己画。标题行仍按
     * UI_HEADER_TEXT_X 起排，给左侧按钮留位。 */

    /* 播放来源徽标：在线播放 / 本地播放（需求：播放器页要显式说明来源）。
     * 单独一行放在标题上方 —— 标题是整宽滚动 label，同行会挤掉它的滚动宽度。 */
    lv_obj_t *badge = lv_label_create(right);
    lv_obj_set_style_text_font(badge, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(badge, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(badge, UI_PX_H(3), LV_PART_MAIN);
    music_view_player_bind_src_badge(badge);

    /* 标题与副标题用独立的 block 元素，不要塞进同一个 label 的 inline span */
    lv_obj_t *title = lv_label_create(right);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t *artist = lv_label_create(right);
    lv_obj_set_style_text_font(artist, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(artist, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(artist, LV_PCT(100));
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);

    /* ★ 建完立刻登记进控件表：换曲时 music_view_player_set_now_playing() 要靠它刷新。
     * 漏了这一步的表现是"首次进页面标题正常、之后换曲标题/歌手不动"（2026-10 的 bug）。 */
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

    /* 歌词区：在封面右侧，占掉标题/歌手与控制排之间的剩余高度 */
    build_lyric_view(right);
    if (music_view_player_lyric_expanded()) {
        music_view_player_lyric_tick();
    }

    /*
     * 进度条：外层 30px 透明"抓取区"（自己按触点 x 算比例，松手才真定位），
     * 里面 10px 的 lv_bar 只负责画：轨道 + 已播填充。
     * 用 lv_bar 而不是 lv_slider：按需求去掉拖拽小圆点，只保留进度条本体。
     */
    lv_obj_t *seek_hit = lv_obj_create(right);
    lv_obj_remove_style_all(seek_hit);
    lv_obj_set_size(seek_hit, LV_PCT(100), UI_PX_H(30));
    lv_obj_set_style_bg_opa(seek_hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(seek_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seek_hit, LV_OBJ_FLAG_CLICKABLE);
    /* 手指横向滑出抓取区也不断手势（与边缘热区同理） */
    lv_obj_add_flag(seek_hit, LV_OBJ_FLAG_PRESS_LOCK);

    /* 只要进度条本体：用 lv_bar（没有 knob 部件），不要拖拽小圆点 */
    lv_obj_t *bar = lv_bar_create(seek_hit);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);   /* 触摸都交给抓取区 */
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, UI_PX_H(10));
    lv_obj_center(bar);
    lv_bar_set_range(bar, 0, 1000);                   /* 千分比，与时长解耦 */
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, UI_PX_H(5), LV_PART_MAIN);
    /* 描边保证"时长未知 / 进度为 0"时轨道也一定看得见 */
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, UI_PX_H(5), LV_PART_INDICATOR);

    lv_obj_t *times = lv_obj_create(right);
    lv_obj_remove_style_all(times);
    lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(times, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cur = lv_label_create(times);
    lv_obj_set_style_text_font(cur, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(cur, UI_COLOR_FG_DIM, LV_PART_MAIN);

    lv_obj_t *tot = lv_label_create(times);
    lv_obj_set_style_text_font(tot, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(tot, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(tot, "--:--");

    /* 两个时间标签与进度条交给 owner，再写初值：
     * 开机恢复的曲目这里直接显示记忆到的进度，而不是 0:00。 */
    music_view_player_bind_times(cur, tot, bar);
    music_view_player_bind_seek(seek_hit);
    music_view_player_set_elapsed_text(st->elapsed_ms);

    /* 控制排 */
    lv_obj_t *ctl = lv_obj_create(right);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_size(ctl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);

    /* 歌曲信息：ⓘ（这里用列表图标）→ 弹窗列出标题/专辑/码率/路径等，可滑动查看。
     * 位置按需求放在**循环模式按钮左边**。 */
    lv_obj_t *info_btn = music_view_make_round_btn(ctl, LV_SYMBOL_LIST, NULL,
                                                  UI_COLOR_PILL, UI_COLOR_FG);
    lv_obj_add_event_cb(info_btn, info_btn_clicked, LV_EVENT_CLICKED, NULL);

    /* 循环模式：点击在 列表循环/单曲循环/随机循环 之间切换（图标 + Toast） */
    lv_obj_t *mode_btn = music_view_make_round_btn(ctl, LV_SYMBOL_LOOP,
                                                   music_view_player_on_mode_click,
                                                   UI_COLOR_PILL, UI_COLOR_FG);
    music_view_player_bind_mode_btn(mode_btn);

    /* 单曲循环角标：LOOP 图标右上角的 "1"（字体里没有 repeat-one 字形） */
    lv_obj_t *badge1 = lv_label_create(mode_btn);
    lv_obj_set_style_text_font(badge1, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(badge1, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(badge1, "1");
    lv_obj_align(badge1, LV_ALIGN_TOP_RIGHT, -UI_PX_W(8), UI_PX_H(6));
    music_view_player_bind_mode_badge(badge1);
    music_view_player_apply_play_mode();

    lv_obj_t *prev_btn = music_view_make_round_btn(ctl, LV_SYMBOL_PREV, NULL,
                                                   UI_COLOR_PILL, UI_COLOR_FG);
    lv_obj_t *play_btn = music_view_make_round_btn(ctl,
                                                   st->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
                                                   NULL, UI_COLOR_ACCENT, lv_color_hex(0x14141A));
    lv_obj_t *next_btn = music_view_make_round_btn(ctl, LV_SYMBOL_NEXT, NULL,
                                                   UI_COLOR_PILL, UI_COLOR_FG);
    music_view_player_wire_transport(prev_btn, play_btn, next_btn);
    music_view_make_round_btn(ctl, LV_SYMBOL_VOLUME_MAX, music_view_output_on_volume,
                              UI_COLOR_PILL, UI_COLOR_FG);

    /*
     * 输出设备选择：图标跟着当前输出走（扬声器/蓝牙/静音）。
     *   点   = 打开选择弹窗
     *   长按 = 直接跳到"设置 → 蓝牙"（要连耳机时少点两下）
     * ★ 短按用 SHORT_CLICKED 的理由见 music_view_output.h。
     */
    music_view_output_make_btn(ctl);

    lv_obj_add_event_cb(scr, music_view_player_page_delete_cb, LV_EVENT_DELETE, NULL);

    music_view_player_build_prepare();

    /*
     * 进播放器页时的封面：
     *   1) 已经有解好的封面（PSRAM 里留着，见音乐页销毁处的说明）→ **直接贴回来**，
     *      不重新下载、不重新解码。这就是"切页面回来封面还在"的关键；
     *   2) 还没有（开机恢复的曲目：start_track_at 排了队但没走起播流程）→ 补排一次解码。
     *
     * 为什么不用记"这块封面属于哪一首"：换曲一定走 start_track_at()/start_track_online_at()
     * 或 online_switch_ui_to()，它们都会 cover_show(NULL,…) 先把旧封面释放掉 ——
     * 所以页面创建时还留着像素，就一定是当前这首的。
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
