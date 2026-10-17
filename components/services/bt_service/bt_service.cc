/*
 * bt_service.cc —— 蓝牙音频（ESP-GMF esp_bt_audio / 经典蓝牙 A2DP Source）
 *
 * ★ 本文件只留**对外门面**：总开关 / init / deinit / 开关记忆查询。
 *   其余职责已按域拆到同目录的 bts_*.cc（纯搬迁，参数/节拍/日志文案与拆分前一致）：
 *
 *     bts_state.cc       共享状态 + 查询访问器 + 地址工具（状态的唯一 owner）
 *     bts_name_cache.cc  设备名 NVS 缓存（MAC → 名字，三级回退取名）
 *     bts_settings.cc    bt_cfg NVS（开关 / 上次设备）+ 开机自动连定时器
 *     bts_audio.cc       SBC 编码器 + 软件 ASRC + 重采样暂存
 *     bts_feed.cc        PCM 环形缓冲 + feed 任务 + write_pcm 流控（节拍来源）
 *     bts_events.cc      蓝牙栈事件回调（发现/连接/流状态/远端媒体键）
 *     bts_scan_pair.cc   搜索 / 连接 / 断开 / 配对表
 *
 *   内部契约头在 include/internal/（不是对外 API，别的组件别 include）。
 *
 * 分工（未变）：
 *   controller            ← 本文件（esp_bt_controller_init/enable CLASSIC_BT）
 *   Bluedroid + GAP + A2DP/AVRCP ← esp_bt_audio（host_config 传进去）
 *   SBC 编码              ← bts_audio.cc（esp_audio_codec 的编码器，见其顶部说明）
 *   PCM 打包与发送        ← esp_bt_audio 的 A2DP 源（打包 + RTP 时间戳 + 发送）
 *
 * 音频通路（未变）：
 *   音频管线 out 回调 → bt_service_write_pcm() → StreamBuffer（PSRAM）
 *   → feed 任务 → 攒满一帧 PCM → SBC 编码 → acquire_write/release_write → A2DP
 *
 * 为什么要中间缓冲 + 独立任务：acquire_write/release_write 会等 A2DP 的发送窗口
 * （几十毫秒级），而 out 回调跑在解码任务里，在那里等会把解码拖停。
 * 缓冲满时**丢帧**（蓝牙侧宁可少一点数据，也不能让本地播放卡住）。
 */
#include "bt_service.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_classic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bts_audio.h"
#include "bts_conn.h"
#include "bts_events.h"
#include "bts_feed.h"
#include "bts_settings.h"
#include "bts_state.h"
#include "bts_volume.h"

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

#define DEV_NAME        "KORVO-1 音乐播放器"   /* 对端看到的蓝牙名 */
#define PIN_CODE        "0000"

/* ---------------------------------------------------------------- 对外接口 */

esp_err_t bt_service_init(void)
{
    if (bts_inited) {
        return ESP_OK;
    }
    /* 先读开关与"上次设备"：开关关着就整个不初始化（射频与内存都不占） */
    bts_settings_load();
    if (!bts_enabled) {
        ESP_LOGI(TAG, "bt disabled by switch, skip init");
        bts_state = BT_ST_UNINIT;
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * 编译期护栏：经典蓝牙（BR/EDR）要求**控制器就是 BR/EDR Only**。
     * 若 sdkconfig 里把它留成默认的 BLE_ONLY（CONFIG_BTDM_CTRL_MODE_BLE_ONLY），
     * 下面的 esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) 拿不到经典蓝牙，
     * 而 esp_bluedroid_init() 会一直等 BR/EDR 控制器的 HCI 应答 —— 现象就是
     * "蓝牙页点扫描没反应"。修法见 sdkconfig.defaults 的蓝牙段落。
     */
#if !CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY
    ESP_LOGE(TAG, "控制器不是 BR/EDR Only 模式（CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY 未开），"
                  "经典蓝牙/A2DP 无法工作");
#endif

    /* 1) 控制器：经典蓝牙（esp_bt_audio 的 host 不负责这一步） */
    esp_bt_controller_config_t ccfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "bt init 1/3: controller init");
    esp_err_t ret = esp_bt_controller_init(&ccfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(ret));
        bts_state = BT_ST_FAILED;
        return ret;
    }
    ESP_LOGI(TAG, "bt init 2/3: controller enable (classic)");
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(ret));
        bts_state = BT_ST_FAILED;
        return ret;
    }

    /* 2) PCM 缓冲 + 喂数据任务（先建好，流一起来就能用） */
    ret = bts_feed_start();            /* PSRAM 优先，失败退回内部 RAM；已经建过就跳过 */
    if (ret != ESP_OK) {
        bts_state = BT_ST_FAILED;
        return ret;
    }

    /* 3) Bluedroid + A2DP Source（esp_bt_audio 一把做完） */
    esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
    snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "%s", DEV_NAME);
    host_cfg.pin_type = ESP_BT_PIN_TYPE_FIXED;
    snprintf((char *)host_cfg.pin_code, sizeof(host_cfg.pin_code), "%s", PIN_CODE);
    /* IO_CAP_NONE = Just Works：耳机/音箱常见的无码配对 */
    host_cfg.iocap = ESP_BT_IO_CAP_NONE;

    esp_bt_audio_config_t cfg = {};
    cfg.host_config   = &host_cfg;
    cfg.event_cb      = bts_audio_event_cb;
    cfg.event_user_ctx = NULL;
    /*
     * A2DP 源（推流给耳机）+ **AVRCP Controller**。
     *
     * 为什么要 AVRCP CT：A2DP 源的音量标准做法是**由源发 AVRCP 绝对音量**
     * （SetAbsoluteVolume）给对端，让耳机/音箱自己衰减。esp_bt_audio 把这条能力
     * 收在 esp_bt_audio_vol.h 里（esp_bt_audio_vol_set_absolute），而它的 vol_ops
     * 只在 AVRCP Controller 连上时才注册 —— 不写这个 role，那个函数会一直返回
     * ESP_ERR_INVALID_STATE（日志 Operation failed: vol_ops.set_absolute is not
     * registered），音量只能退回软件缩放。
     */
    cfg.classic.roles = ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC |
                        ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT;
    /* 发送任务：钉 core 0（core 1 是 LVGL），优先级 5、栈 4 KB */
    cfg.classic.a2dp_src_send_task_core_id    = 0;
    cfg.classic.a2dp_src_send_task_prio       = 5;
    cfg.classic.a2dp_src_send_task_stack_size = 4096;

    /*
     * ⚠ 初始化必须钉在 CONFIG_BT_BLUEDROID_PINNED_TO_CORE 那个核上、优先级低于 BTC
     *   （调用方 main.cc 的 radio_init_task 已经这么建了，别改成别的核/别的优先级）：
     *   上游 esp_bt_audio 的经典蓝牙初始化有竞态 —— bt_audio_a2dp_src_init() 调完
     *   esp_a2d_source_init()（只是往 BTC 投递消息、不等它执行完）就紧接着注册 SBC
     *   codec 的 SEP，而 g_a2dp_on_deinit 这个门禁的初值是 true，只有 BTC 处理完
     *   BTC_AV_SRC_API_INIT_EVT 才会置 false。异核并行时我们一定先跑到 SEP 注册，
     *   拿到 ESP_ERR_INVALID_STATE 并被组件里的 ESP_ERROR_CHECK 变成 abort
     *   （实测过一次"开机无限重启"）。同核 + 低优先级下，BTC 会抢占本任务把 INIT
     *   事件整条处理完，竞态窗口为 0。
     */
    ESP_LOGI(TAG, "bt init 3/3: bluedroid + A2DP source host");
    ret = esp_bt_audio_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_audio_init: %s", esp_err_to_name(ret));
        bts_state = BT_ST_FAILED;
        return ret;
    }

    bts_volume_reset();     /* 新协议栈：AVRCP 绝对音量还没连上，先按不可用算 */
    bt_service_refresh_bonded();
    bts_state   = BT_ST_READY;
    bts_inited  = true;    /* 只有真正成功才算初始化过，失败允许重试 */
    ESP_LOGI(TAG, "bt audio ready (A2DP source), %u bonded", (unsigned)bts_bonded_count);

    /* 开机自动连上次用过的设备（需求）；没有记录就什么都不做 */
    bts_auto_connect_arm();
    return ESP_OK;
}

/* ---------------------------------------------------------------- 总开关 */

bool bt_service_is_enabled(void)
{
    /* 首次查询时懒加载 NVS：UI 在服务尚未 init 时也要能显示正确的开关状态 */
    if (!bts_settings_loaded) {
        bts_settings_load();
        bts_settings_loaded = true;
    }
    return bts_enabled;
}

esp_err_t bt_service_set_enabled(bool on)
{
    if (on == bts_enabled) {
        return ESP_OK;
    }
    bts_enabled = on;
    bts_settings_save_enabled(on);
    ESP_LOGI(TAG, "bt switch -> %s", on ? "on" : "off");
    if (on) {
        /*
         * 重新打开：先等一下再 init。
         * esp_bt_audio_deinit() 之后，BTC 任务还要跑完它的收尾（A2DP 侧的
         * g_a2dp_on_deinit 由 BTC 异步清），立刻重新注册 SBC SEP 会撞
         * ESP_ERR_INVALID_STATE 并 abort。300 ms 足够它跑完；配合"钉在 BTC 核"
         * 一起用（见 app_settings 里的 radio_toggle_task）。
         */
        vTaskDelay(pdMS_TO_TICKS(300));
        return bt_service_init();
    }
    const esp_err_t ret = bt_service_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));      /* 让控制器 deinit 收尾，别立刻又 init */
    return ret;
}

/*
 * 关掉蓝牙：拆掉数据通路（feed 任务 + PCM 缓冲 + 编码器）→
 * esp_bt_audio_deinit()（内含 Bluedroid disable/deinit 与 profile 清理）→
 * 控制器 disable/deinit。
 *
 * 注意**不要**调 esp_bt_mem_release()：那会把控制器的内存永久归还，
 * 之后再也 init 不回来，而需求是"开关能来回切"。
 */
esp_err_t bt_service_deinit(void)
{
    bts_auto_connect_stop();
    /* 先把连接状态机停掉：在途的"15 s 超时 / 1.5 s 重试"定时器会在 esp_timer 任务里
     * 调 esp_bt_audio_*，而下面马上要 deinit 协议栈 —— 不停就是拿定时器戳已释放的栈 */
    bts_conn_cancel();
    if (!bts_inited && !bts_feed_created()) {
        bts_state = BT_ST_UNINIT;
        return ESP_OK;
    }

    bts_streaming = false;
    bts_connected = false;
    bts_stream    = NULL;
    bts_state     = BT_ST_UNINIT;
    bts_volume_reset();     /* 协议栈要拆了：AVRCP 绝对音量不可用 → 退回软件缩放 */

    /*
     * ★ 这两行的顺序不能反：**先删 feed 任务，再关编码器**。
     *
     *   bts_audio_close() 释放的正是 feed 任务正在用的东西
     *   （bts_enc / bts_enc_frame / bts_stage / ASRC 缓冲）——
     *   先关编码器的话，feed 任务此刻可能正在 esp_audio_enc_process() 或
     *   memcpy(bts_enc_frame, ...) 里，就是 use-after-free。
     *   vTaskDelete() 返回后该任务不会再被调度，这时再释放才安全。
     */
    bts_feed_stop();        /* 删 feed 任务，再删 PCM 环形缓冲 */
    bts_audio_close();      /* 关 SBC 编码器 / ASRC，释放帧缓冲与暂存 */

    if (bts_inited) {
        esp_bt_audio_deinit();          /* 协议栈 + profiles（内部会 disable/deinit Bluedroid） */
        bts_inited = false;
    }
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "bt deinit (switch off)");
    return ESP_OK;
}
