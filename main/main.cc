/*
 * ESP32-S31-Korvo-1 流媒体音乐播放器 —— 应用入口
 *
 * M0/M1 阶段：打通官方 esp_board_manager + esp_lvgl_adapter 的显示/触摸链路，
 * 由 components/bsp 收口板级初始化。UI 框架在 components/ui_core（下一步接入）。
 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lv_adapter.h"

#include "bsp.h"
#include "ui_core.h"
#include "ui_metrics.h"      /* UI_FONT_TEXT（输入法借用本工程的位图字体） */
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "splash.h"
#include "audio_service.h"
#include "storage_service.h"   /* 本地曲库 + SD 热插拔监控任务的启动点 */
#include "app_music.h"
#include "app_settings.h"
#include "app_online.h"
#include "app_momotalk.h"
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

/*
 * 同理，让链接器把 cxx_heap.cc.obj 从 libmain.a 里拉出来：
 * libgooglepinyin 用 C++ 的 new，而 libstdc++ 那份 operator new 在分配失败时
 * 抛 std::bad_alloc，在 -fno-exceptions 的 ESP-IDF 上等于直接 abort() 重启。
 * cxx_heap.cc 提供不抛异常（返回 NULL）的版本，但它在 libmain.a 里，
 * 而 libgooglepinyin 排在 libmain.a 之后引用它 —— 不主动引用就永远拉不出来。
 */
extern "C" void cxx_heap_anchor(void);
__attribute__((used)) static void (*const s_cxx_heap_anchor)(void) = &cxx_heap_anchor;

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

/*
 * 开机期内存遥测。
 *
 * 为什么必须打：2026-10 启动崩溃（Wi-Fi `init nvs: failed, ret=101` = NO_MEM，
 * 蓝牙控制器 `orca_ems_env_init malloc pool failed` 之后在错误路径里 panic）
 * 根因是**内部 RAM 不够**，但没有数字就只能在改动之间猜。这行日志把
 * "内部 RAM 当前/历史最低 + PSRAM 当前"摊开，下一次任何内存相关的现象都能当场定位。
 *   内部 RAM 是关键：Wi-Fi / 蓝牙的协议栈与控制器的内存池**只能放内部 RAM**
 *   （只有 >16 KB 的普通 malloc 才会落 PSRAM，见 CONFIG_SPIRAM_USE_MALLOC 的
 *   ALWAYSINTERNAL 阈值）。
 */
static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap[%s]: internal free=%u min=%u | psram free=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* 起射频前要求的最低内部 RAM（实测 Wi-Fi 协议栈 + 蓝牙控制器池子要几十 KB） */
#define RADIO_MIN_INTERNAL (32 * 1024)

/* Wi-Fi / 蓝牙后台初始化（设置页会轮询状态，这里失败也不致命）。
 * 两者的总开关状态存在各自服务的 NVS 里：关着时 init 直接返回
 * ESP_ERR_INVALID_STATE 且不做任何初始化（射频/内存都不占）。
 *
 * ★ 这一步结束 = "开机初始化完成"：main() 里的启动图（assets/init.jpg）就是等这个
 *   信号才淡出、露出首页的（见 splash.h）。所以**两个 init 无论成功失败都要走到底**，
 *   最后一行必须调 boot_splash_init_done()，否则启动图要等到 SPLASH_MAX_MS 才消失。
 *
 * ★★ 开跑之前先看内部 RAM（2026-10 加）：射频初始化在内部 RAM 不足时**不是干净地
 *   返回错误，而是会把控制器搞崩** —— 实测日志是
 *     `[E] EMS: r_orca_ems_env_init malloc pool failed` → `[E] CTRL: orca_ems_env_init failed`
 *     → `Guru Meditation ... olc_if_page_scan_deinit / r_olc_deinit`（蓝牙栈在初始化
 *     失败的错误路径里解引用空指针），整机重启。
 *   宁可不启动射频（本地曲库/播放不受影响），也不能让开机变成重启循环。
 */
static void radio_init_task(void *arg)
{
    (void)arg;
    log_heap("radio:before");

    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < RADIO_MIN_INTERNAL) {
        ESP_LOGE(TAG, "内部 RAM 不足（< %u B）：跳过 Wi-Fi / 蓝牙初始化，"
                      "避免射频控制器在错误路径里崩溃", (unsigned)RADIO_MIN_INTERNAL);
        boot_splash_init_done();
        vTaskDelete(NULL);
        return;
    }

    const esp_err_t wret = wifi_service_init();
    if (wret != ESP_OK) {
        ESP_LOGI(TAG, "wifi not started (switch off or init failed)");
    }
    log_heap("radio:wifi");

    /*
     * ★ 2026-10 实测的崩溃顺序就是这两步连着的：
     *     `esp_wifi_init: ESP_ERR_NO_MEM` → `wifi_init: Failed to deinit Wi-Fi`
     *     → `[E] EMS: r_orca_ems_env_init malloc pool failed`
     *     → `Guru Meditation ... r_olc_deinit`（蓝牙栈在初始化失败的错误路径里空指针）
     *   Wi-Fi 初始化失败时它自己的 deinit 也可能失败（上面那条日志），失败的协议栈
     *   未必把内部 RAM 还干净 —— 这时候再拉蓝牙控制器，等于把最后一点内存也赌上去。
     *   所以：**Wi-Fi 因内存失败 / 此刻内部 RAM 已经很低，就不要再试蓝牙**。
     *   代价只是没有蓝牙（本地与在线播放都不依赖它），换来的是不会整机重启。
     */
    if ((wret == ESP_ERR_NO_MEM ||
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < RADIO_MIN_INTERNAL) &&
        bt_service_is_enabled()) {
        ESP_LOGE(TAG, "跳过蓝牙初始化避免控制器崩溃：wifi_ret=%s, internal free=%u B",
                 esp_err_to_name(wret),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        boot_splash_init_done();
        vTaskDelete(NULL);
        return;
    }

    if (bt_service_init() == ESP_OK) {
        bt_service_set_media_ctrl_cb(on_bt_media_ctrl, NULL);
    } else {
        ESP_LOGI(TAG, "bt not started (switch off or init failed)");
    }
    log_heap("radio:bt");

    boot_splash_init_done();          /* 初始化完成 -> 启动图开始淡出 */
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
    log_heap("after bsp_init");      /* 基线：后面每一步吃多少内部 RAM 都有参照 */

    /*
     * 开机启动图：**先把解码任务发出去**（assets/init.jpg，800x480 JPEG 软解成
     * RGB565 要几百毫秒），让它与下面这一串初始化并行跑 —— 等屏幕起来时图通常
     * 已经就绪，于是"屏幕一亮就是这张图"（见 splash.cc）。
     */
    boot_splash_decode_start();
    log_heap("splash decode kicked");

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

    /*
     * 本地曲库服务：这里必须显式初始化一次 —— SD 热插拔监控任务就是在
     * storage_service_init() 里起来的（板子没有卡检测脚，全靠它轮询）。
     * 只在别的入口惰性初始化的话，用户不开本地音乐页就永远没有热插拔：
     * 插拔卡没有 Toast、状态栏图标不动、换了卡也得重启。
     */
    if (storage_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "storage_service_init failed (本地曲库不可用)");
    }

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
    app_momotalk_register_apps();

    /*
     * UI 框架：状态栏 / 控制中心 / 手势 / 首页（全部在 LVGL 任务上下文建）。
     *
     * 建完**在同一个锁里**立刻盖上启动图（assets/init.jpg）+ Toast "正在初始化..."：
     * 两件事之间不能松锁，否则 LVGL 会先渲染一帧首页 —— 用户看到的就是"首页闪一下
     * 再被盖住"。启动图在最上层（状态栏/返回键/首页都在它下面）；Toast 对象是
     * ui_core_init() 建的，而 ui_toast() 靠 lv_obj_move_foreground() 把自己提到启动图
     * 之上，所以"图盖住一切、Toast 盖住图"（顺序反了就看不见 Toast）。
     * 后台初始化（WiFi/蓝牙）报完成时，启动图淡出 + 上滑，露出已经建好的首页。
     */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        esp_err_t ret = ui_core_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ui_core_init failed: %s", esp_err_to_name(ret));
        }
        /*
         * 拼音输入法用**本工程的 UI 位图字体**，不用它自带的生成字体。
         *
         * 自带那两套只覆盖 tools/gen_charset.py 选的字符集（GB2312 + 十来个
         * UI 符号），键盘上的说明文字一旦超出这个集合就是缺字方框 —— 而同样
         * 一个字在别的页面（用 ui_font_text）显示得好好的，看起来就像"输入法
         * 的字库坏了"。ui_font_text 是"GB2312 + 日文汉字补充"的 fallback 链，
         * 覆盖面更宽，而且整机字体一致。
         *
         * 必须在任何 lv_pinyin_ime_create() 之前调用（字体是建控件时读的），
         * 而两个用输入法的页面（在线搜索、MomoTalk）都是用户点进去才建的，
         * 所以放在 ui_core_init 之后、启动图之前最稳妥。
         */
        lv_pinyin_ime_set_fonts(UI_FONT_TEXT, UI_FONT_TEXT);
        boot_splash_show();
        esp_lv_adapter_unlock();
    }

    /* 硬件按键：必须在 ui_core_init() 之后，音量条的 LVGL 定时器才已经建好 */
    if (bsp_button_init(on_board_key, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "bsp_button_init failed (音量按键不可用)");
    }
    /* UI + 启动图 + 两张 800x480 图（PSRAM）之后的内部 RAM 余额 —— 射频启动就靠它 */
    log_heap("after ui+splash");

    /*
     * Wi-Fi / 蓝牙：初始化要几百毫秒（蓝牙更久），放后台任务里做。
     * 这一步**就是"开机初始化完成"的判据**：main/splash.cc 的启动图一直盖在首页上，
     * 等这个任务最后调 boot_splash_init_done() 才淡出（见 splash.h）。
     * 设置页另有自己的"初始化中…"轮询显示，与此无关。
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