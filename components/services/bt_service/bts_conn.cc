/*
 * bts_conn.cc —— bt_service 内部：A2DP 连接状态机（2026-10）
 *
 * 为什么要有这个文件
 * ------------------
 * 原来 `bt_service_connect()` 只是把 `esp_bt_audio_classic_connect()` 转发一下就
 * 返回：服务里**没有"连接中"这个状态、没有超时、失败也没有任何痕迹**。于是
 *   - 界面只能在点下去的瞬间弹一句 2 秒的 Toast，之后什么都不知道；
 *   - 连点几次就是几次 `BTC_AV_SRC_API_CONNECT_EVT`（`esp_a2d_source_connect()`
 *     只检查 Bluedroid 启停，**不拒绝"正在连接"**），很容易撞 BTA_AV_FAIL_RESOURCES；
 *   - 失败时只来一个 `connected=false` 事件，`bts_state` 回到 READY，与"从没连过"
 *     一模一样 —— 用户看到的就是"点了没反应 / 死活连不上 / 等好久"。
 *
 * 现在把"一次连接"当成有生命周期的过程：
 *
 *   request(bda)
 *     ├─ 正在连同一台          → ESP_ERR_INVALID_STATE（UI 提示"正在连接，请稍后再试"）
 *     ├─ 已连着另一台          → 先 disconnect 旧对端，等 BT_CONN_SWITCH_MS 再连
 *     ├─ 正在搜索（inquiry）   → 先停查询，等 BT_CONN_QUIET_MS 再连   ← ★ 真机踩过的根因
 *     └─ 空闲                  → 极短延时后直接连
 *   每次"发起"都 ++s_gen；超时定时器与延迟重试都带代数校验，过期的回调一律丢掉。
 *
 *   连上          → CONNECTION_STATE_CHG(connected=true)：清目标、停定时器
 *   失败/超时     → tries ≤ 2：1.5 s 后重试（gen++）
 *                 → 否则：BT_ST_CONNECT_FAILED（**保留目标与原因**，UI 显示"重试"）
 *
 * ⚠ 为什么"正在搜索时不能直接连"（2026-10 真机日志，见 AGENTS §11.5.16）
 *   `esp_bt_gap_start_discovery(GENERAL_INQUIRY, 10, 0)` 要跑 **12.8 秒**，而设备
 *   列表 1~2 秒就有结果；用户这时候点连接 = inquiry / page / SDP / AVDTP 抢同一个
 *   射频，Bluedroid 直接报 `BTA_AV_OPEN_EVT::FAILED status: 3`（= BTA_AV_FAIL_STREAM，
 *   AVDTP 流打不开），而查询一结束再点就连得上 —— 表现正是"有时候死活连不到 /
 *   有时候等好久才连上"。所以发起前必须先 `discovery_stop()` 并留一点安静时间。
 *
 * 并发纪律
 * --------
 * 这份状态被三个上下文碰：LVGL 任务（request）、esp_timer 任务（超时/重试）、
 * BTC 任务（连接事件）。全部用一把互斥锁保护，而且**锁内绝不调用任何 bluetooth API**
 * （BTC 回调里也要拿这把锁，锁内再进协议栈就是反向死锁）。
 * `bts_state` 只在锁外写，每次只写一个终值。
 */
#include "bts_conn.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_bt_audio.h"
#include "esp_bt_audio_classic.h"

#include "bts_name_cache.h"
#include "bts_state.h"

static const char *TAG = "bt_conn";

/* 一次连接（**含 2 次重试在内**）的总预算。
 *
 * ⚠ 是"总预算"不是"每试 15 s"：早期实现每试都等 15 s，最坏 15×3 + 2×1.5 ≈ 48 s，
 *   界面上就是"转了两分钟还没结果"。现在从受理那一刻起算 15 s，重试只在预算还够
 *   （剩 > RETRY_MS + 一件尝试的余量）时才排。 */
#define BT_CONN_BUDGET_MS      15000
/* 失败后重试间隔：别贴着 Bluedroid 拆 ACL 的时序（实测失败后 ~4 s 才 disconnect）去撞 */
#define BT_CONN_RETRY_MS        1500
/* 首次 + 2 次重试 */
#define BT_CONN_RETRY_MAX           2
/* 已连着别的设备：先断开，等它落下去再连新的 */
#define BT_CONN_SWITCH_MS        800
/* 停了查询之后给控制器一点时间真正取消 inquiry */
#define BT_CONN_QUIET_MS         400
/* 纯空闲时的最小延时（统一走"延时 → conn_fire"一条路径） */
#define BT_CONN_MIN_MS            50
#define BT_CONN_ERR_LEN           32

typedef enum {
    CONN_IDLE = 0,
    CONN_PREPARE,      /* 已受理，等"停查询 / 断旧对端"落下去再发起 */
    CONN_CONNECTING,   /* 已发起，等事件或超时 */
    CONN_RETRY_WAIT,   /* 失败后等 BT_CONN_RETRY_MS 再发起 */
} conn_phase_t;

static SemaphoreHandle_t  s_lock;
static esp_timer_handle_t s_timer;
static uint32_t           s_timer_gen;   /* 当前装载的定时器属于哪一代（防过期回调） */

static conn_phase_t s_phase;
static uint8_t      s_target[6];         /* 目标 BDA（PREPARE/CONNECTING/RETRY_WAIT/FAILED 时有效） */
static bool         s_target_valid;
static int          s_tries;             /* 本目标已经发起过几次（含首次） */
static uint32_t     s_gen;               /* 每次受理 / 重试 / 收尾都 ++ */
static char         s_err[BT_CONN_ERR_LEN];
static uint32_t     s_req_count;         /* 累计受理次数（只用来打日志/排查） */
static int64_t      s_deadline_us;       /* 本次连接的总预算到期时刻（含重试） */

/* 距离总预算到期还剩多少毫秒（负数 = 已经超了） */
static int64_t conn_budget_left_ms(void)
{
    return (s_deadline_us - esp_timer_get_time()) / 1000;
}

/* ---------------------------------------------------------------- 锁 */

static bool conn_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return false;
        }
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void conn_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ---------------------------------------------------------------- 定时器 */

static void conn_timer_cb(void *arg);
static void conn_fire(void);
static void conn_fail(const char *why, bool tear_down_link);

static bool conn_timer_ready(void)
{
    if (s_timer) {
        return true;
    }
    esp_timer_create_args_t args = {};
    args.callback = conn_timer_cb;
    args.name     = "bt_conn";
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        s_timer = NULL;
        ESP_LOGE(TAG, "create timer failed (连接超时/重试不可用)");
        return false;
    }
    return true;
}

static void conn_timer_arm(uint32_t gen, uint32_t ms)
{
    if (!conn_timer_ready()) {
        return;
    }
    if (conn_lock()) {
        s_timer_gen = gen;
        conn_unlock();
    }
    esp_timer_stop(s_timer);      /* 幂等：重复 arm 时先停，别报 INVALID_STATE */
    esp_timer_start_once(s_timer, (uint64_t)ms * 1000ULL);
}

static void conn_timer_disarm(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
}

/* ---------------------------------------------------------------- 发起 / 失败 */

/* 真正把连接请求发给协议栈（PREPARE / RETRY_WAIT 到点后由定时器调） */
static void conn_fire(void)
{
    uint8_t bda[6];
    uint32_t gen;
    int tries;

    if (!conn_lock()) {
        return;
    }
    if (s_phase != CONN_PREPARE && s_phase != CONN_RETRY_WAIT) {
        conn_unlock();
        return;
    }
    if (!s_target_valid) {
        s_phase = CONN_IDLE;
        conn_unlock();
        return;
    }
    memcpy(bda, s_target, 6);
    s_tries++;
    tries  = s_tries;
    s_phase = CONN_CONNECTING;
    gen     = s_gen;
    conn_unlock();

    /* 先把名字填好：UI 的"正在连接 XXX…"与日志都用 bts_peer_name */
    const bt_dev_t *d = bts_dev_find(bda, false);
    bts_make_peer_name(bda, d ? d->name : NULL);

    bts_state = BT_ST_CONNECTING;

    const int64_t left = conn_budget_left_ms();
    if (left < 500) {
        /* 预算已经耗光（前几次尝试就把 15 s 花完了）：别再发一次 */
        ESP_LOGW(TAG, "connect %s: budget exhausted (%lld ms left)", bts_peer_name, (long long)left);
        conn_fail("连接超时", true);
        return;
    }
    ESP_LOGI(TAG, "connect %s (try %d/%d, budget %lld ms)",
             bts_peer_name, tries, BT_CONN_RETRY_MAX + 1, (long long)left);

    esp_bd_addr_t addr;
    memcpy(addr, bda, 6);
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
    if (ret != ESP_OK) {
        /* 同步失败（正在 deinit / 参数问题）：按一次失败处理，走重试或收尾 */
        ESP_LOGW(TAG, "a2dp connect: %s", esp_err_to_name(ret));
        conn_fail("发起失败", false);
        return;
    }
    /* 单次尝试也不许超过总预算 */
    const uint32_t wait = (uint32_t)((left > BT_CONN_BUDGET_MS) ? BT_CONN_BUDGET_MS : left);
    conn_timer_arm(gen, wait);
}

/*
 * 一次尝试失败（事件/超时/同步错误都走这里）。
 *   tries ≤ BT_CONN_RETRY_MAX → 等 BT_CONN_RETRY_MS 后重试
 *   否则                      → BT_ST_CONNECT_FAILED，保留目标与原因给 UI
 *
 * tear_down_link = true 时顺手把残留 ACL 拆掉：失败后 Bluedroid 自己要约 4 s 才
 * 断开链路（真机日志 `hci cmd send: disconnect` 比失败晚 4 s），马上重试就是跟它
 * 抢时序。注意这个 disconnect 自己也会产生一个 disconnected 事件，它落在
 * RETRY_WAIT 阶段会被忽略（见 bts_conn_on_disconnected）。
 */
static void conn_fail(const char *why, bool tear_down_link)
{
    bool retry = false;
    uint8_t bda[6] = {0};
    uint32_t gen = 0;
    int tries = 0;

    if (!conn_lock()) {
        return;
    }
    if (s_phase != CONN_PREPARE && s_phase != CONN_CONNECTING) {
        conn_unlock();
        return;                      /* 已经收尾/换代：迟到的失败信号，忽略 */
    }
    memcpy(bda, s_target, 6);
    tries = s_tries;
    /*
     * 重试的前提：还有预算（15 s 是**总**预算）。
     * 留 RETRY_MS + 3 s 的余量，否则"等 1.5 s 再试、3 秒后就超时"没有意义。
     */
    const int64_t left = conn_budget_left_ms();
    if (s_tries <= BT_CONN_RETRY_MAX && left > (int64_t)BT_CONN_RETRY_MS + 3000) {
        s_phase = CONN_RETRY_WAIT;
        retry   = true;
    } else {
        s_phase = CONN_IDLE;         /* 目标保留：UI 用它显示"连接失败 + 重试" */
        snprintf(s_err, sizeof(s_err), "%s", why ? why : "连接失败");
    }
    gen = s_gen;
    conn_unlock();

    if (retry) {
        ESP_LOGW(TAG, "connect failed (%s), retry %d/%d in %d ms (budget left %lld ms)",
                 why, tries, BT_CONN_RETRY_MAX, BT_CONN_RETRY_MS, (long long)left);
        if (tear_down_link) {
            esp_bd_addr_t addr;
            memcpy(addr, bda, 6);
            esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
        }
        bts_state = BT_ST_CONNECTING;
        conn_timer_arm(gen, BT_CONN_RETRY_MS);
        return;
    }

    ESP_LOGE(TAG, "connect failed: %s (gave up after %d tries)", why, tries);
    bts_state = BT_ST_CONNECT_FAILED;
    if (tear_down_link) {
        esp_bd_addr_t addr;
        memcpy(addr, bda, 6);
        esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
    }
}

/* ---------------------------------------------------------------- 定时器回调 */

static void conn_timer_cb(void *arg)
{
    (void)arg;
    conn_phase_t ph = CONN_IDLE;

    if (!conn_lock()) {
        return;
    }
    if (s_timer_gen != s_gen) {
        conn_unlock();
        return;                      /* 过期回调（已换代）：一动不动 */
    }
    ph = s_phase;
    conn_unlock();

    switch (ph) {
    case CONN_PREPARE:
    case CONN_RETRY_WAIT:
        conn_fire();
        break;
    case CONN_CONNECTING:
        conn_fail("连接超时", true);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 对外入口 */

esp_err_t bts_conn_request(const uint8_t bda[6])
{
    if (!bda || !bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!conn_timer_ready()) {
        return ESP_FAIL;
    }

    const bool was_discovering = bts_discovering;
    uint8_t old_peer[6] = {0};
    uint8_t superseded[6] = {0};        /* 被"改连别的设备"打断的在途目标 */
    bool    need_switch = false;
    bool    need_abort  = false;
    uint32_t delay = 0;
    uint32_t gen = 0;

    if (!conn_lock()) {
        return ESP_FAIL;
    }
    if (s_target_valid && bts_same_bda(s_target, bda) &&
        (s_phase == CONN_PREPARE || s_phase == CONN_CONNECTING || s_phase == CONN_RETRY_WAIT)) {
        conn_unlock();
        ESP_LOGI(TAG, "already connecting to this device, ignore repeat request");
        return ESP_ERR_INVALID_STATE;      /* UI：正在连接，请稍后再试 */
    }
    /* 用户改连别的设备：把在途那次尝试**主动取消**（否则两路 page 会互抢） */
    if (s_target_valid && s_phase != CONN_IDLE && !bts_same_bda(s_target, bda)) {
        memcpy(superseded, s_target, 6);
        need_abort = true;
    }
    /* 受理：覆盖目标、清计数、换代（旧定时器自然作废）、重设总预算 */
    memcpy(s_target, bda, 6);
    s_target_valid = true;
    s_tries = 0;
    s_err[0] = '\0';
    s_phase = CONN_PREPARE;
    s_gen++;
    s_req_count++;
    gen = s_gen;
    s_deadline_us = esp_timer_get_time() + (int64_t)BT_CONN_BUDGET_MS * 1000;
    conn_unlock();
    ESP_LOGI(TAG, "request #%u accepted (budget %d ms)", (unsigned)s_req_count, BT_CONN_BUDGET_MS);

    if (bts_connected && !bts_same_bda(bts_peer_bda, bda)) {
        memcpy(old_peer, bts_peer_bda, 6);
        need_switch = true;
    }

    /* 1) 改连别的设备：先取消在途那次尝试 */
    if (need_abort) {
        char old_addr[18];
        bt_service_addr_str(superseded, old_addr, sizeof(old_addr));
        ESP_LOGI(TAG, "abort in-flight attempt to %s (target switched)", old_addr);
        esp_bd_addr_t a;
        memcpy(a, superseded, 6);
        esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, a);
        delay = BT_CONN_SWITCH_MS;
    }
    /* 2) 已连着别的设备：先断开（异步），给它时间落下去 */
    if (need_switch) {
        char old_addr[18];
        bt_service_addr_str(old_peer, old_addr, sizeof(old_addr));
        ESP_LOGI(TAG, "switch: disconnect %s first", old_addr);
        esp_bd_addr_t a;
        memcpy(a, old_peer, 6);
        esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, a);
        if (delay < BT_CONN_SWITCH_MS) {
            delay = BT_CONN_SWITCH_MS;
        }
    }
    /* 3) 正在搜索：先停查询 —— 不停的话 AVDTP 打不开（BTA_AV_FAIL_STREAM），见文件头 */
    if (was_discovering) {
        ESP_LOGI(TAG, "stop discovery before connecting");
        esp_bt_audio_classic_discovery_stop();
        if (delay < BT_CONN_QUIET_MS) {
            delay = BT_CONN_QUIET_MS;
        }
    }
    if (delay < BT_CONN_MIN_MS) {
        delay = BT_CONN_MIN_MS;
    }

    bts_state = BT_ST_CONNECTING;
    conn_timer_arm(gen, delay);
    return ESP_OK;
}

/*
 * 连接事件：连上了。
 * 只负责把状态机收尾（清目标、停定时器）；bts_connected / 名字 / NVS / 配对表
 * 由 bts_events.cc 原有的逻辑继续做，behaviour 不变。
 */
void bts_conn_on_connected(const uint8_t bda[6])
{
    (void)bda;
    if (conn_lock()) {
        s_phase = CONN_IDLE;
        s_target_valid = false;
        s_tries = 0;
        s_err[0] = '\0';
        s_gen++;                      /* 让在途的超时/重试全部作废 */
        conn_unlock();
    } else {
        s_target_valid = false;
    }
    conn_timer_disarm();
    bts_state = BT_ST_CONNECTED;
}

/*
 * 连接事件：断开了。
 * 返回 true = 这个事件归连接状态机管（调用方**不要**再把状态改成 READY）；
 * 返回 false = 普通掉线/主动断开，按原来的逻辑走。
 */
bool bts_conn_on_disconnected(const uint8_t bda[6])
{
    bool claimed = false;
    bool is_target = false;
    bool is_peer = false;
    conn_phase_t ph = CONN_IDLE;

    if (!conn_lock()) {
        return false;
    }
    ph = s_phase;
    is_target = s_target_valid && bda && bts_same_bda(s_target, bda);
    /* 旧对端（切换前那台）的断开也要认领：它是我们主动 disconnect 产生的 */
    is_peer = bda && bts_same_bda(bts_peer_bda, bda);

    if (ph == CONN_IDLE) {
        /* 已经收尾（含 CONNECT_FAILED）：只认领目标自己的残余事件，
         * 否则 UI 上的"连接失败 · 点此重试"会被一个迟到的断开事件抹掉 */
        claimed = s_target_valid && is_target;
    } else {
        /* PREPARE / CONNECTING / RETRY_WAIT：目标和旧对端的断开都归状态机管 */
        claimed = is_target || is_peer;
    }
    conn_unlock();

    if (!claimed) {
        return false;
    }
    if (ph == CONN_CONNECTING && is_target) {
        conn_fail("连接失败", true);
    }
    return true;
}

void bts_conn_cancel(void)
{
    if (conn_lock()) {
        s_phase = CONN_IDLE;
        s_tries = 0;
        s_gen++;            /* 让在途的"15 s 超时 / 1.5 s 重试"全部作废 */
        /* 目标也清掉：否则关开关再打开后，UI 会拿一个陈旧的"失败目标"去画转圈 */
        s_target_valid = false;
        s_err[0] = '\0';
        conn_unlock();
    }
    conn_timer_disarm();
}

/* ---------------------------------------------------------------- 查询（UI 用） */

/*
 * 这几个是给 UI 500 ms 轮询用的**只读**查询：
 *   - 读 s_phase / s_target_valid 不加锁（和本组件其它状态一样，volatile 语义够用）；
 *     最坏情况是某一帧读到"目标已换、相位还没换"的中间态，UI 只会少画一次转圈。
 *   - s_target 只在受理时整块 memcpy（6 字节），读到混合值的概率极低且无害。
 */
bool bts_conn_is_busy(void)
{
    const conn_phase_t ph = s_phase;
    return ph == CONN_PREPARE || ph == CONN_CONNECTING || ph == CONN_RETRY_WAIT;
}

const uint8_t *bts_conn_target(void)
{
    return s_target_valid ? s_target : NULL;
}

const char *bts_conn_error_str(void)
{
    return s_err;
}

/* ---------------------------------------------------------------- 对外薄封装 */

bool bt_service_is_connecting(void)
{
    return bts_conn_is_busy() || bts_state == BT_ST_CONNECTING;
}

const uint8_t *bt_service_connect_target(void)
{
    return bts_conn_target();
}

const char *bt_service_connect_error(void)
{
    return bts_conn_error_str();
}

