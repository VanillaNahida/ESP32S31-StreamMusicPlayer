/*
 * play_state.cc —— 播放状态记忆实现（NVS + 防抖落盘任务）
 *
 * 数据布局（NVS namespace "player"）：
 *   "vol"  i32   音量 0~100
 *   "path" str   当前曲目 VFS 路径
 *   "pos"  u32   已播毫秒
 *
 * 落盘策略（flash 寿命 + 实时性的折中，见头文件说明）：
 *   - setter 只改 RAM 并唤醒本模块的任务，返回值立刻拿到；
 *   - 任务醒来后先"多等一会儿"（防抖），期间还有新变化就继续等，
 *     把一段连续操作（按住 VOL+ / 进度每 500 ms 刷新）合并成**一次**写入；
 *   - play_state_commit() 走强制路径：不防抖，立刻写。
 */
#include "play_state.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "play_state";

#define NVS_NS              "player"
#define KEY_VOL             "vol"
#define KEY_PATH            "path"
#define KEY_POS             "pos"

/* 防抖窗口：最后一次改动之后静默这么久才落盘 */
#define PLAY_STATE_DEBOUNCE_MS   8000
/* 进度至少变化这么多毫秒才值得再写一次 flash */
#define PLAY_STATE_MIN_STEP_MS   3000
#define PLAY_STATE_TASK_STACK    3072

static bool              s_ready;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static nvs_handle_t      s_nvs;
static bool              s_nvs_open;

/* 内存态（s_lock 保护） */
static int      s_vol;
static char     s_path[PLAY_STATE_PATH_MAX];
static uint32_t s_pos;
static bool     s_dirty_vol;
static bool     s_dirty_track;
static bool     s_force;         /* 强制落盘（不防抖） */

/* 已落盘态（只有本模块任务碰，不需要锁） */
static int      s_saved_vol = -1;
static char     s_saved_path[PLAY_STATE_PATH_MAX];
static uint32_t s_saved_pos;

/* ---------------------------------------------------------------- 落盘 */

static void ps_flush(void)
{
    int      vol;
    uint32_t pos;
    bool     w_vol, w_track, force;
    char     path[PLAY_STATE_PATH_MAX];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    vol   = s_vol;
    pos   = s_pos;
    force = s_force;
    s_force = false;
    memcpy(path, s_path, sizeof(path));

    /* 没变（或变化太小）就不写 flash：一次 NVS 提交要几毫秒，而且有寿命 */
    w_vol = s_dirty_vol && (force || vol != s_saved_vol);
    const bool path_changed = (strcmp(path, s_saved_path) != 0);
    const uint32_t delta = (pos > s_saved_pos) ? (pos - s_saved_pos) : (s_saved_pos - pos);
    w_track = s_dirty_track && (force || path_changed || delta >= PLAY_STATE_MIN_STEP_MS);

    if (w_vol) {
        s_saved_vol   = vol;
        s_dirty_vol   = false;
    }
    if (w_track) {
        memcpy(s_saved_path, path, sizeof(s_saved_path));
        s_saved_pos   = pos;
        s_dirty_track = false;
    }
    xSemaphoreGive(s_lock);

    if (!w_vol && !w_track) {
        return;
    }
    if (!s_nvs_open) {
        return;
    }

    /* NVS 只由本任务写，不存在并发 set */
    if (w_vol) {
        nvs_set_i32(s_nvs, KEY_VOL, (int32_t)vol);
    }
    if (w_track) {
        nvs_set_str(s_nvs, KEY_PATH, path);
        nvs_set_u32(s_nvs, KEY_POS, pos);
    }
    esp_err_t err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit failed: %s", esp_err_to_name(err));
        /* 写失败就把脏标志放回去，下次再试 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (w_vol)   s_dirty_vol   = true;
        if (w_track) s_dirty_track = true;
        xSemaphoreGive(s_lock);
        return;
    }
    ESP_LOGD(TAG, "saved: vol=%d path='%s' pos=%ums", w_vol ? vol : s_saved_vol,
             w_track ? path : s_saved_path, (unsigned)(w_track ? pos : s_saved_pos));
}

static void play_state_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 睡到有人报告状态变化 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * 防抖：静默 PLAY_STATE_DEBOUNCE_MS 才真正落盘。
         * 循环条件里先看 s_force：play_state_commit()（暂停/停止/换曲）要求
         * **立刻**落盘，不能在这里再等一个防抖窗口。
         */
        while (!s_force &&
               ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PLAY_STATE_DEBOUNCE_MS)) != 0) {
            /* 又有新变化：重新开始数静默时间 */
        }
        ps_flush();
    }
}

static void ps_wake(bool force)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (force) {
            s_force = true;
        }
        xSemaphoreGive(s_lock);
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ---------------------------------------------------------------- 初始化 */

esp_err_t play_state_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 本工程没有别处显式初始化 NVS（Wi-Fi 驱动会顺手做），
     * 而播放状态要在 audio_service_init() 之前读出来，所以这里自己来一遍。
     * nvs_flash_init 是幂等的，重复调用返回 ESP_OK。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s (不做记忆)", esp_err_to_name(err));
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) {
        s_nvs_open = true;

        int32_t vol = PLAY_STATE_VOL_DEFAULT;
        if (nvs_get_i32(s_nvs, KEY_VOL, &vol) == ESP_OK) {
            if (vol < 0)   vol = 0;
            if (vol > 100) vol = 100;
            s_vol        = (int)vol;
            s_saved_vol  = (int)vol;
        } else {
            s_vol        = PLAY_STATE_VOL_DEFAULT;
            s_saved_vol  = -1;      /* -1 = 还没有落盘过 */
        }

        size_t len = sizeof(s_path);
        if (nvs_get_str(s_nvs, KEY_PATH, s_path, &len) == ESP_OK && s_path[0]) {
            uint32_t pos = 0;
            nvs_get_u32(s_nvs, KEY_POS, &pos);
            s_pos = pos;
            memcpy(s_saved_path, s_path, sizeof(s_saved_path));
            s_saved_pos = pos;
        } else {
            s_path[0] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "nvs_open(%s): %s", NVS_NS, esp_err_to_name(err));
        s_vol = PLAY_STATE_VOL_DEFAULT;
    }

    if (xTaskCreate(play_state_task, "play_state", PLAY_STATE_TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "task create failed (只记忆在 RAM 里)");
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready: vol=%d track='%s' pos=%ums",
             s_vol, s_path[0] ? s_path : "(无)", (unsigned)s_pos);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 音量 */

int play_state_volume_get(int fallback)
{
    if (!s_ready) {
        return fallback;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_vol;
    xSemaphoreGive(s_lock);
    return v;
}

void play_state_volume_set(int percent)
{
    if (!s_ready) {
        return;
    }
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = (percent != s_vol);
    s_vol = percent;
    if (changed) {
        s_dirty_vol = true;
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        ps_wake(false);
    }
}

/* ---------------------------------------------------------------- 曲目 / 进度 */

esp_err_t play_state_track_get(char *path, size_t cap, uint32_t *pos_ms)
{
    if (!path || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    path[0] = '\0';
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool has = (s_path[0] != '\0');
    if (has) {
        /* 用 snprintf 而不是 strncpy：保证 NUL 结尾，且不越界 */
        snprintf(path, cap, "%s", s_path);
        if (pos_ms) {
            *pos_ms = s_pos;
        }
    }
    xSemaphoreGive(s_lock);
    return has ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void play_state_track_set(const char *path, uint32_t pos_ms)
{
    if (!s_ready || !path || !path[0]) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = (strcmp(s_path, path) != 0) || (pos_ms != s_pos);
    if (changed) {
        snprintf(s_path, sizeof(s_path), "%s", path);
        s_pos         = pos_ms;
        s_dirty_track = true;
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        ps_wake(false);
    }
}

void play_state_track_clear(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_path[0]     = '\0';
    s_pos         = 0;
    s_dirty_track = true;
    xSemaphoreGive(s_lock);
    ps_wake(true);
}

void play_state_commit(void)
{
    if (!s_ready) {
        return;
    }
    ps_wake(true);
}
