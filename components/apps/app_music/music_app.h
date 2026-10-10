/*
 * music_app.h —— 内部：定时器门面（只有音乐 APP 自己的模块会 include）
 *
 * 对外契约始终是 app_music.h（名字与签名不许动）；这里只放"组件内部跨文件"的东西。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 确保两个常驻定时器存在（播放器页与 app_music_register_apps 双保险：谁先跑谁建）。
 * 实现与创建权在 music_app.cc；页面销毁时**绝不要**删它们
 * （删掉就等于"离开播放器页就不再续播/不再响应耳机按键"）。
 */
void music_view_player_ensure_timers(void);

#ifdef __cplusplus
}
#endif
