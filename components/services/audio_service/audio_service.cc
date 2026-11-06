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
 *   audio_output.cc   输出级（重采样 + 四路输出：扬声器/蓝牙/USB 声卡/静音 + 时间轴）
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
#include "usb_audio_service.h"   /* USB 声卡插拔：拔出要立刻切回板载 */

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
 * 蓝牙 AVRCP 绝对音量：对端改了音量
 *
 * 耳机/手机自己把音量拧了，AVRCP 通过 ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE 报上来。
 * 我们只**同步状态**（内存 + NVS + 输出级缓存），绝不回发 —— 回发会让两个设备
 * 互相刷音量（见 audio_output_note_peer_volume 的注释）。
 *
 * ⚠ 跑在蓝牙栈任务上下文：不碰 LVGL；play_state_volume_set() 自带防抖与锁，
 *   不会在这里写穿 flash。
 * =========================================================================*/
static void on_bt_volume_from_peer(int percent, void *user)
{
    (void)user;
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    /* 只有当前真的走蓝牙输出时才认这条事件（否则是别的链路的残留） */
    if (audio_output_route() != AUDIO_OUT_BLUETOOTH) {
        return;
    }
    s_volume = percent;
    audio_output_note_peer_volume(percent);
    bsp_audio_set_mute(percent == 0);
    play_state_volume_set(percent);   /* 唯一的 NVS 落盘点仍在这里 */
    ESP_LOGI(TAG, "peer set volume -> %d%%", percent);
}

/* ===========================================================================
 * 初始化
 * =========================================================================*/

/*
 * USB 声卡插拔的 UI 提示（**单生产者/单消费者**：写的是 USB 驱动任务，读的是状态栏）。
 * 只存一句人话 + 一个"有新消息"标志；字符串先写、标志后置，读到 true 时内容一定完整。
 */
static char           s_usb_msg[96];
static volatile bool  s_usb_msg_pending;

bool audio_service_take_usb_msg(char *buf, size_t size)
{
    if (!s_usb_msg_pending) {
        return false;
    }
    s_usb_msg_pending = false;
    if (buf && size) {
        snprintf(buf, size, "%s", s_usb_msg);
    }
    return true;
}

/*
 * USB 声卡插拔（**在 USB/UAC 驱动任务里调用**：不碰 LVGL、不做重活）。
 *
 * 插入：只提示（`USB 声卡已连接：<产品名>（24bit）`）—— 需求是"连着的时候才出现
 *       可切换项"，切不切由用户决定；
 * 拔出：**立即**把路由切回板载扬声器（硬需求），并提示是"已断开"还是"已拔出"。
 *       三条保险里的第二条：
 *   ① USB 设备回调已经先把 s_ready 清了；
 *   ② 这里把 audio_output 的路由切回去（纯标志 + 功放/静音时序，不阻塞）；
 *   ③ write_out() 万一还拿到旧路由，写失败时也会就地回落。
 */
static void on_usb_audio_state(bool ready, void *user)
{
    (void)user;
    if (ready) {
        ESP_LOGI(TAG, "USB audio device ready (%s, %d bit)",
                 usb_audio_device_name(), usb_audio_bit_depth());
        return;                      /* 提示文案由 notice 回调给（带设备名/位深） */
    }
    if (audio_output_route() == AUDIO_OUT_USB) {
        ESP_LOGW(TAG, "USB audio gone -> switch back to speaker immediately");
        audio_output_set_route(AUDIO_OUT_SPEAKER);
        /* 覆盖 notice 刚写好那句，让用户知道声音去哪了 */
        snprintf(s_usb_msg, sizeof(s_usb_msg), "USB 声卡已断开，已切回扬声器");
        s_usb_msg_pending = true;
    }
}

/* USB 驱动任务给的"一句话"（插入 / 拔出 / 用不了）：抄下来给状态栏弹 Toast */
static void on_usb_audio_notice(const char *msg, void *user)
{
    (void)user;
    if (!msg || !msg[0]) {
        return;
    }
    ESP_LOGI(TAG, "USB notice: %s", msg);
    snprintf(s_usb_msg, sizeof(s_usb_msg), "%s", msg);
    s_usb_msg_pending = true;
}

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

    /* 蓝牙 AVRCP 绝对音量：对端改了音量要回到这里（蓝牙可能还没 init，
     * 这只是把回调记下来，bt_service 初始化时会用）。 */
    bt_service_set_volume_cb(on_bt_volume_from_peer, NULL);

    /* USB 声卡插拔回调：只登记，USB 主机由 main 在首帧之后 usb_audio_init() 起来 */
    usb_audio_set_state_cb(on_usb_audio_state, NULL);
    usb_audio_set_notice_cb(on_usb_audio_notice, NULL);

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
    if (out == AUDIO_OUT_USB && !usb_audio_is_ready()) {
        return ESP_ERR_INVALID_STATE;   /* 没插声卡（或还没起流）同理 */
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
    /* USB 声卡没有"codec 静音寄存器"可写，走 UAC 的 volume（0 = 静音） */
    if (audio_output_route() == AUDIO_OUT_USB) {
        usb_audio_set_volume(mute ? 0 : s_volume);
    }
    return bsp_audio_set_mute(mute);
}
