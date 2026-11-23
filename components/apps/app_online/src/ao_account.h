/*
 * ao_account.h —— 网易云**账号与登录状态机**（纯逻辑，不依赖 LVGL）
 *
 * 从 ao_model 里分出来的原因：那一份把"歌单队列/分页/懒加载"和"账号/登录"都装在一起
 * 超过了 600 行的上限；两者本来就是蓝图 §6.3 里分开列的两块可单测逻辑。
 *
 * 本模块负责（**不含二维码绘制** —— 那是 ao_qr_login.cc 的事）：
 *   - 账号信息（昵称）与账号歌单列表的拉取状态 + 失败退避节流
 *   - 第 0 页"正在看哪一份详情"（每日推荐 / 某个账号歌单）与其分页游标
 *   - 登录成功 / 退出登录 / 判定未登录 时的状态复位
 *
 * 与 ao_model 一样的两条规矩：**不 include lvgl / ui_***，时间一律由调用方把
 * `lv_tick_get()` 当参数传进来（形参名统一 `now`）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "ao_common.h"

/* ---------------------------------------------------------------- 第 0 页详情 */

/* 账号歌单详情当前页号（页数算法在 ao_model.h 的 ao_acct_page_count） */
int  ao_acct_page(void);
void ao_acct_page_set(int page);

bool ao_acct_detail(void);
/* "一次性推荐列表"详情（每日推荐 / QQ 每日30首 / 私人雷达）——三者共用同一个队列 Key */
bool ao_acct_daily(void);
int  ao_acct_sel(void);

/*
 * 一次性推荐列表的来源（只影响标题文案，不影响数据）：
 *   0 = 网易云每日推荐  1 = QQ 每日30首  2 = QQ 私人雷达
 */
#define AO_FEED_NETEASE_DAILY 0
#define AO_FEED_QQ_DAILY      1
#define AO_FEED_QQ_RADAR      2
void ao_acct_feed_enter(int kind);   /* 进推荐详情（kind 见上） */
int  ao_acct_feed(void);             /* 当前推荐来源 */
void ao_acct_feed_set(int kind);     /* 结果回来时确认来源（ao_result 用） */

/* 账号页那半份"详情身份 Key"（每日推荐标记 / 账号歌单 ID / 空串）。
 * ao_model.cc 的 ao_detail_want_key() 在"不是 meting 歌单详情"时委托给它。 */
const char *ao_acct_detail_want_key(void);

void ao_acct_detail_exit(void);        /* 返回列表：detail/sel/daily/page 全复位 */
void ao_acct_daily_enter(void);        /* 进"每日推荐"详情（= feed_enter(0)） */
void ao_acct_pl_enter(int idx);        /* 进某个账号歌单详情 */
void ao_acct_detail_close(void);       /* 只把详情关掉（读不出来时退回列表用） */
void ao_acct_detail_mark_closed(void); /* 只清 detail 标志（online_on_delete 用） */
/* OREQ_DAILY 成功：每日推荐是一整份（33 首左右），不需要懒加载 */
void ao_daily_loaded(void);

/* ---------------------------------------------------------------- 账号 / 登录 */

const char *ao_login_name(void);
size_t      ao_acct_pls_count(void);
bool        ao_acct_pls_busy(void);
/* 账号歌单列表（LVGL 侧只读；换新/释放都在本模块里做） */
online_acct_playlist_t *ao_acct_playlists(size_t *n);
uint32_t    ao_backoff_ms(void);

/* 昵称从两个地方来：**NVS 里存的那份**（重启后立刻可用）和 /user/account 的结果。
 * 这里只做"NVS → s_login_name"的补齐，避免每次重画都把界面上的新昵称盖回去。 */
void ao_login_name_sync(void);
/* 直接写昵称（QQ 的昵称在抓歌单时才拿到） */
void ao_login_name_set(const char *name);

/* 失败：间隔翻倍（上限 60 s）；成功：复位成 5 s */
void ao_backoff_update(bool ok);
/* 重新进页面：重试节奏从头算（失败过也立刻再试） */
void ao_backoff_reset(void);
/* online_on_delete()：把"已经拉过吗 / 在途吗"复位（下次进来重新拉），**不动 try_tick** */
void ao_session_end_account(void);

/* 账号页的"该发请求了吗"：一次没拉过就拉；失败过就按节流重试
 * （重启后 Wi-Fi 还没连上时第一次必然失败，不重试就整轮都是空的）。 */
void ao_acct_kick_if_needed(uint32_t now);
/* 昵称同理：失败（断网/服务没起来）时不能把"问过了"钉死，否则用户名永远是空的 */
void ao_acct_nick_kick_if_needed(uint32_t now);

/* OREQ_ACCOUNT 结果：记时间戳 + 退避 + 昵称（不含 rebuild/下单） */
void ao_account_result(esp_err_t err, const char *nick, uint32_t now);
/* 账号有效时顺手把账号歌单拉回来（调用方在做完 rebuild 之后再调，顺序与拆分前一致） */
void ao_acct_pls_after_account(bool account_ok, uint32_t now);
/* OREQ_ACCT_PLS 结果：换新列表 + 退避 + 失败日志 */
void ao_acct_pls_result(esp_err_t err, online_acct_playlist_t *pls, size_t n, uint32_t now);
/* 扫码登录成功（803）：换账号了，昵称/歌单/退避全部从头算（清掉上个账号的昵称） */
void ao_login_success_forget(uint32_t now);
/* 扫码登录成功后立刻验证 cookie 并取昵称（成功后自动拉歌单） */
void ao_login_success_pull_account(void);
/* OREQ_LOGOUT：清昵称 + 清歌单 + 复位拉取状态（换账号后绝不能先看到别人的歌单） */
void ao_logout_reset(uint32_t now);
/* 账号页发现"当前判定未登录"：拉取状态复位 + 清昵称 + 退回列表（**不动页码**） */
void ao_acct_logged_out_view(void);
