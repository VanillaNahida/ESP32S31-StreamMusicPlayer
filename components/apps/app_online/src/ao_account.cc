/*
 * ao_account.cc —— 网易云**账号与登录状态机**（纯逻辑，一行 LVGL 都没有）
 *
 * 装的是第 0 页「我的歌单」里跟账号有关的一切：
 *   - "正在看哪一份详情"（每日推荐 / 某个账号歌单）+ 它的分页游标
 *   - 账号信息（昵称）与账号歌单列表的拉取状态、失败退避节流
 *   - 登录成功 / 退出登录 / 判定未登录 时的状态复位
 * 二维码**怎么画**、状态行文字怎么显示在 ao_qr_login.cc；800/801/802/803 怎么走
 * 在 ao_result.cc。
 *
 * 规矩（与 ao_model.cc 一致）：不 include lvgl / ui_*；"现在几点"由调用方用
 * `lv_tick_get()` 传进来（形参名 `now`），本模块自己绝不调 lv_tick_get()。
 */
#include "ao_account.h"

#include "ao_provider.h"
#include "ao_work.h"

static const char *TAG = AO_TAG;

/* ---- 第 0 页详情状态 ---- */
static int       s_acct_page;        /* 账号歌单详情当前页号（0 起） */
static bool      s_acct_detail;      /* true = 正在看某个账号歌单的歌曲列表 */
static bool      s_acct_daily;       /* 详情来源：true = 一次性推荐列表 */
static int       s_acct_feed;        /* 推荐来源（AO_FEED_*，只影响标题） */
static int       s_acct_sel = -1;    /* 详情对应的歌单下标 */
static online_acct_playlist_t *s_acct_pls;   /* 账号歌单列表（本模块持有，LVGL 侧只读） */
static size_t    s_acct_pls_n;

/*
 * "正在拉账号歌单/昵称"的中间态 + 失败后的节流重试。
 *
 * ★ 为什么必须能重试（用户 2026-10 报"重启后进在线页，歌单什么的都没加载出来"）：
 *   原先 s_acct_pls_asked 是**一次性**标志 —— 开机后进页面时 Wi-Fi 可能还没连上，
 *   那一次请求失败之后这个标志仍然是 true，poll_cb 里"网络状态变了就重建页面"
 *   也拉不动它，于是整轮开机都停在"还没取到歌单"。现在失败后允许按 5 秒节流重试。
 */
static bool      s_acct_pls_asked;    /* 已经拉过一次歌单列表 */
static bool      s_acct_pls_busy;     /* 请求在路上（列表区显示转圈） */
static uint32_t  s_acct_pls_try_tick; /* 上次请求**结束**的时刻 */
static uint32_t  s_acct_nick_try_tick;/* 上次取昵称**结束**的时刻 */
static uint32_t  s_acct_backoff_ms = ACCT_RETRY_MS;
static char      s_login_name[64];    /* 已登录账号昵称（空 = 未知/未登录） */
static bool      s_acct_asked;        /* 已经拉过一次账号信息（避免反复请求） */

/* ================================================================ 第 0 页详情 */

int  ao_acct_page(void)         { return s_acct_page; }
void ao_acct_page_set(int page) { s_acct_page = page; }

bool ao_acct_detail(void)       { return s_acct_detail; }
bool ao_acct_daily(void)        { return s_acct_daily; }
int  ao_acct_sel(void)          { return s_acct_sel; }

/* 账号页那半份详情 Key（拆出去的那一段与拆分前逐字一致，判断顺序也没变） */
const char *ao_acct_detail_want_key(void)
{
    if (!s_acct_detail) {
        return "";
    }
    if (s_acct_daily) {
        /* 三个入口三种 Key：换源/换种类时旧列表立刻"不匹配"，不会被当成这一份画出来 */
        if (s_acct_feed == AO_FEED_QQ_DAILY) {
            return DETAIL_KEY_QQ_DAILY;
        }
        if (s_acct_feed == AO_FEED_QQ_RADAR) {
            return DETAIL_KEY_QQ_RADAR;
        }
        return DETAIL_KEY_DAILY;
    }
    if (s_acct_sel >= 0 && (size_t)s_acct_sel < s_acct_pls_n) {
        return s_acct_pls[s_acct_sel].id;
    }
    return "";
}

void ao_acct_detail_exit(void)
{
    s_acct_detail = false;
    s_acct_sel    = -1;
    s_acct_daily  = false;
    s_acct_feed   = AO_FEED_NETEASE_DAILY;   /* 回到列表：下次进推荐默认网易云口径 */
    s_acct_page   = 0;
}

void ao_acct_feed_enter(int kind)
{
    s_acct_daily  = true;
    s_acct_feed   = kind;
    s_acct_sel    = -1;
    s_acct_detail = true;
    s_acct_page   = 0;
}

void ao_acct_daily_enter(void)
{
    ao_acct_feed_enter(AO_FEED_NETEASE_DAILY);
}

int ao_acct_feed(void)
{
    return s_acct_feed;
}

void ao_acct_feed_set(int kind)
{
    s_acct_feed = kind;
}

void ao_acct_pl_enter(int idx)
{
    s_acct_daily  = false;
    s_acct_sel    = idx;
    s_acct_detail = true;
    s_acct_page   = 0;
}

void ao_acct_detail_close(void)
{
    s_acct_detail = false;
    s_acct_sel    = -1;
}

void ao_acct_detail_mark_closed(void)
{
    s_acct_detail = false;
}

void ao_daily_loaded(void)
{
    /* 一次性推荐列表是一整份（每日推荐 33 首 / QQ 两个源各 30 首），不需要懒加载 */
    s_acct_daily  = true;
    s_acct_detail = true;
    s_acct_page   = 0;
}

/* ================================================================ 账号 / 登录 */

const char *ao_login_name(void)     { return s_login_name; }
size_t      ao_acct_pls_count(void) { return s_acct_pls_n; }
bool        ao_acct_pls_busy(void)  { return s_acct_pls_busy; }
uint32_t    ao_backoff_ms(void)     { return s_acct_backoff_ms; }

online_acct_playlist_t *ao_acct_playlists(size_t *n)
{
    if (n) {
        *n = s_acct_pls_n;
    }
    return s_acct_pls;
}

/*
 * 昵称从两个地方来：**NVS 里存的那份**（重启后立刻可用）和 /user/account 的结果。
 * 这里只做"NVS → s_login_name"的补齐，避免每次重画都把界面上的新昵称盖回去。
 */
void ao_login_name_sync(void)
{
    if (s_login_name[0]) {
        return;
    }
    const char *n = ao_nickname();
    if (n && n[0]) {
        snprintf(s_login_name, sizeof(s_login_name), "%s", n);
    }
}

/* 直接写昵称（QQ 的昵称是抓歌单时才知道的，见 ao_result.cc 的 OREQ_ACCT_PLS） */
void ao_login_name_set(const char *name)
{
    if (name && name[0]) {
        snprintf(s_login_name, sizeof(s_login_name), "%s", name);
    }
}

/* 失败：间隔翻倍（上限 60 s）；成功：复位成 5 s */
void ao_backoff_update(bool ok)
{
    if (ok) {
        s_acct_backoff_ms = ACCT_RETRY_MS;
        return;
    }
    if (s_acct_backoff_ms < ACCT_RETRY_MAX_MS) {
        s_acct_backoff_ms *= 2;
        if (s_acct_backoff_ms > ACCT_RETRY_MAX_MS) {
            s_acct_backoff_ms = ACCT_RETRY_MAX_MS;
        }
    }
}

void ao_backoff_reset(void)
{
    s_acct_backoff_ms = ACCT_RETRY_MS;
}

void ao_session_end_account(void)
{
    /* 请求序号/重试节流复位：下次进来重新拉（不能沿用上一轮"问过了"的状态）。
     * ⚠ 只清这三个标志，**不动 try_tick 与退避间隔** —— 与拆分前逐字一致。 */
    s_acct_pls_asked = false;
    s_acct_pls_busy  = false;
    s_acct_asked     = false;
}

/*
 * 账号页的"该发请求了吗"：一次没拉过就拉；失败过就按节流重试
 * （重启后 Wi-Fi 还没连上时第一次必然失败，不重试就整轮都是空的）。
 */
void ao_acct_kick_if_needed(uint32_t now)
{
    if (!online_has_network()) {
        return;                          /* 没网：别空转，等网络事件触发重建 */
    }
    /* 已经有列表了就不再自动拉（否则每次进页面都会闪一下转圈）：
     * 要刷新就靠"退出重进"那条路（on_delete 会把 asked 复位）或用户手动重进页面 */
    if (s_acct_pls_n > 0) {
        return;
    }
    if (!s_acct_pls_asked) {
        s_acct_pls_asked  = true;
        s_acct_pls_busy   = true;
        s_acct_pls_try_tick = now;
        ao_work_submit(OREQ_ACCT_PLS, NULL);
        return;
    }
    if (s_acct_pls_busy) {
        return;                          /* 还在路上 */
    }
    if ((uint32_t)(now - s_acct_pls_try_tick) >= s_acct_backoff_ms) {
        s_acct_pls_busy     = true;
        s_acct_pls_try_tick = now;
        ESP_LOGI(TAG, "retry account playlists (上次失败/为空, 间隔 %u ms)",
                 (unsigned)s_acct_backoff_ms);
        ao_work_submit(OREQ_ACCT_PLS, NULL);
    }
}

/* 昵称同理：失败（断网/服务没起来）时不能把"问过了"钉死，否则用户名永远是空的 */
void ao_acct_nick_kick_if_needed(uint32_t now)
{
    if (s_login_name[0] || !online_has_network()) {
        return;
    }
    if (!s_acct_asked) {
        s_acct_asked        = true;
        s_acct_nick_try_tick = now;
        ao_work_submit(OREQ_ACCOUNT, NULL);
        return;
    }
    if ((uint32_t)(now - s_acct_nick_try_tick) >= s_acct_backoff_ms) {
        s_acct_nick_try_tick = now;
        ao_work_submit(OREQ_ACCOUNT, NULL);
    }
}

void ao_account_result(esp_err_t err, const char *nick, uint32_t now)
{
    s_acct_asked = true;
    s_acct_nick_try_tick = now;              /* 下次重试窗口从这里开始算 */
    ao_backoff_update(err == ESP_OK);
    if (err == ESP_OK) {
        snprintf(s_login_name, sizeof(s_login_name), "%s", nick ? nick : "");
    } else if (err == ESP_ERR_INVALID_STATE) {
        /* 服务端明确回了"没有 profile" = cookie 真的失效了 → 回未登录态 */
        s_login_name[0] = '\0';
        ESP_LOGW(TAG, "账号信息无效：cookie 已失效，请重新扫码");
    } else {
        /*
         * ★ 网络/HTTP 失败**不等于**未登录！cookie 还在 SD 上，把界面打回未登录
         *   正是用户看到的"扫码成功后反而没反应"。这里保留登录态，只记日志。
         *   昵称会由 ao_acct_nick_kick_if_needed() 按节流（5 s 起、失败翻倍，
         *   上限 60 s）自动重试（以前这里"问过就不再问"，断网那次失败就让用户名
         *   永远空着）。
         */
        ESP_LOGW(TAG, "账号信息获取失败(%s)：保留登录态，稍后重试", esp_err_to_name(err));
    }
}

void ao_acct_pls_after_account(bool account_ok, uint32_t now)
{
    if (account_ok && !s_acct_pls_asked) {
        s_acct_pls_asked = true;      /* 登录有效 → 顺手把账号歌单拉回来 */
        s_acct_pls_busy  = true;
        s_acct_pls_try_tick = now;
        ao_work_submit(OREQ_ACCT_PLS, NULL);
    }
}

void ao_acct_pls_result(esp_err_t err, online_acct_playlist_t *pls, size_t n, uint32_t now)
{
    s_acct_pls_busy = false;
    s_acct_pls_try_tick = now;       /* 失败重试的节流窗口从这里开始 */
    if (s_acct_pls) {
        free(s_acct_pls);
        s_acct_pls = NULL;
    }
    s_acct_pls   = pls;
    s_acct_pls_n = n;
    ao_backoff_update(err == ESP_OK && n > 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "账号歌单拉取失败: %s（%u ms 后自动重试）", esp_err_to_name(err),
                 (unsigned)s_acct_backoff_ms);
    }
}

void ao_login_success_forget(uint32_t now)
{
    s_acct_asked = false;            /* 换账号了：重新拉昵称 */
    s_acct_nick_try_tick = now;
    s_acct_pls_asked = false;        /* 歌单也要重拉（绝不能显示上个账号的） */
    s_acct_pls_busy  = false;
    s_acct_pls_try_tick = now;
    s_acct_backoff_ms = ACCT_RETRY_MS;   /* 新账号：重试节奏从头算 */
    /* 上个账号的昵称（NVS 里那份）不能留给新账号看：清掉，
     * 等下面那次 /user/account 回来再填 */
    s_login_name[0] = '\0';
    ao_nickname_clear();
}

void ao_login_success_pull_account(void)
{
    ao_work_submit(OREQ_ACCOUNT, NULL);   /* 立刻验证 cookie 并取昵称（成功后自动拉歌单） */
}

void ao_logout_reset(uint32_t now)
{
    s_login_name[0] = '\0';
    s_acct_asked = true;              /* 退出后别再自动拉账号 */
    s_acct_nick_try_tick = now;
    /*
     * 退出必须把上一个账号的歌单清干净：否则换账号登录后会先看到**别人的歌单**，
     * 直到新列表回来才刷新 —— 既困惑又不安全。s_acct_pls_asked 一起复位，
     * 保证下次登录会重新拉。
     */
    s_acct_pls_asked = false;
    s_acct_pls_busy  = false;
    s_acct_pls_try_tick = now;
    if (s_acct_pls) {
        free(s_acct_pls);
        s_acct_pls = NULL;
    }
    s_acct_pls_n = 0;
    s_acct_detail = false;
    s_acct_sel = -1;
    s_acct_daily = false;
}

/*
 * 账号页重建时读一次"到底登录没有"：未登录就把拉取状态复位（退出/未登录：下次登录
 * 重新拉），昵称也清掉，详情退回列表（否则会停在"载入中"转圈上）。
 * ⚠ 只清 asked/busy/昵称/详情，**不动 try_tick 与页码** —— 与拆分前逐字一致。
 */
void ao_acct_logged_out_view(void)
{
    s_acct_pls_asked = false;
    s_acct_pls_busy  = false;
    s_acct_asked     = false;
    s_login_name[0]  = '\0';
    s_acct_detail = false;
    s_acct_sel    = -1;
    s_acct_daily  = false;
}
