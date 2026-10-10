/*
 * music_view_player.h —— 播放器页（视图层）
 *
 * 页面拆成三个文件，各自职责单一，彼此只共享 music_view_internal.h 的控件表：
 *   - music_view_page.cc    布局骨架（建控件 + 注册事件）
 *   - music_view_player.cc  数值刷新、来源/模式、缓冲提示、进度条手势、封面贴图、销毁
 *   - music_view_lyric.cc   歌词数据与渲染（高亮/居中不变式）
 *
 * ★ 对外（业务层）只认这个头文件：它不 include audio_service.h / online_service.h /
 *   storage_service.h 的任何"业务动作"，页面上要显示的东西全部经 music_state.h 读，
 *   要改变播放的动作全部调 music_player.h。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建整页（路由 page_desc_t.create 用）：实现分散在上面三个文件里 */
lv_obj_t *music_view_player_build(void);

/*
 * 圆形按钮工厂。
 * code 一般传 LV_EVENT_CLICKED；**有长按动作的按钮要传 LV_EVENT_SHORT_CLICKED** ——
 * 见输出设备按钮处的说明。播放器页与输出设备按钮共用同一套样式。
 */
lv_obj_t *music_view_make_round_btn_ex(lv_obj_t *parent, const char *sym, lv_event_cb_t cb,
                                       lv_event_code_t code, lv_color_t bg, lv_color_t fg);
lv_obj_t *music_view_make_round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb,
                                    lv_color_t bg, lv_color_t fg);

/* 页面建完后建的两个常驻定时器（tick / 蓝牙媒体键）的双保险入口 */
void music_view_player_build_prepare(void);

/* ---------------------------------------------------------- 控件绑定（只给 music_view_page.cc） */

void music_view_player_bind_cover(lv_obj_t *box, lv_obj_t *img);
void music_view_player_bind_src_badge(lv_obj_t *badge);
void music_view_player_bind_times(lv_obj_t *cur, lv_obj_t *tot, lv_obj_t *seek);
void music_view_player_bind_seek(lv_obj_t *seek_hit);
void music_view_player_bind_mode_btn(lv_obj_t *mode_btn);
void music_view_player_bind_mode_badge(lv_obj_t *badge);
void music_view_player_wire_transport(lv_obj_t *prev_btn, lv_obj_t *play_btn, lv_obj_t *next_btn);

/* 页面 DELETE 回调（视图 + 业务收尾） */
void music_view_player_page_delete_cb(lv_event_t *e);
/* 轻点封面 → 展开/收起歌词 */
void music_view_player_on_cover_click(lv_event_t *e);
/* 循环模式按钮 */
void music_view_player_on_mode_click(lv_event_t *e);

/* ---------------------------------------------------------- 数值刷新（业务层调用） */

void music_view_player_set_now_playing(const char *title, const char *artist);
void music_view_player_set_play_icon(bool playing);
void music_view_player_set_elapsed_text(uint32_t elapsed_ms);
void music_view_player_set_total_text(uint32_t dur_ms);
void music_view_player_set_total_text_placeholder(void);
/* 进度条（千分比）。时长未知（dur_ms == 0）时**不写**（与拆分前的 tick 一致） */
void music_view_player_set_progress(int32_t permille, uint32_t dur_ms);
/* 进度条归零（换曲时无条件写一次） */
void music_view_player_reset_progress(void);
/* tick 用：刷进度条 + 已播/总时长（手指正在拖时整段跳过） */
void music_view_player_refresh_timesliders(void);

void music_view_player_apply_src_badge(void);
void music_view_player_apply_play_mode(void);

/* 缓冲提示：显示 + 立刻刷一帧文字（速度/百分比）。内部置 music_state.buf_show。 */
void music_view_player_set_buffering_hint(void);
/* tick 用：按当前状态刷缓冲提示文字 */
void music_view_player_refresh_buffer_hint(void);

/* ---------------------------------------------------------- 歌词（转发到 music_view_lyric.cc） */

void music_view_player_lyric_reset(void);
/* 边车 / 内嵌三级查找；返回是否真的拿到了歌词 */
bool music_view_player_lyric_load(const char *audio_path);
bool music_view_player_lyric_parse(const char *lrc_text);
void music_view_player_lyric_rebuild(void);
void music_view_player_lyric_sync(void);          /* 定位之后强制重贴 */
void music_view_player_lyric_apply_mode(void);    /* 说明行/歌词列显隐 + 藏转圈 */
void music_view_player_lyric_tick(void);          /* 行号变了才动 */
bool music_view_player_lyric_expanded(void);
bool music_view_player_lyric_timed(void);

/* ---------------------------------------------------------- 封面（像素产权在 music_cover.cc） */

void music_view_player_cover_show(uint8_t *px, int w, int h);
/* 进页面时把留着的封面直接贴回（不重新解码） */
void music_view_player_cover_reattach(void);
/* tick 用：取走工作线程的封面产出并上屏，然后按需拉起下一次解码 */
void music_view_player_cover_pump(void);

/* ---------------------------------------------------------- 其它 */

void music_view_player_out_btn_sync(void);      /* 输出按钮图标跟着实际输出走 */
void music_view_player_set_seeking(bool on);

/* 页面销毁：清空控件表 + 作废在屏状态（定时器不删、封面像素不释放） */
void music_view_player_on_delete(void);

#ifdef __cplusplus
}
#endif
