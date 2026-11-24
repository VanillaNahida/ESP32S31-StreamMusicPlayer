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
 * 控制排上的图标键工厂（按设计稿 assets/UIDesign/播放器UI重构参考.svg）。
 *
 * 设计稿的控制排**没有按钮底色**，只有一个个图标；唯一的例外是中央播放键
 * （半透明白圆 48 px，见 music_view_make_play_btn）。
 *   - 命中区 44x44（UI_MIN_TOUCH），图标 20 px 居中；
 *   - 透明底上没有"按下变暗"的天然反馈，所以这里挂 LV_STATE_PRESSED 的 55% 不透明度；
 *   - code 一般传 LV_EVENT_CLICKED；**有长按动作的按钮要传 LV_EVENT_SHORT_CLICKED**
 *     （见 music_view_output.h 的说明）。
 *
 * fg / fg_opa 就是设计稿里那个图标的颜色与不透明度（例如控制排两端是 65% 白）。
 */
lv_obj_t *music_view_make_icon_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb,
                                   lv_event_code_t code, lv_color_t fg, lv_opa_t fg_opa);

/*
 * 同上，但图标是**烘焙好的位图**（lv_image）而不是字形。
 *
 * 为什么要这个：输出设备按钮要用用户给的 SVG（assets/icons/out_device.svg 烘成
 * ui_icons.h 里的 out_device），字形里没有那个图形。图标是白色烘的，
 * fg 用 image_recolor 上色、fg_opa 用整对象的 opa —— 与字形版观感一致。
 * 返回的按钮**第 0 个子对象**就是那个 lv_image（换配色时取它）。
 */
lv_obj_t *music_view_make_icon_btn_image(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                         lv_event_cb_t cb, lv_event_code_t code,
                                         lv_color_t fg, lv_opa_t fg_opa);

/*
 * 中央播放/暂停键：48x48 半透明白圆（#FFFFFF 16%）+ 20 px 白色图标。
 * ⚠ 图标 label 必须是按钮的**第 0 个子对象**：加载态的转圈、
 *   set_play_icon() 的换字形都按这个约定取（见 music_view_player.cc）。
 */
lv_obj_t *music_view_make_play_btn(lv_obj_t *parent);

/* 页面建完后建的两个常驻定时器（tick / 蓝牙媒体键）的双保险入口 */
void music_view_player_build_prepare(void);

/*
 * 封面左下角渠道胶囊的三套**平台主题色**（2026-11 用户：每个胶囊按不同平台的主题色
 * 显示对应的颜色）。取值是平台自己的品牌色：
 *   网易云  红（assets/icons/netease.png 图标底色 #F01010 一族）
 *   QQ音乐  绿（QQ 音乐图标里那个音符的 #00B070）
 *   本地播放 中性石板灰（本地没有"平台"，用不抢眼的灰，压在任何封面上都读得清）
 * 定义放头文件：建控件（music_view_page.cc）与按音源刷新（music_view_player.cc）
 * 两处都要用，不能各写一份。
 */
#define MUSIC_SRC_COLOR_LOCAL    0x5A6672
#define MUSIC_SRC_COLOR_NETEASE  0xE01020
#define MUSIC_SRC_COLOR_QQ       0x00B070
#define MUSIC_SRC_COLOR_ONLINE   0x2F7FBF   /* 在线但取不到渠道（兜底） */

/* ---------------------------------------------------------- 控件绑定（只给 music_view_page.cc） */

/* 封面容器 + 真封面 + 占位层（占位层 = 渐变卡片，有真封面时藏起来） */
void music_view_player_bind_cover(lv_obj_t *box, lv_obj_t *img, lv_obj_t *placeholder);
/*
 * 封面左下角的渠道胶囊（本地播放 / QQ音乐 / 网易云）：
 * 文案与底色由 apply_src_badge() 按当前音源刷；点它自己会隐藏（状态落 NVS）。
 * ⚠ 封面上**不再有任何歌名/来源文字**（2026-11 用户），所以没有 bind_band_title 了。
 */
void music_view_player_bind_src_badge(lv_obj_t *badge);
/* 渠道胶囊显隐（点它 = 隐藏；播放器设置里有开关；状态落 NVS，默认显示） */
void music_view_player_src_badge_set_visible(bool on);
bool music_view_player_src_badge_visible(void);
/* 标题 / 歌手。★ 必须绑：set_now_playing() 靠这两个指针换曲时刷新，
 *   漏绑的表现是"首次进页面正常、之后换曲标题不动"（2026-10 修过这个 bug）。 */
void music_view_player_bind_now_playing(lv_obj_t *title, lv_obj_t *artist);
/* cur/tot：左下/右下的两个时间；seek：lv_bar 本体；knob：进度条上的白圆点 */
void music_view_player_bind_times(lv_obj_t *cur, lv_obj_t *tot, lv_obj_t *seek,
                                  lv_obj_t *knob);
void music_view_player_bind_seek(lv_obj_t *seek_hit);
/* 红心（控制排里的第 6 个键）：btn（透明，只有图标）+ img（heart_on/off）+ spin（请求中转圈） */
void music_view_player_bind_like(lv_obj_t *btn, lv_obj_t *img, lv_obj_t *spin);
/* tick 用：按 music_state 的 like_* 画红心（内部缓存上次画的状态，没变零开销） */
void music_view_player_like_apply(void);
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
/*
 * "加载中"：解析在线地址 / 抓封面歌词 / 缓冲的那段时间。
 * 播放按钮会**换成转圈并禁用点击** —— 这段时间按播放没有意义，而且会去搅
 * 正在建立的会话（真机现象：加载时点播放 → 封面丢了、或干脆不响）。
 * 传 false 恢复播放/暂停图标。**幂等**，可以每个 tick 调。
 */
void music_view_player_set_busy(bool busy);
/* 当前是不是加载态（业务侧决定"要不要受理播放/拖动"时问它） */
bool music_view_player_is_busy(void);
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

/*
 * 主题色（跟随封面）应用到本页控件：单曲循环角标、各种转圈的弧、频谱柱。
 * ⚠ 重构稿（2026-11）的进度条/播放键是**白色**（不再是封面主色），所以这里
 *   不再改那两处 —— 封面主色仍然由频谱柱与加载转圈体现。
 * **幂等**（内部缓存上次的 rgb，没变直接返回），tick 每 500 ms 调一次。
 */
void music_view_player_accent_apply(void);

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
