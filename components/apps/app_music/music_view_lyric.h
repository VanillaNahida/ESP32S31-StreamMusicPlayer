/*
 * music_view_lyric.h —— 歌词引擎（视图层内部）
 *
 * 从 app_music.cc 的 1182–1258、2446–2488、2549–2589 整段搬出来。
 * 这里持有**歌词数据**（lrc_t）与"当前高亮行"这个状态量，是唯一的 owner：
 *   - 业务层不直接碰歌词数据；它经 music_view_player.h 的歌词接口调用本模块
 *     （加载 / 解析在线文本 / 重建 / 定位后重贴）；
 *    - 歌词标签控件（歌词列）由播放器页建好后交给本模块（music_view_lyric_bind）。
 *
 * ⚠ 高亮不变式见 lyric_apply_cur 的注释：换行时必须**先读旧值**再赋值。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 播放器页建完歌词列后把控件交给本模块（页面销毁时传 NULL） */
void music_view_lyric_bind(lv_obj_t *lyric_box, lv_obj_t *lyric_hint,
                           lv_obj_t *hint_row, lv_obj_t *buf_spin, lv_obj_t *lyric_col);

void music_view_lyric_reset(void);
bool music_view_lyric_load(const char *audio_path);   /* 边车 / 内嵌三级查找 */
bool music_view_lyric_parse(const char *lrc_text);    /* 在线 LRC 文本 */
void music_view_lyric_rebuild(void);                  /* 按当前歌词缓冲重建行 */
void music_view_lyric_tick(void);                     /* 行号变了才动 */
void music_view_lyric_sync(void);                     /* 定位后强制重贴（别清 cur） */
void music_view_lyric_apply_mode(void);               /* 说明行/歌词列可见性 + 藏转圈 */
void music_view_lyric_toggle(void);                   /* 轻点封面：展开/收起 */
int  music_view_lyric_count(void);                    /* 已建行数（lyric_apply_mode 判据） */
bool music_view_lyric_expanded(void);
bool music_view_lyric_timed(void);

/* 说明行控件（缓冲提示要往上写字；页面不在时返回 NULL）。
 * 这两个小口子让"缓冲提示"不必知道歌词控件的布局。 */
lv_obj_t *music_view_lyric_hint_label(void);
void      music_view_lyric_spin_show(void);
/* 主题色变化（music_view_player_accent_apply）：缓冲转圈的弧色跟着封面主题走 */
void      music_view_lyric_set_accent(uint32_t rgb);

/* 页面销毁时调：清掉歌词控件引用与高亮行（歌词**数据**留着，重进页面直接用） */
void music_view_lyric_on_page_delete(void);

#ifdef __cplusplus
}
#endif
