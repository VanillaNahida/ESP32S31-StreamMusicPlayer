/*
 * music_like.cc —— 播放器页的红心（"我喜欢的音乐"）
 *
 * 两件事：
 *   ① **状态查询**：当前这首在线歌在不在"我喜欢"里（/likelist 拉全量 id，二分）。
 *   ② **加 / 取消**：/like?id=&like=true|false。
 *
 * ★ 线程纪律（与 music_online 一致，改之前先看那一段的注释）：
 *   网络请求**只能**在工作线程（"like" 任务）里做 —— LVGL 任务里做一次 TLS
 *   握手就是几百毫秒的掉帧。工作线程只写 music_state 上的 volatile 字段，
 *   LVGL 任务在 tick 里消费。
 *
 * ★ 代次（like_seq）：用户点了红心立刻切歌是常态。工作线程跑完时曲目可能已经
 *   换了，若不校验代次，上一首的"已喜欢"就会贴到新歌上（红心亮错歌）。所以
 *   工作线程最后写 like_pub = 请求发起时的 seq，消费方只在
 *   like_pub == like_seq（并且 id 也对得上）时才相信 like_state。
 *
 * ★ 为什么还要一张"最近 8 首"的小表：服务端的 /likelist 有时滞后（刚加完立刻
 *   重拉还能看到旧表）。会话内改过的结果记一份，同步时优先用它，
 *   "刚点亮、切走再切回来又变空心"就不会出现。它只活在工作线程里，重启即清。
 */
#include "music_like.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "online_service.h"
#include "ui_core.h"

#include "music_online.h"
#include "music_state.h"

static const char *TAG = "app_music";

/* 8 KB 和 music_online 的 12 KB 比小一截：这里只有一个 GET + 一个小 JSON
 * （/likelist 最多几十 KB），没有 TLS 长连接重协商那套。 */
#define LIKE_TASK_STACK      (8 * 1024)
/* 同步失败后的退避（tick 数，500 ms/tick ⇒ 10 s）。别每 tick 都去打接口：
 * 没网的时候那等于每 500 ms 一次失败超时，还占着工作线程。 */
#define LIKE_BACKOFF_TICKS   20
/* 会话内"最近改过"的曲目数 */
#define LIKE_RECENT_MAX      8

typedef enum {
    LIKE_OP_SYNC = 0,     /* 只查状态 */
    LIKE_OP_ADD  = 1,     /* 加进"我喜欢" */
    LIKE_OP_DEL  = 2,     /* 移出"我喜欢" */
} like_op_t;

typedef struct {
    uint32_t seq;                     /* 发起时的曲目代次 */
    like_op_t op;
    char     id[24];
} like_req_t;

/* ---------------------------------------------------------------- 工作线程私有状态
 *
 * ★ 下面这些**只由工作线程读写**（LVGL 侧一律通过 music_state 的中转字段）。
 *   唯一例外是 like_uid_snapshot 在首次同步前的初始化，也在工作线程里完成。 */

static uint64_t *s_ids;               /* "我喜欢"的全量 id（升序去重） */
static size_t    s_ids_n;
static bool      s_ids_loaded;
static char      s_uid_snapshot[24];  /* 这张表属于哪个 uid（换账号就作废） */
/* 这张表属于哪个音源（换平台/换账号都要作废重拉） */
static uint8_t   s_src_snapshot = ONLINE_SRC_NETEASE;

typedef struct {
    char id[24];
    bool liked;
} like_recent_t;

static like_recent_t s_recent[LIKE_RECENT_MAX];
static int           s_recent_next;

/* LVGL 侧读写：退避计数（static 就够，反正只在 LVGL 任务里动） */
static int s_backoff;

static void recent_put(const char *id, bool liked)
{
    if (!id || !id[0]) {
        return;
    }
    for (int i = 0; i < LIKE_RECENT_MAX; i++) {
        if (s_recent[i].id[0] && strcmp(s_recent[i].id, id) == 0) {
            s_recent[i].liked = liked;      /* 已有条目：就地更新，不占新槽 */
            return;
        }
    }
    like_recent_t *r = &s_recent[s_recent_next];
    s_recent_next = (s_recent_next + 1) % LIKE_RECENT_MAX;
    snprintf(r->id, sizeof(r->id), "%s", id);
    r->liked = liked;
}

static bool recent_get(const char *id, bool *liked)
{
    if (!id || !id[0]) {
        return false;
    }
    for (int i = 0; i < LIKE_RECENT_MAX; i++) {
        if (s_recent[i].id[0] && strcmp(s_recent[i].id, id) == 0) {
            *liked = s_recent[i].liked;
            return true;
        }
    }
    return false;
}

static void ids_drop(void)
{
    free(s_ids);
    s_ids = NULL;
    s_ids_n = 0;
    s_ids_loaded = false;
}

/*
 * 保证 s_ids 是"当前音源 + 当前账号"的那张表；返回 false = 拿不到（未登录/网络失败）。
 *
 * 两个平台各有一套"我喜欢"：
 *   网易云 /likelist?uid=<uid>       （全量 id，几百字节）
 *   QQ    /qq/user/<euin>/fav/songs  （按页给歌，见 online_qq_likelist）
 * 换平台时必须重拉：同一个数字 id 在两边是完全不同的歌。
 */
static bool ids_ensure(void)
{
    const uint8_t src = music_online_current_src();
    const char *uid = (src == ONLINE_SRC_QQ) ? online_qq_musicid() : online_netease_uid();
    if (s_ids_loaded && s_src_snapshot == src && strcmp(s_uid_snapshot, uid) == 0) {
        return true;
    }
    ids_drop();
    uint64_t *ids = NULL;
    size_t n = 0;
    const esp_err_t err = (src == ONLINE_SRC_QQ) ? online_qq_likelist(&ids, &n)
                                                 : online_netease_likelist(&ids, &n);
    if (err != ESP_OK) {
        return false;
    }
    s_ids = ids;
    s_ids_n = n;
    s_ids_loaded = true;
    s_src_snapshot = src;
    /* ★ snapshot 要在调用**之后**取：uid 为空时 likelist 内部会先打一次
     *   /user/account 把 uid 补上，之后 online_netease_uid() 才有值。 */
    snprintf(s_uid_snapshot, sizeof(s_uid_snapshot), "%s",
             (src == ONLINE_SRC_QQ) ? online_qq_musicid() : online_netease_uid());
    return true;
}

/* 当前音源登录着吗 */
static bool like_logged_in(void)
{
    return (music_online_current_src() == ONLINE_SRC_QQ) ? online_qq_logged_in()
                                                         : online_netease_logged_in();
}

/* ---------------------------------------------------------------- 工作线程 */

static void like_task(void *arg)
{
    like_req_t *req = (like_req_t *)arg;
    music_state_t *st = music_state();
    int state = 0;        /* 0 = 本次没得出状态（不改） */
    int result = 0;       /* toast 用 */

    if (!like_logged_in()) {
        if (req->op != LIKE_OP_SYNC) {
            result = 3;                 /* 请先登录 */
        }
        st->like_failed = true;         /* 查不到状态 → 让 LVGL 侧退避 */
    } else if (req->op == LIKE_OP_SYNC) {
        if (ids_ensure()) {
            state = online_netease_liked_has(s_ids, s_ids_n, req->id) ? 1 : 2;
        } else {
            st->like_failed = true;
        }
    } else {
        const bool want = (req->op == LIKE_OP_ADD);
        const esp_err_t err = (music_online_current_src() == ONLINE_SRC_QQ)
                                  ? online_qq_like(req->id, want)
                                  : online_netease_like(req->id, want);
        if (err == ESP_OK) {
            state  = want ? 1 : 2;
            result = want ? 1 : 2;
            recent_put(req->id, want);
            s_ids_loaded = false;       /* 表变了：下次同步重拉一份，别在数组里插/删 */
        } else if (err == ESP_ERR_INVALID_STATE) {
            result = 3;                 /* cookie 过期 / 未登录 */
        } else {
            result = 4;
        }
    }

    /* 会话内刚点过的那首优先（服务端 likelist 可能还没反映） */
    if (state == 0) {
        bool liked = false;
        if (recent_get(req->id, &liked)) {
            state = liked ? 1 : 2;
        }
    }

    if (state) {
        snprintf(st->like_pub_id, sizeof(st->like_pub_id), "%s", req->id);
        st->like_state = state;
        st->like_pub = req->seq;        /* ★ 发布：消费方据此判断"这条结果还算数吗" */
    }
    if (result) {
        st->like_result = result;
    }

    ESP_LOGD(TAG, "like task done: id=%s op=%d state=%d result=%d", req->id, (int)req->op,
             state, result);
    free(req);
    st->like_busy = false;              /* ★ 最后才清 busy（清了 LVGL 就会接着调度下一次） */
    vTaskDelete(NULL);
}

/* LVGL 任务：起一个工作线程。返回 false = 没起来（busy / 没内存） */
static bool like_kick(like_op_t op, const char *id, uint32_t seq)
{
    music_state_t *st = music_state();
    if (st->like_busy) {
        return false;
    }
    like_req_t *req = (like_req_t *)calloc(1, sizeof(*req));
    if (!req) {
        return false;
    }
    req->seq = seq;
    req->op  = op;
    snprintf(req->id, sizeof(req->id), "%s", id);

    /* ★ 先占位再起线程：反过来的话线程可能先跑完把 busy 清掉，这里再置 true，
     *   就再也没人清了（红心永远转圈）。 */
    st->like_busy = true;
    if (xTaskCreate(like_task, "like", LIKE_TASK_STACK, req, 2, NULL) != pdPASS) {
        st->like_busy = false;
        free(req);
        ESP_LOGW(TAG, "like task 起不来（内存不足）");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- LVGL 任务侧 */

/*
 * 把「刚收藏的那首」记进 online_service（内存 + NVS）。
 *
 * 用户需求（2026-11）："我喜欢的音乐的封面没有成功使用最新收藏的一首歌的封面作为封面" ——
 * 歌单卡片的封面原来取列表第一首（通常是最早收藏的那首），所以收藏完回去看封面永远不变。
 * 这里在**加收藏成功**的那一刻把这首歌（带封面地址）记下来，由 app_online 打开
 * 「我喜欢的音乐」时优先拿它当封面。
 *
 * 只在 id 对得上时记：like_pub_id 是"那次请求针对的那首"，切歌后 like_cur_id 已经换了，
 * 拿 on_songs[index] 会记错一首（红心代次那一套防的就是这个）。
 */
static void remember_liked_song(const music_state_t *st)
{
    if (!st->on_mode || !st->on_songs || st->index < 0 ||
        (size_t)st->index >= st->on_count) {
        return;
    }
    const online_song_t *song = &((const online_song_t *)st->on_songs)[st->index];
    char id[24];
    snprintf(id, sizeof(id), "%u", (unsigned)song->id);
    if (strcmp(id, st->like_cur_id) != 0) {
        return;                          /* 记的不是这一首：算了，宁可少记也不要记错 */
    }
    online_last_liked_song_set(song);
}

void music_like_tick(void)
{
    music_state_t *st = music_state();

    /* ① 先消费 toast —— 那是"用户刚才那一下"的反馈，与当前是哪首歌无关 */
    if (st->like_result) {
        switch (st->like_result) {
        case 1:  ui_toast("已添加到我喜欢歌单", 1600);
                 remember_liked_song(st);
                 break;
        case 2:  ui_toast("已取消喜欢", 1600); break;
        case 3:  ui_toast((music_online_current_src() == ONLINE_SRC_QQ)
                              ? "请先登录 QQ 音乐" : "请先登录网易云账号", 1800); break;
        default: ui_toast("操作失败，请稍后重试", 1600); break;
        }
        st->like_result = 0;
    }

    /* ② 同步失败 → 退避一段时间再试 */
    if (st->like_failed) {
        st->like_failed = false;
        s_backoff = LIKE_BACKOFF_TICKS;
    }
    if (st->like_busy) {
        return;
    }

    /* ③ 取当前曲目 id：本地曲目 / 没在线队列 → 状态归零（视图据此隐藏红心） */
    char id[24];
    if (!music_online_current_id(id, sizeof(id))) {
        if (st->like_cur_id[0]) {
            st->like_cur_id[0] = '\0';
            st->like_state = 0;
            st->like_pub = MUSIC_LIKE_PUB_NONE;
            st->like_seq = st->like_seq + 1;   /* volatile 自增在 C++26 被弃用(-Werror=volatile) */
            s_backoff = 0;
        }
        return;
    }

    /* ④ 换了歌：代次 ++、状态清空。旧结果靠代次自动作废，不用等它跑完。 */
    if (strcmp(id, st->like_cur_id) != 0) {
        snprintf(st->like_cur_id, sizeof(st->like_cur_id), "%s", id);
        st->like_state = 0;
        st->like_pub = MUSIC_LIKE_PUB_NONE;
        st->like_seq = st->like_seq + 1;   /* volatile 自增在 C++26 被弃用(-Werror=volatile) */
        s_backoff = 0;
        return;                          /* 下一个 tick 再同步（别和换曲抢同一个 tick） */
    }

    /* ⑤ 未登录：不查（省一次必然失败的请求），红心保持空心 */
    if (!like_logged_in()) {
        st->like_state = 0;
        return;
    }

    /* ★ 只有代次对得上（like_pub == like_seq）且 id 也是这一首，状态才算"已知"。
     *   否则是换歌前那个请求迟到的结果，不能挡住这次同步。 */
    if (st->like_state != 0 && st->like_pub == st->like_seq &&
        strcmp(st->like_pub_id, id) == 0) {
        return;                          /* 已知 */
    }
    if (s_backoff > 0) {
        s_backoff--;
        return;
    }
    like_kick(LIKE_OP_SYNC, id, st->like_seq);
}

void music_like_toggle(void)
{
    music_state_t *st = music_state();

    char id[24];
    if (!music_online_current_id(id, sizeof(id))) {
        return;                          /* 本地曲目：按钮本来是隐藏的，这里兜底 */
    }
    if (st->like_busy) {
        return;                          /* 转圈中：忽略连点 */
    }
    if (!like_logged_in()) {
        ui_toast((music_online_current_src() == ONLINE_SRC_QQ) ? "请先登录 QQ 音乐"
                                                              : "请先登录网易云账号", 1800);
        return;
    }

    /* 还没拿到状态：这次点击先用来"查状态"，并告诉用户再点一次。
     * 不直接猜成"未喜欢→加" —— 猜错的代价是用户以为取消了收藏。 */
    if (st->like_state == 0) {
        if (strcmp(id, st->like_cur_id) != 0) {
            snprintf(st->like_cur_id, sizeof(st->like_cur_id), "%s", id);
            st->like_seq = st->like_seq + 1;   /* volatile 自增在 C++26 被弃用(-Werror=volatile) */
        }
        ui_toast("正在获取收藏状态，请稍候", 1500);
        like_kick(LIKE_OP_SYNC, id, st->like_seq);
        return;
    }

    like_kick((st->like_state == 1) ? LIKE_OP_DEL : LIKE_OP_ADD, id, st->like_seq);
}
