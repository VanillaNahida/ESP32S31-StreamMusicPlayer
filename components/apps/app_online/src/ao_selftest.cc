/*
 * ao_selftest.cc —— 在线链路自检（整段受 ONLINE_SELFTEST 门控，平时是空壳）
 *
 * 用途：无 UI 交互验证
 *   抓歌单 → 二进制落盘 → 读回 → 下载第一首音频 → 抓封面 → 解析流地址 → 起播
 * 整条链；结束时把测试歌单从 NVS/SD 清掉（音频缓存留着），日志里打 SELFTEST。
 * 用来在没有触摸操作的情况下验证 HTTP / 重定向 / 磁盘 / 解码这条链。
 *
 * ⚠ 这是唯一允许直接调 audio_service_* 的地方（其余在线起播一律交给 app_music，
 *   见 docs/audio-stack.md §5）。平时保持 ONLINE_SELFTEST=0。
 */
#include "ao_selftest.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_service.h"
#include "online_service.h"

#include "ao_common.h"

#if ONLINE_SELFTEST
#include <stdlib.h>
#include <string.h>

static const char *TAG = AO_TAG;

static void online_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(12000));      /* 等 Wi-Fi 连上 + SNTP 校时 */
    ESP_LOGI(TAG, "SELFTEST begin");
    esp_err_t err = online_playlist_add("24381616");
    ESP_LOGI(TAG, "SELFTEST add: %s (%s)", esp_err_to_name(err),
             online_last_error() ? online_last_error() : "-");
    online_song_t *songs = NULL;
    size_t n = 0;
    err = online_playlist_load("24381616", &songs, &n);
    ESP_LOGI(TAG, "SELFTEST load: %s count=%u", esp_err_to_name(err), (unsigned)n);
    if (err == ESP_OK && songs && n > 0) {
        char path[256] = {0};
        err = online_song_prepare(&songs[0], path, sizeof(path));
        ESP_LOGI(TAG, "SELFTEST prepare '%s' -> %s, path=%s (%s)",
                 songs[0].title, esp_err_to_name(err), path,
                 online_last_error() ? online_last_error() : "-");
        uint8_t *jpg = NULL;
        size_t jl = 0;
        ESP_LOGI(TAG, "SELFTEST cover: %s (%u bytes)",
                 esp_err_to_name(online_song_cover_data(&songs[0], &jpg, &jl)),
                 (unsigned)jl);
        free(jpg);
        char *lrc = NULL;
        ESP_LOGI(TAG, "SELFTEST lyrics: %s (%u bytes)",
                 esp_err_to_name(online_song_lyrics_data(&songs[0], &lrc, NULL)),
                 (unsigned)(lrc ? strlen(lrc) : 0));
        free(lrc);

        /* 流式：解析最终地址 + 真的起播（验证 HTTP reader IO + TLS 这条链） */
        char stream[560] = {0};
        err = online_song_stream_url(&songs[0], stream, sizeof(stream));
        ESP_LOGI(TAG, "SELFTEST stream url: %s (%s)", esp_err_to_name(err),
                 online_last_error() ? online_last_error() : "-");
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SELFTEST play_uri: %s",
                     esp_err_to_name(audio_service_play_uri(stream)));
            vTaskDelay(pdMS_TO_TICKS(10000));   /* 放 10 秒，看是否真的在出声/推进 */
            audio_info_t info = {};
            const bool has_info = audio_service_get_info(&info);
            ESP_LOGI(TAG, "SELFTEST playing=%d info=%d rate=%d br=%d dur=%u",
                     (int)audio_service_is_playing(), (int)has_info,
                     info.sample_rate, info.bitrate, (unsigned)info.duration_ms);
            audio_service_stop();
        }
        free(songs);
    }
    online_playlist_remove("24381616");    /* 别把测试歌单留在用户列表里 */
    ESP_LOGI(TAG, "SELFTEST done");
    vTaskDelete(NULL);
}
#endif   /* ONLINE_SELFTEST */

void ao_selftest_start(void)
{
#if ONLINE_SELFTEST
    xTaskCreate(online_selftest_task, "online_st", WORK_TASK_STACK, NULL, 3, NULL);
#endif
}
