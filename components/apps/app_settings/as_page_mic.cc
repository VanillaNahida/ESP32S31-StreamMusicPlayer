/*
 * as_page_mic.cc —— 麦克风测试二级页
 *
 * 需求（用户原话）：
 *   "设置页添加一个麦克风测试（模拟输入法语音输入现状），录制 8 秒音频到 PSRAM
 *    然后播放测试"
 *
 * 这个页面存在的理由，是语音识别一直"识别为空 / 漏字"，而根因是**麦克风电平太低**：
 * 真机日志里 peak 只有 93~106（约 -50…-29 dBFS），而正常说话是 -20…-6 dBFS。
 * 电平低到服务端的 VAD 都不触发，于是：
 *   - 识别为空   ← 一整段被当成静音（服务端"正常"回一个空结果，组件报 err=0）
 *   - 识别漏字   ← 只有偶尔的峰值越过阈值
 *
 * 所以这一页做两件事，都是为了让"输入状况"可见、可听：
 *   1. **实时电平条**（绿色 ≥ -20 dBFS 正常 / 黄色 -40…-20 偏小 / 灰色静音）
 *   2. **录 8 秒 → 回放**：录的是**和识别完全相同**的 16 kHz 单声道 PCM
 *      （走组件的 lv_pinyin_ime_voice_monitor()，与识别共用同一条 audio_read 回调、
 *        同一个 40 ms 节拍），所以听到的就是讯飞会听到的。回放走 audio_service
 *      的常规播放通道（写一个临时 WAV 到 SD 卡再播），不做裸 PCM 注入。
 *
 * 线程纪律（与整个 app_settings 一致）：
 *   - 组件的 monitor 跑在它自己的任务里；本页只在 LVGL 定时器里读电平（加锁读）。
 *   - 写 WAV / 播放在 LVGL 任务上下文里同步做（8 秒 256 KB，SD 卡写入是毫秒级）。
 */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_service.h"
#include "ui_core.h"
#include "ui_metrics.h"
#include "voice_service.h"

#include "as_page_shell.h"
#include "as_widgets.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

static const char *TAG = "app_settings";

/* ============================================================ 参数 */

/* 8 秒 @ 16 kHz 单声道 16bit = 8 × 16000 × 2 = 256000 字节 */
#define MIC_TEST_SECONDS   8
#define MIC_TEST_RATE      16000
#define MIC_TEST_CAPACITY  (MIC_TEST_SECONDS * MIC_TEST_RATE * 2)

/* 录好的音频落到 SD 卡，再交给 audio_service 常规播放（它认文件，不认裸 PCM） */
#define MIC_TEST_WAV_PATH  "/sdcard/mic_test.wav"

/* 电平条范围：与组件里的判据一致（-20 正常下限，-40 静音上限） */
#define MIC_DB_MIN  (-60)
#define MIC_DB_MAX  (0)

#define MIC_TICK_MS 50      /* 电平条刷新周期：50 ms 已经比人眼需要的快 */

/* ============================================================ 状态 */

static lv_obj_t *s_body;         /* 页面 body（离开时置 NULL，定时器据此退出） */
static lv_obj_t *s_level_bar;
static lv_obj_t *s_level_text;   /* "峰值 -29 dBFS（偏小）" */
static lv_obj_t *s_status_text;  /* 录音状态 / 进度 */
static lv_obj_t *s_btn_rec;      /* 大按钮：开始 / 停止录音 */
static lv_obj_t *s_btn_play;     /* 回放上一次录音 */
static lv_timer_t *s_timer;

static int16_t  *s_cap;          /* PSRAM 里的录音缓冲 */
static bool      s_recording;    /* 录音进行中 */
static bool      s_have_take;    /* 已有可回放的录音 */

/* ============================================================ 小工具 */

/**
 * 写一个 16 bit 单声道 WAV（44 字节标准头 + PCM）。
 *
 * 为什么不自己往 codec 灌 PCM：audio_service 的对外接口只认文件/URI
 * （见 audio_service.h，没有裸 PCM 入口），而写临时 WAV 还能顺带留下一个可以用
 * 电脑播放、拿去分析的文件 —— 排查"到底录到了什么"时这个文件比任何日志都有用。
 */
static bool mic_write_wav(const char *path, const int16_t *pcm, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打开 %s 失败（SD 卡挂了吗？）", path);
        return false;
    }

    const uint32_t data_len = (uint32_t)bytes;
    const uint32_t byte_rate = MIC_TEST_RATE * 2;      /* 单声道 16bit */
    uint8_t h[44];

    memcpy(h + 0, "RIFF", 4);
    const uint32_t riff_len = 36u + data_len;
    memcpy(h + 4, &riff_len, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16u;
    memcpy(h + 16, &fmt_len, 4);
    const uint16_t audio_fmt = 1u;                     /* PCM */
    memcpy(h + 20, &audio_fmt, 2);
    const uint16_t channels = 1u;
    memcpy(h + 22, &channels, 2);
    const uint32_t rate = MIC_TEST_RATE;
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    const uint16_t block_align = 2u;
    memcpy(h + 32, &block_align, 2);
    const uint16_t bits = 16u;
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_len, 4);

    const bool ok = (fwrite(h, 1, sizeof(h), f) == sizeof(h)) &&
                    (fwrite(pcm, 1, bytes, f) == bytes);
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "写 %s 失败", path);
    }
    return ok;
}

/** 电平 → 人话（和组件日志里那套判据保持一致，免得同一件事两种说法）。 */
static const char *mic_level_verdict(int db)
{
    if (db >= -20) {
        return "正常";
    }
    if (db >= -40) {
        return "偏小（识别会掉字）";
    }
    return "静音（认不出来）";
}

static uint32_t mic_level_color(int db)
{
    if (db >= -20) {
        return 0x3E9E4A;    /* 绿 */
    }
    if (db >= -40) {
        return 0xC8A03C;    /* 黄 */
    }
    return 0x6E6E78;        /* 灰 */
}

/** 把电平条/文字刷成当前状态。 */
static void mic_update_level(void)
{
    if (s_level_bar == NULL) {
        return;
    }

    lv_obj_t *ime = voice_service_ime();
    lv_pinyin_ime_voice_level_t lv;
    if (ime == NULL || !lv_pinyin_ime_voice_get_level(ime, &lv)) {
        lv_bar_set_value(s_level_bar, MIC_DB_MIN, LV_ANIM_OFF);
        as_label_set_if_changed(s_level_text, "等待麦克风数据…");
        return;
    }

    int db = lv.peak_dbfs;
    if (db < MIC_DB_MIN) {
        db = MIC_DB_MIN;
    } else if (db > MIC_DB_MAX) {
        db = MIC_DB_MAX;
    }
    lv_bar_set_value(s_level_bar, db, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(mic_level_color(lv.peak_dbfs)),
                              LV_PART_INDICATOR);

    char buf[96];
    snprintf(buf, sizeof(buf), "峰值 %d dBFS · %s", lv.peak_dbfs,
             mic_level_verdict(lv.peak_dbfs));
    as_label_set_if_changed(s_level_text, buf);
}

/** 定时器：录音中刷状态，平时只刷电平。 */
static void mic_tick(lv_timer_t *t)
{
    (void)t;
    if (s_body == NULL) {
        /* 页面已经离开：停掉录音、删自身（见 mic_on_delete） */
        return;
    }

    mic_update_level();

    if (s_recording) {
        lv_obj_t *ime = voice_service_ime();
        const size_t got = (ime != NULL) ? lv_pinyin_ime_voice_captured_bytes(ime) : 0;
        const unsigned done_ms = (unsigned)((got / 2u) * 1000u / MIC_TEST_RATE);
        char buf[80];
        snprintf(buf, sizeof(buf), "录音中… %u.%us / %ds",
                 done_ms / 1000u, (done_ms / 100u) % 10u, MIC_TEST_SECONDS);
        as_label_set_if_changed(s_status_text, buf);

        /* 录满（或组件已收尾）就切回"可回放"状态 */
        if (got >= MIC_TEST_CAPACITY ||
            lv_pinyin_ime_voice_state(voice_service_voice()) == LV_PINYIN_IME_VOICE_DONE) {
            s_recording = false;
            s_have_take = (got > 0);
            as_label_set_if_changed(s_status_text,
                                    s_have_take ? "录音完成，可以回放了"
                                                : "没有录到数据（检查麦克风）");
            if (s_btn_play != NULL) {
                if (s_have_take) {
                    lv_obj_remove_state(s_btn_play, LV_STATE_DISABLED);
                } else {
                    lv_obj_add_state(s_btn_play, LV_STATE_DISABLED);
                }
            }
        }
    }
}

/* ============================================================ 按钮 */

static void mic_on_rec(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ime = voice_service_ime();
    if (ime == NULL) {
        ui_toast("键盘还没接上语音输入", 2000);
        return;
    }

    if (s_recording) {
        /* 提前收工：录到多少算多少 */
        lv_pinyin_ime_voice_stop(voice_service_voice());
        s_recording = false;
        const size_t got = lv_pinyin_ime_voice_captured_bytes(ime);
        s_have_take = (got > 0);
        as_label_set_if_changed(s_status_text, s_have_take ? "已停止，可以回放" : "没录到数据");
        if (s_btn_play != NULL) {
            if (s_have_take) {
                lv_obj_remove_state(s_btn_play, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(s_btn_play, LV_STATE_DISABLED);
            }
        }
        return;
    }

    if (s_cap == NULL) {
        /* 录 8 秒要 256 KB，放内部 RAM 太奢侈 —— 一律分配在 PSRAM */
        s_cap = (int16_t *)heap_caps_malloc(MIC_TEST_CAPACITY, MALLOC_CAP_SPIRAM);
        if (s_cap == NULL) {
            ESP_LOGE(TAG, "PSRAM 分配 %d 字节失败", MIC_TEST_CAPACITY);
            ui_toast("PSRAM 不够，无法录音", 2500);
            return;
        }
    }

    /*
     * 关键：这里走的是组件的**监听模式**（不连网、不识别、不花额度），而它和真实
     * 识别共用同一条 audio_read 回调与同一个 40 ms 节拍 —— 所以量到的电平、
     * 录到的 PCM，就是识别时讯飞会听到的东西。这正是"模拟输入法语音输入现状"。
     */
    lv_pinyin_ime_voice_reset_level(ime);
    s_have_take = false;
    if (s_btn_play != NULL) {
        lv_obj_add_state(s_btn_play, LV_STATE_DISABLED);
    }

    const int rc = lv_pinyin_ime_voice_monitor(ime, s_cap, MIC_TEST_CAPACITY);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动监听失败: %d", rc);
        ui_toast("麦克风打开失败", 2500);
        return;
    }
    s_recording = true;
    as_label_set_if_changed(s_status_text, "录音中… 对着麦克风正常说话");
    ESP_LOGI(TAG, "mic test: 开始录 %d 秒", MIC_TEST_SECONDS);
}

static void mic_on_play(lv_event_t *e)
{
    (void)e;
    if (!s_have_take || s_cap == NULL) {
        return;
    }
    lv_obj_t *ime = voice_service_ime();
    const size_t got = (ime != NULL) ? lv_pinyin_ime_voice_captured_bytes(ime) : 0;
    if (got == 0) {
        ui_toast("没有录音可回放", 2000);
        return;
    }

    if (!mic_write_wav(MIC_TEST_WAV_PATH, s_cap, got)) {
        ui_toast("写 WAV 失败（SD 卡？）", 2500);
        return;
    }

    /*
     * 用常规播放通道回放：audio_service 会解码 → 重采样到 48k → ES8389。
     * 8 秒的 16k 单声道 WAV，GMF 直接认。
     */
    if (audio_service_play_path(MIC_TEST_WAV_PATH) != ESP_OK) {
        ui_toast("回放失败", 2000);
        return;
    }
    as_label_set_if_changed(s_status_text, "正在回放刚才录到的声音…");
    ESP_LOGI(TAG, "mic test: 回放 %u 字节（%s）", (unsigned)got, MIC_TEST_WAV_PATH);
}

static void mic_on_delete(lv_event_t *e)
{
    (void)e;
    /* 页面销毁：停掉可能还在跑的监听，拆掉定时器，别让回调碰已释放的控件 */
    lv_obj_t *ime = voice_service_ime();
    if (ime != NULL && (s_recording ||
                        lv_pinyin_ime_voice_state(voice_service_voice()) ==
                            LV_PINYIN_IME_VOICE_LISTENING)) {
        lv_pinyin_ime_voice_cancel(voice_service_voice());
    }
    s_recording = false;
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_body = NULL;
    s_level_bar = NULL;
    s_level_text = NULL;
    s_status_text = NULL;
    s_btn_rec = NULL;
    s_btn_play = NULL;
    /*
     * s_cap（256 KB PSRAM）**故意不释放**：这一页会被反复进出，每次重新 malloc 既慢
     * 又容易在碎片化之后失败；留一份常驻比每次申请更稳。
     */
}

/* ============================================================ 构页 */

lv_obj_t *page_mic(void)
{
    lv_obj_t *scr = as_make_screen();
    s_body = as_page_shell(scr, "麦克风测试", NULL, NULL);

    if (!voice_service_available()) {
        as_make_row(s_body, LV_SYMBOL_WARNING, 0xC0584B, "语音模块没编进来", NULL);
        return scr;
    }
    if (voice_service_ime() == NULL) {
        as_make_row(s_body, LV_SYMBOL_WARNING, 0xC0584B, "键盘还没接上语音输入", NULL);
        return scr;
    }

    /* --- 说明：这一页量到的就是识别时服务端会听到的 --- */
    lv_obj_t *hint = lv_label_create(s_body);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_text(hint,
                      "这一页不连网、不识别，只读麦克风 —— 用的是和语音输入完全相同\n"
                      "的采集路径，所以这里看到的电平就是讯飞会听到的电平。\n"
                      "正常说话应该是 -20 ~ -6 dBFS；低于 -40 基本等于静音，\n"
                      "那正是「识别为空」的原因。");
    lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(hint, 8, LV_PART_MAIN);

    /* --- 电平条 --- */
    lv_obj_t *card = lv_obj_create(s_body);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_level_bar = lv_bar_create(card);
    lv_obj_set_width(s_level_bar, LV_PCT(100));
    lv_obj_set_height(s_level_bar, 14);
    lv_bar_set_range(s_level_bar, MIC_DB_MIN, MIC_DB_MAX);
    lv_bar_set_value(s_level_bar, MIC_DB_MIN, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_level_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_level_bar, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x6E6E78), LV_PART_INDICATOR);

    s_level_text = lv_label_create(card);
    lv_label_set_text(s_level_text, "等待麦克风数据…");
    lv_obj_set_style_text_color(s_level_text, UI_COLOR_FG, LV_PART_MAIN);

    /* --- 录音/回放 --- */
    s_status_text = lv_label_create(s_body);
    lv_label_set_text(s_status_text, "点下面开始录 8 秒");
    lv_obj_set_style_text_color(s_status_text, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_status_text, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_status_text, 4, LV_PART_MAIN);

    s_btn_rec = as_make_flat_btn(s_body, "开始录 8 秒", mic_on_rec, NULL);
    lv_obj_set_width(s_btn_rec, LV_PCT(100));
    lv_obj_set_height(s_btn_rec, 48);
    lv_obj_set_style_bg_color(s_btn_rec, UI_COLOR_ACCENT, LV_PART_MAIN);

    s_btn_play = as_make_flat_btn(s_body, "回放刚才的录音", mic_on_play, NULL);
    lv_obj_set_width(s_btn_play, LV_PCT(100));
    lv_obj_set_height(s_btn_play, 48);
    if (!s_have_take) {
        lv_obj_add_state(s_btn_play, LV_STATE_DISABLED);
    }

    lv_obj_add_event_cb(scr, mic_on_delete, LV_EVENT_DELETE, NULL);
    s_timer = lv_timer_create(mic_tick, MIC_TICK_MS, NULL);
    mic_update_level();

    ESP_LOGI(TAG, "mic test page created");
    return scr;
}

/* 子页描述符（静态存储期：router 直接存 id/create 指针） */
const subpage_desc_t *as_sub_mic(void)
{
    static const subpage_desc_t SUB_MIC = { page_mic, "set_mic" };
    return &SUB_MIC;
}
