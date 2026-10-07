/*
 * app_online.cc —— 在线音乐（网易云歌单 / 搜索 / 播放）
 *
 * 需求对应：
 *   - 添加网易云歌单 ID → 抓取歌单 → **二进制存到 SD**（都在 online_service 里）
 *   - 播放歌单里的在线歌曲：**先下载到 SD 再交给 app_music 播放**，
 *     于是封面 / 歌词 / 进度 / 拖动定位全部复用本地播放那一套
 *   - 中文拼音输入法搜索：搜索浮层用 lvgl_pinyin_ime 键盘，
 *     关键词走服务端的 type=search（关键词放 id=，内部做百分号编码，
 *     支持中文），没网时退化成扫本地已保存歌单
 *   - 歌单/搜索的结果都交给 app_music 当"当前队列"，上下曲一起工作
 *
 * 线程模型（照 app_music 的老规矩）：
 *   LVGL 任务只做 UI；HTTP（抓歌单/搜索/下载）全在 online_work 任务里做，
 *   结果写进一组静态槽位 + 一个"完成"标志，由 200 ms 的 LVGL 定时器搬到界面上。
 */
#include "app_online.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_app_registry.h"

#include "app_music.h"
#include "audio_service.h"
#include "online_service.h"
#include "wifi_service.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

static const char *TAG = "app_online";

#define WORK_TASK_STACK   (8 * 1024)   /* HTTP + TLS + JSON 都在这里 */
#define POLL_MS           200
#define SEARCH_MAX        200          /* 搜索结果上限 */

/*
 * 在线链路自检：置 1 编译后，开机会自动跑一遍
 *   抓歌单 → 二进制落盘 → 读回 → 下载第一首音频 → 抓封面
 * 跑完把歌单从 NVS/SD 清掉（音频缓存留着），日志里打 SELFTEST。
 * 用来在没有 UI 交互的情况下验证 HTTP/重定向/磁盘/解码这条链，
 * 平时保持 0。
 */
#define ONLINE_SELFTEST   0

#if ONLINE_SELFTEST
static void online_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(12000));      /* 等 Wi-Fi 连上 + SNTP 校时 */
    ESP_LOGI(TAG, "SELFTEST begin");
    esp_err_t err = online_playlist_add("24381616");
    ESP_LOGI(TAG, "SELFTEST add: %s (%s)", esp_err_to_name(err),
             online_last_error() ? online_last_error() : "-");
    online_song_t *songs = NULL;
    size_t n = 0;
    err = online_playlist_load("24381616", &songs, &n);
    ESP_LOGI(TAG, "SELFTEST load: %s count=%u", esp_err_to_name(err), (unsigned)n);
    if (err == ESP_OK && songs && n > 0) {
        char path[256] = {0};
        err = online_song_prepare(&songs[0], path, sizeof(path));
        ESP_LOGI(TAG, "SELFTEST prepare '%s' -> %s, path=%s (%s)",
                 songs[0].title, esp_err_to_name(err), path,
                 online_last_error() ? online_last_error() : "-");
        char jpg[192] = {0};
        ESP_LOGI(TAG, "SELFTEST cover: %s",
                 esp_err_to_name(online_song_cover(&songs[0], jpg, sizeof(jpg))));
        char lrc[192] = {0};
        ESP_LOGI(TAG, "SELFTEST lyrics: %s",
                 esp_err_to_name(online_song_lyrics(&songs[0], lrc, sizeof(lrc))));

        /* 流式：解析最终地址 + 真的起播（验证 GMF 的 HTTP reader IO + TLS 这条链） */
        char stream[560] = {0};
        err = online_song_stream_url(&songs[0], stream, sizeof(stream));
        ESP_LOGI(TAG, "SELFTEST stream url: %s (%s)", esp_err_to_name(err),
                 online_last_error() ? online_last_error() : "-");
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SELFTEST play_uri: %s",
                     esp_err_to_name(audio_service_play_uri(stream)));
            vTaskDelay(pdMS_TO_TICKS(10000));   /* 放 10 秒，看是否真的在出声/推进 */
            audio_info_t info = {};
            const bool has_info = audio_service_get_info(&info);
            ESP_LOGI(TAG, "SELFTEST playing=%d info=%d rate=%d br=%d dur=%u",
                     (int)audio_service_is_playing(), (int)has_info,
                     info.sample_rate, info.bitrate, (unsigned)info.duration_ms);
            audio_service_stop();
        }
        free(songs);
    }
    online_playlist_remove("24381616");    /* 别把测试歌单留在用户列表里 */
    ESP_LOGI(TAG, "SELFTEST done");
    vTaskDelete(NULL);
}
#endif

/* ---------------------------------------------------------------- 工作线程 */

typedef enum {
    OREQ_NONE = 0,
    OREQ_ADD,        /* arg = 歌单 ID */
    OREQ_LOAD,       /* arg = 歌单 ID */
    OREQ_SEARCH,     /* arg = 关键词 */
    OREQ_REMOVE,     /* arg = 歌单 ID */
} oreq_kind_t;

typedef struct {
    int  kind;
    char arg[64];
} oreq_t;

static QueueHandle_t s_req_q;
static TaskHandle_t  s_work_task;

/* 结果槽位（写方是工作线程，读方是 LVGL 定时器；kind 最后写、最先清） */
static volatile int   s_res_kind;
static int            s_res_err;
static online_song_t *s_res_songs;
static size_t         s_res_count;
static char           s_res_msg[64];

static void work_publish(int kind, esp_err_t err, online_song_t *songs, size_t count, const char *msg)
{
    s_res_err   = (int)err;
    s_res_songs = songs;
    s_res_count = count;
    snprintf(s_res_msg, sizeof(s_res_msg), "%s", msg ? msg : "");
    s_res_kind  = kind;              /* 最后置位 */
}

static void online_work_task(void *arg)
{
    (void)arg;
    oreq_t req;
    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (req.kind) {
        case OREQ_ADD: {
            ESP_LOGI(TAG, "fetch playlist %s ...", req.arg);
            esp_err_t err = online_playlist_add(req.arg);
            work_publish(OREQ_ADD, err, NULL, 0, req.arg);
            break;
        }
        case OREQ_LOAD: {
            online_song_t *arr = NULL;
            size_t n = 0;
            esp_err_t err = online_playlist_load(req.arg, &arr, &n);
            work_publish(OREQ_LOAD, err, arr, n, req.arg);
            break;
        }
        case OREQ_SEARCH: {
            online_song_t *arr = NULL;
            size_t n = 0;
            esp_err_t err = online_search(req.arg, &arr, &n, SEARCH_MAX);
            work_publish(OREQ_SEARCH, err, arr, n, req.arg);
            break;
        }
        case OREQ_REMOVE: {
            esp_err_t err = online_playlist_remove(req.arg);
            work_publish(OREQ_REMOVE, err, NULL, 0, req.arg);
            break;
        }
        default:
            break;
        }
    }
}

static void work_submit(int kind, const char *arg)
{
    if (!s_req_q) {
        ui_toast("在线服务未就绪", 1500);
        return;
    }
    oreq_t req = {};
    req.kind = kind;
    if (arg) {
        snprintf(req.arg, sizeof(req.arg), "%s", arg);
    }
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        ui_toast("上一个操作还没做完", 1500);
    }
}

/* ---------------------------------------------------------------- 界面状态 */

static lv_obj_t *s_root;        /* 页面 screen */
static lv_obj_t *s_chips;       /* 歌单 chip 行 */
static lv_obj_t *s_list;        /* 歌曲列表容器 */
static lv_obj_t *s_hint;        /* 顶部说明行 */
static lv_obj_t *s_title_lbl;   /* 当前列表标题 */
static lv_timer_t *s_poll;

/* 当前显示（= 交给 app_music 的）队列 */
static online_song_t *s_songs;
static size_t         s_song_n;
static int            s_sel_pl = -1;      /* 选中的歌单下标，-1 = 搜索结果/空 */
/* 被替换下来、但播放器可能还在引用的旧数组：等播放器切到新数组后再释放 */
static online_song_t *s_stale;

static char s_cur_pl_id[ONLINE_ID_MAX];

static void online_rebuild_chips(void);
static void online_rebuild_list(void);

/* 释放"上一份"队列（必须在 app_music 已经切到新数组之后调用） */
static void online_drop_stale(void)
{
    if (s_stale) {
        free(s_stale);
        s_stale = NULL;
    }
}

/* 安装一份新队列（LOAD/SEARCH 的结果） */
static void online_install(online_song_t *songs, size_t count, const char *pl_id)
{
    online_drop_stale();
    if (s_songs) {
        s_stale = s_songs;          /* 播放器可能还在用它，等切歌后再放 */
    }
    s_songs  = songs;
    s_song_n = count;
    snprintf(s_cur_pl_id, sizeof(s_cur_pl_id), "%s", pl_id ? pl_id : "");
    online_rebuild_list();
}

/* ---------------------------------------------------------------- 播放 */

static void online_row_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= s_song_n) {
        return;
    }
    /* 播放器接手这份队列之后，被替换下来的旧数组就可以放了 */
    online_drop_stale();
    app_music_play_online(s_songs, s_song_n, (size_t)idx);
    app_music_open_player();       /* 直接进播放器页看封面/歌词 */
}

/* ---------------------------------------------------------------- 列表/歌单 */

static lv_obj_t *make_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                          const char *title, const char *sub)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(40), UI_PX_H(40));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, UI_PCT_X(60));
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, UI_PX_H(52), 0);

    if (sub && sub[0]) {
        lv_obj_t *s = lv_label_create(row);
        lv_obj_set_style_text_font(s, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, UI_PCT_X(22));
        lv_label_set_text(s, sub);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

static void online_rebuild_list(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);
    if (s_title_lbl) {
        if (s_song_n == 0) {
            lv_label_set_text(s_title_lbl, s_sel_pl >= 0 ? "歌单为空" : "还没有歌曲");
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "共 %u 首", (unsigned)s_song_n);
            lv_label_set_text(s_title_lbl, buf);
        }
    }
    if (s_song_n == 0) {
        lv_obj_t *hint = lv_label_create(s_list);
        lv_obj_set_style_text_font(hint, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(hint, online_has_network()
                                    ? "点右上角「+」添加网易云歌单 ID，或「搜索」找歌"
                                    : "Wi-Fi 未连接：进「设置」连上网络再添加歌单");
        return;
    }
    for (size_t i = 0; i < s_song_n; i++) {
        lv_obj_t *row = make_row(s_list, LV_SYMBOL_AUDIO, 0x3E9E86,
                                 s_songs[i].title, s_songs[i].artist);
        lv_obj_add_event_cb(row, online_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void online_chip_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const char *id = online_playlist_id((size_t)idx);
    if (!id) {
        return;
    }
    if (s_sel_pl == idx && s_song_n > 0) {
        return;                     /* 已经在这个歌单上 */
    }
    s_sel_pl = idx;
    online_rebuild_chips();
    if (s_title_lbl) {
        lv_label_set_text(s_title_lbl, "读取中…");
    }
    work_submit(OREQ_LOAD, id);
}

static void online_rebuild_chips(void)
{
    if (!s_chips) {
        return;
    }
    lv_obj_clean(s_chips);
    const size_t n = online_playlist_count();
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_chips);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(l, "（还没有歌单，点右上角 + 添加）");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *id = online_playlist_id(i);
        lv_obj_t *chip = lv_button_create(s_chips);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, UI_PX_H(38));
        lv_obj_set_style_pad_hor(chip, UI_PX_W(14), LV_PART_MAIN);
        lv_obj_set_style_radius(chip, UI_PX_H(19), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chip, (s_sel_pl == (int)i) ? UI_COLOR_ACCENT : UI_COLOR_PILL,
                                  LV_PART_MAIN);
        lv_obj_add_event_cb(chip, online_chip_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(chip);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, (s_sel_pl == (int)i) ? lv_color_hex(0x14141A) : UI_COLOR_FG,
                                    LV_PART_MAIN);
        char buf[32];
        snprintf(buf, sizeof(buf), "歌单 %s", id);
        lv_label_set_text(l, buf);
        lv_obj_center(l);
    }
}

/* ---------------------------------------------------------------- 浮层：添加歌单 */

static lv_obj_t *s_modal;        /* 当前浮层（添加/搜索） */
static lv_obj_t *s_modal_ta;     /* 输入框 */
static lv_obj_t *s_ime;          /* 拼音键盘（懒建，全局复用） */
static lv_obj_t *s_ime_box;      /* 拼音键盘的容器（决定按键大小） */
static lv_obj_t *s_kb;           /* 歌单 ID 用的普通键盘（LVGL 自带，带数字） */
static int       s_modal_kind;   /* OREQ_ADD / OREQ_SEARCH */

static void modal_close(void)
{
    /* 两个键盘都挂在 lv_layer_top 上（不是浮层的子对象），这里只隐藏 */
    if (s_ime) {
        lv_pinyin_ime_hide(s_ime);
    }
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_modal) {
        lv_obj_delete_async(s_modal);   /* 异步删：本函数可能在键盘事件回调里被调 */
        s_modal = NULL;
    }
    s_modal_ta = NULL;
    s_modal_kind = OREQ_NONE;
}

static void modal_submit(lv_event_t *e)
{
    (void)e;
    if (!s_modal_ta || !s_modal) {
        return;
    }
    const char *txt = lv_textarea_get_text(s_modal_ta);
    if (!txt || !txt[0]) {
        ui_toast("先输入内容", 1200);
        return;
    }
    if (!online_has_network()) {
        ui_toast("Wi-Fi 未连接", 1500);
        return;
    }
    const int kind = s_modal_kind;
    work_submit(kind, txt);
    modal_close();
}

/*
 * 浮层销毁（异步删除或页面切走时都会来）：
 * 键盘挂在 lv_layer_top 上，不归浮层所有，所以这里只需要把指针清掉。
 */
static void modal_on_delete(lv_event_t *e)
{
    (void)e;
    s_modal = NULL;
    s_modal_ta = NULL;
    if (s_ime) {
        lv_pinyin_ime_hide(s_ime);
    }
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/*
 * 键盘的按键尺寸是 lv_pinyin_ime_create() 里按**父对象宽度**算一次的
 * （IME_UI_UNIT_PX = 可用宽度 / 12）。刚建出来的容器还没跑过 layout，
 * lv_obj_get_width() 是 0，组件会退化成"按 320 px 算" → 键盘特别小。
 * 所以这里先把容器塞进一个显式宽度、并强制跑一次 layout，再建键盘。
 */
static void modal_build_keyboards(lv_obj_t *ta, bool search)
{
    if (search) {
        if (s_kb) {
            lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        }
        if (!s_ime) {
            s_ime_box = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(s_ime_box);
            lv_obj_set_width(s_ime_box, LV_PCT(96));
            lv_obj_set_height(s_ime_box, LV_SIZE_CONTENT);
            lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(4));
            lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_update_layout(s_ime_box);      /* ★ 让宽度立刻生效（见上面注释） */
            s_ime = lv_pinyin_ime_create(s_ime_box);
            if (!s_ime) {
                ESP_LOGW(TAG, "pinyin ime create failed (dict 分区没烧？)");
                ui_toast("拼音输入法不可用（dict 分区未烧写），可先用英文搜索", 2500);
            }
        }
        if (s_ime) {
            lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(4));
            lv_pinyin_ime_attach(s_ime, ta);
            lv_pinyin_ime_show(s_ime);
        }
        return;
    }

    /* 歌单 ID：字母数字就够，用 LVGL 自带键盘（自带数字行与 OK 键） */
    if (s_ime) {
        lv_pinyin_ime_hide(s_ime);
    }
    if (!s_kb) {
        s_kb = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(s_kb, LV_PCT(96), LV_PCT(46));
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(4));
        lv_obj_set_style_text_font(s_kb, UI_FONT_TEXT, LV_PART_ITEMS);
        /* 自带键盘的 OK / 关闭键：直接走"确定"与"取消" */
        lv_obj_add_event_cb(s_kb, modal_submit, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_kb, [](lv_event_t *e) { (void)e; modal_close(); }, LV_EVENT_CANCEL, NULL);
    }
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

/* 建浮层：标题 + 输入框 +（可选拼音键盘）+ 确定/取消 */
static void modal_open(int kind)
{
    if (s_modal) {
        return;
    }
    s_modal_kind = kind;
    const bool search = (kind == OREQ_SEARCH);

    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, modal_on_delete, LV_EVENT_DELETE, NULL);
    s_modal = scrim;

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(96));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_PX_H(14), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, UI_PX_H(8));

    lv_obj_t *title = lv_label_create(panel);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(title, search ? "搜索歌曲（歌名/歌手）" : "添加网易云歌单 ID");

    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, UI_PX_H(48));
    lv_obj_set_style_text_font(ta, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, UI_COLOR_FG, LV_PART_MAIN);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 60);
    lv_textarea_set_placeholder_text(ta, search ? "输入歌名或歌手（可用拼音键盘）"
                                                : "例如 24381616");
    s_modal_ta = ta;

    lv_obj_t *btns = lv_obj_create(panel);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_button_create(btns);
    lv_obj_set_size(cancel, UI_PX_W(96), UI_PX_H(40));
    lv_obj_set_style_bg_color(cancel, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, [](lv_event_t *e) { (void)e; modal_close(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_obj_set_style_text_font(cl, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(cl, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(cl, "取消");
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(btns);
    lv_obj_set_size(ok, UI_PX_W(96), UI_PX_H(40));
    lv_obj_set_style_bg_color(ok, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_add_event_cb(ok, modal_submit, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(ok);
    lv_obj_set_style_text_font(ol, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ol, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(ol, search ? "搜索" : "添加");
    lv_obj_center(ol);

    if (search) {
        /* 拼音键盘见 modal_build_keyboards()：只建一次、挂在 layer_top 上 */
        modal_build_keyboards(ta, true);
    } else {
        /* 歌单 ID 用 LVGL 自带键盘（可输数字），否则用户根本没法输入 ID */
        modal_build_keyboards(ta, false);
    }
}

/* ---------------------------------------------------------------- 顶部按钮 */

static void on_add_clicked(lv_event_t *e)
{
    (void)e;
    modal_open(OREQ_ADD);
}

static void on_search_clicked(lv_event_t *e)
{
    (void)e;
    modal_open(OREQ_SEARCH);
}

static void on_remove_clicked(lv_event_t *e)
{
    (void)e;
    const int idx = s_sel_pl;
    if (idx < 0) {
        ui_toast("先在下面选一个歌单", 1600);
        return;
    }
    const char *id = online_playlist_id((size_t)idx);
    if (!id) {
        return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "已删除歌单 %s", id);
    work_submit(OREQ_REMOVE, id);
    s_sel_pl = -1;
    ui_toast(msg, 1500);
}

/* ---------------------------------------------------------------- 结果处理 */

static void on_result(void)
{
    const int kind = s_res_kind;
    if (kind == OREQ_NONE) {
        return;
    }
    const esp_err_t err = (esp_err_t)s_res_err;
    online_song_t *songs = s_res_songs;
    const size_t count = s_res_count;
    char msg[64];
    snprintf(msg, sizeof(msg), "%s", s_res_msg);

    s_res_songs = NULL;
    s_res_count = 0;
    s_res_kind  = OREQ_NONE;
    s_res_msg[0] = '\0';

    switch (kind) {
    case OREQ_ADD:
        if (err == ESP_OK) {
            ui_toast("歌单已保存到 SD", 1500);
            online_rebuild_chips();
            /* 新加的自动选中并读出来 */
            for (size_t i = 0; i < online_playlist_count(); i++) {
                const char *id = online_playlist_id(i);
                if (id && strcmp(id, msg) == 0) {
                    s_sel_pl = (int)i;
                    online_rebuild_chips();
                    work_submit(OREQ_LOAD, id);
                    break;
                }
            }
        } else if (err == ESP_ERR_INVALID_STATE) {
            ui_toast("Wi-Fi 未连接", 1800);
        } else {
            /* 把服务里的具体原因带出来（HTTP 400 / 网络错误 / 写 SD 失败…） */
            const char *why = online_last_error();
            char buf[96];
            snprintf(buf, sizeof(buf), "抓取失败：%s", why ? why : "ID 不对或网络不通");
            ui_toast(buf, 2600);
        }
        break;
    case OREQ_LOAD:
        if (err == ESP_OK && songs) {
            online_install(songs, count, msg);
            ui_toast("歌单已载入", 1200);
        } else {
            free(songs);
            if (s_title_lbl) {
                lv_label_set_text(s_title_lbl, "读取失败");
            }
            ui_toast("歌单读取失败", 1800);
        }
        break;
    case OREQ_SEARCH:
        if (err == ESP_OK && songs) {
            s_sel_pl = -1;               /* 搜索结果是临时列表 */
            online_install(songs, count, "");
            online_rebuild_chips();
            ui_toast("搜索完成", 1200);
        } else {
            free(songs);
            ui_toast("没有找到匹配的歌曲", 1800);
        }
        break;
    case OREQ_REMOVE:
        online_rebuild_chips();
        online_drop_stale();
        if (s_songs) {
            free(s_songs);
            s_songs = NULL;
        }
        s_song_n = 0;
        online_rebuild_list();
        break;
    default:
        free(songs);
        break;
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    /* 网络状态变了就刷新一下提示（连上网/断网） */
    static bool s_net_last;
    const bool net = online_has_network();
    if (net != s_net_last) {
        s_net_last = net;
        online_rebuild_list();
    }
    on_result();
}

/* ---------------------------------------------------------------- 页面 */

static void online_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    modal_close();
    s_root = NULL;
    s_chips = NULL;
    s_list = NULL;
    s_hint = NULL;
    s_title_lbl = NULL;
}

static lv_obj_t *page_online(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, UI_PX_H(6), LV_PART_MAIN);
    /* 纵向 flex：标题行 / 歌单 chips / 统计行 / 列表（列表 flex_grow 吃掉剩余高度）。
     * 为什么不用"绝对定位 + 给列表留 pad_top"：那样列表容器会整屏盖住 chips，
     * LVGL 的容器默认 clickable，会把歌单 chip 的点击全吞掉。 */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    s_root = scr;

    /* 标题行 */
    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), UI_PX_H(40));
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(title, "在线音乐");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    struct {
        const char *sym;
        lv_event_cb_t cb;
    } btns[] = {
        { LV_SYMBOL_PLUS,  on_add_clicked },
        { LV_SYMBOL_LIST,  on_search_clicked },
        { LV_SYMBOL_TRASH, on_remove_clicked },
    };
    lv_obj_t *last = NULL;
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
        lv_obj_t *b = lv_button_create(head);
        lv_obj_set_size(b, UI_PX_H(36), UI_PX_H(36));
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, UI_COLOR_PILL, LV_PART_MAIN);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
        lv_label_set_text(l, btns[i].sym);
        lv_obj_center(l);
        if (last) {
            lv_obj_align_to(b, last, LV_ALIGN_OUT_LEFT_MID, -UI_PX_W(8), 0);
        } else {
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        last = b;
    }

    /* 歌单 chips 行（横向滚动） */
    s_chips = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chips);
    lv_obj_set_size(s_chips, LV_PCT(100), UI_PX_H(46));
    lv_obj_set_flex_flow(s_chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_chips, UI_PX_W(8), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_chips, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_chips, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_chips, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_chips, UI_PX_W(4), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_chips, UI_PX_H(2), LV_PART_SCROLLBAR);

    /* 列表标题（共 N 首 / 提示） */
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title_lbl, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_title_lbl, "");

    /* 歌曲列表：吃掉剩余高度，自己带滚动条（需求：列表右侧滚动条） */
    s_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_right(s_list, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_list, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list, UI_PX_W(6), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, UI_PX_H(3), LV_PART_SCROLLBAR);

    online_rebuild_chips();
    online_rebuild_list();

    lv_obj_add_event_cb(scr, online_on_delete, LV_EVENT_DELETE, NULL);
    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    ESP_LOGI(TAG, "online page created (%u playlist(s))", (unsigned)online_playlist_count());
    return scr;
}

/* ---------------------------------------------------------------- 注册 */

void app_online_register_apps(void)
{
    static const app_desc_t online = {
        "online", "在线音乐", LV_SYMBOL_WIFI, 0x3E9E86,
        page_online, NULL, NULL, 20, 0
    };
    ui_app_register(&online);

    if (!s_req_q) {
        s_req_q = xQueueCreate(4, sizeof(oreq_t));
    }
    if (s_req_q && !s_work_task) {
        if (xTaskCreate(online_work_task, "online_work", WORK_TASK_STACK, NULL, 3,
                        &s_work_task) != pdPASS) {
            s_work_task = NULL;
            ESP_LOGW(TAG, "online work task create failed");
        }
    }
#if ONLINE_SELFTEST
    xTaskCreate(online_selftest_task, "online_st", WORK_TASK_STACK, NULL, 3, NULL);
#endif
}
