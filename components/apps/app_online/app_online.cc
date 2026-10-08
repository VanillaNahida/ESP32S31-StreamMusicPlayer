/*
 * app_online.cc —— 网易云音乐（网易云歌单 / 搜索 / 播放）
 *
 * 需求对应：
 *   - 添加网易云歌单 ID → 抓取歌单 → **二进制存到 SD**（都在 online_service 里）
 *   - 播放歌单里的在线歌曲：默认**流式播放**（音频边下边播，不落 SD），
 *     下载失败时回退"先下载到 SD 再播"；封面/歌词一律缓存在 **PSRAM**，
 *     不写 SD 卡（SD 读写是之前切歌卡顿的根因）
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
#include "ui_icons.h"      /* &netease_logo（桌面图标位图，由 PNG 生成） */

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
 * 拼音键盘的目标高度（fit height，见文件头与 lvgl_pinyin_ime 的
 * lv_pinyin_ime_set_fit_height）。组件按"候选条 + 可见行数"反推出行高，
 * 让整块键盘正好占半屏 —— 按键会随父容器宽度自适应缩放。
 */
#define IME_FIT_HEIGHT    UI_PX_H(240)   /* UI_H / 2：键盘恰好半屏 */

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
        uint8_t *jpg = NULL;
        size_t jl = 0;
        ESP_LOGI(TAG, "SELFTEST cover: %s (%u bytes)",
                 esp_err_to_name(online_song_cover_data(&songs[0], &jpg, &jl)),
                 (unsigned)jl);
        free(jpg);
        char *lrc = NULL;
        ESP_LOGI(TAG, "SELFTEST lyrics: %s (%u bytes)",
                 esp_err_to_name(online_song_lyrics_data(&songs[0], &lrc, NULL)),
                 (unsigned)(lrc ? strlen(lrc) : 0));
        free(lrc);

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

/* ---------------------------------------------------------------- 页面骨架
 *
 * 网易云音乐是**左右滑动的两页**：第 0 页"我的歌单"、第 1 页"歌曲搜索"，
 * 顶部那两段文字既是标题也是 tab。
 *
 * 实现：两页并排放在 s_host 里（LVGL 默认把子对象裁剪在父对象范围内，所以
 * 超出部分自动看不见），切页 = 一条 lv_anim 把两页的 x 一起平移，220 ms 缓出 ——
 * 和 ui_router 的页面动画同节奏，视觉上是一套。
 */
static lv_obj_t *s_root;         /* 页面 screen */
static lv_obj_t *s_tab_pl;       /* 顶部文字：我的歌单（兼 tab） */
static lv_obj_t *s_tab_srch;     /* 顶部文字：歌曲搜索（兼 tab） */
static lv_obj_t *s_host;         /* 两页的宿主（负责裁剪，页宽也从它取） */
static lv_obj_t *s_page_pl;      /* 第 0 页容器 */
static lv_obj_t *s_page_srch;    /* 第 1 页容器 */
static int       s_page;         /* 当前页 0/1 */
static int32_t   s_page_off;     /* 当前平移量（px）—— 动画的起始值 */
static lv_timer_t *s_poll;

/* 第 0 页：先是歌单列表；点进某个歌单后，**同一页**变成"歌单详情"（歌曲列表），
 * 顶部给一行 "‹ 歌单列表" 返回。为什么不做成压栈的第二页：单页内切换状态更少、
 * 也不用把"加载结果该进哪个列表"跨页路由（那个最容易出竞态）。 */
static lv_obj_t *s_pl_list;      /* 行容器（歌单行 / 歌曲行） */
static lv_obj_t *s_pl_hint;      /* 顶部说明行 */
static bool      s_pl_detail;    /* true = 正在看某个歌单的歌曲列表 */
static int       s_sel_pl = -1;  /* 详情对应的歌单下标；-1 = 歌单列表 */

/* 第 1 页：搜索 */
static lv_obj_t *s_srch_ta;      /* 输入框 */
static lv_obj_t *s_srch_hint;    /* 列表上方小标题（历史搜索 / 搜索结果） */
static lv_obj_t *s_srch_clear;   /* 右上角"清除"（只在显示历史时出现） */
static lv_obj_t *s_srch_list;    /* 历史 / 结果 列表 */
static bool      s_srch_has_res; /* 列表里是搜索结果(true) 还是历史(false) */
/*
 * ★ "请求已经发出去、结果还没回来"的中间态。
 * 少了它就会出现用户报的那个怪现象（2026-10）：搜索请求刚发出去时，
 * 为了让结果回来能就地替换，代码先把 s_srch_has_res 置真"占住结果状态"——
 * 可这时列表里还没有任何数据，于是画出来的就是**"没有找到匹配的歌曲"**，
 * 而实际上正在搜。现在这段区间由 s_srch_busy 表示，界面显示"正在搜索…"+转圈。
 */
static bool      s_srch_busy;

/* 当前显示（= 交给 app_music 的）队列 */
static online_song_t *s_songs;
static size_t         s_song_n;
/* 被替换下来、但播放器可能还在引用的旧数组：等播放器切到新数组后再释放 */
static online_song_t *s_stale;
/* s_songs 是谁的结果：歌单详情 or 搜索结果（两份结果共用一个数组，避免双份
 * 所有权带来的重复释放；谁的结果谁显示，另一个页面显示自己的内容） */
enum { OWNER_PL = 0, OWNER_SRCH = 1 };
static int  s_owner = OWNER_PL;

static char s_cur_pl_id[ONLINE_ID_MAX];

/* ---- 浮层（添加歌单 / 重命名）与拼音键盘（懒建、全局复用） ----
 * 声明放在这里（而不是浮层那段代码旁边），因为上面的搜索页要用到
 * ime_show_for()/ime_hide()/ime_open()。 */
static lv_obj_t *s_modal;        /* 当前浮层 */
static lv_obj_t *s_modal_ta;     /* 浮层里的输入框 */
static lv_obj_t *s_ime;          /* 拼音键盘（懒建，全局复用） */
static lv_obj_t *s_ime_box;      /* 拼音键盘的容器（宽度决定按键大小） */
static lv_obj_t *s_kb;           /* 歌单 ID 用的普通键盘（LVGL 自带，带数字） */
static int       s_modal_kind;   /* OREQ_ADD / MODAL_RENAME */

#define MODAL_RENAME   (-100)    /* 只在本文件用：重命名浮层（不是网络请求） */

#define PAGE_ANIM_MS  220        /* 两页左右滑动的动画时长（与 ui_router 同节奏） */

static void online_rebuild_page_pl(void);
static void online_rebuild_page_srch(void);
static void page_show(int which, bool animate);
static void modal_open(int kind);
static void modal_submit(lv_event_t *e);
static void srch_do_search(const char *kw);
static void srch_ime_ready(lv_event_t *e);
static void ime_show_for(lv_obj_t *ta);
static void ime_hide(void);
/* 定义在浮层那一段，但上面的 ime_show_for() 要用它 */
static void modal_build_keyboards(lv_obj_t *ta, bool search);

/* 释放"上一份"队列（必须在 app_music 已经切到新数组之后调用） */
static void online_drop_stale(void)
{
    if (s_stale) {
        free(s_stale);
        s_stale = NULL;
    }
}

/* 安装一份新队列（LOAD/SEARCH 的结果） */
static void online_install(online_song_t *songs, size_t count, const char *pl_id, int owner)
{
    online_drop_stale();
    if (s_songs && s_songs != songs) {
        s_stale = s_songs;          /* 播放器可能还在用它，等切歌后再放 */
    }
    s_songs  = songs;
    s_song_n = count;
    s_owner  = owner;
    /*
     * 歌单 ID 的契约长度是 ONLINE_ID_MAX（网易云是数字 ID，服务端存 NVS 时也按
     * 这个长度截断），而这里拿到的 pl_id 来自 64 字节的请求字段 —— 按契约精度拷贝，
     * 超长的本来就不是合法 ID（不按契约截断的话，s_cur_pl_id 会比服务侧存的 ID 长，
     * 后面 strcmp 对不上，UI 上"当前歌单"的高亮就丢了）。
     */
    snprintf(s_cur_pl_id, sizeof(s_cur_pl_id), "%.*s",
             (int)(sizeof(s_cur_pl_id) - 1), pl_id ? pl_id : "");
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

/* ---------------------------------------------------------------- 行构造 */

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

/* 歌曲列表（歌单详情 / 搜索结果共用）：s_songs 里挑 count 首画出来 */
static void fill_song_rows(lv_obj_t *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *row = make_row(list, LV_SYMBOL_AUDIO, 0x3E9E86,
                                 s_songs[i].title, s_songs[i].artist);
        lv_obj_add_event_cb(row, online_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

/* ---------------------------------------------------------------- 第 0 页：歌单 */

static void pl_back_clicked(lv_event_t *e)
{
    (void)e;
    s_pl_detail = false;
    s_sel_pl = -1;
    online_rebuild_page_pl();
}

/*
 * 打开歌单详情。**挂在 LV_EVENT_SHORT_CLICKED 上，不是 CLICKED** —— 这一点是整个
 * 长按菜单能用的前提，别再改回 CLICKED：
 *
 *   LVGL 的松手处理（src/indev/lv_indev.c 的 indev_proc_release）是
 *       if(indev->pointer.pressed) {
 *           if(indev->long_pr_sent == 0) indev_proc_short_click(indev);  ← 只有"短按"才进
 *           send_event(LV_EVENT_CLICKED, indev_act);                     ← 无条件发！
 *       }
 *   而 LV_EVENT_SHORT_CLICKED 是在 indev_proc_short_click() **里面**发的（1603 行），
 *   所以它**永远不会跟在一次长按后面**。
 *
 * 用 CLICKED 的话：长按弹菜单的同时还会补一个 CLICKED 落到这一行 → 菜单底下又被
 * 换成歌单详情；曾经用"时间标志"去挡，但那依赖"松手事件"和"异步建菜单"的先后顺序，
 * 实测挡不住（用户报：长按既弹菜单又进歌单页、界面卡死）。
 */
/* 长按菜单的遮罩与"关闭"（完整定义在下面，这里先给声明：上面这个点击处理要用） */
static lv_obj_t *s_menu;
static void pl_menu_close(void);

static void pl_row_clicked(lv_event_t *e)
{
    /* 兜底：菜单开着的时候不允许换页（正常路径下遮罩会把点击吃掉） */
    if (s_menu) {
        pl_menu_close();
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const char *id = online_playlist_id((size_t)idx);
    if (!id) {
        return;
    }
    s_sel_pl = idx;
    s_pl_detail = true;
    if (s_pl_hint) {
        lv_label_set_text(s_pl_hint, "载入中…");
    }
    online_rebuild_page_pl();      /* 先画出"载入中"的空壳 */
    work_submit(OREQ_LOAD, id);
}

/* 长按歌单行 → 重命名 / 更新 / 删除 菜单（s_menu 的声明在上面，点击处理要用） */
static int       s_menu_idx = -1;
/* 重命名目标（pl_menu_close() 会清 s_menu_idx，所以先存到这份里） */
static int       s_rename_idx = -1;
/* 已经排了"等手指抬起再弹菜单"的异步调用（防重复排） */
static bool      s_menu_pending;
/* 菜单弹出的时刻：刚弹出的极短时间里忽略遮罩上的"点空白关闭" */
static uint32_t  s_menu_tick;

#define MENU_GRACE_MS  200      /* 这段时间内遮罩上的 CLICKED 不算"点空白关闭" */

static void pl_menu_build(int idx);
static void pl_menu_deferred(void *arg);

static void pl_menu_close(void)
{
    s_menu_pending = false;
    if (s_menu) {
        /* 渐出（面板是遮罩的第一个子对象）：动画结束由 ui_popup_out 自己删 */
        ui_popup_out(s_menu, lv_obj_get_child(s_menu, 0));
        s_menu = NULL;
    }
    s_menu_idx = -1;
}

static void pl_menu_deleted(lv_event_t *e)
{
    (void)e;
    s_menu = NULL;
    s_menu_idx = -1;
}

static void pl_menu_btn(lv_event_t *e)
{
    const int action = (int)(intptr_t)lv_event_get_user_data(e);   /* 0=重命名 1=更新 2=删除 */
    const int idx = s_menu_idx;
    const char *id = (idx >= 0) ? online_playlist_id((size_t)idx) : NULL;
    if (!id) {
        pl_menu_close();
        return;
    }
    char id_copy[ONLINE_ID_MAX];
    snprintf(id_copy, sizeof(id_copy), "%s", id);   /* 删掉菜单后 id 指针可能失效 */
    pl_menu_close();

    if (action == 0) {
        /* pl_menu_close() 会把 s_menu_idx 清掉：先把目标歌单存进 s_rename_idx */
        s_rename_idx = idx;
        modal_open(MODAL_RENAME);
        return;
    }
    if (action == 1) {
        if (!online_has_network()) {
            ui_toast("Wi-Fi 未连接", 1500);
            return;
        }
        ui_toast("正在更新歌单，请稍后…", 1500);
        work_submit(OREQ_ADD, id_copy);   /* ADD 对已存在的 ID 就是"重新抓取刷新" */
        return;
    }
    ui_toast("已删除该歌单", 1500);
    if (s_sel_pl == idx) {
        s_sel_pl = -1;
        s_pl_detail = false;
    }
    work_submit(OREQ_REMOVE, id_copy);
}

/*
 * 长按歌单行：**只记录目标并排一个异步调用**，菜单本体等手指抬起来再建。
 *
 * ★ 为什么不能在长按回调里直接建（这是"菜单点不动、还点到下层布局"的根因）：
 *   长按那一刻手指还按在屏幕上，而 LVGL 在按压期间会**不断重新命中手指下的对象**
 *   （lv_indev.c 的 pointer_search_obj，注释写着 "Without `LV_OBJ_FLAG_PRESS_LOCK`
 *   new widget can be found while pressing"）。歌单行没有 PRESS_LOCK，所以
 *   全屏遮罩一建出来就顶在手指底下 → 下一轮 act_obj 变成遮罩；
 *   而 indev_proc_release 里**长按之后照样补发 CLICKED**（它只跳过 short_click）
 *   → 松手那个 CLICKED 落在遮罩上 → "点空白关掉"把刚弹出的菜单自己删掉，
 *   之后所有点击自然都落到下层页面（= "点到了底部的布局"）。
 *   顺带还有个更危险的：万一手指正压在某个按钮上，松手会当场触发它（最坏是删歌单）。
 */
static void pl_row_long_pressed(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= online_playlist_count() || s_menu || s_menu_pending) {
        return;
    }
    s_menu_idx     = idx;
    s_menu_pending = true;
    lv_async_call(pl_menu_deferred, NULL);
}

/* 还按着就下一轮再看 —— 宁可晚一点弹，也不能让"长按那次按压"碰到菜单 */
static void pl_menu_deferred(void *arg)
{
    (void)arg;
    if (!s_menu_pending) {
        return;
    }
    const lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lv_async_call(pl_menu_deferred, NULL);
        return;
    }
    const int idx = s_menu_idx;
    s_menu_pending = false;
    if (idx >= 0 && (size_t)idx < online_playlist_count()) {
        pl_menu_build(idx);
    }
}

/* 点遮罩空白处关掉菜单；刚弹出的极短时间里不认（那一下只可能是长按的余波） */
static void pl_menu_scrim_clicked(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_menu_tick) < MENU_GRACE_MS) {
        return;
    }
    pl_menu_close();
}

/* 点菜单自己的空白处不该关掉菜单（CLICKED 会从子对象冒泡上来） */
static void pl_menu_panel_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

/* 真正建菜单（只在手指离开屏幕后被调用） */
static void pl_menu_build(int idx)
{
    /*
     * ★ 防泄漏：菜单已经在了就直接返回。
     * 万一有两个"建菜单"的请求挤在一起（长按事件重复/异步重入），第二块全屏遮罩
     * 会把 s_menu 覆盖掉 —— 旧那块没人删、还盖在最上面吃掉所有点击，
     * 现象就是"界面卡死怎么点都没反应"。宁可少弹一次，也不能留下孤儿遮罩。
     */
    if (s_menu) {
        return;
    }
    s_menu_tick = lv_tick_get();

    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, pl_menu_scrim_clicked, LV_EVENT_CLICKED, NULL);   /* 点空白关掉 */
    lv_obj_add_event_cb(scrim, pl_menu_deleted, LV_EVENT_DELETE, NULL);
    s_menu = scrim;

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, UI_PCT_X(56));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_PX_H(14), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, UI_PX_H(12), LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, pl_menu_panel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(panel);

    /* 渐入（淡入 + 上滑）——共用 ui_core 的弹层动画，见 ui_popup_in 的说明 */
    ui_popup_in(s_menu, panel);

    const char *nm = online_playlist_name((size_t)idx);
    lv_obj_t *ttl = lv_label_create(panel);
    lv_obj_set_style_text_font(ttl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(ttl, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(ttl, (nm && nm[0]) ? nm : "歌单");

    static const char *k_txt[3] = { "重命名", "更新（重新抓取）", "删除歌单" };
    static const uint32_t k_col[3] = { 0x5B6FB5, 0x3E9E86, 0xB5504E };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(panel);
        lv_obj_set_size(b, LV_PCT(100), UI_PX_H(44));
        lv_obj_set_style_radius(b, UI_PX_H(10), LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(k_col[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_event_cb(b, pl_menu_btn, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, UI_COLOR_FG, LV_PART_MAIN);
        lv_label_set_text(l, k_txt[i]);
        lv_obj_center(l);
    }
}

static void online_rebuild_page_pl(void)
{
    if (!s_pl_list) {
        return;
    }
    lv_obj_clean(s_pl_list);

    /* ---- 详情：某个歌单的歌曲列表 ---- */
    if (s_pl_detail) {
        const char *id = (s_sel_pl >= 0) ? online_playlist_id((size_t)s_sel_pl) : NULL;
        const char *nm = (s_sel_pl >= 0) ? online_playlist_name((size_t)s_sel_pl) : NULL;
        if (s_pl_hint) {
            char buf[96];
            if (s_owner == OWNER_PL && s_song_n > 0) {
                snprintf(buf, sizeof(buf), "%s · 共 %u 首",
                         (nm && nm[0]) ? nm : (id ? id : "歌单"), (unsigned)s_song_n);
            } else {
                snprintf(buf, sizeof(buf), "%s", (nm && nm[0]) ? nm : (id ? id : "歌单"));
            }
            lv_label_set_text(s_pl_hint, buf);
        }
        /* 顶部返回行（全局返回键是退出本 APP，这里要回"歌单列表"） */
        lv_obj_t *back = make_row(s_pl_list, LV_SYMBOL_LEFT, 0x5B6FB5, "歌单列表", NULL);
        lv_obj_add_event_cb(back, pl_back_clicked, LV_EVENT_CLICKED, NULL);

        if (s_owner == OWNER_PL && s_song_n > 0) {
            fill_song_rows(s_pl_list, s_song_n);
        } else if (s_owner == OWNER_SRCH) {
            /* s_songs 里现在是搜索结果：别把别人的结果画进歌单详情 */
            lv_obj_t *h = lv_label_create(s_pl_list);
            lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
            lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
            lv_label_set_text(h, "点一下上面的歌单重新载入");
        } else {
            /*
             * 没数据 = 正在载入：**无限转圈** + 一行字
             * （用户 2026-10 要求：所有"等东西"的地方都用转圈动画，别只给一行文字）。
             * lv_spinner 默认就是无限循环（LV_SPINNER_TYPE_SPINNING_ARC），不用自己起 timer。
             */
            lv_obj_t *box = lv_obj_create(s_pl_list);
            lv_obj_remove_style_all(box);
            lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(box, UI_PX_H(28), LV_PART_MAIN);
            lv_obj_set_style_pad_row(box, UI_PX_H(12), LV_PART_MAIN);
            lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *sp = lv_spinner_create(box);
            lv_obj_set_size(sp, UI_PX_H(48), UI_PX_H(48));
            lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_MAIN);
            lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(sp, UI_COLOR_PILL, LV_PART_MAIN);
            lv_obj_set_style_arc_color(sp, UI_COLOR_ACCENT, LV_PART_INDICATOR);
            lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *h = lv_label_create(box);
            lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
            lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
            lv_label_set_text(h, "载入中…");
        }
        return;
    }

    /* ---- 歌单列表 ---- */
    const size_t n = online_playlist_count();
    if (s_pl_hint) {
        char buf[64];
        snprintf(buf, sizeof(buf), "共 %u 个歌单（长按可重命名/更新/删除）", (unsigned)n);
        lv_label_set_text(s_pl_hint, buf);
    }
    if (n == 0) {
        lv_obj_t *h = lv_label_create(s_pl_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_text(h, online_has_network()
                                ? "还没有歌单：点右上角「+」添加网易云歌单 ID\n（也可以切到「歌曲搜索」直接找歌）"
                                : "Wi-Fi 未连接：进「设置」连上网络再添加歌单");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *id = online_playlist_id(i);
        const char *nm = online_playlist_name(i);
        lv_obj_t *row = make_row(s_pl_list, LV_SYMBOL_LIST, 0x5B6FB5,
                                 (nm && nm[0]) ? nm : (id ? id : "歌单"),
                                 (nm && nm[0]) ? id : NULL);
        lv_obj_add_event_cb(row, pl_row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        /* 长按 = 重命名/更新/删除（需求："长按歌单项可重命名、更新、删除歌单"） */
        lv_obj_add_event_cb(row, pl_row_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

/* ---------------------------------------------------------------- 第 1 页：搜索 */

static bool ime_open(void)
{
    /* 注意看的是**键盘对象**的隐藏位，不是容器：容器是 SIZE_CONTENT，
     * 键盘一隐藏它就塌成 0 高，但"开没开"要看键盘自己。 */
    return s_ime && !lv_obj_has_flag(s_ime, LV_OBJ_FLAG_HIDDEN);
}

static void srch_row_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_srch_has_res) {
        online_row_clicked(e);          /* 结果行 = 点歌播放 */
        return;
    }
    /* 历史行：把词填回输入框并立刻搜一次 */
    const char *kw = online_history_at((size_t)idx);
    if (!kw || !kw[0]) {
        return;
    }
    if (s_srch_ta) {
        lv_textarea_set_text(s_srch_ta, kw);
    }
    srch_do_search(kw);
}

static void srch_row_long_pressed(lv_event_t *e)
{
    if (s_srch_has_res) {
        return;                          /* 只对历史生效 */
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (online_history_remove((size_t)idx) == ESP_OK) {
        ui_toast("已删除这条搜索记录", 1200);
        online_rebuild_page_srch();
    }
}

static void srch_clear_clicked(lv_event_t *e)
{
    (void)e;
    online_history_clear();
    ui_toast("搜索记录已清空", 1200);
    online_rebuild_page_srch();
}

/*
 * 第 1 页的列表内容：
 *   - 有搜索结果        → 显示结果（需求："改为显示搜索结果歌曲列表"）
 *   - 输入框为空且键盘关 → 显示历史（需求："只有输入框为空并关闭输入法的时候才显示历史"）
 *   - 其它（正在输入）  → 只显示一行提示，别把历史塞在键盘上面
 */
static void online_rebuild_page_srch(void)
{
    if (!s_srch_list) {
        return;
    }
    lv_obj_clean(s_srch_list);

    const char *txt = s_srch_ta ? lv_textarea_get_text(s_srch_ta) : "";
    const bool empty = (!txt || !txt[0]);
    const bool show_hist = !s_srch_has_res && !s_srch_busy && empty && !ime_open();

    if (s_srch_hint) {
        lv_label_set_text(s_srch_hint,
                          s_srch_busy ? "正在搜索"
                                      : (s_srch_has_res ? "搜索结果"
                                                        : (show_hist ? "历史搜索"
                                                                     : "输入后按键盘上的对勾搜索")));
    }
    if (s_srch_clear) {
        /* "清除"只在显示历史时出现（需求：右上角"清除"清除搜索记录） */
        if (show_hist && online_history_count() > 0) {
            lv_obj_remove_flag(s_srch_clear, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_srch_clear, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 正在搜索：转圈 + 文案。这一段 LVGL 任务没被占住（请求在工作线程），
     * 所以转圈是真的在转。 */
    if (s_srch_busy) {
        lv_obj_t *box = lv_obj_create(s_srch_list);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, UI_PX_H(24), LV_PART_MAIN);
        lv_obj_set_style_pad_row(box, UI_PX_H(12), LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sp = lv_spinner_create(box);
        lv_obj_set_size(sp, UI_PX_H(44), UI_PX_H(44));
        lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, UI_COLOR_PILL, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, UI_COLOR_ACCENT, LV_PART_INDICATOR);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *t = lv_label_create(box);
        lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(t, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text_fmt(t, "正在搜索「%.40s」…", txt);
        return;
    }

    if (s_srch_has_res) {
        if (s_owner != OWNER_SRCH || s_song_n == 0) {
            lv_obj_t *h = lv_label_create(s_srch_list);
            lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
            lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
            lv_label_set_text(h, "没有找到匹配的歌曲");
            return;
        }
        fill_song_rows(s_srch_list, s_song_n);
        return;
    }

    if (!show_hist) {
        lv_obj_t *h = lv_label_create(s_srch_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_text(h, online_has_network()
                                ? "输入歌名或歌手，按键盘上的对勾（或回车）开始搜索"
                                : "Wi-Fi 未连接：进「设置」连上网络再搜索");
        return;
    }

    const size_t n = online_history_count();
    if (n == 0) {
        lv_obj_t *h = lv_label_create(s_srch_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(h, "还没有搜索记录（长按记录可删除单条）");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *kw = online_history_at(i);
        lv_obj_t *row = make_row(s_srch_list, LV_SYMBOL_EDIT, 0x3E9E86, kw ? kw : "", NULL);
        /* ★ 同上：有长按动作的行必须听 SHORT_CLICKED，否则"长按删记录"会顺带搜一次 */
        lv_obj_add_event_cb(row, srch_row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, srch_row_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

static void srch_do_search(const char *kw)
{
    if (!kw || !kw[0]) {
        return;
    }
    if (!online_has_network()) {
        ui_toast("Wi-Fi 未连接", 1500);
        return;
    }
    online_history_add(kw);          /* 记历史（服务侧去重 + 提到最前） */
    s_srch_busy    = true;           /* 先进入"正在搜索"态（见 s_srch_busy 的说明） */
    s_srch_has_res = false;          /* 结果没回来之前不算"有结果"，避免误报"没找到" */
    ime_hide();
    online_rebuild_page_srch();
    work_submit(OREQ_SEARCH, kw);
}

/*
 * 输入框事件。
 *
 * ★ **绝不能在 LV_EVENT_DEFOCUSED 里收键盘**（2026-10 用户报"点一下键盘它自己就收了"）：
 *   点键盘上的按键时，焦点会从输入框转到那个按键对象上（按键也是可聚焦的 widget），
 *   输入框随即收到 DEFOCUSED —— 早期版本在这个分支里 ime_hide()，
 *   于是"按第一个键，键盘就没了"。
 *   现在收键盘只有三个入口：
 *     1) 输入框为空时按对勾/回车（见 srch_ime_ready）；
 *     2) 点列表/空白区（见 srch_hide_ime_clicked）；
 *     3) 离开本页、或切回"我的歌单"页（online_on_delete / page_show）。
 */
static void srch_ta_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        ime_show_for(s_srch_ta);
        online_rebuild_page_srch();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        /* 改了输入内容：旧的搜索结果就不算数了 */
        s_srch_has_res = false;
        online_rebuild_page_srch();
    }
}

/*
 * 点列表/空白处 → 收起键盘。顺手重画一次：这样"输入框为空 + 键盘已收起"
 * 三个条件齐了，历史记录会自己回来（见 online_rebuild_page_srch 的规则）。
 * 只挂在列表上（它是输入框的**兄弟**节点）：点输入框本身不会走到这里。
 */
static void srch_hide_ime_clicked(lv_event_t *e)
{
    (void)e;
    if (ime_open()) {
        ime_hide();
        online_rebuild_page_srch();
    }
}

/* 输入法上的对勾 / 回车（组件在"没有未上屏拼音"时发这个事件） */
static void srch_ime_ready(lv_event_t *e)
{
    (void)e;
    const char *txt = s_srch_ta ? lv_textarea_get_text(s_srch_ta) : "";
    if (txt && txt[0]) {
        srch_do_search(txt);
    } else {
        ime_hide();                  /* 输入框是空的：对勾 就是"收起键盘" */
        online_rebuild_page_srch();
    }
}

/* ---------------------------------------------------------------- 切页动画 */

static void slide_exec_cb(void *var, int32_t v)
{
    (void)var;
    s_page_off = v;
    int32_t w = lv_obj_get_width(s_host);
    if (w <= 0) {
        w = lv_display_get_horizontal_resolution(NULL);
    }
    lv_obj_set_x(s_page_pl, -v);
    lv_obj_set_x(s_page_srch, w - v);
}

static void tab_style(lv_obj_t *l, bool active)
{
    lv_obj_set_style_text_color(l, active ? UI_COLOR_ACCENT : UI_COLOR_FG_DIM, LV_PART_MAIN);
}

static void page_show(int which, bool animate)
{
    if (!s_host) {
        return;
    }
    s_page = which ? 1 : 0;
    tab_style(s_tab_pl, s_page == 0);
    tab_style(s_tab_srch, s_page == 1);
    /* 切回"我的歌单"就把键盘收了：别让它继续盖在歌单页上 */
    if (s_page != 1) {
        ime_hide();
    }

    /* 页宽 = 宿主的宽；万一还没 layout（宽 0）就用屏幕宽兜底，别让两页叠一起 */
    int32_t w = lv_obj_get_width(s_host);
    if (w <= 0) {
        w = lv_display_get_horizontal_resolution(NULL);
    }
    const int32_t target = (int32_t)s_page * w;
    if (!animate || target == s_page_off) {
        slide_exec_cb(NULL, target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    /* var 只是 anim 的载体，exec 里按下标同时挪两页 */
    lv_anim_set_var(&a, &s_page);
    lv_anim_set_values(&a, s_page_off, target);
    lv_anim_set_duration(&a, PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, slide_exec_cb);
    lv_anim_start(&a);
}

static void tab_pl_clicked(lv_event_t *e)   { (void)e; page_show(0, true); }
static void tab_srch_clicked(lv_event_t *e) { (void)e; page_show(1, true); }

/* ---------------------------------------------------------------- 浮层：添加歌单 */

/* 浮层与拼音键盘的状态声明已经提到上面（搜索页要用 ime_show_for/ime_hide），
 * 这里只留浮层自身的实现。 */

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
        /* 渐出（面板是遮罩的第一个子对象）：动画结束由 ui_popup_out 自己删。
         * 本函数可能在键盘事件回调里被调，所以这里只"开始淡出"，不阻塞。 */
        ui_popup_out(s_modal, lv_obj_get_child(s_modal, 0));
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

    /* 重命名浮层：就地改名字，不走网络任务 */
    if (s_modal_kind == MODAL_RENAME) {
        if (s_rename_idx >= 0) {
            online_playlist_rename((size_t)s_rename_idx, txt ? txt : "");
            ui_toast("重命名成功", 1200);
            online_rebuild_page_pl();
        }
        s_rename_idx = -1;
        modal_close();
        return;
    }

    if (!txt || !txt[0]) {
        ui_toast("请输入内容", 1200);
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
 * 拼音键盘上的对勾 / 回车 → 组件在"没有未上屏拼音"时发 ready 事件。
 * 语义按当前上下文分派：
 *   - 重命名浮层开着 → 等于"确定"（提交重命名）
 *   - 否则（搜索页）  → 输入框非空就开始搜索，空则收起键盘
 */
static void ime_ready_cb(lv_event_t *e)
{
    if (s_modal && s_modal_kind == MODAL_RENAME) {
        modal_submit(e);
        return;
    }
    srch_ime_ready(e);
}

/* 收起拼音键盘与自带键盘（搜索页失焦、搜索开始时都要收） */
static void ime_hide(void)
{
    if (s_ime) {
        lv_pinyin_ime_hide(s_ime);
    }
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 给某个输入框挂上拼音键盘并显示（键盘只建一次，挂在 lv_layer_top 上） */
static void ime_show_for(lv_obj_t *ta)
{
    if (ta) {
        modal_build_keyboards(ta, true);
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
            /*
             * ★ 宽度必须给**绝对像素**：组件的按键尺寸是 create() 时按父对象宽度
             * 算一次的（IME_UI_UNIT_PX = 可用宽度 / 12）。给百分比 + 刚建出来的
             * 对象还没跑 layout，lv_obj_get_width() 会返回 0 → 组件退化成"按 320
             * 算"，键位只有 ~23 px，就是"键盘太小、不好按"的原因。
             * 给满屏宽 → 约 63 px/键，整块约占半屏。
             */
            lv_obj_set_width(s_ime_box,
                             lv_display_get_horizontal_resolution(NULL) - 2 * UI_PX_W(8));
            lv_obj_set_height(s_ime_box, LV_SIZE_CONTENT);
            lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(2));
            lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_ime_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_update_layout(s_ime_box);      /* ★ 让宽度立刻生效（见上面注释） */
            /*
             * ★ 目标高度必须在 create() 之前设：组件按"fit 高度 ÷ 可见行数"反推出
             * 行高，键盘正好占半屏（IME_FIT_HEIGHT = UI_H/2）。行高随父容器宽度
             * 自适应（按键 flex grow），模式/面板切换时会按新行数重新分摊。
             */
            lv_pinyin_ime_set_fit_height(IME_FIT_HEIGHT);
            s_ime = lv_pinyin_ime_create(s_ime_box);
            if (!s_ime) {
                ESP_LOGW(TAG, "pinyin ime create failed (dict 分区没烧？)");
                ui_toast("拼音输入法不可用。", 2500);
            } else {
                /*对勾 / 回车（无未上屏拼音时触发）→ 搜索或提交重命名 */
                lv_obj_add_event_cb(s_ime, ime_ready_cb, lv_pinyin_ime_event_ready(), NULL);
            }
        }
        if (s_ime) {
            lv_obj_align(s_ime_box, LV_ALIGN_BOTTOM_MID, 0, -UI_PX_H(4));
            lv_pinyin_ime_attach(s_ime, ta);
            lv_pinyin_ime_show(s_ime);
            /*
             * ★ 键盘挂在 lv_layer_top() 上、只建一次；而**每次开浮层都会新建一个
             * scrim**（同样是 layer_top 的子对象，排在后面）—— 于是第二次开始
             * 打开搜索，遮罩就压在键盘之上，键盘看得到却点不动。
             * 把键盘提到最后一个子对象，保证它永远在遮罩之上。
             */
            lv_obj_move_foreground(s_ime_box);
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
    /* 同上：歌单 ID 键盘也可能被后来新建的 scrim 压住 */
    lv_obj_move_foreground(s_kb);
}

/* 建浮层：标题 + 输入框 +（可选拼音键盘）+ 确定/取消 */
static void modal_open(int kind)
{
    if (s_modal) {
        return;
    }
    s_modal_kind = kind;
    const bool search = (kind == OREQ_SEARCH);
    const bool rename = (kind == MODAL_RENAME);

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
    /* 渐入（淡入 + 上滑）：和长按菜单、Toast 用同一套动效 */
    ui_popup_in(scrim, panel);

    lv_obj_t *title = lv_label_create(panel);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(title, rename ? "重命名歌单"
                                   : (search ? "搜索歌曲（歌名/歌手）" : "添加网易云歌单 ID"));

    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, UI_PX_H(48));
    lv_obj_set_style_text_font(ta, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, UI_COLOR_FG, LV_PART_MAIN);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 60);
    lv_textarea_set_placeholder_text(ta, rename ? "输入新的歌单名（可留空恢复显示 ID）"
                                                : (search ? "输入歌名或歌手（可用拼音键盘）"
                                                          : "例如 24381616"));
    s_modal_ta = ta;
    /* 重命名：把当前名字（没有就填 ID）预填进去，用户改两个字就行 */
    if (rename && s_rename_idx >= 0) {
        const char *nm = online_playlist_name((size_t)s_rename_idx);
        const char *id = online_playlist_id((size_t)s_rename_idx);
        lv_textarea_set_text(ta, (nm && nm[0]) ? nm : (id ? id : ""));
    }

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
    lv_label_set_text(ol, rename ? "保存" : (search ? "搜索" : "添加"));
    lv_obj_center(ol);

    /* 重命名可能起中文名 → 也要拼音键盘；只有"歌单 ID"用数字键盘 */
    modal_build_keyboards(ta, search || rename);
}

/* ---------------------------------------------------------------- 顶部按钮 */

static void on_add_clicked(lv_event_t *e)
{
    (void)e;
    modal_open(OREQ_ADD);
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
    case OREQ_ADD:                    /* 添加 / 更新歌单走同一条路径（已存在就是刷新） */
        if (err == ESP_OK) {
            ui_toast("歌单已更新", 1500);
        } else if (err == ESP_ERR_INVALID_STATE) {
            ui_toast("Wi-Fi 未连接", 1800);
        } else {
            /* 把服务里的具体原因带出来（HTTP 400 / 网络错误 / 写 SD 失败…） */
            const char *why = online_last_error();
            char buf[96];
            snprintf(buf, sizeof(buf), "抓取失败：%s", why ? why : "ID 不对或网络不通");
            ui_toast(buf, 2600);
        }
        online_rebuild_page_pl();
        break;
    case OREQ_LOAD:
        if (err == ESP_OK && songs) {
            online_install(songs, count, msg, OWNER_PL);
            ui_toast("歌单已载入", 1200);
        } else {
            free(songs);
            ui_toast("歌单读取失败", 1800);
            s_pl_detail = false;      /* 读不出来就退回歌单列表，别停在空壳上 */
            s_sel_pl = -1;
        }
        online_rebuild_page_pl();
        break;
    case OREQ_SEARCH:
        s_srch_busy = false;         /* 结果到了（或失败了）：退出"正在搜索"态 */
        if (err == ESP_OK && songs) {
            online_install(songs, count, "", OWNER_SRCH);
            s_srch_has_res = true;
            ui_toast("搜索完成", 1200);
        } else {
            free(songs);
            /* 仍然占住"结果"这个状态：显示"没找到"，而不是退回历史列表 */
            s_srch_has_res = true;
            ui_toast("没有找到匹配的歌曲", 1800);
        }
        online_rebuild_page_srch();
        break;
    case OREQ_REMOVE:
        /* 只重画页面：**绝不能**在这里 free(s_songs) —— 播放器可能正在放它，
         * 等下一次 online_install 把它挪进 s_stale 再释放（那才是安全时机）。 */
        online_rebuild_page_pl();
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
        online_rebuild_page_pl();
        online_rebuild_page_srch();
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
    pl_menu_close();
    /*
     * ★ 拼音键盘和自带键盘挂在 lv_layer_top 上，**不归本页所有**（只建一次、
     * 全局复用），所以这里只收起、不能删 —— 删了下次搜索就得重建（还要重新
     * 加载词库，卡一秒）。
     */
    ime_hide();

    s_root = NULL;
    s_tab_pl = NULL;
    s_tab_srch = NULL;
    s_host = NULL;
    s_page_pl = NULL;
    s_page_srch = NULL;
    s_pl_list = NULL;
    s_pl_hint = NULL;
    s_srch_ta = NULL;
    s_srch_hint = NULL;
    s_srch_clear = NULL;
    s_srch_list = NULL;
    s_srch_has_res = false;
    s_pl_detail = false;
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

    /* 标题行（返回键由 ui_router/ui_core 统一叠在顶层，不再每页自己画） */
    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), UI_PX_H(40));
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(title, "网易云音乐");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, UI_HEADER_TEXT_X, 0);

    /* 右上角只剩"添加歌单"：搜索已经是下面的 tab，删除/更新在歌单行的长按菜单里 */
    lv_obj_t *add = lv_button_create(head);
    lv_obj_set_size(add, UI_PX_H(36), UI_PX_H(36));
    lv_obj_set_style_radius(add, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(add, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_add_event_cb(add, on_add_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(add);
    lv_obj_set_style_text_font(al, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(al, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_center(al);
    lv_obj_align(add, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- 顶部两个 tab：文字即标题，点一下切页 ---- */
    lv_obj_t *tabs = lv_obj_create(scr);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_PCT(100), UI_PX_H(42));
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, UI_PX_W(26), LV_PART_MAIN);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);

    s_tab_pl = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_pl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_tab_pl, "我的歌单");
    lv_obj_add_flag(s_tab_pl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_pl, UI_PX_H(12));   /* 文字本身太小，扩大点击区 */
    lv_obj_add_event_cb(s_tab_pl, tab_pl_clicked, LV_EVENT_CLICKED, NULL);

    s_tab_srch = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_srch, UI_FONT_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_tab_srch, "歌曲搜索");
    lv_obj_add_flag(s_tab_srch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_srch, UI_PX_H(12));
    lv_obj_add_event_cb(s_tab_srch, tab_srch_clicked, LV_EVENT_CLICKED, NULL);

    /* ---- 两页的宿主：吃掉剩余高度，并把两页裁剪在可视范围内 ---- */
    s_host = lv_obj_create(scr);
    lv_obj_remove_style_all(s_host);
    lv_obj_set_width(s_host, LV_PCT(100));
    lv_obj_set_flex_grow(s_host, 1);
    lv_obj_remove_flag(s_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_host, LV_OBJ_FLAG_CLICKABLE);

    /* ================= 第 0 页：我的歌单 ================= */
    s_page_pl = lv_obj_create(s_host);
    lv_obj_remove_style_all(s_page_pl);
    lv_obj_set_size(s_page_pl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_page_pl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page_pl, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_page_pl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_page_pl, 0, 0);

    s_pl_hint = lv_label_create(s_page_pl);
    lv_obj_set_style_text_font(s_pl_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pl_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_pl_hint, LV_PCT(100));
    lv_label_set_long_mode(s_pl_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_pl_hint, "");

    s_pl_list = lv_obj_create(s_page_pl);
    lv_obj_remove_style_all(s_pl_list);
    lv_obj_set_width(s_pl_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_pl_list, 1);
    lv_obj_set_style_pad_right(s_pl_list, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_pl_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_pl_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_pl_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_pl_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_pl_list, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_pl_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_pl_list, UI_PX_W(6), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_pl_list, UI_PX_H(3), LV_PART_SCROLLBAR);

    /* ================= 第 1 页：歌曲搜索 ================= */
    s_page_srch = lv_obj_create(s_host);
    lv_obj_remove_style_all(s_page_srch);
    lv_obj_set_size(s_page_srch, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_page_srch, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page_srch, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_page_srch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_page_srch, LV_PCT(100), 0);   /* 初始在右边一屏外 */

    s_srch_ta = lv_textarea_create(s_page_srch);
    lv_obj_set_width(s_srch_ta, LV_PCT(100));
    lv_obj_set_height(s_srch_ta, UI_PX_H(46));
    lv_obj_set_style_text_font(s_srch_ta, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_srch_ta, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_srch_ta, UI_COLOR_FG, LV_PART_MAIN);
    lv_textarea_set_one_line(s_srch_ta, true);
    lv_textarea_set_max_length(s_srch_ta, 60);
    lv_textarea_set_placeholder_text(s_srch_ta, "输入歌名或歌手，按键盘对勾 搜索");
    lv_obj_add_event_cb(s_srch_ta, srch_ta_event, LV_EVENT_ALL, NULL);

    /* 列表小标题（左）+ "清除"（右，只在显示历史时出现） */
    lv_obj_t *srch_head = lv_obj_create(s_page_srch);
    lv_obj_remove_style_all(srch_head);
    lv_obj_set_size(srch_head, LV_PCT(100), UI_PX_H(26));
    lv_obj_remove_flag(srch_head, LV_OBJ_FLAG_SCROLLABLE);

    s_srch_hint = lv_label_create(srch_head);
    lv_obj_set_style_text_font(s_srch_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_srch_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_srch_hint, "历史搜索");
    lv_obj_align(s_srch_hint, LV_ALIGN_LEFT_MID, 0, 0);

    s_srch_clear = lv_label_create(srch_head);
    lv_obj_set_style_text_font(s_srch_clear, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_srch_clear, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(s_srch_clear, "清除");
    lv_obj_add_flag(s_srch_clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_srch_clear, UI_PX_H(10));
    lv_obj_add_event_cb(s_srch_clear, srch_clear_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_srch_clear, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s_srch_clear, LV_OBJ_FLAG_HIDDEN);

    s_srch_list = lv_obj_create(s_page_srch);
    lv_obj_remove_style_all(s_srch_list);
    lv_obj_set_width(s_srch_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_srch_list, 1);
    lv_obj_set_style_pad_right(s_srch_list, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_srch_list, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_srch_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_srch_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_srch_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_srch_list, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_srch_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_srch_list, UI_PX_W(6), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_srch_list, UI_PX_H(3), LV_PART_SCROLLBAR);
    /* 点列表/空白处收键盘（"点别处收起键盘"的手感；输入框自己是列表的兄弟，不受影响） */
    lv_obj_add_flag(s_srch_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_srch_list, srch_hide_ime_clicked, LV_EVENT_CLICKED, NULL);

    /*
     * ★ 首次定位必须在**整屏跑过一次 layout 之后**：页宽取自 lv_obj_get_width(s_host)，
     * 而 s_host 的宽度是父级 flex（flex_grow）算出来的 —— 只 update 它自己拿不到。
     * 没 layout 时宽是 0，两页会叠在一起。
     */
    lv_obj_update_layout(scr);
    s_srch_has_res = false;
    page_show(0, false);
    online_rebuild_page_pl();
    online_rebuild_page_srch();

    lv_obj_add_event_cb(scr, online_on_delete, LV_EVENT_DELETE, NULL);
    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    ESP_LOGI(TAG, "online page created (%u playlist(s))", (unsigned)online_playlist_count());
    return scr;
}

/* ---------------------------------------------------------------- 注册 */

void app_online_register_apps(void)
{
    static const app_desc_t online = {
        "online", "网易云音乐", LV_SYMBOL_WIFI, 0x3E9E86,
        page_online, NULL, NULL, 20, 0,
        &netease_logo,          /* 桌面图标用网易云的 PNG（gen_icon.py 烘成位图） */
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
