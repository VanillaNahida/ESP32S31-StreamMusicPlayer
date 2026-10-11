/*
 * music_info.h —— "歌曲信息"弹窗（播放器页控制排最左边那个 ⓘ 按钮）
 *
 * 弹窗挂在 `lv_layer_top()` 上（ui_ba 的对话框），**不随播放器页销毁** ——
 * 所以离开页面时必须显式 `music_info_close()`（在 music_view_player_on_delete 里调）。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void music_info_open(void);
void music_info_close(void);

/* 页面销毁时调：把弹窗收掉（它不归页面所有，不关就会跟着用户跑到别的页面去） */
void music_info_on_page_delete(void);

#ifdef __cplusplus
}
#endif
