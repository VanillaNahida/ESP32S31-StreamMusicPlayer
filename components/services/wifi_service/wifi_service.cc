/*
 * wifi_service.cc —— Wi-Fi STA 服务实现（esp_wifi + esp_netif + esp_event）
 *
 * 职责边界：
 *   - 只做 STA：扫描、连一个 AP、把 (ssid,pass) 存进自己的 NVS、忘记
 *   - 状态变更全在事件回调里写成静态变量，UI 轮询取（回调在 sys_evt 任务，
 *     不许碰 LVGL，也不做重活）
 *   - **扫描结果的处理不放事件回调**：回调只 xTaskNotifyGive 一下，真正的
 *     esp_wifi_scan_get_ap_records + 排序 + 日志在 wifi_evt_task 里做
 *     （见下面 on_wifi_evt 上方的长注释，这是"扫描 Wi-Fi 就崩"的根因）
 *   - 初始化阻塞（netif + wifi start + 自动连），调用方放后台任务
 */
#include "wifi_service.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "nvs.h"

#include <stdlib.h>      /* setenv */
#include <time.h>        /* tzset */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_svc";

#define NVS_NS      "wifi_cfg"
#define NVS_KEY     "aps"
#define NVS_KEY_ON  "on"        /* Wi-Fi 总开关（记忆状态） */

/* SNTP 服务器：国内优先用阿里；失败也只影响 https 与时钟显示 */
#define SNTP_SERVER "ntp.aliyun.com"

/* 连接失败/掉线后的自动重试：快速 5 次（每 4 秒），之后转 60 秒一次的慢重试 */
#define WIFI_RETRY_MAX        5
#define WIFI_RETRY_DELAY_MS   4000
#define WIFI_RETRY_SLOW_MS    60000
/* 开机首次连接的延时：等 PHY/扫描就绪，见 wifi_service_init() 里的注释 */
#define WIFI_FIRST_CONNECT_MS 1500

typedef struct {
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
} wifi_cred_t;

typedef struct {
    uint8_t     count;
    wifi_cred_t list[WIFI_SAVED_MAX];
} wifi_store_t;

static volatile bool        s_ready;
static volatile bool        s_init_done;
static volatile wifi_state_t s_state = WIFI_ST_UNINIT;
static volatile bool        s_scanning;
static esp_netif_t         *s_netif;
/* 总开关：关掉之后 WiFi 彻底 deinit（释放内存/射频），开的时候再 init */
static volatile bool        s_enabled = true;
static bool                 s_settings_loaded;   /* 是否已经从 NVS 读过开关 */
static volatile bool        s_registered;   /* 事件处理器是否已注册 */
static bool                 s_sntp_started;
static int                  s_retry_left;   /* 还能自动重试几次 */
static volatile bool        s_retry_slow;   /* true = 快速额度用完，转慢速重试 */
static volatile int         s_last_reason;  /* 最近一次断开/失败的原因码（UI 显示用） */
static esp_timer_handle_t   s_retry_timer;
static volatile bool        s_retry_pending; /* 重试定时器已装载（未触发） */

static wifi_ap_t            s_ap[WIFI_AP_MAX];
static volatile size_t      s_ap_count;

static wifi_store_t         s_store;
static char                 s_conn_ssid[WIFI_SSID_MAX];
static char                 s_ip[16];
static volatile int         s_rssi;

/* ---------------------------------------------------------------- NVS 存取 */

static void store_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_store);
    if (nvs_get_blob(h, NVS_KEY, &s_store, &len) != ESP_OK || len != sizeof(s_store)) {
        memset(&s_store, 0, sizeof(s_store));
    }
    uint8_t on = 1;
    /* 没存过就默认开（老固件升级上来的也一样：默认行为不变） */
    s_enabled = (nvs_get_u8(h, NVS_KEY_ON, &on) == ESP_OK) ? (on != 0) : true;
    nvs_close(h);
    if (s_store.count > WIFI_SAVED_MAX) {
        s_store.count = 0;
    }
}

/* 只写开关，不动 AP 列表 */
static void store_save_enabled(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_ON, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void store_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_KEY, &s_store, sizeof(s_store));
    nvs_commit(h);
    nvs_close(h);
}

static const wifi_cred_t *store_find(const char *ssid)
{
    if (!ssid) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_store.count; i++) {
        if (strcmp(s_store.list[i].ssid, ssid) == 0) {
            return &s_store.list[i];
        }
    }
    return NULL;
}

static void store_put(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return;
    }
    if (s_store.count < WIFI_SAVED_MAX) {
        if (!store_find(ssid)) {
            s_store.count++;
        }
    }
    /* 放到最后一个槽位（最近的优先自动连），并去重 */
    wifi_cred_t *slot = NULL;
    for (uint8_t i = 0; i < s_store.count; i++) {
        if (strcmp(s_store.list[i].ssid, ssid) == 0) {
            slot = &s_store.list[i];
        }
    }
    if (!slot) {
        slot = &s_store.list[s_store.count - 1];
    }
    snprintf(slot->ssid, sizeof(slot->ssid), "%s", ssid);
    snprintf(slot->pass, sizeof(slot->pass), "%s", pass ? pass : "");
    store_save();
}

static void store_del(const char *ssid)
{
    if (!ssid) {
        return;
    }
    for (uint8_t i = 0; i < s_store.count; i++) {
        if (strcmp(s_store.list[i].ssid, ssid) != 0) {
            continue;
        }
        for (uint8_t j = i + 1; j < s_store.count; j++) {
            s_store.list[j - 1] = s_store.list[j];
        }
        s_store.count--;
        memset(&s_store.list[s_store.count], 0, sizeof(wifi_cred_t));
        store_save();
        return;
    }
}

static void refresh_ap_saved_flag(void)
{
    for (size_t i = 0; i < s_ap_count; i++) {
        s_ap[i].saved = (store_find(s_ap[i].ssid) != NULL);
    }
}

/* ---------------------------------------------------------------- 事件 */

/*
 * 扫描结果缓冲放**静态区**，不要放栈上：
 * 一块 wifi_ap_record_t ≈ 72 字节 × 24 ≈ 1.7 KB，谁把它放栈上谁爆栈。
 * 处理只在 wifi_evt_task 一个任务里做，静态缓冲不存在竞争。
 */
static wifi_ap_record_t s_scan_recs[WIFI_AP_MAX];

/*
 * 扫描结果处理任务。
 *
 * 为什么要把这几行从事件回调里搬出来（这是"扫 Wi-Fi 必崩"的根因）：
 *   esp_event 的事件回调跑在默认事件循环的 **sys_evt** 任务里，它的栈由
 *   CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE 决定（默认 2304 字节，本工程抬到
 *   4096）。除了我们自己的局部变量，回调里还有 WiFi 驱动的帧、以及
 *   esp_log → vfprintf → __flockfile → xQueueTakeMutexRecursive 这一整条打印链
 *   （本身就要几百字节）。实测就是：
 *       Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
 *       Detected in task "sys_evt" at xTaskGetCurrentTaskHandle
 *       #10 on_wifi_evt (...) at wifi_service.cc:169
 *   也就是说"在事件回调里打一条 ESP_LOGI"就足以把 sys_evt 的栈写穿。
 *
 * 现在的分工：
 *   回调（sys_evt）  只置标志 / 通知任务 —— 栈开销接近 0
 *   本任务（4 KB）   取结果、填表、排序、打日志 —— 崩也崩自己
 *
 * 用 ulTaskNotifyTake 而不是队列：通知可以合并，"又扫完一次"重复计数没有意义。
 */
static TaskHandle_t s_evt_task;

static void wifi_evt_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint16_t num = WIFI_AP_MAX;
        if (esp_wifi_scan_get_ap_records(&num, s_scan_recs) == ESP_OK) {
            if (num > WIFI_AP_MAX) {
                num = WIFI_AP_MAX;
            }
            for (uint16_t i = 0; i < num; i++) {
                snprintf(s_ap[i].ssid, sizeof(s_ap[i].ssid), "%s",
                         (const char *)s_scan_recs[i].ssid);
                s_ap[i].rssi     = s_scan_recs[i].rssi;
                s_ap[i].authmode = (uint8_t)s_scan_recs[i].authmode;
                s_ap[i].saved    = false;
            }
            s_ap_count = num;
            refresh_ap_saved_flag();
        }

        s_scanning = false;
        if (s_state == WIFI_ST_SCANNING) {
            s_state = WIFI_ST_IDLE;
        }
        ESP_LOGI(TAG, "scan done: %u AP", (unsigned)s_ap_count);
    }
}

/*
 * Wi-Fi 事件回调（sys_evt 任务）：**只做状态与通知，不做重活、不做格式化打印**。
 * 连接/断开这两条各自只有一次 ESP_LOGx，实测在 4096 字节栈下安全；
 * 扫描那条曾经就是在这里爆的，所以它现在只发通知。
 */

/*
 * 断开原因码 → 人话。
 *
 * 需求背景："配网时连不上"只打一句"密码错/找不到？"根本没法定位：密码错是
 * 15/202/204，AP 不在范围是 201，被自己人掐掉是 8。这里把最常见的几个翻出来。
 */
static const char *wifi_reason_str(int reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:                     return "未指定";
    case WIFI_REASON_AUTH_EXPIRE:                     return "认证过期";
    case WIFI_REASON_AUTH_LEAVE:                      return "被 AP 解除认证";
    case WIFI_REASON_ASSOC_TOOMANY:                   return "AP 关联数已满";
    case WIFI_REASON_ASSOC_LEAVE:                     return "本方主动离开（不是失败）";
    case WIFI_REASON_ASSOC_NOT_AUTHED:                return "关联时未认证";
    case WIFI_REASON_MIC_FAILURE:                     return "MIC 校验失败：密码错";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:          return "四次握手超时：密码错";
    case WIFI_REASON_802_1X_AUTH_FAILED:              return "802.1X 认证失败（企业级网络？）";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:           return "加密套件被拒";
    case WIFI_REASON_BEACON_TIMEOUT:                  return "AP 信标丢失（信号差）";
    case WIFI_REASON_NO_AP_FOUND:                     return "没找到这个 AP（信号弱/在 5G 频段？）";
    case WIFI_REASON_AUTH_FAIL:                       return "认证失败：密码错";
    case WIFI_REASON_ASSOC_FAIL:                      return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:               return "握手超时：密码错";
    case WIFI_REASON_CONNECTION_FAIL:                 return "连接失败";
    case WIFI_REASON_AP_TSF_RESET:                    return "AP 重启了";
    case WIFI_REASON_ROAMING:                         return "漫游到别的 AP";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "没有加密方式兼容的 AP（该 AP 只用 WPA3/企业级？）";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "AP 不满足 authmode 阈值（加密方式对不上）";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:   return "信号强度低于阈值";
    default:                                          return "见 esp_wifi_types.h";
    }
}

/* 内部连接实现（不碰重试额度），见 wifi_service_connect 的注释 */
static esp_err_t wifi_connect_raw(const char *ssid, const char *pass, bool save);

/*
 * 把重试额度补满并退出慢速模式。
 * 只有两种情况该调：**连上了**，或者**用户主动发起连接**（手动点一下不代表
 * 自动重试也该重新数）。自动重试自己绝不能调 —— 那正是原来无限重连的根因。
 */
static void wifi_retry_budget_refill(void)
{
    s_retry_left = WIFI_RETRY_MAX;
    s_retry_slow = false;
}

/*
 * 连接失败/掉线后的自动重试（需求："wifi 连接失败后没有重试机制"）。
 * 用一次性 esp_timer 延时触发，最多 WIFI_RETRY_MAX 次；连上就补满。
 * 不用 vTaskDelay 阻塞：事件回调跑在 sys_evt 任务里，不能堵。
 */
static void wifi_retry_cb(void *arg)
{
    (void)arg;
    s_retry_pending = false;
    if (!s_enabled || !s_ready) {
        return;
    }
    if (!s_retry_slow && s_retry_left <= 0) {
        return;
    }
    /*
     * 注意这里**只**判"已经连上"，不能判 WIFI_ST_CONNECTING：
     * 开机/重新打开开关时我们先把状态置成 CONNECTING 再延时连，
     * 那样判断的话定时器一到就直接 return，等于永远不连（踩过这个坑：
     * 日志里只有 "wifi ready" 之后再无 "got ip"）。
     */
    if (s_state == WIFI_ST_CONNECTED) {
        return;
    }
    const wifi_cred_t *c = store_find(s_conn_ssid);
    if (!c) {
        return;                 /* 没存过密码，重试也没意义 */
    }
    if (s_retry_slow) {
        ESP_LOGI(TAG, "slow retry '%s'", s_conn_ssid);
    } else {
        s_retry_left--;
        ESP_LOGI(TAG, "connect '%s' (try, %d retry left)", s_conn_ssid, s_retry_left);
    }
    /*
     * ★ 必须走 wifi_connect_raw：以前这里调 wifi_service_connect_saved()，
     *   而那个函数会把 s_retry_left 重新补满 —— 于是每重试一次额度就回到 5，
     *   日志里永远是 "4 retry left"，WIFI_RETRY_MAX 完全失效、无限重连。
     */
    wifi_connect_raw(c->ssid, c->pass, false);
}

static void wifi_retry_arm_ms(uint32_t ms)
{
    if (!s_enabled || !s_init_done) {
        return;
    }
    if (!s_retry_slow && s_retry_left <= 0) {
        return;
    }
    if (!s_retry_timer) {
        esp_timer_create_args_t args = {};
        args.callback = wifi_retry_cb;
        args.name     = "wifi_retry";
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) {
            s_retry_timer = NULL;
            return;
        }
    }
    s_retry_pending = true;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000ULL);
}

/*
 * 掉线/失败后排下一次重试。
 *
 * 两级节奏：前 WIFI_RETRY_MAX 次 4 秒一次（快速恢复），用完之后**不放弃**，
 * 转成 1 分钟一次的慢重试。
 *   - 为什么不能无限快重试：密码错/企业级网络时就是每 4 秒一轮"连接→失败"，
 *     日志被刷满、射频一直忙（以前的实现就是这样：额度被 wifi_service_connect()
 *     反复补满，永远停在 "4 retry left"）。
 *   - 为什么也不能用完就彻底停：AP 重启、走出信号盲区都是临时的，停了就再也
 *     回不来，只能等用户去关开一次开关。
 */
static void wifi_retry_arm(void)
{
    if (!s_retry_slow && s_retry_left <= 0) {
        s_retry_slow = true;
        ESP_LOGW(TAG, "quick retries exhausted, retry '%s' every %d s from now on",
                 s_conn_ssid, (int)(WIFI_RETRY_SLOW_MS / 1000));
    }
    wifi_retry_arm_ms(s_retry_slow ? WIFI_RETRY_SLOW_MS : WIFI_RETRY_DELAY_MS);
}

static void on_wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_SCAN_DONE:
        if (s_evt_task) {
            xTaskNotifyGive(s_evt_task);
        }
        break;
    case WIFI_EVENT_STA_START:
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_state = WIFI_ST_CONNECTED;
        ESP_LOGI(TAG, "connected to '%s'", s_conn_ssid);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        const int reason = d ? d->reason : -1;
        s_ip[0] = '\0';
        s_rssi  = 0;
        /*
         * ★ ASSOC_LEAVE = "本方要求离开"，是我们自己调 esp_wifi_disconnect() 的产物，
         *   **不是**连接失败。以前不分原因一律当失败，于是：
         *     切网络/重试时先 disconnect 再 connect
         *     → 那个 ASSOC_LEAVE 事件几毫秒后才被 sys_evt 派发
         *     → 把刚发起的连接当场判成 FAILED 并排下一次重试
         *     → 4 秒后重复，新连接永远活不过那几毫秒。
         *   日志特征就是 "connecting to 'X' (save=0)" 之后 6 ms 就
         *   "connect 'X' failed (密码错/找不到？)" —— 正常的认证/握手失败
         *   至少要几十到几百毫秒，不可能 6 ms。
         */
        if (reason == WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGI(TAG, "left '%s' on request, reason %d (%s)",
                     s_conn_ssid, reason, wifi_reason_str(reason));
            break;
        }
        if (s_state == WIFI_ST_CONNECTING) {
            s_state = WIFI_ST_FAILED;
            s_last_reason = reason;
            ESP_LOGW(TAG, "connect '%s' failed: reason %d (%s)",
                     s_conn_ssid, reason, wifi_reason_str(reason));
        } else {
            s_state = WIFI_ST_IDLE;
            s_last_reason = reason;
            ESP_LOGW(TAG, "disconnected from '%s': reason %d (%s)",
                     s_conn_ssid, reason, wifi_reason_str(reason));
        }
        wifi_retry_arm();      /* 失败/掉线都自动重试（上限见 WIFI_RETRY_MAX） */
        break;
    }
    default:
        break;
    }
}

static void on_ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP || !data) {
        return;
    }
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    /* 同样别把 wifi_ap_record_t（≈72 B）放 sys_evt 的栈上；回调串行派发，静态安全 */
    static wifi_ap_record_t s_ap_info;
    if (esp_wifi_sta_get_ap_info(&s_ap_info) == ESP_OK) {
        s_rssi = s_ap_info.rssi;
    }
    s_state = WIFI_ST_CONNECTED;
    s_last_reason = 0;                /* 连上了：清掉上次的失败原因 */
    wifi_retry_budget_refill();       /* 连上了就把重试额度补满、退出慢速模式 */
    ESP_LOGI(TAG, "got ip %s", s_ip);

    /*
     * 拿到 IP 就起 SNTP（非阻塞，就在这个回调里初始化也没关系）。
     * 为什么需要时间：
     *   1) 在线音乐要访问 https CDN，证书校验依赖系统时间，时间不对会直接握手失败；
     *   2) 首页大时钟与日期一直是"等待校时"，同步后自然就好了。
     * 时区固定东八区（setenv 在 IDF 里是线程安全的，只需要做一次）。
     */
    if (!s_sntp_started) {
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
        if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
            s_sntp_started = true;
            ESP_LOGI(TAG, "sntp started (%s)", SNTP_SERVER);
        } else {
            ESP_LOGW(TAG, "sntp init failed (https 可能因时间不准失败)");
        }
    }
}

/* ---------------------------------------------------------------- 对外接口 */

esp_err_t wifi_service_init(void)
{
    if (s_init_done) {
        return ESP_OK;
    }

    /* 先读开关与已存 AP：开关关着就什么都不做（射频与内存都不占） */
    store_load();
    if (!s_enabled) {
        ESP_LOGI(TAG, "wifi disabled by switch, skip init");
        s_state = WIFI_ST_UNINIT;
        return ESP_ERR_INVALID_STATE;
    }
    s_init_done = true;

    /*
     * 扫描结果处理任务先建起来：它在 esp_wifi_start() 之前就绪，
     * 任何时刻到达的 WIFI_EVENT_SCAN_DONE 都不会丢通知。
     * 栈 4 KB 是给 esp_wifi_scan_get_ap_records + ESP_LOGI 留的余量
     * （结果缓冲本身在静态区，不占这里的栈）。
     */
    if (!s_evt_task) {
        if (xTaskCreate(wifi_evt_task, "wifi_evt", 4096, NULL, 4, &s_evt_task) != pdPASS) {
            s_evt_task = NULL;
            ESP_LOGE(TAG, "create wifi_evt task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {   /* 已建过也算 OK */
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) {
            ESP_LOGE(TAG, "create default wifi sta failed");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        s_state = WIFI_ST_FAILED;
        return ret;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_evt, NULL, NULL);
    s_registered = true;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK) {
        ret = esp_wifi_start();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(ret));
        s_state = WIFI_ST_FAILED;
        return ret;
    }

    store_load();
    s_state = WIFI_ST_IDLE;
    s_ready = true;
    ESP_LOGI(TAG, "wifi ready, %u saved AP", (unsigned)s_store.count);

    /* 有存过的网络就自动连最近一次成功的那个（开机自动连接）。
     * 不立刻连：esp_wifi_start() 刚返回时 PHY/扫描还没就绪，实测第一次
     * 连接会直接失败（日志里 "authmode threshold changes from OPEN to WPA2"
     * 后紧跟 connect failed），白等 4 秒重试。这里先等 1.5 秒再连，
     * 开机反而更快连上（wifi_retry_cb 走的是同一套"用已保存密码重连"）。 */
    if (s_store.count > 0) {
        const wifi_cred_t *c = &s_store.list[s_store.count - 1];
        s_conn_ssid[0] = '\0';
        snprintf(s_conn_ssid, sizeof(s_conn_ssid), "%s", c->ssid);
        wifi_retry_budget_refill();
        s_state = WIFI_ST_CONNECTING;
        wifi_retry_arm_ms(WIFI_FIRST_CONNECT_MS);
    }
    return ESP_OK;
}

/*
 * 关掉 Wi-Fi：**彻底 deinit**，把驱动内存和射频都还回去（需求："不开启就释放资源"）。
 * 顺序：先摘事件（避免 deinit 期间回调还往已释放的对象里写）→ 停 SNTP →
 * 停/关 wifi → 销毁 netif → 清所有状态，之后 set_enabled(true) 会重新走一遍 init。
 */
esp_err_t wifi_service_deinit(void)
{
    if (!s_init_done) {
        s_ready = false;
        s_state = WIFI_ST_UNINIT;
        return ESP_OK;
    }
    s_ready = false;
    s_scanning = false;
    s_ip[0] = '\0';
    s_rssi = 0;
    s_ap_count = 0;
    s_state = WIFI_ST_UNINIT;

    if (s_registered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_evt);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_evt);
        s_registered = false;
    }
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy(s_netif);     /* destroy_default_* 的内部实现 */
        s_netif = NULL;
    }
    s_init_done = false;
    ESP_LOGI(TAG, "wifi deinit (switch off)");
    return ESP_OK;
}

bool wifi_service_is_enabled(void)
{
    /* 首次查询时懒加载一次 NVS：UI 在服务尚未 init 时也要能显示正确的开关状态 */
    if (!s_settings_loaded) {
        store_load();
        s_settings_loaded = true;
    }
    return s_enabled;
}

esp_err_t wifi_service_set_enabled(bool on)
{
    if (on == s_enabled) {
        return ESP_OK;
    }
    s_enabled = on;
    store_save_enabled(on);
    ESP_LOGI(TAG, "wifi switch -> %s", on ? "on" : "off");
    if (on) {
        return wifi_service_init();
    }
    return wifi_service_deinit();
}

bool wifi_service_has_ip(void) { return s_ip[0] != '\0'; }

/* NVS 里存过这个 SSID 的密码？（UI 里"密码留空确认"时用它决定走已保存凭据） */
bool wifi_service_is_saved(const char *ssid)
{
    if (!s_settings_loaded) {
        store_load();
        s_settings_loaded = true;
    }
    return store_find(ssid) != NULL;
}

/* 时间是否已经同步（https 证书校验依赖它） */
bool wifi_service_time_ready(void)
{
    return s_sntp_started && (esp_netif_sntp_sync_wait(0) == ESP_OK);
}

bool wifi_service_is_ready(void)          { return s_ready; }
wifi_state_t wifi_service_state(void)     { return s_state; }
bool wifi_service_is_scanning(void)       { return s_scanning; }
size_t wifi_service_ap_count(void)        { return s_ap_count; }

const wifi_ap_t *wifi_service_ap(size_t idx)
{
    return (idx < s_ap_count) ? &s_ap[idx] : NULL;
}

esp_err_t wifi_service_scan_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        return ESP_OK;
    }
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    esp_err_t ret = esp_wifi_scan_start(&sc, false);   /* 异步，结果在 SCAN_DONE */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(ret));
        return ret;
    }
    s_scanning = true;
    s_state = WIFI_ST_SCANNING;
    return ESP_OK;
}

/*
 * 真正下发配置并连接。
 *
 * **不碰 s_retry_left** —— 自动重试必须走这条，否则每重试一次额度就被补满
 * （见 wifi_retry_cb 里的说明）。对外那两个入口负责"用户主动连 → 补满额度"。
 */
static esp_err_t wifi_connect_raw(const char *ssid, const char *pass, bool save)
{
    if (!s_ready || !ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t wc = {};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    if (pass) {
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    }

    /*
     * threshold.authmode：以前一律填 WIFI_AUTH_OPEN（注释写"加密/开放都接受"），
     * 但只要给了密码，IDF 就会先打
     *   "Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2"
     * 再从内部改掉它。这句警告正好夹在 "connecting to" 和 "connect failed" 中间，
     * 非常容易被当成失败原因。按实际加密方式填就干净了：
     *   扫描结果里有这个 AP → 用它报的 authmode（WPA2 / WPA3 / 开放都对得上）；
     *   没有（开机自动连、重试）→ 有密码按 WPA2_PSK，没密码按开放。
     */
    uint8_t auth = 0;
    bool from_scan = false;
    for (size_t i = 0; i < s_ap_count; i++) {
        if (strcmp(s_ap[i].ssid, ssid) == 0) {
            auth = s_ap[i].authmode;
            from_scan = true;
            break;
        }
    }
    if (!from_scan) {
        auth = (pass && pass[0]) ? (uint8_t)WIFI_AUTH_WPA2_PSK : (uint8_t)WIFI_AUTH_OPEN;
    }
    /* 给了密码但（扫描说）是开放/WEP：按 WPA2 试 —— 有些 AP 在扫描结果里把加密
     * 报成开放，IDF 内部也是这么兜的，这里显式写出来免得又冒出那句阈值警告 */
    if (pass && pass[0] && auth < (uint8_t)WIFI_AUTH_WPA_PSK) {
        auth = (uint8_t)WIFI_AUTH_WPA2_PSK;
    }
    wc.sta.threshold.authmode = (wifi_auth_mode_t)auth;

    const wifi_state_t prev = s_state;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(ret));
        return ret;
    }
    snprintf(s_conn_ssid, sizeof(s_conn_ssid), "%s", ssid);
    s_ip[0] = '\0';
    s_state = WIFI_ST_CONNECTING;

    /*
     * 只有"确实还挂着或正在连"才需要先断开。以前是无条件 disconnect + connect：
     * disconnect 会让驱动取消正在进行的连接并投递一个 ASSOC_LEAVE 事件，紧接着
     * connect 刚发起就被那个事件判成失败（on_wifi_evt 里现在会忽略它），
     * 于是连接永远活不过几毫秒。空闲时直接 connect 即可。
     */
    if (prev == WIFI_ST_CONNECTED || prev == WIFI_ST_CONNECTING) {
        esp_wifi_disconnect();
    }

    ret = esp_wifi_connect();
    if (ret == ESP_ERR_WIFI_CONN) {
        ret = ESP_OK;          /* 已经有一次连接在进行中：等它的结果就行 */
    }
    if (ret != ESP_OK) {
        s_state = WIFI_ST_FAILED;
        return ret;
    }
    if (save) {
        store_put(ssid, pass);
        refresh_ap_saved_flag();
    }
    ESP_LOGI(TAG, "connecting to '%s' (auth=%d%s save=%d)", ssid, (int)auth,
             from_scan ? " from-scan" : "", (int)save);
    return ESP_OK;
}

esp_err_t wifi_service_connect(const char *ssid, const char *pass, bool save)
{
    wifi_retry_budget_refill();       /* 用户主动连一次 → 自动重试额度重新补满 */
    return wifi_connect_raw(ssid, pass, save);
}

esp_err_t wifi_service_connect_saved(const char *ssid)
{
    const wifi_cred_t *c = store_find(ssid);
    if (!c) {
        return ESP_ERR_NOT_FOUND;
    }
    wifi_retry_budget_refill();       /* 同上：用户点了"用已保存的密码连" */
    return wifi_connect_raw(c->ssid, c->pass, false);
}

esp_err_t wifi_service_disconnect(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = WIFI_ST_IDLE;
    s_ip[0] = '\0';
    s_rssi  = 0;
    return esp_wifi_disconnect();
}

esp_err_t wifi_service_forget(const char *ssid)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    store_del(ssid);
    if (strcmp(s_conn_ssid, ssid) == 0 && s_state == WIFI_ST_CONNECTED) {
        wifi_service_disconnect();
    }
    refresh_ap_saved_flag();
    ESP_LOGI(TAG, "forgot '%s'", ssid);
    return ESP_OK;
}

const char *wifi_service_conn_ssid(void) { return s_conn_ssid; }
const char *wifi_service_ip(void)        { return s_ip; }
int         wifi_service_rssi(void)      { return s_rssi; }

const char *wifi_service_last_reason_str(void)
{
    return (s_last_reason != 0) ? wifi_reason_str(s_last_reason) : "";
}

size_t wifi_service_saved_count(void)    { return s_store.count; }

const char *wifi_service_saved_ssid(size_t idx)
{
    return (idx < s_store.count) ? s_store.list[idx].ssid : NULL;
}
