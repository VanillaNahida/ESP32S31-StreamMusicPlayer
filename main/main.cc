/*
 * ESP32-S31-Korvo-1 流媒体音乐播放器 —— 应用入口
 *
 * M0/M1 阶段：打通官方 esp_board_manager + esp_lvgl_adapter 的显示/触摸链路，
 * 由 components/bsp 收口板级初始化。UI 框架在 components/ui_core（下一步接入）。
 */
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lv_adapter.h"

#include "bsp.h"
#include "ui_core.h"
#include "audio_service.h"
#include "app_music.h"
#include "app_settings.h"
#include "app_online.h"
#include "wifi_service.h"
#include "bt_service.h"
#include "play_state.h"
#include "online_service.h"

/*
 * 让链接器把 ime_dict_stub.c.obj 从 libmain.a 里拉出来。
 *
 * 静态库只按"当前未定义的符号"收成员，而 liblvgl_pinyin_ime.a 排在 libmain.a
 * **之后**：等轮到它时才发现缺 _binary_dict_pinyin_dat_start，
 * 可 libmain.a 已经处理完了，那个成员永远拉不出来 → undefined reference。
 * 这里主动引用一次（used 防止被优化掉），成员就会被收进来（见 ime_dict_stub.c）。
 */
extern "C" const uint8_t _binary_dict_pinyin_dat_start[1];
__attribute__((used)) static const uint8_t *const s_ime_dict_stub_ref = _binary_dict_pinyin_dat_start;

static const char *TAG = "app_main";

/* ------------------------------------------------------------------ */
/* LVGL 适配器（官方 esp_lvgl_adapter）                                 */
/* 顺序必须为：init → register display/touch → start                    */
/* ------------------------------------------------------------------ */
static esp_err_t lvgl_adapter_init(void)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_priority     = 4;
    cfg.task_stack_size   = 8 * 1024;
    cfg.task_core_id      = 1;
    cfg.tick_period_ms    = 5;
    cfg.task_max_delay_ms = 500;
    return esp_lv_adapter_init(&cfg);
}

/* 蓝牙耳机上的媒体按键（AVRCP Target）→ 播放器。回调在蓝牙栈任务里，
 * app_music 内部只置标志，实际动作在 LVGL 任务里执行 */
static void on_bt_media_ctrl(int cmd, void *user)
{
    (void)user;
    app_music_on_media_ctrl(cmd);
}

/* Wi-Fi / 蓝牙后台初始化（设置页会轮询状态，这里失败也不致命）。
 * 两者的总开关状态存在各自服务的 NVS 里：关着时 init 直接返回
 * ESP_ERR_INVALID_STATE 且不做任何初始化（射频/内存都不占）。 */
static void radio_init_task(void *arg)
{
    (void)arg;
    if (wifi_service_init() != ESP_OK) {
        ESP_LOGI(TAG, "wifi not started (switch off or init failed)");
    }
    if (bt_service_init() == ESP_OK) {
        bt_service_set_media_ctrl_cb(on_bt_media_ctrl, NULL);
    } else {
        ESP_LOGI(TAG, "bt not started (switch off or init failed)");
    }
    vTaskDelete(NULL);
}

/*
 * 板载 ADC 按键回调。
 *
 * 运行在 iot_button 的 esp_timer 周期任务上下文（**不是** LVGL 任务），
 * 所以这里只能做非阻塞的轻动作：ui_volume_key_step() 内部只累加计数，
 * 真正的音量下发 + 右侧音量条弹出由 ui_core 的 LVGL 定时器完成。
 *
 * 按键映射以原理图为准：SW6(380mV)=VOL+，SW5(820mV)=VOL-
 * （esp_boards 的 YAML label 把这两个键写反了，见 bsp_input.cc 文件头）。
 */
static void on_board_key(bsp_key_t key, bsp_key_evt_t evt, void *user)
{
    (void)user;
    if (evt == BSP_KEY_EVT_RELEASE) {
        return;
    }
    switch (key) {
    case BSP_KEY_VOL_UP:
        ui_volume_key_step(+1);
        break;
    case BSP_KEY_VOL_DOWN:
        ui_volume_key_step(-1);
        break;
    default:
        break;   /* MODE / SET 暂未绑定功能 */
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S31 stream music player boot ===");

    ESP_ERROR_CHECK(bsp_init());

    /*
     * 播放状态记忆（音量 / 曲目 / 进度，存 NVS）：必须在 audio_service_init()
     * 之前，音量才能一开机就是上次的值（否则先以默认音量开 codec，
     * 再"跳"一下变到记忆值，听感上就是开机爆一声）。
     * 失败不致命：退化成"不记忆"，UI 与播放照常。
     */
    if (play_state_init() != ESP_OK) {
        ESP_LOGW(TAG, "play_state_init failed (音量/进度不做记忆)");
    }

    /* 板级外设（惰性初始化各自设备，未插卡/无触摸都不致命） */
    bsp_audio_init();
    bsp_sdcard_mount();
    bsp_led_set(0, 0, 12);

    /* 在线音乐：建 /sdcard/online 目录树 + 读回保存的歌单 ID（不需要网络） */
    online_service_init();

    /* 播放服务（ESP-GMF / esp_audio_simple_player） */
    if (audio_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio_service_init failed (无声音，但 UI 继续可用)");
    }

    /* LVGL 适配器先起来，再注册显示与触摸 */
    ESP_ERROR_CHECK(lvgl_adapter_init());

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(bsp_display_register_lvgl(&disp));

    lv_indev_t *indev = NULL;
    bsp_touch_register_lvgl(disp, &indev);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    /* 先注册真实 APP（覆盖 ui_home 的同 id 占位页），再起 UI */
    app_music_register_apps();
    app_settings_register_apps();
    app_online_register_apps();

    /* UI 框架：状态栏 / 控制中心 / 手势 / 首页（全部在 LVGL 任务上下文建） */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        esp_err_t ret = ui_core_init();
        esp_lv_adapter_unlock();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ui_core_init failed: %s", esp_err_to_name(ret));
        }
    }

    /* 硬件按键：必须在 ui_core_init() 之后，音量条的 LVGL 定时器才已经建好 */
    if (bsp_button_init(on_board_key, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "bsp_button_init failed (音量按键不可用)");
    }

    /*
     * Wi-Fi / 蓝牙：初始化要几百毫秒（蓝牙更久），放后台任务里做，
     * 首页照常先出来；设置页轮询就绪状态，期间显示"初始化中…"。
     *
     * ⚠ 这个任务**必须钉在蓝牙协议栈任务（BTC）所在的那个核上**，而且优先级
     *   要低于它（BTC = configMAX_PRIORITIES - 6 ≈ 19，这里只用 3）。
     *
     *   原因（实测过一次"开机无限重启"，别再改回去）：
     *   上游 esp_bt_audio 的经典蓝牙初始化有竞态 ——
     *     bt_audio_a2dp_src_init() 调完 esp_a2d_source_init()（它只是往 BTC
     *     投递一条消息，**不等 BTC 执行完**）就紧接着注册 SBC codec 的 SEP；
     *   而 esp_a2d_source_register_stream_endpoint() 用 btc_av.c 里的
     *   `g_a2dp_on_deinit` 做门禁 —— 这个标志的**初值是 true**，只有 BTC 任务
     *   处理完 BTC_AV_SRC_API_INIT_EVT 才会置成 false。
     *   两块核并行跑时我们这条线程一定先跑到 SEP 注册，于是拿到
     *   ESP_ERR_INVALID_STATE，被组件里的 ESP_ERROR_CHECK 变成 abort：
     *     ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
     *     func: bt_audio_a2dp_src_init  expression: a2dp_src_register_sbc_sep(seid)
     *   钉到同一个核之后，btc_transfer_context() 投递消息会立刻抢占本任务
     *   （同核高优先级唤醒 → 立即切换），BTC 把 INIT 事件整条处理完才轮到我们
     *   继续往下走，竞态窗口就是 0。
     */
    xTaskCreatePinnedToCore(radio_init_task, "radio_init", 10 * 1024, NULL, 3, NULL,
                            CONFIG_BT_BLUEDROID_PINNED_TO_CORE);

    ESP_LOGI(TAG, "M1 bring-up done");
}