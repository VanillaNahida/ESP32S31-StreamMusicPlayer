/*
 * ao_provider.h —— 在线音乐 APP 的**音源选择**（网易云 / QQ 音乐）
 *
 * 桌面上的两个图标（网易云音乐、QQ 音乐）打开的是**同一套页面代码**：
 *   app_online.cc 的 page_online() 按 ao_provider() 决定标题、文案与后端接口。
 * 这样三页骨架、分页/懒加载、浮层、播放器联动只有一份，两家不会走成两个样子。
 *
 * ⚠ 约束：一个进程里同时只会有一个在线页（从桌面进入、返回即销毁），
 *   所以用 static 就够了，不需要引用计数。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AO_PROV_NETEASE = 0,
    AO_PROV_QQ      = 1,
} ao_prov_t;

/* 建页前设置（page_netease() / page_qqmusic() 的第一句） */
void ao_provider_set(int p);
int  ao_provider(void);
bool ao_is_qq(void);

/* 文案：标题 / 桌面名 / 账号名 / 扫码提示（两家措辞不同，见实现） */
const char *ao_provider_title(void);
const char *ao_provider_account_name(void);
const char *ao_provider_scan_hint(void);
const char *ao_provider_scan_status(void);

/* 当前音源的登录态与昵称（把 online_netease_* / online_qq_* 收敛成一处判断） */
bool        ao_logged_in(void);
const char *ao_nickname(void);
/* 重新读一次持久化的登录态（网易云要等 SD；QQ 是 NVS，恒可用） */
void        ao_login_state_reload(void);
/* 清掉当前音源在 NVS 里的昵称（退出登录 / 换账号时） */
void        ao_nickname_clear(void);

#ifdef __cplusplus
}
#endif
