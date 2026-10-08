/*
 * bsp_input.cc —— WS2812 RGB 状态灯 + ADC 分压按键
 *
 * 按键是 GPIO42 上的电阻分压（ADC1 ch0），四档：VOL+ / VOL- / MODE / SET。
 * 采样、去抖、长按计时全部交给板级 adc_button_group 设备（esp_board_manager
 * 内部用官方 iot_button 组件），本文件只做"按键下标 → 逻辑键"的翻译与回调
 * 分发。**不自己写 adc_oneshot**：S31 上手动配 attenuation 会直接 abort
 * （见 skills/工程规范/11_遇到的问题.md F1）。
 *
 * ⚠ 档位与键名的对应关系以**原理图**为准：
 *   原理图 ESP32-S31-Korvo-1 "KEY Array"（VCC_3V3 → R77 10K → 按键 → Rn → GND）：
 *     SW6  R82 1.3K → 3.3*1.3/(10+1.3)  = 0.38 V → VOL+
 *     SW5  R81 3.3K → 3.3*3.3/(10+3.3)  = 0.82 V → VOL-
 *     SW4  R80 6.8K → 3.3*6.8/(10+6.8)  = 1.34 V → MODE
 *     SW3  R79 13K  → 3.3*13/(10+13)    = 1.87 V → SET
 *   而 esp_boards 的 board_devices.yaml 把 label 按电压升序写成了
 *   ["SET","MODE","VOLUME_DOWN","VOLUME_UP"]，即 380mV 当成 SET、
 *   1870mV 当成 VOLUME_UP —— 与原理图正好把 SET/VOL+、MODE/VOL- 弄反。
 *   所以这里**不按 YAML 的 label 反查**，而是按下标映射（下标 = 电压升序）：
 *     0 = VOL+，1 = VOL-，2 = MODE，3 = SET。
 *   若哪天实测发现手上的板子相反，只改下面 k_key_map 一处即可。
 */
#include "bsp.h"
#include "bsp_internal.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "iot_button.h"
#include "led_strip.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "bsp_input";

#define LED_STRIP_DEV   ESP_BOARD_DEVICE_NAME_LED_STRIP       /* "led_strip" */
#define ADC_BUTTON_DEV  ESP_BOARD_DEVICE_NAME_ADC_BUTTON_GROUP /* "adc_button_group" */

/*
 * 长按判定：板级 YAML 给的是 2000 ms，按音量键要等 2 秒才开始连续调整太迟钝，
 * 这里覆盖成 600 ms（"按一下一格、按住不放连续调"的手感）。
 */
#define KEY_LONG_MS     (600)

/*
 * 长按连续触发：iot_button 的 BUTTON_LONG_PRESS_HOLD 由
 * CONFIG_BUTTON_LONG_PRESS_HOLD_SERIAL_TIME_MS(=20 ms) 的节拍驱动，
 * 直接用等于 50 格/秒，太快了；每 6 次才下发一步 ≈ 8 格/秒。
 */
#define KEY_HOLD_STEP   (6)

/* ---------------------------------------------------------------- RGB LED */

static led_strip_handle_t s_strip;

static esp_err_t led_ensure(void)
{
    if (s_strip) {
        return ESP_OK;
    }
    esp_err_t ret = bsp_device_ensure(LED_STRIP_DEV);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "init %s failed: %s", LED_STRIP_DEV, esp_err_to_name(ret));
        return ret;
    }
    void *handle = NULL;
    ret = esp_board_manager_get_device_handle(LED_STRIP_DEV, &handle);
    if (ret != ESP_OK || !handle) {
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }
    s_strip = ((dev_led_strip_handles_t *)handle)->strip_handle;
    return s_strip ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t ret = led_ensure();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = led_strip_set_pixel(s_strip, 0, r, g, b);
    if (ret == ESP_OK) {
        ret = led_strip_refresh(s_strip);
    }
    return ret;
}

esp_err_t bsp_led_off(void)
{
    esp_err_t ret = led_ensure();
    if (ret != ESP_OK) {
        return ret;
    }
    return led_strip_clear(s_strip);
}

/* ---------------------------------------------------------------- ADC 按键 */

/* 下标 = ADC 电压升序（驱动 dev_button_sub_adc_multi.c 就是按 voltage_range 升序建句柄的）。
 * 这张表只在**认不出 YAML label** 时兜底，正常路径走 key_from_label()。 */
static const bsp_key_t k_key_map[] = {
    BSP_KEY_VOL_UP,
    BSP_KEY_VOL_DOWN,
    BSP_KEY_MODE,
    BSP_KEY_SET,
};

/*
 * ★ 逻辑键**以板级 YAML 的 `button_labels` 为准**，不再"按下标猜"。
 *
 * 为什么改（2026-10，用户报"VOL_DOWN/VOL_UP 被当成 MODE/SET"）：
 * 原来那张 k_key_map 的依据是文件头注释里那份 label 列表
 * （["SET","MODE","VOLUME_DOWN","VOLUME_UP"]）—— 那是**旧版 esp_boards** 的写法。
 * 现在 managed_components/espressif__esp_boards/esp32_s31_korvo_1/board_devices.yaml 是：
 *     voltage_range: [380, 820, 1180, 1570, 1980, 2410]
 *     button_labels: ["VOLUME_UP","VOLUME_DOWN","SET","PLAY","Mode","REC"]
 * 前提变了，手写的下标表自然错位：下标 2 现在是 SET（代码却叫它 MODE）、
 * 下标 3 是 PLAY（代码叫它 SET），4/5 还会被丢掉（表里只有 4 项）。
 * 所以这里直接读 YAML 的 label 来认键；认不出的（PLAY/REC…）按下标兜底。
 */
static bsp_key_t key_from_label(const char *label, uint8_t idx)
{
    if (label) {
        if (strcasecmp(label, "VOLUME_UP") == 0 || strcasecmp(label, "VOL_UP") == 0) {
            return BSP_KEY_VOL_UP;
        }
        if (strcasecmp(label, "VOLUME_DOWN") == 0 || strcasecmp(label, "VOL_DOWN") == 0) {
            return BSP_KEY_VOL_DOWN;
        }
        if (strcasecmp(label, "SET") == 0) {
            return BSP_KEY_SET;
        }
        if (strcasecmp(label, "MODE") == 0) {
            return BSP_KEY_MODE;
        }
    }
    const size_t n = sizeof(k_key_map) / sizeof(k_key_map[0]);
    return (idx < n) ? k_key_map[idx] : BSP_KEY_MAX;
}

static bsp_key_cb_t s_key_cb;
static void        *s_key_user;
static uint8_t      s_hold_cnt[BSP_KEY_MAX];
static bool         s_key_ready;

/* 只用于日志：现场点一下就能确认"哪个物理键被认成了哪个逻辑键" */
static const char *key_name(bsp_key_t key)
{
    switch (key) {
    case BSP_KEY_VOL_UP:   return "VOL+";
    case BSP_KEY_VOL_DOWN: return "VOL-";
    case BSP_KEY_MODE:     return "MODE";
    default:               return "SET";
    }
}

static void key_event_cb(void *btn, void *usr)
{
    bsp_key_t key = (bsp_key_t)(intptr_t)usr;
    if (!s_key_cb || key >= BSP_KEY_MAX) {
        return;
    }

    switch (iot_button_get_event((button_handle_t)btn)) {
    case BUTTON_PRESS_DOWN:
        s_hold_cnt[key] = 0;
        /* 按下立刻响应：按一下 = 一格 */
        ESP_LOGI(TAG, "%s down", key_name(key));
        s_key_cb(key, BSP_KEY_EVT_PRESS, s_key_user);
        break;
    case BUTTON_LONG_PRESS_HOLD:
        if (++s_hold_cnt[key] >= KEY_HOLD_STEP) {
            s_hold_cnt[key] = 0;
            ESP_LOGD(TAG, "%s hold", key_name(key));
            s_key_cb(key, BSP_KEY_EVT_REPEAT, s_key_user);
        }
        break;
    case BUTTON_PRESS_UP:
        s_hold_cnt[key] = 0;
        ESP_LOGD(TAG, "%s up", key_name(key));
        s_key_cb(key, BSP_KEY_EVT_RELEASE, s_key_user);
        break;
    default:
        break;
    }
}

esp_err_t bsp_button_init(bsp_key_cb_t cb, void *user)
{
    if (s_key_ready) {
        return ESP_OK;
    }
    if (!esp_board_manager_check_name(ADC_BUTTON_DEV)) {
        ESP_LOGW(TAG, "board has no %s device", ADC_BUTTON_DEV);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = bsp_device_ensure(ADC_BUTTON_DEV);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init %s failed: %s", ADC_BUTTON_DEV, esp_err_to_name(ret));
        return ret;
    }

    void *handle = NULL;
    ret = esp_board_manager_get_device_handle(ADC_BUTTON_DEV, &handle);
    if (ret != ESP_OK || !handle) {
        ESP_LOGE(TAG, "get %s handle failed: %s", ADC_BUTTON_DEV, esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    /* 先接上回调再注册：注册期间 iot_button 的 esp_timer 任务已经可能在派发事件，
     * 反过来的话这几个微秒里的事件会被丢掉 */
    s_key_cb    = cb;
    s_key_user  = user;

    dev_button_handles_t *btns = (dev_button_handles_t *)handle;

    /* 把 YAML 里的档位/名字打出来：这是"哪个物理键 = 哪个逻辑键"的唯一权威依据，
     * 和丝印对不上时一眼就能看出是板级 YAML 的问题还是代码的问题 */
    dev_button_config_t *bcfg = NULL;
    void *cfg_raw = NULL;
    if (esp_board_manager_get_device_config(ADC_BUTTON_DEV, &cfg_raw) == ESP_OK && cfg_raw) {
        bcfg = (dev_button_config_t *)cfg_raw;
    }

    size_t map_n = sizeof(k_key_map) / sizeof(k_key_map[0]);
    for (uint8_t i = 0; i < btns->num_buttons; i++) {
        const char  *label  = bcfg ? bcfg->sub_cfg.adc.multi.button_labels[i] : NULL;
        const uint16_t v_mv = bcfg ? bcfg->sub_cfg.adc.multi.voltage_range[i] : 0;
        const bsp_key_t key = key_from_label(label, i);
        if (key >= BSP_KEY_MAX) {
            ESP_LOGW(TAG, "btn %u (%u mV, label='%s') 没有对应功能，跳过", i, v_mv,
                     label ? label : "?");
            continue;
        }
        ESP_LOGI(TAG, "btn %u: %u mV label='%s' -> %s", i, v_mv, label ? label : "?",
                 key_name(key));

        button_handle_t h = btns->button_handles[i];
        if (!h) {
            continue;
        }
        /* 覆盖 YAML 的 2000 ms 长按判定（见 KEY_LONG_MS 说明） */
        iot_button_set_param(h, BUTTON_LONG_PRESS_TIME_MS, (void *)(intptr_t)KEY_LONG_MS);
        s_hold_cnt[key] = 0;
        /* 注册自己的回调而不是 esp_board_device_callback_register：
         * 后者只注册 YAML 里 events_cfg 打开的几档事件（不含 LONG_PRESS_HOLD），
         * 而"长按连续调整"正需要 HOLD。 */
        ret = iot_button_register_cb(h, BUTTON_PRESS_DOWN, NULL, key_event_cb,
                                     (void *)(intptr_t)key);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register PRESS_DOWN for key %d failed: %s", i, esp_err_to_name(ret));
            return ret;
        }
        iot_button_register_cb(h, BUTTON_LONG_PRESS_HOLD, NULL, key_event_cb,
                               (void *)(intptr_t)key);
        iot_button_register_cb(h, BUTTON_PRESS_UP, NULL, key_event_cb,
                               (void *)(intptr_t)key);
    }
    (void)map_n;

    s_key_ready = true;
    ESP_LOGI(TAG, "adc buttons ready (%u keys, long=%dms, hold_step=%d)",
             (unsigned)btns->num_buttons, KEY_LONG_MS, KEY_HOLD_STEP);
    return ESP_OK;
}
