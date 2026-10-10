/*
 * bts_settings.cc —— bt_service 内部：开关/上次设备的 NVS 记忆 + 开机自动连定时器
 *
 * 从 bt_service.cc 395–443（bt_settings_load / save_enabled / save_last /
 * bt_auto_connect_cb）与 init 末尾的自动连定时器那一段（1112–1126）原样搬来：
 * NVS 键名、3 s 延时、日志文案都与拆分前一致。
 */

#include "bts_settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bt_service.h"
#include "bts_state.h"

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

/* 总开关 + "上次连过的设备"（需求：记忆开关状态、开机自动连已保存设备） */
#define NVS_NS          "bt_cfg"
#define NVS_KEY_ON      "on"
#define NVS_KEY_LAST    "last"
/* 开机自动连延时：控制器 + 协议栈起来后还要让 A2DP 侧稳定一下 */
#define AUTO_CONNECT_MS (3000)

/* 总开关（NVS 记忆；关掉时控制器 + 协议栈一起 deinit 释放资源） */
volatile bool bts_enabled = true;
/* 是否已经从 NVS 读过开关 */
bool          bts_settings_loaded;

static uint8_t            bts_last_bda[6];      /* 上次连上的耳机/音箱 */
static bool               bts_has_last;
static esp_timer_handle_t bts_auto_timer;       /* 开机自动连的一次性定时器 */

void bts_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                      /* 没存过 = 默认开、没有上次设备 */
    }
    uint8_t on = 1;
    bts_enabled = (nvs_get_u8(h, NVS_KEY_ON, &on) == ESP_OK) ? (on != 0) : true;
    size_t len = sizeof(bts_last_bda);
    bts_has_last = (nvs_get_blob(h, NVS_KEY_LAST, bts_last_bda, &len) == ESP_OK && len == 6);
    nvs_close(h);
}

void bts_settings_save_enabled(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_ON, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void bts_settings_save_last(const uint8_t bda[6])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_KEY_LAST, bda, 6);
    nvs_commit(h);
    nvs_close(h);
    memcpy(bts_last_bda, bda, 6);
    bts_has_last = true;
}

/* 开机自动连上次的设备（延时到协议栈稳定后跑，esp_timer 任务上下文） */
static void bts_auto_connect_cb(void *arg)
{
    (void)arg;
    if (!bts_enabled || bts_connected || !bts_has_last) {
        return;
    }
    ESP_LOGI(TAG, "auto connect to saved device");
    bt_service_connect(bts_last_bda);
}

/* init 末尾：有"上次设备"就（重）起一次性定时器；没有记录就什么都不做 */
void bts_auto_connect_arm(void)
{
    if (!bts_has_last) {
        return;
    }
    if (!bts_auto_timer) {
        esp_timer_create_args_t args = {};
        args.callback = bts_auto_connect_cb;
        args.name     = "bt_auto_conn";
        if (esp_timer_create(&args, &bts_auto_timer) != ESP_OK) {
            bts_auto_timer = NULL;
        }
    }
    if (bts_auto_timer) {
        esp_timer_stop(bts_auto_timer);
        esp_timer_start_once(bts_auto_timer, (uint64_t)AUTO_CONNECT_MS * 1000ULL);
    }
}

void bts_auto_connect_stop(void)
{
    if (bts_auto_timer) {
        esp_timer_stop(bts_auto_timer);
    }
}
