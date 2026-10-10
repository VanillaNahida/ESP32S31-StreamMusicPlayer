/*
 * bts_events.cc —— bt_service 内部：蓝牙栈事件回调
 *
 * 从 bt_service.cc 887–1020（bt_audio_event_cb）与 1391–1395
 * （bt_service_set_media_ctrl_cb）原样搬来：五个分支的判断、名字"只增不减"的策略、
 * NVS 写入时机、日志文案全部与拆分前逐字一致。
 *
 * ⚠ 回调跑在蓝牙栈（BTC）任务上下文：`bts_name_save` / `bts_settings_save_last` 会写
 *   flash，`bt_service_refresh_bonded` 会查配对表 —— 它们原来就在这里做，位置没动。
 */

#include "bts_events.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_stream.h"
#include "esp_gap_bt_api.h"
/* ⚠ FreeRTOS.h 必须在 stream_buffer.h 之前（xStreamBufferReset / pdPASS 用得到） */
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "bts_audio.h"
#include "bts_feed.h"
#include "bts_name_cache.h"
#include "bts_settings.h"
#include "bts_state.h"

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

/* 远端媒体键回调（注册在 LVGL 任务、调用在 BT 回调任务） */
bt_media_ctrl_cb_t bts_ctrl_cb;
void              *bts_ctrl_user;

void bts_audio_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    (void)user_data;
    switch (event) {
    case ESP_BT_AUDIO_EVENT_DISCOVERY_STATE_CHG: {
        const esp_bt_audio_event_discovery_st_t *st = (esp_bt_audio_event_discovery_st_t *)event_data;
        bts_discovering = st->discovering;
        if (!bts_discovering) {
            ESP_LOGI(TAG, "discovery done: %u device(s)", (unsigned)bts_dev_count);
            if (bts_state == BT_ST_DISCOVERING) {
                bts_state = BT_ST_READY;
            }
        } else {
            bts_state = BT_ST_DISCOVERING;
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED: {
        const esp_bt_audio_event_device_discovered_t *d =
            (esp_bt_audio_event_device_discovered_t *)event_data;
        bt_dev_t *e = bts_dev_find(d->addr, true);
        if (e) {
            /*
             * ★ 名字"只增不减"（2026-10 修"扫描时设备名有时变成 MAC 地址"）：
             *   一次查询会对**同一台设备上报多包**，而名字（EIR / 异步
             *   read_remote_name 的结果）往往**不在第一包里**。
             *   原来是每包都无条件重写 e->name，后面只要再来一包不带名字的，
             *   就会把已经拿到的好名字**覆盖成 MAC**（看起来"名字一会儿有一会儿没有"）。
             *   现在：有名字才写；仍然没名字（从没见过）才退回地址显示。
             */
            if (d->name[0] && strncmp(e->name, d->name, sizeof(e->name)) != 0) {
                snprintf(e->name, sizeof(e->name), "%s", d->name);
                /* 名字**只有**扫描里拿得到：立刻缓存到 NVS，否则下次开机自动连
                 * 这台设备时（不扫描）界面上只会显示 MAC 地址。 */
                bts_name_save(d->addr, e->name);
            }
            e->rssi     = (int8_t)d->rssi;
            e->is_audio = (((d->disc_data.classic.cod & ESP_BT_COD_MAJOR_DEV_BIT_MASK)
                            >> ESP_BT_COD_MAJOR_DEV_BIT_OFFSET) == ESP_BT_COD_MAJOR_DEV_AV);
            for (size_t i = 0; i < bts_bonded_count; i++) {
                if (bts_same_bda(bts_bonded[i].bda, d->addr)) {
                    e->bonded = true;
                    break;
                }
            }
            if (e->name[0] == '\0') {
                /* 这一包还没带名字：先用 NVS 里缓存过的（"名字只增不减"的最后一道保险） */
                bts_name_load(e->bda, e->name, sizeof(e->name));
            }
            if (e->name[0] == '\0') {
                /* 从没见过名字的设备：退回地址显示（等后面的包带来真名） */
                bt_service_addr_str(e->bda, e->name, sizeof(e->name));
            }
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        const esp_bt_audio_event_connection_st_t *st =
            (esp_bt_audio_event_connection_st_t *)event_data;
        if (st->tech != ESP_BT_AUDIO_TECH_CLASSIC) {
            break;
        }
        bts_connected = st->connected;
        if (st->connected) {
            char addr[18];
            bt_service_addr_str(st->addr, addr, sizeof(addr));
            const bt_dev_t *d = bts_dev_find(st->addr, false);
            bts_make_peer_name(st->addr, d ? d->name : NULL);
            /* 名字可能来自扫描表或 NVS：顺手同步进缓存（是 MAC 兜底时函数内部会跳过） */
            bts_name_save(st->addr, bts_peer_name);
            memcpy(bts_peer_bda, st->addr, 6);
            bts_state = BT_ST_CONNECTED;
            ESP_LOGI(TAG, "a2dp connected: %s (%s)", bts_peer_name, addr);
            /* 记住这台设备：下次开机自动连它（需求） */
            bts_settings_save_last(st->addr);
            bt_service_refresh_bonded();
        } else {
            bts_streaming = false;
            bts_stream    = NULL;
            bts_state     = BT_ST_READY;
            ESP_LOGW(TAG, "a2dp disconnected");
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        const esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;
        switch (st->state) {
        case ESP_BT_AUDIO_STREAM_STATE_STARTED: {
            bts_stream    = st->stream_handle;
            bts_streaming = true;
            bts_state     = BT_ST_STREAMING;
            if (xStreamBufferReset(bts_pcm) != pdPASS) {
                ESP_LOGW(TAG, "pcm buffer reset failed");
            }
            /* 协商到的实际码流参数：采样率跟音源不一致会有音高偏差，
             * 先打出来，方便对照；重采样留给下一步（esp_asrc）。
             * channels 是声道**位掩码**，popcount 才是声道数。 */
            esp_bt_audio_stream_codec_info_t info = {};
            if (esp_bt_audio_stream_get_codec_info(st->stream_handle, &info) == ESP_OK) {
                bts_rate     = (int)info.sample_rate;
                bts_channels = __builtin_popcount((unsigned)info.channels);
                ESP_LOGI(TAG, "a2dp stream started: %d Hz, %d ch, %u bit, PCM/frame %u B",
                         (int)info.sample_rate, (int)bts_channels,
                         (unsigned)info.bits, (unsigned)info.frame_size);
            }
            ESP_LOGI(TAG, "streaming, PCM buffer %d bytes", (int)PCM_BUF_BYTES);
            break;
        }
        case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
        case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
            if (bts_stream == st->stream_handle) {
                bts_stream    = NULL;
                bts_streaming = false;
                bts_state     = bts_connected ? BT_ST_CONNECTED : BT_ST_READY;
            }
            ESP_LOGI(TAG, "a2dp stream stopped");
            break;
        default:
            break;
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_MEDIA_CTRL_CMD: {
        const esp_bt_audio_event_media_ctrl_t *cmd = (esp_bt_audio_event_media_ctrl_t *)event_data;
        ESP_LOGI(TAG, "remote media cmd: %d", (int)cmd->cmd);
        if (bts_ctrl_cb) {
            bts_ctrl_cb((int)cmd->cmd, bts_ctrl_user);
        }
        break;
    }
    default:
        break;
    }
}

void bt_service_set_media_ctrl_cb(bt_media_ctrl_cb_t cb, void *user)
{
    bts_ctrl_cb   = cb;
    bts_ctrl_user = user;
}
