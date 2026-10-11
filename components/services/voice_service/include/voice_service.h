/*
 * voice_service.h —— 语音输入的宿主层（网络 + 麦克风 + 自动暂停播放）
 *
 * lvgl_pinyin_ime 的语音模块只做讯飞协议，网络与麦克风由应用提供（见
 * lv_pinyin_ime_voice.h 开头）。这一层就是那个"应用侧"：
 *
 *   - WebSocket：esp_websocket_client 实现 ws_open / ws_send_text / ws_close
 *   - 麦克风   ：板级 audio_adc 器件（ES8389 ADC，独立 I2S in）按 16 kHz/16bit/单声道读
 *   - 识别期间自动暂停播放、结束自动恢复
 *
 * 用法（两处调用）：
 *
 *     voice_service_init();                 // app_main 里一次
 *     voice_service_attach_ime(ime);        // 每把要支持语音的键盘建好后各调一次
 *
 * 密钥来自 Kconfig（menuconfig → Component config → lvgl_pinyin_ime → Voice input），
 * 没配的话 init() 会在日志里明确提示，按语音键会得到"未配置"的提示。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

/*
 * 组件的公开语音头。voice_service_voice() 要返回 lv_pinyin_ime_voice_t*，
 * 所以这里必须带上它 —— 少了这一行，下面那个声明会在**每个**包含本头的组件里
 * 报 "'lv_pinyin_ime_voice_t' does not name a type"（实测连 app_settings 之外
 * 三个组件一起挂）。
 */
#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 语音功能是否编进来了（CONFIG_LV_PINYIN_IME_VOICE_ENABLE）。 */
bool voice_service_available(void);

/** 建信号量、检查密钥；不连网、不开麦（那些在真正开始识别时按需做）。幂等。 */
esp_err_t voice_service_init(void);

/** 把语音能力接到这把键盘上（幂等；同一把键盘重复调不会建第二个会话）。 */
void voice_service_attach_ime(lv_obj_t *ime);

/**
 * 取最近一次 attach 的那把键盘（没接过返回 NULL）。
 *
 * 给"设置 → 麦克风测试"用：那一页的电平条与 8 秒录音要通过
 * lv_pinyin_ime_voice_get_level() / lv_pinyin_ime_voice_monitor() 操作组件，
 * 而这两个 API 以**键盘对象**为入口（和键盘上的语音键同一个入口）。
 */
lv_obj_t *voice_service_ime(void);

/**
 * 取当前的语音会话句柄（没接过返回 NULL）。
 *
 * ★ 注意区分两者：**面向会话**的 API（state / stop / cancel / error …）要的是这个
 *   句柄，而**面向键盘**的 API（attach / get_level / monitor / key_pressed …）要的是
 *   上面的 lv_obj_t*。混用会在编译期报 "cannot convert lv_obj_t* to
 *   lv_pinyin_ime_voice_t*" —— 那一页第一版就是这么挂的。
 */
lv_pinyin_ime_voice_t *voice_service_voice(void);

#ifdef __cplusplus
}
#endif
