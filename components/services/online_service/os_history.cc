/*
 * os_history.cc —— 搜索历史（NVS 持久化，新的在最前，同一关键词去重提前）
 *
 * 逐字搬自原 online_service.cc：
 *   nvs_load_history     1913–1935
 *   nvs_save_history     1937–1955
 *   hist_move_to_front   1958–1969
 *   online_history_count/at/add/remove/clear 1971–2027
 */
#include "internal/os_history.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "internal/os_state.h"
#include "internal/os_util.h"
#include "online_service.h"

static const char *TAG = "online";

static char   s_hist[ONLINE_HISTORY_MAX][ONLINE_KW_MAX];
static size_t s_hist_n;

void os_history_nvs_load(void)
{
    s_hist_n = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    if (nvs_get_u8(h, NVS_KEY_SH_CNT, &n) == ESP_OK) {
        if (n > ONLINE_HISTORY_MAX) {
            n = ONLINE_HISTORY_MAX;
        }
        for (uint8_t i = 0; i < n; i++) {
            char key[12];
            snprintf(key, sizeof(key), NVS_KEY_SH_FMT, i);
            size_t len = sizeof(s_hist[s_hist_n]);
            if (nvs_get_str(h, key, s_hist[s_hist_n], &len) == ESP_OK && s_hist[s_hist_n][0]) {
                s_hist_n++;
            }
        }
    }
    nvs_close(h);
}

static void nvs_save_history(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_SH_CNT, (uint8_t)s_hist_n);
    for (size_t i = 0; i < ONLINE_HISTORY_MAX; i++) {
        char key[12];
        snprintf(key, sizeof(key), NVS_KEY_SH_FMT, (int)i);
        if (i < s_hist_n) {
            nvs_set_str(h, key, s_hist[i]);
        } else {
            nvs_erase_key(h, key);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

/* 把第 idx 条挪到最前（其余后移），用于"再次搜同一个词"和"删单条" */
static void hist_move_to_front(size_t idx)
{
    if (idx == 0 || idx >= s_hist_n) {
        return;
    }
    char tmp[ONLINE_KW_MAX];
    os_str_copy(tmp, sizeof(tmp), s_hist[idx]);
    for (size_t i = idx; i > 0; i--) {
        os_str_copy(s_hist[i], sizeof(s_hist[0]), s_hist[i - 1]);
    }
    os_str_copy(s_hist[0], sizeof(s_hist[0]), tmp);
}

size_t online_history_count(void)
{
    return s_hist_n;
}

const char *online_history_at(size_t idx)
{
    return (idx < s_hist_n) ? s_hist[idx] : NULL;
}

esp_err_t online_history_add(const char *kw)
{
    if (!kw || !kw[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 已有同一个词：提到最前，不新增（这是搜索历史的常规行为） */
    for (size_t i = 0; i < s_hist_n; i++) {
        if (strcmp(s_hist[i], kw) == 0) {
            hist_move_to_front(i);
            nvs_save_history();
            return ESP_OK;
        }
    }
    const size_t n = (s_hist_n < ONLINE_HISTORY_MAX) ? (s_hist_n + 1) : ONLINE_HISTORY_MAX;
    for (size_t i = n - 1; i > 0; i--) {
        os_str_copy(s_hist[i], sizeof(s_hist[0]), s_hist[i - 1]);
    }
    os_str_copy(s_hist[0], sizeof(s_hist[0]), kw);
    s_hist_n = n;
    nvs_save_history();
    return ESP_OK;
}

esp_err_t online_history_remove(size_t idx)
{
    if (idx >= s_hist_n) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = idx + 1; i < s_hist_n; i++) {
        os_str_copy(s_hist[i - 1], sizeof(s_hist[0]), s_hist[i]);
    }
    s_hist_n--;
    s_hist[s_hist_n][0] = '\0';
    nvs_save_history();
    return ESP_OK;
}

esp_err_t online_history_clear(void)
{
    s_hist_n = 0;
    for (size_t i = 0; i < ONLINE_HISTORY_MAX; i++) {
        s_hist[i][0] = '\0';
    }
    nvs_save_history();
    ESP_LOGI(TAG, "search history cleared");
    return ESP_OK;
}
