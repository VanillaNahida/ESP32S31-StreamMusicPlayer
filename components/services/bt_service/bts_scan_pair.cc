/*
 * bts_scan_pair.cc —— bt_service 内部：搜索 / 连接 / 断开 / 配对表
 *
 * 从 bt_service.cc 1237–1262、1271–1321、1323–1368、1377–1389 原样搬来
 * （发现开关、A2DP 连接/断开、媒体起停、配对表刷新、移除配对）。
 * 这些函数全是 bt_service.h 的对外契约，所以本模块不需要内部头文件。
 *
 * 表查询访问器（dev_count/dev、bonded_count/bonded）在 bts_state.cc —— 它们只是
 * 读 bts_state.cc 拥有的设备表，归状态模块更自然。
 */

#include "bt_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_bt_audio.h"          /* esp_bt_audio_defs.h：ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC */
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"
#include "esp_gap_bt_api.h"

#include "bts_conn.h"
#include "bts_name_cache.h"
#include "bts_state.h"

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

esp_err_t bt_service_start_discovery(void)
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bts_discovering) {
        return ESP_OK;
    }
    bts_dev_count = 0;                  /* 每次搜索重新列表 */
    esp_err_t ret = esp_bt_audio_classic_discovery_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "discovery start: %s", esp_err_to_name(ret));
        return ret;
    }
    bts_discovering = true;
    bts_state = BT_ST_DISCOVERING;
    return ESP_OK;
}

esp_err_t bt_service_stop_discovery(void)
{
    if (!bts_discovering) {
        return ESP_OK;
    }
    return esp_bt_audio_classic_discovery_stop();
}

esp_err_t bt_service_connect(const uint8_t bda[6])
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * 真正的连接过程（停查询 → 断旧对端 → 延时发起 → 超时 → 重试）全在
     * bts_conn.cc 的状态机里。这里只保留"入口"这一件事：
     * 原来那句 `esp_bt_audio_classic_connect()` 是**直接**发的 —— 搜索还在跑的时候
     * 发出去必然和 inquiry 抢射频（真机报 BTA_AV_FAIL_STREAM），而且没有任何
     * "连接中/失败"的状态可言，UI 只能干等。
     */
    return bts_conn_request(bda);
}

esp_err_t bt_service_disconnect(const uint8_t bda[6])
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_bd_addr_t addr;
    memcpy(addr, bda, 6);
    return esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
}

esp_err_t bt_service_media_start(void)
{
    if (!bts_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bts_streaming) {
        return ESP_OK;
    }
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "media start: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bt_service_media_stop(void)
{
    if (!bts_connected) {
        return ESP_OK;
    }
    return esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
}

esp_err_t bt_service_refresh_bonded(void)
{
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        bts_bonded_count = 0;
        return ESP_OK;
    }
    if (num > BT_DEV_MAX) {
        num = BT_DEV_MAX;
    }
    esp_bd_addr_t list[BT_DEV_MAX];
    int n = num;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
        return ESP_FAIL;
    }
    if (n < 0) {
        n = 0;
    }
    bts_bonded_count = (size_t)n;
    for (int i = 0; i < n; i++) {
        bt_dev_t *b = &bts_bonded[i];
        memset(b, 0, sizeof(*b));
        memcpy(b->bda, list[i], 6);
        b->bonded = true;
        const bt_dev_t *d = bts_dev_find(list[i], false);
        if (d && bts_name_is_real(list[i], d->name)) {
            snprintf(b->name, sizeof(b->name), "%s", d->name);
        }
        if (d) {
            b->is_audio = d->is_audio;
            b->rssi     = d->rssi;
        }
        if (b->name[0] == '\0') {
            /* 本次没扫到 / 只扫到 MAC：查 NVS 缓存（开机直接看"已配对"也有名字） */
            bts_name_load(b->bda, b->name, sizeof(b->name));
        }
        if (b->name[0] == '\0') {
            bt_service_addr_str(b->bda, b->name, sizeof(b->name));
        }
        bt_dev_t *e = bts_dev_find(list[i], false);
        if (e) {
            e->bonded = true;
        }
    }
    return ESP_OK;
}

esp_err_t bt_service_remove_bond(const uint8_t bda[6])
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_bd_addr_t addr;
    memcpy(addr, bda, 6);
    esp_err_t ret = esp_bt_gap_remove_bond_device(addr);
    if (ret == ESP_OK) {
        bt_service_refresh_bonded();
    }
    return ret;
}
