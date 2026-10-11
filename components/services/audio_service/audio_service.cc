/*
 * audio_service.cc —— 播放服务对外门面
 *
 * 这一层只做四件事，**不含任何解码/传输/输出细节**：
 *   1. 参数校验与对外 API 的语义（含"音量写 NVS"这种唯一落盘点）
 *   2. 会话锁的取放（起播/停止/seek 重开这三类会动会话的流程）
 *   3. 事件上报（audio_notify → 上层 audio_event_cb_t）
 *   4. 起播流程的编排：本地文件 / 在线流（"先准备传输，再起解码"）
 *
 * 实现分散在：
 *   audio_engine.cc   espaudiocore 会话与事件
 *   audio_output.cc   输出级（重采样 + 三路输出 + 时间轴）
 *   audio_http.cc     在线 HTTP 传输
 *   audio_seek.cc     拖动定位
 * 契约见 audio_internal.h（**三条线程纪律写在那里，改代码前先读**）。
 *
 * 与上一版（ESP-GMF / esp_audio_simple_player）的行为差异，只列会影响上层的：
 *   - `audio_service_seek_supported()` 对本地文件**恒真**（不再依赖码率探测）；
 *     在线仅当容器是 MP3/ADTS-AAC 且已知总长+时长时为真；
 *   - `audio_service_seek_landing_ms()` 恒返回 false（落点核对机制已退休）；
 *   - 在线遇到**不能流式解码**的容器（M4A/OGG/Opus）返回 ESP_ERR_NOT_SUPPORTED，
 *     上层应改走"先下载到 SD 再当本地文件播"（app_music 已有这条回退路）。
 */
#include "audio_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp.h"
#include "bt_service.h"
#include "play_state.h"

static const char *TAG = "audio_svc";

static bool             s_inited;
static int              s_volume = PLAY_STATE_VOL_DEFAULT;
static audio_event_cb_t s_evt_cb;
static void            *s_evt_user;
/** 起播请求序号：网络等待期间若来了新的点歌请求，旧的那次就自己放弃 */
static volatile uint32_t s_req_seq;

/* ===========================================================================
 * 事件上报
 * =========================================================================*/

void audio_notify(audio_evt_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt, s_evt_user);
    }
}

void audio_service_set_event_cb(audio_event_cb_t cb, void *user)
{
    s_evt_cb = cb;
    s_evt_user = user;
}

/* ===========================================================================
 * 初始化
 * =========================================================================*/

esp_err_t audio_service_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 音量来自 NVS（play_state_init() 必须在 main 里先跑过；没跑就是默认值）。
     * 这里只读不写 —— 写 NVS 的唯一落盘点在 audio_service_set_volume()。 */
    s_volume = play_state_volume_get(PLAY_STATE_VOL_DEFAULT);

    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed");
        return ESP_FAIL;
    }
    /* 板载输出固定 48k/2ch/16bit：与 audio_output 的重采样目标一致，
     * 也与 ES8389 的 TDM 配置一致（见 bsp_audio.cc） */
    esp_err_t err = bsp_audio_open_output(AUDIO_OUT_RATE, AUDIO_OUT_CHANNELS, AUDIO_OUT_BITS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (audio_engine_init() != ESP_OK) {
        return ESP_FAIL;
    }
    if (audio_output_init() != ESP_OK) {
        return ESP_FAIL;
    }

    /* 起播前保持静音，避免开机的"哒"声 */
    bsp_audio_set_mute(true);
    bsp_audio_pa_enable(false);
    audio_output_set_volume(s_volume);

    s_inited = true;
    ESP_LOGI(TAG, "ready: espaudiocore decode -> asrc -> %d Hz/%dch/%dbit, volume %d",
             AUDIO_OUT_RATE, AUDIO_OUT_CHANNELS, AUDIO_OUT_BITS, s_volume);
    return ESP_OK;
}

/* ===========================================================================
 * 起播
 * =========================================================================*/

esp_err_t audio_service_play_path(const char *vfs_abs_path)
{
    if (!vfs_abs_path || vfs_abs_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    s_req_seq = s_req_seq + 1;
    const uint32_t seq = s_req_seq;

    audio_core_lock();
    esp_err_t err = audio_engine_start_file(vfs_abs_path);
    audio_core_unlock();

    /* 本地播放不需要在线传输（幂等；放在锁外，它会等馈送任务退出） */
    audio_http_stop();

    if (seq != s_req_seq) {
        return ESP_ERR_INVALID_STATE;      /* 期间来了更新的请求 */
    }
    return err;
}

esp_err_t audio_service_play_uri(const char *uri)
{
    if (!uri || !uri[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(uri, "http", 4) != 0) {
        return audio_service_play_path(uri);
    }

    s_req_seq = s_req_seq + 1;
    const uint32_t seq = s_req_seq;

    /* 1) 先停掉旧会话：**必须在重开传输之前**，否则旧解码器会看到被抽空的
     *    ringbuf，把"暂时没数据"当成流结束，给上层发一个莫名其妙的 FINISHED。 */
    audio_core_lock();
    audio_engine_stop();
    audio_core_unlock();

    /* 2) 建连 + 嗅探容器（网络动作，**不持锁**：慢网络下不能卡住 LVGL 任务） */
    esp_err_t err = audio_http_prepare(uri);
    if (err != ESP_OK) {
        return err;
    }
    err = audio_http_start(0);
    if (err != ESP_OK) {
        return err;
    }
    if (!audio_http_wait_ready(8000)) {
        ESP_LOGW(TAG, "stream connect failed: %s", uri);
        audio_http_stop();
        return ESP_ERR_TIMEOUT;
    }
    if (seq != s_req_seq) {
        audio_http_stop();
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_http_streamable()) {
        ESP_LOGW(TAG, "容器 fmt=%d 不能边下边解（Vorbis/Opus/M4A 需可 seek 的源）",
                 audio_http_format());
        audio_http_stop();
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 3) 起解码会话（进锁前再确认一次没有更新的请求，比如用户按了停止） */
    audio_core_lock();
    err = (seq != s_req_seq) ? ESP_ERR_INVALID_STATE
                             : audio_engine_start_stream(audio_http_format());
    audio_core_unlock();
    if (err != ESP_OK) {
        audio_http_stop();
        return err;
    }
    return ESP_OK;
}

/* ===========================================================================
 * 传输控制
 * =========================================================================*/

esp_err_t audio_service_pause(void)
{
    return audio_engine_pause();
}

esp_err_t audio_service_resume(void)
{
    return audio_engine_resume();
}

esp_err_t audio_service_toggle(void)
{
    if (!audio_engine_session_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (audio_engine_running()) {
        return audio_service_pause();
    }
    return audio_service_resume();
}

esp_err_t audio_service_stop(void)
{
    s_req_seq = s_req_seq + 1;   /* volatile 上不能用 ++（C++20 起 -Werror=volatile） */

    audio_core_lock();
    audio_engine_stop();
    audio_core_unlock();

    audio_http_stop();

    bsp_audio_set_mute(true);
    bsp_audio_pa_enable(false);
    return ESP_OK;
}

bool audio_service_is_playing(void)
{
    /* **不含暂停态**（与上一版语义一致）：上层用它决定"要不要推进进度" */
    return audio_engine_running();
}

bool audio_service_output_active(void)
{
    return audio_output_flowing();
}

/* ===========================================================================
 * 拖动定位
 * =========================================================================*/

bool audio_service_seek_supported(void)
{
    return audio_seek_supported();
}

esp_err_t audio_service_seek_ms(uint32_t pos_ms)
{
    return audio_seek_to(pos_ms);
}

bool audio_service_seek_landing_ms(uint32_t *out_ms)
{
    return audio_seek_landing_ms(out_ms);
}

void audio_service_set_duration_ms(uint32_t dur_ms)
{
    audio_engine_set_duration_hint(dur_ms);
}

/* ===========================================================================
 * 曲目信息 / 在线流观测
 * =========================================================================*/

bool audio_service_get_info(audio_info_t *out)
{
    return audio_engine_info_get(out);
}

bool audio_service_stream_progress(int *percent)
{
    return audio_http_progress(percent);
}

bool audio_service_stream_speed(int *kbps)
{
    return audio_http_speed(kbps);
}

/** 当前播放位置（毫秒）——本组件内部时间轴（送去硬件的字节 ÷ 192 B/ms） */
uint32_t audio_service_position_ms(void)
{
    return audio_output_position_ms();
}

/** 解码源格式（采样率/声道/位深）——"歌曲信息"弹窗用，在线也有值 */
bool audio_service_source_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    return audio_output_source_format(rate, channels, bits);
}

/** 平均码率（bit/s）：本地探测值 / 在线"总字节÷时长"（含实测纠偏） */
uint32_t audio_service_bitrate(void)
{
    return audio_engine_bitrate();
}

/* ===========================================================================
 * 输出通道 / 音量
 * =========================================================================*/

esp_err_t audio_service_set_output(audio_out_t out)
{
    if (out == AUDIO_OUT_BLUETOOTH && !bt_service_is_connected()) {
        return ESP_ERR_INVALID_STATE;   /* 没连上就别假装切过去了 */
    }
    audio_output_set_route(out);
    return ESP_OK;
}

audio_out_t audio_service_get_output(void)
{
    return audio_output_route();
}

esp_err_t audio_service_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    audio_output_set_volume(percent);

    /* 音量 0 视为静音（寄存器静音比软件缩放到 0 更干净） */
    bsp_audio_set_mute(percent == 0);

    /* **唯一**的 NVS 写入点：别在输出模块里再加一处，否则长按 VOL+ 会狂写 flash */
    play_state_volume_set(percent);
    return ESP_OK;
}

int audio_service_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_service_set_mute(bool mute)
{
    return bsp_audio_set_mute(mute);
}
