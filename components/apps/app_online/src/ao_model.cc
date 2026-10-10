/*
 * ao_model.cc —— app_online 的**歌单队列 / 分页 / 懒加载模型**（纯逻辑，一行 LVGL 都没有）
 *
 * 这里装的是"跟画界面无关"的状态与迁移：队列所有权、分页数学、详情身份 Key、
 * 账号歌单懒加载游标、页面进入/销毁时的一次性复位。
 * 账号与登录状态机（昵称、退避节流、账号歌单拉取状态）在 ao_account.cc。
 *
 * 规矩（改之前先读）：
 *   1) **不 include 任何 lvgl / ui_* 头**。凡是需要"现在几点"，都由调用方把
 *      `lv_tick_get()` 的结果当参数传进来（形参名统一 `now`）——这样本文件在 host 上
 *      可以直接编译单测（见 docs/refactor/04_app_online.md §6.3）。
 *   2) **绝不 free s_songs**（播放器可能还持有）；旧队列一律走 s_stale →
 *      ao_queue_drop_stale() → app_music_online_detach()。
 *   3) 懒加载缓冲**一次 calloc、指针永不 realloc**（播放器存的就是这个指针）。
 *   4) 日志文案与拆分前逐字一致（真机排查按它过滤）。
 */
#include "ao_model.h"

#include "ao_account.h"
#include "ao_work.h"

static const char *TAG = AO_TAG;

/*
 * ★ 故意**不 include app_music.h**：那个头文件 include 了 lvgl.h，一进来就把整个
 *   LVGL 拖进这个"纯逻辑"编译单元（本文件要能在 host 上直接编译单测）。
 *   签名必须与 app_music.h 保持一致 —— 改那边时这里一起改。
 */
extern "C" void app_music_online_detach(const online_song_t *songs);

/* 当前显示（= 交给 app_music 的）队列 */
static online_song_t *s_songs;
static size_t         s_song_n;
/* 被替换下来、但播放器可能还在引用的旧数组：等播放器切到新数组后再释放 */
static online_song_t *s_stale;
/* s_songs 是谁的结果（OWNER_*） */
static int  s_owner = OWNER_PL;

/*
 * s_songs 属于"哪一个详情"。歌单详情页只画**自己那一份**；Key 对不上就显示转圈。
 *
 * ★ 这是"点进歌单先看到转圈、加载成功才显示歌单"的关键（用户 2026-10 报
 *   "播放列表的歌曲我挨个点开了怎么都是同样的"）：原先只判
 *   `s_owner == OWNER_PL && s_song_n > 0`，而点进第二个歌单时 s_songs 里还是
 *   **上一个歌单**的歌（owner 也还是 OWNER_PL），于是列表先把上一个歌单的歌顶上来
 *   —— 用户看到、点到的全是上一个歌单的歌。现在用"歌单 ID"当身份：
 *     meting 歌单 → 歌单 ID
 *     账号歌单   → 账号歌单 ID（判断在 ao_account.cc）
 *     每日推荐   → DETAIL_KEY_DAILY（不是歌单，给个专用标记）
 *     搜索结果   → 空串（详情页不该画它）
 */
static char s_cur_pl_id[ONLINE_ID_MAX];

/* ---- 分页 + 懒加载运行期状态 ---- */
static int    s_pl_page;            /* 我添加的歌单详情当前页号 */
static int    s_srch_page;          /* 搜索结果当前页号 */
static size_t s_songs_cap;          /* s_songs 的槽位数（懒加载缓冲的容量） */
static size_t s_songs_total;        /* 歌单总首数（账号歌单已知；0 = 未知） */
static bool   s_acct_lazy;          /* s_songs 是账号歌单的懒加载缓冲（可增量追加） */
static bool   s_fetch_busy;         /* 正在补页 */
static bool   s_fetch_fail;         /* 上次补页失败：给"重试"出口，别自己死循环 */

/* ---- 「我添加的歌单」详情状态 ---- */
static bool      s_pl_detail;       /* true = 正在看某个歌单的歌曲列表 */
static int       s_sel_pl = -1;     /* 详情对应的歌单下标；-1 = 歌单列表 */

/* 详情请求的"发出时刻"（超时兜底起点；见 ao_detail_wait_rows） */
static uint32_t s_detail_tick;

/* ================================================================ 分页数学 */

/* 第 page 页的第一首在 s_songs 里的**绝对下标**（点行即播/懒加载都要用它） */
size_t ao_page_first(int page)
{
    return (page > 0) ? (size_t)page * ONLINE_PAGE_ROWS : 0;
}

int ao_page_count(size_t n)
{
    if (n == 0) {
        return 1;                       /* 空列表也算一页，免得页码乱跳 */
    }
    return (int)((n + ONLINE_PAGE_ROWS - 1) / ONLINE_PAGE_ROWS);
}

int ao_page_clamp(int page, size_t n)
{
    const int pages = ao_page_count(n);
    if (page < 0) {
        return 0;
    }
    return (page >= pages) ? (pages - 1) : page;
}

/*
 * 账号歌单详情的页数。
 * 总首数已知（正常情况）就是 page_count(total)；
 * **未知时（服务端没给 trackCount）要额外小心**：如果最后一页正好填满，也必须多给
 * 一页 —— 否则用户翻到"第 1/1 页"就再也点不动，只能看到前 100 首。
 * 那一页点下去会去取；服务端没有更多歌时返回空页，结果处理里按"到末尾"收尾
 * （s_songs_total = s_song_n），多出来的这一页自己消失。
 */
int ao_acct_page_count(void)
{
    if (s_songs_total) {
        return ao_page_count(s_songs_total);
    }
    const int pages = ao_page_count(s_song_n);
    if (s_song_n > 0 && (s_song_n % ONLINE_PAGE_ROWS) == 0 && s_song_n < s_songs_cap) {
        return pages + 1;
    }
    return pages;
}

int  ao_pl_page(void)            { return s_pl_page; }
void ao_pl_page_set(int page)    { s_pl_page = page; }
int  ao_srch_page(void)          { return s_srch_page; }
void ao_srch_page_set(int page)  { s_srch_page = page; }

/* ================================================================ 详情身份 Key */

/* 详情页要的那份 key（歌单 ID / 每日推荐标记）；返回空串表示"没有明确目标" */
const char *ao_detail_want_key(void)
{
    if (s_pl_detail && s_sel_pl >= 0) {
        const char *id = online_playlist_id((size_t)s_sel_pl);
        return id ? id : "";
    }
    /* 不是 meting 歌单详情：账号页那半份（每日推荐 / 账号歌单）委托给 ao_account */
    return ao_acct_detail_want_key();
}

/* 当前缓冲是不是"这个详情要的那一份"（**懒加载刚开的空壳也算**：一首都没载入） */
bool ao_detail_key_matches(void)
{
    const char *want = ao_detail_want_key();
    return (want[0] && s_cur_pl_id[0] && s_songs != NULL && strcmp(s_cur_pl_id, want) == 0);
}

/* 现在内存里这份 s_songs 是 ao_detail_want_key() 要的，**而且已经有歌可画** */
bool ao_detail_owns_songs(void)
{
    return (s_song_n > 0 && ao_detail_key_matches());
}

void ao_detail_mark_request(uint32_t now)
{
    s_detail_tick = now;
}

uint32_t ao_detail_tick(void)
{
    return s_detail_tick;
}

bool ao_pl_detail(void)           { return s_pl_detail; }
int  ao_pl_sel(void)              { return s_sel_pl; }

void ao_pl_detail_enter(int idx)
{
    s_sel_pl    = idx;
    s_pl_detail = true;
    s_pl_page   = 0;                 /* 新歌单从第 1 页看起 */
}

void ao_pl_detail_exit(void)
{
    s_pl_detail = false;
    s_sel_pl    = -1;
    s_pl_page   = 0;
}

void ao_pl_detail_drop(void)
{
    s_pl_detail = false;
    s_sel_pl    = -1;
}

/* ================================================================ 队列所有权 */

online_song_t *ao_queue_songs(void) { return s_songs; }
size_t         ao_queue_count(void) { return s_song_n; }
size_t         ao_queue_cap(void)   { return s_songs_cap; }
int            ao_queue_owner(void) { return s_owner; }
const char    *ao_queue_key(void)   { return s_cur_pl_id; }

/*
 * 回收"被替换下来的旧队列"（s_stale）。
 *
 * ★ **绝不在这里 free**（2026-10 修 use-after-free）：播放器可能还存着这个数组的
 *   指针（用户点歌之后又打开别的歌单时，app_music 的 s_on_songs 仍指着它），
 *   直接 free 会让上/下一首、自动续播读到已释放内存。所有权交给 app_music：
 *   它在换队列/退回本地播放时自己放（见 app_music_online_detach）。
 */
void ao_queue_drop_stale(void)
{
    if (s_stale) {
        app_music_online_detach(s_stale);
        s_stale = NULL;
    }
}

/*
 * 换掉当前队列 s_songs。
 *
 * ★ 老的那份**绝不在这里 free**（2026-10 修 use-after-free）：播放器可能还存着它的
 *   指针（用户点歌之后又打开别的歌单时，app_music 的 s_on_songs 仍指着它），
 *   直接 free 会让上/下一首、自动续播读到已释放内存。统一走 s_stale →
 *   ao_queue_drop_stale() → app_music_online_detach()：播放器还在用就由它接管
 *   （它换队列/退回本地播放时自己放），没用就立刻放掉。
 */
void ao_queue_replace(online_song_t *buf, size_t cap, size_t count,
                      const char *key, int owner)
{
    ao_queue_drop_stale();
    if (s_songs && s_songs != buf) {
        s_stale = s_songs;
    }
    s_songs       = buf;
    s_song_n      = count;
    s_songs_cap   = cap;
    s_owner       = owner;
    /* 懒加载/分页状态跟着换队列一起复位 */
    s_acct_lazy   = false;
    s_songs_total = 0;
    s_fetch_busy  = false;
    s_fetch_fail  = false;
    /*
     * 歌单 ID 的契约长度是 ONLINE_ID_MAX（网易云是数字 ID，服务端存 NVS 时也按
     * 这个长度截断），而这里拿到的 pl_id 来自 64 字节的请求字段 —— 按契约精度拷贝，
     * 超长的本来就不是合法 ID（不按契约截断的话，s_cur_pl_id 会比服务侧存的 ID 长，
     * 后面 strcmp 对不上，UI 上"当前歌单"的高亮就丢了）。
     */
    snprintf(s_cur_pl_id, sizeof(s_cur_pl_id), "%.*s",
             (int)(sizeof(s_cur_pl_id) - 1), key ? key : "");
}

/* 安装一份**整份**队列（meting 歌单 / 搜索 / 每日推荐的结果） */
void ao_queue_install(online_song_t *songs, size_t count, const char *pl_id, int owner)
{
    ao_queue_replace(songs, count, count, pl_id, owner);
}

/* OREQ_ADD 成功后：内存里那份（s_songs）已经过期，把身份标记清掉，
 * 下次点进这个歌单会重新读，而不是先把旧内容顶上来 */
void ao_queue_key_clear_if(const char *key)
{
    if (s_cur_pl_id[0] && key && strcmp(s_cur_pl_id, key) == 0) {
        s_cur_pl_id[0] = '\0';
    }
}

/* ================================================================ 账号歌单懒加载 */

bool   ao_acct_lazy(void)   { return s_acct_lazy; }
size_t ao_songs_total(void) { return s_songs_total; }
bool   ao_fetch_fail(void)  { return s_fetch_fail; }

void ao_fetch_fail_clear(void)
{
    s_fetch_fail = false;
}

/*
 * 打开一个账号歌单：按"歌单总首数"一次性分配好缓冲（**指针从此不变** ——
 * 播放器的队列指针一直有效），但**先不填**，由 ao_lazy_fetch_if_needed()
 * 按当前页的需要去取（懒加载）。
 *
 * total = 0 表示"服务端没给 trackCount"：那就按最大容量分配（宁可多占点 PSRAM，
 * 也不能让用户只能看到前 100 首）；真实总数会在第一次"取不满一页"时被修正。
 */
bool ao_lazy_begin(const char *pl_id, size_t total)
{
    size_t cap = total ? total : ONLINE_SONGS_MAX;
    if (cap > ONLINE_SONGS_MAX) {
        cap = ONLINE_SONGS_MAX;
    }
    if (cap < ONLINE_PAGE_ROWS) {
        cap = ONLINE_PAGE_ROWS;
    }
    online_song_t *buf = (online_song_t *)calloc(cap, sizeof(online_song_t));
    if (!buf) {
        ESP_LOGW(TAG, "acct buffer alloc failed (%u slots)", (unsigned)cap);
        return false;
    }
    ao_queue_replace(buf, cap, 0, pl_id, OWNER_ACCT);
    s_acct_lazy   = true;
    s_songs_total = total;
    ESP_LOGI(TAG, "acct lazy buffer: %u slots (playlist trackCount=%u)",
             (unsigned)cap, (unsigned)total);
    return true;
}

/* 把刚取到的一页**追加到已载入末尾**（必须连续，否则丢弃：说明结果过期了） */
void ao_lazy_append(size_t off, const online_song_t *page, size_t n)
{
    if (!s_acct_lazy || !s_songs || n == 0) {
        return;
    }
    if (off != s_song_n || off + n > s_songs_cap) {
        ESP_LOGW(TAG, "drop page (off=%u n=%u loaded=%u cap=%u)",
                 (unsigned)off, (unsigned)n, (unsigned)s_song_n, (unsigned)s_songs_cap);
        return;
    }
    memcpy(&s_songs[off], page, n * sizeof(online_song_t));
    s_song_n = off + n;
}

/*
 * 懒加载的唯一入口：详情页"当前页还缺歌"时补一段（一次最多 ACCT_FETCH_MAX 首）。
 * 结果到了会重新渲染；如果还是不够，渲染时再调一次 —— 于是它天然是"按需"的，
 * 用户翻到第几页才取到第几页。
 */
void ao_lazy_fetch_if_needed(uint32_t now)
{
    if (!s_acct_lazy || !s_songs || !ao_acct_detail()) {
        return;
    }
    if (s_fetch_busy || s_fetch_fail) {
        return;
    }
    const size_t pls_n = ao_acct_pls_count();
    const int sel = ao_acct_sel();
    if (sel < 0 || (size_t)sel >= pls_n) {
        return;
    }
    /* 目标：当前页要显示到 (page+1) * ONLINE_PAGE_ROWS 首 */
    size_t need = ao_page_first(ao_acct_page()) + ONLINE_PAGE_ROWS;
    if (s_songs_total && need > s_songs_total) {
        need = s_songs_total;
    }
    if (s_song_n >= need) {
        return;                             /* 已经够了（或本来就到头了） */
    }
    size_t want = need - s_song_n;
    if (want > ACCT_FETCH_MAX) {
        want = ACCT_FETCH_MAX;
    }
    if (s_song_n + want > s_songs_cap) {
        want = s_songs_cap - s_song_n;
    }
    if (want == 0) {
        return;
    }
    s_fetch_busy = true;
    s_detail_tick = now;                    /* 超时兜底从这次请求算起 */
    const online_acct_playlist_t *p = ao_acct_playlists(NULL) + sel;
    ESP_LOGI(TAG, "acct lazy fetch: '%s' offset=%u count=%u",
             p->id, (unsigned)s_song_n, (unsigned)want);
    ao_work_submit_ex(OREQ_ACCT_SONGS, p->id, (int)s_song_n, (int)want);
}

/*
 * OREQ_ACCT_SONGS 的一页结果：**追加**而不是"安装" —— 缓冲是 ao_lazy_begin()
 * 一次性分配好的，指针从头到尾不变，所以播放器那边的队列指针一直有效。
 * 日志文案与拆分前逐字一致（真机排查按它过滤）。
 */
ao_page_outcome_t ao_lazy_handle_page(int off_i, int want, esp_err_t err, const char *key,
                                      const online_song_t *page, size_t n)
{
    s_fetch_busy = false;
    const size_t off = (off_i > 0) ? (size_t)off_i : 0;
    if (err == ESP_OK && page && n > 0) {
        if (s_acct_lazy && strcmp(s_cur_pl_id, key ? key : "") == 0) {
            ao_lazy_append(off, page, n);
            /* 要一页却给不满一页 = 到歌单末尾了：把总数按实际修正，
             * 免得列表一直显示"还有下一页" */
            if (n < (size_t)want) {
                s_songs_total = s_song_n;
            }
            return AO_PAGE_APPENDED;
        }
        ESP_LOGW(TAG, "drop acct page: buffer=%s req=%s lazy=%d", s_cur_pl_id,
                 key ? key : "", (int)s_acct_lazy);
        return AO_PAGE_NONE;
    }
    if (off > 0 && (err == ESP_ERR_NOT_FOUND || (err == ESP_OK && n == 0))) {
        /*
         * 服务端"没有更多歌了"（空页）—— 只有总首数未知时才会走到这儿
         * （已知总数时页数算得准，不会去要点不存在的页）。
         * 这不是失败：按已载入数收尾，那个多出来的"下一页"自己就消失了。
         */
        ESP_LOGI(TAG, "acct playlist ended at %u songs", (unsigned)s_song_n);
        s_songs_total = s_song_n;
        return AO_PAGE_ENDED;
    }
    /* 这一页没取到：给"重试"出口（渲染时画出来），别自己死循环重发 */
    s_fetch_fail = true;
    return AO_PAGE_FAILED;
}

/* ================================================================ 页面生命周期 */

void ao_session_begin(uint32_t now)
{
    ao_detail_mark_request(now);         /* 详情超时兜底的起点（别沿用上一轮页面的旧时刻） */
    ao_backoff_reset();                  /* 重新进 APP：重试节奏从头算（失败过也立刻再试） */
    /* 分页/懒加载：页码从第 1 页开始（静态变量跨页面实例存活，必须显式复位） */
    ao_acct_page_set(0);
    s_pl_page    = 0;
    s_srch_page  = 0;
    s_fetch_busy = false;
    s_fetch_fail = false;
}

void ao_session_end(void)
{
    /* 请求序号/重试节流复位：下次进来重新拉（不能沿用上一轮"问过了"的状态） */
    ao_session_end_account();
    /* 分页/懒加载状态复位：s_songs 还在内存里（播放器可能还在放），但页码、
     * 在途标志不能留到下一次进页面 */
    ao_acct_page_set(0);
    s_pl_page    = 0;
    s_srch_page  = 0;
    s_fetch_busy = false;
    s_fetch_fail = false;
}

void ao_detail_close_all(void)
{
    s_pl_detail = false;
    ao_acct_detail_mark_closed();
}
