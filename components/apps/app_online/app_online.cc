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
#include "ui_ba.h"          /* 碧蓝档案风格弹窗工具箱（对话框/二次确认统一走它） */
#include "ui_app_registry.h"
#include "ui_icons.h"      /* &netease_logo（桌面图标位图，由 PNG 生成） */

#include "app_music.h"
#include "audio_service.h"
#include "online_service.h"
#include "media_meta.h"    /* 二维码 PNG → RGB565（与封面同一套按魔数分派的解码器） */
#include "wifi_service.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

static const char *TAG = "app_online";

/*
 * 工作线程栈。
 * ★ 2026-10 从 8 KB 提到 16 KB：真机上点「扫码登录」崩过一次
 *   `Guru Meditation (Stack protection fault)`，崩在 snprintf，栈指针只越过下界
 *   ~60 字节 —— 也就是 8 KB 本来就贴着边（HTTP 客户端内部 + JSON 解析 + 二维码
 *   路径都要吃栈）。同时把 online_service 里 cookie 压缩的两个大缓冲改成了 static，
 *   两头一起留出余量。任务栈只能用内部 RAM，这里多占 8 KB。
 */
#define WORK_TASK_STACK   (16 * 1024)
#define POLL_MS           200
#define SEARCH_MAX        200          /* 搜索结果上限 */

/*
 * ---------------------------------------------------------------- 列表分页 + 懒加载
 *
 * 需求（2026-10 用户）："歌曲一共有 600 多个，建议做懒加载和按需加载，不要一次把
 * 所有的列表内容加载出来"。两件事都得做，缺一不可：
 *
 *   1) **渲染分页**：每行是 5~6 个 LVGL 对象，600 首就是 3000+ 对象，滚动每帧都要
 *      遍历它们（本地列表就是为此按 120 行分页的，见 AGENTS §12.2）。现在屏幕上一页
 *      最多 ONLINE_PAGE_ROWS 行 + 两行"上一页/下一页"。
 *   2) **网络懒加载**：账号歌单走 `/playlist/track/all`，**每首都是完整 song detail
 *      对象（≈2.3 KB/首）**，一次要 300 首 ≈700 KB 就超单响应上限 NET_JSON_MAX(512 KB)
 *      而整单失败（就是"歌单读取失败"的根因）。现在按 100 首一页，**只取当前页
 *      需要的那一段**。
 *
 * 关键设计：账号歌单的缓冲**按"歌单总首数"一次性分配好，指针从头到尾不变** ——
 * 于是可以"边翻页边往里追加"，而播放器（app_music 存的是同一个指针）永远有效，
 * 不需要任何 realloc/换指针的危险操作。
 *
 * 这几个宏在文件前半就要用到（工作线程按 ACCT_FETCH_MAX 分配），所以放在这里。
 */
#define ONLINE_PAGE_ROWS   100      /* 屏幕上一页显示多少首歌 */
#define ONLINE_SONGS_MAX   1000     /* 单个账号歌单最多载入多少首（≈1.2 MB PSRAM） */
#define ACCT_FETCH_MAX     200      /* 单次"补页"最多取多少首（= 2 个 HTTP 分页） */

#define PG_ACCT   0                 /* 分页行用：账号歌单详情 */
#define PG_PL     1                 /* 分页行用：我添加的歌单详情 */
#define PG_SRCH   2                 /* 分页行用：搜索结果 */

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
    OREQ_QR_KEY,     /* 网易云扫码：取 unikey + 二维码 PNG（无参数） */
    OREQ_QR_POLL,    /* 网易云扫码：轮询，arg = unikey */
    OREQ_LOGOUT,     /* 退出网易云登录 */
    OREQ_ACCOUNT,    /* 取账号信息（昵称） */
    OREQ_ACCT_PLS,   /* 账号歌单列表（worker 内部先取 uid） */
    OREQ_ACCT_SONGS, /* arg = 歌单 ID（账号歌单的歌曲） */
    OREQ_DAILY,      /* 每日推荐歌曲 */
} oreq_kind_t;

typedef struct {
    int  kind;
    char arg[64];
    int  off;        /* OREQ_ACCT_SONGS：从歌单第几首开始取（懒加载分页用） */
    int  cnt;        /* OREQ_ACCT_SONGS：这次要取多少首 */
} oreq_t;

static QueueHandle_t s_req_q;
static TaskHandle_t  s_work_task;
/* 在线页是否活着：工作线程发布结果前看一眼（页面没了就没人消费那份结果了） */
static volatile bool  s_page_alive;

/* 结果槽位（写方是工作线程，读方是 LVGL 定时器；kind 最后写、最先清） */
static volatile int   s_res_kind;
static int            s_res_err;
static online_song_t *s_res_songs;
static size_t         s_res_count;
static char           s_res_msg[64];
/* OREQ_ACCT_SONGS 的附加信息：这页从第几首开始、本来要取多少首
 * （**必须在 work_publish 之前写好** —— kind 是发布标志） */
static int            s_res_off;
static int            s_res_want;

/*
 * 二维码登录的额外负载（work_publish 只带歌单数组，图片/状态码另走这几个）。
 * ⚠ 必须在 work_publish() 写 s_res_kind **之前**赋值 —— kind 是发布标志，
 *   它一置位，LVGL 侧随时可能来读。
 */
static uint8_t      *s_res_img;        /* 二维码 PNG（调用方 free） */
static size_t        s_res_img_len;
static volatile int  s_res_qr;         /* 800/801/802/803 */
static online_acct_playlist_t *s_res_pls;   /* 账号歌单列表（调用方 free） */
static size_t        s_res_pls_n;

/* ---- 第 0 页「我的歌单」：网易云账号页 ---- */
#define ACCT_PL_MAX  200               /* 账号歌单最多显示多少个（自己创建 + 收藏）
                                        * 40 → 200：用户要求"列出我账号里的所有歌单"
                                        * （每个约 390 B，200 个 ≈ 78 KB，PSRAM 够） */
#define ACCT_SONGS_MAX 300             /* **每日推荐**一次最多取多少首（~33 首的量级）。
                                        * 账号歌单不再用这个上限：它走懒加载 + 分页
                                        * （见 ONLINE_SONGS_MAX / ONLINE_PAGE_ROWS） */
static lv_obj_t *s_page_acct;          /* 页容器 */
static lv_obj_t *s_acct_back;          /* 详情页的"‹ 返回"行（**固定**在列表上方，见下） */
static lv_obj_t *s_tab_acct;           /* 顶部页签 */
static lv_obj_t *s_acct_hint;          /* 顶部说明行 */
static lv_obj_t *s_acct_list;          /* 行容器（每日推荐 / 歌单行 / 歌曲行） */
static bool      s_acct_detail;        /* true = 正在看某个账号歌单的歌曲列表 */
static bool      s_acct_daily;         /* 详情来源：true = 每日推荐 */
static int       s_acct_sel = -1;      /* 详情对应的歌单下标 */
static online_acct_playlist_t *s_acct_pls;   /* 账号歌单列表（LVGL 侧持有） */
static size_t    s_acct_pls_n;
static bool      s_acct_pls_asked;     /* 已经拉过一次歌单列表 */
/*
 * "正在拉账号歌单/昵称"的中间态 + 失败后的节流重试。
 *
 * ★ 为什么必须能重试（用户 2026-10 报"重启后进在线页，歌单什么的都没加载出来"）：
 *   原先 s_acct_pls_asked 是**一次性**标志 —— 开机后进页面时 Wi-Fi 可能还没连上，
 *   那一次请求失败之后这个标志仍然是 true，poll_cb 里"网络状态变了就重建页面"
 *   也拉不动它，于是整轮开机都停在"还没取到歌单"。现在失败后允许按 5 秒节流重试。
 */
static bool      s_acct_pls_busy;      /* 请求在路上（列表区显示转圈） */
static uint32_t  s_acct_pls_try_tick;  /* 上次请求**结束**的时刻 */
static uint32_t  s_acct_nick_try_tick; /* 上次取昵称**结束**的时刻 */
/* 重试间隔：失败一次翻一倍，最多 ACCT_RETRY_MAX_MS —— 服务端一直不通/ cookie 失效时
 * 不至于每 5 秒打一次（那种情况可能持续几小时）。成功或重新进页面时复位。 */
#define ACCT_RETRY_MS       5000
#define ACCT_RETRY_MAX_MS  60000
static uint32_t  s_acct_backoff_ms = ACCT_RETRY_MS;

/* ---- 二维码登录浮层状态（LVGL 侧） ---- */
#define QR_POLL_MS      1500           /* 轮询间隔：服务端语义是"扫码/确认"事件，1.5 s 够灵敏 */
#define QR_IMG_MAX_DIM  240            /* 二维码显示尺寸（要能被相机清晰识别，别缩太小） */
static lv_obj_t     *s_qr_scrim;       /* 全屏遮罩（浮层根） */
static ui_ba_dialog_t s_qr_dlg;        /* BA 风格对话框句柄（与 s_qr_scrim 同生共死） */
static lv_obj_t     *s_qr_img;         /* 二维码图片 */
static lv_image_dsc_t *s_qr_dsc;       /* 图片描述符（堆上，随浮层释放） */
static uint8_t      *s_qr_px;          /* 解码后的 RGB565（media_meta_cover_free 释放） */
static lv_obj_t     *s_qr_status;      /* 状态行：等待扫码 / 已扫码待确认 / 已失效… */
static lv_timer_t   *s_qr_timer;       /* 轮询定时器 */
static char          s_qr_key[64];     /* 当前 unikey */
static char          s_login_name[64]; /* 已登录账号昵称（空 = 未知/未登录） */
static bool          s_acct_asked;     /* 已经拉过一次账号信息（避免反复请求） */

/* ---------------- 二维码登录：实现 ----------------
 *
 * 浮层一律从**点击回调**里打开（不是在按压期间建）：按压期间建的遮罩会被同一轮
 * re-hit-test 命中，松手那一下无条件发的 CLICKED 会把刚弹出的浮层自己关掉
 * —— 这就是 §14.6 记的那个坑，别在新弹层上重犯。
 */
static void work_submit(int kind, const char *arg);   /* 前向声明：按钮回调要用 */
/* 带分页参数（off/cnt）的版本：账号歌单懒加载用 */
static void work_submit_ex(int kind, const char *arg, int off, int cnt);
/* 前向声明：账号状态行/账号页要用（定义在"账号页"那一节） */
static void acct_nick_kick_if_needed(void);
static void login_name_sync(void);

static void qr_status_set(const char *txt)
{
    if (s_qr_status) {
        lv_label_set_text(s_qr_status, txt ? txt : "");
    }
}

static void qr_stop_poll(void)
{
    if (s_qr_timer) {
        lv_timer_delete(s_qr_timer);
        s_qr_timer = NULL;
    }
}

/* 二维码 PNG → RGB565 → 贴到浮层（复用 media_meta 的解码器，呼叫方传入的 PNG 由本函数释放） */
static void qr_show_png(uint8_t *png, size_t len)
{
    uint8_t *px = NULL;
    int w = 0;
    int h = 0;
    const esp_err_t err = media_meta_image_mem_rgb565(png, len, QR_IMG_MAX_DIM, &px, &w, &h);
    free(png);
    if (err != ESP_OK || !px) {
        ESP_LOGW(TAG, "qr png decode failed: %s", esp_err_to_name(err));
        qr_status_set("二维码解析失败，点「刷新」重试");
        return;
    }

    if (s_qr_px) {
        media_meta_cover_free(s_qr_px);
        s_qr_px = NULL;
    }
    if (s_qr_dsc) {
        free(s_qr_dsc);
        s_qr_dsc = NULL;
    }
    lv_image_dsc_t *dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
    if (!dsc) {
        media_meta_cover_free(px);
        return;
    }
    s_qr_px = px;
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = (uint32_t)w;
    dsc->header.h      = (uint32_t)h;
    dsc->header.stride = (uint32_t)w * 2u;
    dsc->data_size     = (uint32_t)w * 2u * (uint32_t)h;
    dsc->data          = px;
    s_qr_dsc = dsc;
    if (s_qr_img) {
        lv_image_set_src(s_qr_img, dsc);
    }
    ESP_LOGI(TAG, "qr image %dx%d", w, h);
}

static void qr_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_qr_key[0]) {
        work_submit(OREQ_QR_POLL, s_qr_key);
    }
}

static void qr_close(void)
{
    qr_stop_poll();
    s_qr_key[0] = '\0';
    /* 下滑 + 缩小 + 淡出（ui_ba），动画结束由 ui_popup_out 自己删掉整块浮层 */
    ui_ba_close(&s_qr_dlg);
    s_qr_scrim = NULL;
    s_qr_img = NULL;
    s_qr_status = NULL;
    if (s_qr_dsc) {
        free(s_qr_dsc);
        s_qr_dsc = NULL;
    }
    if (s_qr_px) {
        media_meta_cover_free(s_qr_px);
        s_qr_px = NULL;
    }
}

static void qr_cancel_clicked(lv_event_t *e)
{
    (void)e;
    qr_close();
}

static void qr_refresh_clicked(lv_event_t *e)
{
    (void)e;
    qr_stop_poll();
    s_qr_key[0] = '\0';
    qr_status_set("正在获取二维码…");
    work_submit(OREQ_QR_KEY, NULL);
}

static void qr_open(void)
{
    if (s_qr_scrim) {
        return;                          /* 已经开着：绝不留第二块遮罩（孤儿遮罩会吃掉所有点击） */
    }

    /* 碧蓝档案风格白卡片：标题 + 黄色下划线 + 右上角 ✕（见 ui_ba.h） */
    s_qr_dlg = ui_ba_open("扫码登录", 46, LV_ALIGN_CENTER, 0);
    s_qr_scrim = s_qr_dlg.scrim;
    /* ✕ / 点空白 都走同一个关闭路径（要顺手停轮询、释放二维码内存） */
    ui_ba_on_close(&s_qr_dlg, [](void *user) { (void)user; qr_close(); }, NULL);
    ui_ba_scrim_dismissible(&s_qr_dlg, true);

    ui_ba_hint(s_qr_dlg.body, "用网易云音乐 App 扫码，即可获取你的账号歌单与每日推荐");

    s_qr_img = lv_image_create(s_qr_dlg.body);
    lv_obj_set_size(s_qr_img, UI_PX_H(QR_IMG_MAX_DIM), UI_PX_H(QR_IMG_MAX_DIM));
    /* 图还没到时给个占位底色：面板高度不跳变，用户也知道"正在出图" */
    lv_obj_set_style_bg_color(s_qr_img, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_qr_img, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_qr_img, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_qr_img, UI_PX_H(6), LV_PART_MAIN);

    s_qr_status = lv_label_create(s_qr_dlg.body);
    lv_obj_set_style_text_font(s_qr_status, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_qr_status, lv_color_hex(BA_TEXT_BODY), LV_PART_MAIN);
    lv_label_set_text(s_qr_status, "正在获取二维码…");

    ui_ba_btn(s_qr_dlg.btn_row, "刷新二维码", false, NULL, qr_refresh_clicked, NULL);
    ui_ba_btn(s_qr_dlg.btn_row, "取消", true, NULL, qr_cancel_clicked, NULL);

    work_submit(OREQ_QR_KEY, NULL);      /* 打开即取二维码（工作线程里两步合并） */
}

static void login_clicked(lv_event_t *e)
{
    (void)e;
    qr_open();
}

/* 「退出登录」的二次确认：清 cookie 是不可逆的（要重新扫码），必须问一次 */
static void logout_confirmed(void *user)
{
    (void)user;
    work_submit(OREQ_LOGOUT, NULL);
}

static void logout_clicked(lv_event_t *e)
{
    (void)e;
    ui_ba_confirm_ex("退 出 登 录", "确认退出网易云账号吗？\n退出后需要重新扫码登录。",
                     "退出", "取消", true, logout_confirmed, NULL);
}

/* 账号状态行：未登录 = 「扫码登录」入口；已登录 = 昵称 + 「退出登录」 */
static void acct_row_build(lv_obj_t *col)
{
    const bool in = online_netease_logged_in();
    /* 昵称先用 NVS 里存的那份（重启后不用等 /user/account 往返） */
    login_name_sync();
    if (in) {
        acct_nick_kick_if_needed();      /* 还没有昵称就取一次；失败会节流重试 */
    }

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *txt = lv_label_create(row);
    lv_obj_set_style_text_font(txt, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, in ? UI_COLOR_ACCENT : UI_COLOR_FG, LV_PART_MAIN);
    if (in) {
        lv_label_set_text_fmt(txt, "已登录：%.24s",
                              s_login_name[0] ? s_login_name : "网易云账号");
    } else {
        lv_label_set_text(txt, "未登录网易云");
    }
    lv_obj_align(txt, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *btn = lv_label_create(row);
    lv_obj_set_style_text_font(btn, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(btn, in ? "退出登录" : "扫码登录");
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, UI_PX_H(12));
    lv_obj_add_event_cb(btn, in ? logout_clicked : login_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void work_publish(int kind, esp_err_t err, online_song_t *songs, size_t count, const char *msg)
{
    /*
     * 页面已经关了（用户退出本 APP）：这份结果没人会来取 —— 就地释放，别留在槽位里。
     * 不清的话：数组/二维码/歌单列表泄漏，而且工作线程下一轮要白等 3 秒。
     * （页面的 DELETE 回调里也会清一次，那是"结果已经发布、人走了"的情况。）
     */
    if (!s_page_alive) {
        free(songs);
        free(s_res_img);
        s_res_img     = NULL;
        s_res_img_len = 0;
        free(s_res_pls);
        s_res_pls   = NULL;
        s_res_pls_n = 0;
        s_res_kind  = OREQ_NONE;
        return;
    }
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
        /*
         * ★ 结果槽位只有一份：先等 LVGL 侧的 200 ms 轮询把**上一份**搬走，
         *   再开始下一个请求。否则两份结果会撞在一起（后一份覆盖前一份）：
         *   被覆盖的那个数组/图片既不会被界面用上、也没人 free（泄漏），
         *   表现是偶发的"点了歌单没反应 / 歌单列表不刷新 / 搜索卡在转圈"。
         *   最多等 3 秒（万一 LVGL 侧正忙），超时就照旧发布，不让工作线程卡死。
         */
        for (int i = 0; i < 300 && s_res_kind != OREQ_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
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
        case OREQ_QR_KEY: {
            /* 取 unikey + 二维码 PNG 合并成一次工作线程往返（服务端两步都是普通 GET） */
            char key[64] = {0};
            uint8_t *img = NULL;
            size_t img_len = 0;
            esp_err_t err = online_netease_qr_key(key, sizeof(key));
            if (err == ESP_OK) {
                err = online_netease_qr_create(key, NULL, 0, &img, &img_len);
            }
            s_res_img     = img;          /* 必须在 work_publish 之前赋值（kind 是发布标志） */
            s_res_img_len = img_len;
            work_publish(OREQ_QR_KEY, err, NULL, 0, key);
            break;
        }
        case OREQ_QR_POLL: {
            int code = 0;
            esp_err_t err = online_netease_qr_poll(req.arg, &code);
            s_res_qr = code;
            work_publish(OREQ_QR_POLL, err, NULL, 0, NULL);
            break;
        }
        case OREQ_LOGOUT: {
            esp_err_t err = online_netease_logout();
            work_publish(OREQ_LOGOUT, err, NULL, 0, NULL);
            break;
        }
        case OREQ_ACCOUNT: {
            online_account_t acct = {};
            esp_err_t err = online_netease_account(&acct);
            work_publish(OREQ_ACCOUNT, err, NULL, 0, (err == ESP_OK) ? acct.nickname : NULL);
            break;
        }
        case OREQ_ACCT_PLS: {
            /* 先拿 uid（顺带验证 cookie 还有效），再拉歌单列表 */
            online_account_t acct = {};
            esp_err_t err = online_netease_account(&acct);
            online_acct_playlist_t *pls = NULL;
            size_t n = 0;
            if (err == ESP_OK) {
                pls = (online_acct_playlist_t *)calloc(ACCT_PL_MAX, sizeof(*pls));
                if (pls) {
                    err = online_netease_account_playlists(acct.uid, pls, ACCT_PL_MAX, &n);
                } else {
                    err = ESP_ERR_NO_MEM;
                }
            }
            s_res_pls   = pls;            /* 同样要在 work_publish 之前赋值 */
            s_res_pls_n = n;
            work_publish(OREQ_ACCT_PLS, err, NULL, 0, acct.nickname);
            break;
        }
        case OREQ_ACCT_SONGS: {
            /*
             * **只取一页**（懒加载）：req.off 起、req.cnt 首。
             * 服务端这个接口每首是完整 song detail（≈2.3 KB/首），一次取太多就超
             * NET_JSON_MAX(512 KB) 而整单失败 —— 所以 app_online 只按"当前页需要"
             * 下单，这里再按 100 首一页去循环（见 online_netease_playlist_page）。
             * 失败时也把 arr 交给 work_publish：LVGL 侧的失败分支会 free 它。
             */
            const size_t want = (req.cnt > 0) ? (size_t)req.cnt : ACCT_FETCH_MAX;
            online_song_t *arr = (online_song_t *)calloc(want, sizeof(*arr));
            size_t n = 0;
            esp_err_t err = arr ? online_netease_playlist_page(req.arg, (size_t)req.off,
                                                               arr, want, &n)
                                : ESP_ERR_NO_MEM;
            s_res_off  = req.off;           /* 必须在 work_publish 之前写好 */
            s_res_want = (int)want;
            work_publish(OREQ_ACCT_SONGS, err, arr, n, req.arg);
            break;
        }
        case OREQ_DAILY: {
            online_song_t *arr = (online_song_t *)calloc(ACCT_SONGS_MAX, sizeof(*arr));
            size_t n = 0;
            esp_err_t err = arr ? online_netease_daily_songs(arr, ACCT_SONGS_MAX, &n)
                                : ESP_ERR_NO_MEM;
            work_publish(OREQ_DAILY, err, arr, n, "");
            break;
        }
        default:
            break;
        }
    }
}

static void work_submit_ex(int kind, const char *arg, int off, int cnt)
{
    if (!s_req_q) {
        ui_toast("在线服务未就绪", 1500);
        return;
    }
    oreq_t req = {};
    req.kind = kind;
    req.off  = off;
    req.cnt  = cnt;
    if (arg) {
        snprintf(req.arg, sizeof(req.arg), "%s", arg);
    }
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        ui_toast("上一个操作还没做完", 1500);
    }
}

static void work_submit(int kind, const char *arg)
{
    work_submit_ex(kind, arg, 0, 0);
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
static lv_obj_t *s_pl_back;      /* 详情页的"‹ 歌单列表"行（**固定**在列表上方） */
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
enum { OWNER_PL = 0, OWNER_SRCH = 1, OWNER_ACCT = 2 };
static int  s_owner = OWNER_PL;

/*
 * s_songs 属于"哪一个详情"。歌单详情页只画**自己那一份**；Key 对不上就显示转圈。
 *
 * ★ 这是"点进歌单先看到转圈、加载成功才显示歌单"的关键（用户 2026-10 报
 *   "播放列表的歌曲我挨个点开了怎么都是同样的"）：原先只判
 *   `s_owner == OWNER_PL && s_song_n > 0`，而点进第二个歌单时 s_songs 里还是
 *   **上一个歌单**的歌（owner 也还是 OWNER_PL），于是列表先把上一个歌单的歌顶上来
 *   —— 用户看到、点到的全是上一个歌单的歌。现在用"歌单 ID"当身份：
 *     meting 歌单 → 歌单 ID
 *     账号歌单   → 账号歌单 ID
 *     每日推荐   → DETAIL_KEY_DAILY（不是歌单，给个专用标记）
 *     搜索结果   → 空串（详情页不该画它）
 */
/*
 * ⚠ 十六进制转义后面紧跟 'd'（合法的十六进制字符）会被**并进同一个转义**里：
 *   `"\x01daily"` 等于 `\x01d` + "aily" —— `\x01d` 超出一个字节，GCC 直接
 *   `error: hex escape sequence out of range`（-Werror 下编译不过）。
 *   标准写法是拆成两个字符串字面量，让转义在 `\x01` 处结束。
 */
#define DETAIL_KEY_DAILY  "\x01" "daily"
static char s_cur_pl_id[ONLINE_ID_MAX];

/* 详情页要的那份 key（歌单 ID / 每日推荐标记）；返回空串表示"没有明确目标" */
static const char *detail_want_key(void)
{
    if (s_pl_detail && s_sel_pl >= 0) {
        const char *id = online_playlist_id((size_t)s_sel_pl);
        return id ? id : "";
    }
    if (s_acct_detail) {
        if (s_acct_daily) {
            return DETAIL_KEY_DAILY;
        }
        if (s_acct_sel >= 0 && (size_t)s_acct_sel < s_acct_pls_n) {
            return s_acct_pls[s_acct_sel].id;
        }
    }
    return "";
}

/* 当前缓冲是不是"这个详情要的那一份"（**懒加载刚开的空壳也算**：一首都没载入） */
static bool detail_key_matches(void)
{
    const char *want = detail_want_key();
    return (want[0] && s_cur_pl_id[0] && s_songs != NULL && strcmp(s_cur_pl_id, want) == 0);
}

/* 现在内存里这份 s_songs 是 detail_want_key() 要的，**而且已经有歌可画** */
static bool detail_owns_songs(void)
{
    return (s_song_n > 0 && detail_key_matches());
}

/*
 * ---------------------------------------------------------------- 列表分页 + 懒加载
 *
 * 宏（ONLINE_PAGE_ROWS / ONLINE_SONGS_MAX / ACCT_FETCH_MAX / PG_*）定义在文件头部，
 * 因为工作线程也要用 ACCT_FETCH_MAX。这里只放运行期状态。
 */
static int    s_acct_page;          /* 账号歌单详情当前页号（0 起） */
static int    s_pl_page;            /* 我添加的歌单详情当前页号 */
static int    s_srch_page;          /* 搜索结果当前页号 */
static size_t s_songs_cap;          /* s_songs 的槽位数（懒加载缓冲的容量） */
static size_t s_songs_total;        /* 歌单总首数（账号歌单已知；0 = 未知） */
static bool   s_acct_lazy;          /* s_songs 是账号歌单的懒加载缓冲（可增量追加） */
static bool   s_fetch_busy;         /* 正在补页 */
static bool   s_fetch_fail;         /* 上次补页失败：给"重试"出口，别自己死循环 */

/* 第 page 页的第一首在 s_songs 里的**绝对下标**（点行即播/懒加载都要用它） */
static size_t page_first(int page)
{
    return (page > 0) ? (size_t)page * ONLINE_PAGE_ROWS : 0;
}

static int page_count(size_t n)
{
    if (n == 0) {
        return 1;                       /* 空列表也算一页，免得页码乱跳 */
    }
    return (int)((n + ONLINE_PAGE_ROWS - 1) / ONLINE_PAGE_ROWS);
}

static int page_clamp(int page, size_t n)
{
    const int pages = page_count(n);
    if (page < 0) {
        return 0;
    }
    return (page >= pages) ? (pages - 1) : page;
}

/*
 * 账号歌单详情的页数。
 * 总首数已知（正常情况）就是 page_count(total)；
 * **未知时（服务端没给 trackCount）要额外小心**：如果最后一页正好填满，也必须多给
 * 一页 —— 否则用户翻到"第 1/1 页"就再也点不动，只能看到前 100 首。
 * 那一页点下去会去取；服务端没有更多歌时返回空页，结果处理里按"到末尾"收尾
 * （s_songs_total = s_song_n），多出来的这一页自己消失。
 */
static int acct_page_count(void)
{
    if (s_songs_total) {
        return page_count(s_songs_total);
    }
    const int pages = page_count(s_song_n);
    if (s_song_n > 0 && (s_song_n % ONLINE_PAGE_ROWS) == 0 && s_song_n < s_songs_cap) {
        return pages + 1;
    }
    return pages;
}

/*
 * 详情请求的"发出时刻"：转圈超过 DETAIL_TIMEOUT_MS 还没结果就换成一个"重试"入口
 * （网络很慢、或者请求根本没被工作线程收下时，不至于一直转到天荒地老）。
 * 20 秒 > HTTP_TIMEOUT_MS(15 s)，不会把"正常的慢请求"误判成超时。
 * 占位界面本体在 detail_wait_rows()（定义在 loading_rows 之后）。
 */
#define DETAIL_TIMEOUT_MS  20000
static uint32_t s_detail_tick;

static void detail_mark_request(void)
{
    s_detail_tick = lv_tick_get();
}

/* ---- 浮层（添加歌单 / 重命名）与拼音键盘（懒建、全局复用） ----
 * 声明放在这里（而不是浮层那段代码旁边），因为上面的搜索页要用到
 * ime_show_for()/ime_hide()/ime_open()。 */
static lv_obj_t *s_modal;        /* 当前浮层（BA 对话框的 scrim） */
static ui_ba_dialog_t s_modal_dlg;   /* BA 对话框句柄（与 s_modal 同生共死） */
static lv_obj_t *s_modal_ta;     /* 浮层里的输入框 */
static lv_obj_t *s_ime;          /* 拼音键盘（懒建，全局复用） */
static lv_obj_t *s_ime_box;      /* 拼音键盘的容器（宽度决定按键大小） */
static lv_obj_t *s_kb;           /* 歌单 ID 用的普通键盘（LVGL 自带，带数字） */
static int       s_modal_kind;   /* OREQ_ADD / MODAL_RENAME */

#define MODAL_RENAME   (-100)    /* 只在本文件用：重命名浮层（不是网络请求） */

#define PAGE_ANIM_MS  220        /* 两页左右滑动的动画时长（与 ui_router 同节奏） */

static void online_rebuild_page_pl(void);
static void online_rebuild_page_srch(void);
static void online_rebuild_page_acct(void);
static void page_show(int which, bool animate);
static void modal_open(int kind);
static void modal_submit(lv_event_t *e);
static void srch_do_search(const char *kw);
static void srch_ime_ready(lv_event_t *e);
static void ime_show_for(lv_obj_t *ta);
static void ime_hide(void);
/* 定义在浮层那一段，但上面的 ime_show_for() 要用它 */
static void modal_build_keyboards(lv_obj_t *ta, bool search);

/*
 * 回收"被替换下来的旧队列"（s_stale）。
 *
 * ★ **绝不在这里 free**（2026-10 修 use-after-free）：播放器可能还存着这个数组的
 *   指针（用户点歌之后又打开别的歌单时，app_music 的 s_on_songs 仍指着它），
 *   直接 free 会让上/下一首、自动续播读到已释放内存。所有权交给 app_music：
 *   它在换队列/退回本地播放时自己放（见 app_music_online_detach）。
 */
static void online_drop_stale(void)
{
    if (s_stale) {
        app_music_online_detach(s_stale);
        s_stale = NULL;
    }
}

/*
 * 换掉当前队列 s_songs。
 *
 * ★ 老的那份**绝不在这里 free**（2026-10 修 use-after-free）：播放器可能还存着它的
 *   指针（用户点歌之后又打开别的歌单时，app_music 的 s_on_songs 仍指着它），
 *   直接 free 会让上/下一首、自动续播读到已释放内存。统一走 s_stale →
 *   online_drop_stale() → app_music_online_detach()：播放器还在用就由它接管
 *   （它换队列/退回本地播放时自己放），没用就立刻放掉。
 */
static void queue_replace(online_song_t *buf, size_t cap, size_t count,
                          const char *key, int owner)
{
    online_drop_stale();
    if (s_songs && s_songs != buf) {
        s_stale = s_songs;
    }
    s_songs       = buf;
    s_song_n      = count;
    s_songs_cap   = cap;
    s_owner       = owner;
    /* 懒加载/分页状态跟着换队列一起复位 */
    s_acct_lazy   = false;
    s_songs_total = 0;
    s_fetch_busy  = false;
    s_fetch_fail  = false;
    /*
     * 歌单 ID 的契约长度是 ONLINE_ID_MAX（网易云是数字 ID，服务端存 NVS 时也按
     * 这个长度截断），而这里拿到的 pl_id 来自 64 字节的请求字段 —— 按契约精度拷贝，
     * 超长的本来就不是合法 ID（不按契约截断的话，s_cur_pl_id 会比服务侧存的 ID 长，
     * 后面 strcmp 对不上，UI 上"当前歌单"的高亮就丢了）。
     */
    snprintf(s_cur_pl_id, sizeof(s_cur_pl_id), "%.*s",
             (int)(sizeof(s_cur_pl_id) - 1), key ? key : "");
}

/* 安装一份**整份**队列（meting 歌单 / 搜索 / 每日推荐的结果） */
static void online_install(online_song_t *songs, size_t count, const char *pl_id, int owner)
{
    queue_replace(songs, count, count, pl_id, owner);
}

/* ------------------------------------------------- 账号歌单：懒加载缓冲（见文件头） */

/*
 * 打开一个账号歌单：按"歌单总首数"一次性分配好缓冲（**指针从此不变** ——
 * 播放器的队列指针一直有效），但**先不填**，由 acct_detail_fetch_if_needed()
 * 按当前页的需要去取（懒加载）。
 *
 * total = 0 表示"服务端没给 trackCount"：那就按最大容量分配（宁可多占点 PSRAM，
 * 也不能让用户只能看到前 100 首）；真实总数会在第一次"取不满一页"时被修正。
 */
static bool acct_lazy_begin(const char *pl_id, size_t total)
{
    size_t cap = total ? total : ONLINE_SONGS_MAX;
    if (cap > ONLINE_SONGS_MAX) {
        cap = ONLINE_SONGS_MAX;
    }
    if (cap < ONLINE_PAGE_ROWS) {
        cap = ONLINE_PAGE_ROWS;
    }
    online_song_t *buf = (online_song_t *)calloc(cap, sizeof(online_song_t));
    if (!buf) {
        ESP_LOGW(TAG, "acct buffer alloc failed (%u slots)", (unsigned)cap);
        return false;
    }
    queue_replace(buf, cap, 0, pl_id, OWNER_ACCT);
    s_acct_lazy   = true;
    s_songs_total = total;
    ESP_LOGI(TAG, "acct lazy buffer: %u slots (playlist trackCount=%u)",
             (unsigned)cap, (unsigned)total);
    return true;
}

/* 把刚取到的一页**追加到已载入末尾**（必须连续，否则丢弃：说明结果过期了） */
static void acct_lazy_append(size_t off, const online_song_t *page, size_t n)
{
    if (!s_acct_lazy || !s_songs || n == 0) {
        return;
    }
    if (off != s_song_n || off + n > s_songs_cap) {
        ESP_LOGW(TAG, "drop page (off=%u n=%u loaded=%u cap=%u)",
                 (unsigned)off, (unsigned)n, (unsigned)s_song_n, (unsigned)s_songs_cap);
        return;
    }
    memcpy(&s_songs[off], page, n * sizeof(online_song_t));
    s_song_n = off + n;
}

/*
 * 懒加载的唯一入口：详情页"当前页还缺歌"时补一段（一次最多 ACCT_FETCH_MAX 首）。
 * 结果到了会重新渲染；如果还是不够，渲染时再调一次 —— 于是它天然是"按需"的，
 * 用户翻到第几页才取到第几页。
 */
static void acct_detail_fetch_if_needed(void)
{
    if (!s_acct_lazy || !s_songs || !s_acct_detail) {
        return;
    }
    if (s_fetch_busy || s_fetch_fail) {
        return;
    }
    if (s_acct_sel < 0 || (size_t)s_acct_sel >= s_acct_pls_n) {
        return;
    }
    /* 目标：当前页要显示到 (page+1) * ONLINE_PAGE_ROWS 首 */
    size_t need = page_first(s_acct_page) + ONLINE_PAGE_ROWS;
    if (s_songs_total && need > s_songs_total) {
        need = s_songs_total;
    }
    if (s_song_n >= need) {
        return;                             /* 已经够了（或本来就到头了） */
    }
    size_t want = need - s_song_n;
    if (want > ACCT_FETCH_MAX) {
        want = ACCT_FETCH_MAX;
    }
    if (s_song_n + want > s_songs_cap) {
        want = s_songs_cap - s_song_n;
    }
    if (want == 0) {
        return;
    }
    s_fetch_busy = true;
    detail_mark_request();                  /* 超时兜底从这次请求算起 */
    ESP_LOGI(TAG, "acct lazy fetch: '%s' offset=%u count=%u",
             s_acct_pls[s_acct_sel].id, (unsigned)s_song_n, (unsigned)want);
    work_submit_ex(OREQ_ACCT_SONGS, s_acct_pls[s_acct_sel].id, (int)s_song_n, (int)want);
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

/*
 * 左右滑动切页。
 *
 * 为什么挂 GESTURE 而不是自己算 PRESSED/RELEASED 位移：LVGL 只在"被按的对象
 * **无法**在该方向滚动"时才发 LV_EVENT_GESTURE，而发过 gesture 之后它会置
 * gesture_sent，**松手不再补发 CLICKED** —— 正好避免"滑一下顺手把歌点开了"。
 * 本页的三个列表都只允许纵向滚动（LV_DIR_VER），所以列表上的横滑会冒到这里。
 */
static void pager_gesture(lv_event_t *e)
{
    (void)e;
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        page_show(s_page + 1, true);      /* 手指向左划 → 看右边那一页 */
    } else if (dir == LV_DIR_RIGHT) {
        page_show(s_page - 1, true);      /* 手指向右划 → 看左边那一页 */
    }
}

/* 每个列表行都挂一份：屏幕绝大部分面积是行，横滑多从这里起手 */
static void pager_attach(lv_obj_t *o)
{
    if (o) {
        lv_obj_add_event_cb(o, pager_gesture, LV_EVENT_GESTURE, NULL);
    }
}

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
    pager_attach(row);               /* 横滑切页（见 pager_gesture） */

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

/* 歌曲列表（歌单详情 / 搜索结果共用）：画 s_songs[first .. first+count) 这一段 */
static void fill_song_rows_range(lv_obj_t *list, size_t first, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const size_t idx = first + i;
        lv_obj_t *row = make_row(list, LV_SYMBOL_AUDIO, 0x3E9E86,
                                 s_songs[idx].title, s_songs[idx].artist);
        lv_obj_add_event_cb(row, online_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    }
}

/* 列表内容整段换掉之后回到顶部。
 * 列表是 lv_obj_clean + 重建的，滚动位置会留在原处 —— 进详情/翻页后若不归零，
 * 新内容会从中间开始显示（用户看到的像是"漏了前几首"）。 */
static void list_scroll_top(lv_obj_t *list)
{
    if (list) {
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
}

/* 翻页行的点击：user_data = (which << 1) | dir（dir: 0 = 上一页, 1 = 下一页） */
static void page_row_clicked(lv_event_t *e)
{
    const int v = (int)(intptr_t)lv_event_get_user_data(e);
    const int which = v >> 1;
    const int dir = (v & 1) ? +1 : -1;
    lv_obj_t *list = NULL;
    if (which == PG_ACCT) {
        s_acct_page += dir;
        online_rebuild_page_acct();
        list = s_acct_list;
    } else if (which == PG_PL) {
        s_pl_page += dir;
        online_rebuild_page_pl();
        list = s_pl_list;
    } else {
        s_srch_page += dir;
        online_rebuild_page_srch();
        list = s_srch_list;
    }
    list_scroll_top(list);
}

/* "‹ 上一页 / 下一页 ›"两行（页码提示由各页自己写在 hint 里） */
static void page_rows(lv_obj_t *list, int which, int page, int pages)
{
    if (page > 0) {
        lv_obj_t *r = make_row(list, LV_SYMBOL_LEFT, 0x5B6FB5, "上一页", NULL);
        lv_obj_add_event_cb(r, page_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((which << 1) | 0));
    }
    if (page + 1 < pages) {
        lv_obj_t *r = make_row(list, LV_SYMBOL_RIGHT, 0x5B6FB5, "下一页", NULL);
        lv_obj_add_event_cb(r, page_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((which << 1) | 1));
    }
}

/*
 * 详情页顶部的"‹ 返回上一级"行。
 *
 * ★ 必须由**页容器**（s_page_acct / s_page_pl）持有，**不能放进可滚动的列表里**：
 *   2026-10 用户报"歌单的返回按钮为什么会跟着滑动？导致无法快速返回上一级" ——
 *   返回行原来是 s_*_list 的子对象，而列表是 flex_grow + LV_DIR_VER 可滚动的，
 *   一往下翻歌就把返回入口滚出屏幕外了，只能滑回顶部才能点。
 *   现在它是页面 flex 列里的**第一行**（在 hint 之上），列表滚动与它无关。
 *
 * 隐藏时 flex 会自动跳过它（lv_flex.c 里 `LV_OBJ_FLAG_HIDDEN` 的对象不参与布局），
 * 所以"歌单列表"状态下不会留一条空白。
 */
static lv_obj_t *make_back_label(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, UI_PX_H(14));   /* 手指点得准一点 */
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);      /* 默认藏起来，进详情才显示 */
    return l;
}

/*
 * "等东西"的统一视觉：无限转圈 + 一行字。
 * lv_spinner 默认就是无限循环（LV_SPINNER_TYPE_SPINNING_ARC），不用自己起 timer。
 * 歌单详情 / 账号歌单 / 搜索 / 账号页都走它，四个地方长得一样。
 */
static void loading_rows(lv_obj_t *list, const char *text)
{
    lv_obj_t *box = lv_obj_create(list);
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
    lv_label_set_text(h, text ? text : "载入中…");
}

/*
 * 详情页"还没拿到自己那份数据"时的占位：转圈；超过 DETAIL_TIMEOUT_MS 就换成一个
 * "重试"入口（reload_cb 是这套详情对应的重新载入回调）。
 * 为什么要有超时出口：请求万一没被工作线程收下（队列满）就永远没有结果，
 * 只给转圈的话用户只能退出重进。
 */
static void detail_wait_rows(lv_obj_t *list, lv_event_cb_t reload_cb)
{
    if (lv_tick_elaps(s_detail_tick) < DETAIL_TIMEOUT_MS) {
        loading_rows(list, "载入中…");
        return;
    }
    lv_obj_t *h = lv_label_create(list);
    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_text(h, "载入超时（网络慢或请求丢了），可以重试一次");
    lv_obj_t *reload = make_row(list, LV_SYMBOL_REFRESH, 0x3E9E86, "重试", NULL);
    if (reload_cb) {
        lv_obj_add_event_cb(reload, reload_cb, LV_EVENT_CLICKED, NULL);
    }
}

/* ============================================================ 第 0 页：我的歌单
 *
 * 未登录 = 一屏提示 + 「扫码登录」；已登录 = 「每日推荐」+ 账号歌单（我创建的/收藏）。
 * 点进去复用"同页换内容"的详情模式（和 meting 歌单详情同一套做法，少一层跨页路由）。
 */
static void acct_back_clicked(lv_event_t *e)
{
    (void)e;
    s_acct_detail = false;
    s_acct_sel = -1;
    s_acct_daily = false;
    s_acct_page = 0;
    online_rebuild_page_acct();
}

static void acct_daily_clicked(lv_event_t *e)
{
    (void)e;
    s_acct_daily = true;
    s_acct_sel = -1;
    s_acct_detail = true;
    s_acct_page = 0;
    detail_mark_request();               /* 超时兜底计时（见 detail_wait_rows） */
    online_rebuild_page_acct();          /* 先把"载入中"铺上，再发请求 */
    list_scroll_top(s_acct_list);
    work_submit(OREQ_DAILY, NULL);        /* 每日推荐只有一份（33 首），不做懒加载 */
}

static void acct_pl_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= s_acct_pls_n) {
        return;
    }
    s_acct_daily = false;
    s_acct_sel = idx;
    s_acct_detail = true;
    s_acct_page = 0;
    detail_mark_request();               /* 超时兜底计时（见 detail_wait_rows） */
    /*
     * ★ 懒加载：先按"歌单总首数"把缓冲分配好（指针从此不变），**只取第一页**；
     *   其余等用户翻页时按需取（见 acct_detail_fetch_if_needed）。
     *   歌单几百首也照样秒开 —— 大头（600 × 2.3 KB ≈ 1.4 MB 的 JSON）不再一次拉。
     */
    if (!acct_lazy_begin(s_acct_pls[idx].id, (size_t)s_acct_pls[idx].track_count)) {
        ui_toast("内存不足，打不开这个歌单", 2000);
        s_acct_detail = false;
        s_acct_sel = -1;
        online_rebuild_page_acct();
        return;
    }
    online_rebuild_page_acct();          /* 先铺"载入中" */
    list_scroll_top(s_acct_list);        /* 新详情从第一首开始显示 */
    acct_detail_fetch_if_needed();       /* 再取第一页 */
}

static void acct_login_clicked(lv_event_t *e)
{
    (void)e;
    qr_open();                           /* 和"我添加的歌单"页顶部那个入口是同一套浮层 */
}

/*
 * 昵称从两个地方来：**NVS 里存的那份**（重启后立刻可用）和 /user/account 的结果。
 * 这里只做"NVS → s_login_name"的补齐，避免每次重画都把界面上的新昵称盖回去。
 */
static void login_name_sync(void)
{
    if (s_login_name[0]) {
        return;
    }
    const char *n = online_netease_nickname();
    if (n && n[0]) {
        snprintf(s_login_name, sizeof(s_login_name), "%s", n);
    }
}

/* 账号页的"该发请求了吗"：一次没拉过就拉；失败过就按节流重试
 * （重启后 Wi-Fi 还没连上时第一次必然失败，不重试就整轮都是空的）。 */
/* 失败：间隔翻倍（上限 60 s）；成功：复位成 5 s */
static void acct_backoff_update(bool ok)
{
    if (ok) {
        s_acct_backoff_ms = ACCT_RETRY_MS;
        return;
    }
    if (s_acct_backoff_ms < ACCT_RETRY_MAX_MS) {
        s_acct_backoff_ms *= 2;
        if (s_acct_backoff_ms > ACCT_RETRY_MAX_MS) {
            s_acct_backoff_ms = ACCT_RETRY_MAX_MS;
        }
    }
}

static void acct_kick_if_needed(void)
{
    if (!online_has_network()) {
        return;                          /* 没网：别空转，等网络事件触发重建 */
    }
    /* 已经有列表了就不再自动拉（否则每次进页面都会闪一下转圈）：
     * 要刷新就靠"退出重进"那条路（on_delete 会把 asked 复位）或用户手动重进页面 */
    if (s_acct_pls_n > 0) {
        return;
    }
    if (!s_acct_pls_asked) {
        s_acct_pls_asked  = true;
        s_acct_pls_busy   = true;
        s_acct_pls_try_tick = lv_tick_get();
        work_submit(OREQ_ACCT_PLS, NULL);
        return;
    }
    if (s_acct_pls_busy) {
        return;                          /* 还在路上 */
    }
    if (lv_tick_elaps(s_acct_pls_try_tick) >= s_acct_backoff_ms) {
        s_acct_pls_busy     = true;
        s_acct_pls_try_tick = lv_tick_get();
        ESP_LOGI(TAG, "retry account playlists (上次失败/为空, 间隔 %u ms)",
                 (unsigned)s_acct_backoff_ms);
        work_submit(OREQ_ACCT_PLS, NULL);
    }
}

/* 详情页里"重新载入"：当前详情（每日推荐 / 某个账号歌单）再取一次 */
static void acct_reload_clicked(lv_event_t *e)
{
    (void)e;
    s_fetch_fail = false;
    detail_mark_request();
    if (s_acct_daily) {
        online_rebuild_page_acct();          /* 先把转圈画上 */
        work_submit(OREQ_DAILY, NULL);
        return;
    }
    if (s_acct_sel < 0 || (size_t)s_acct_sel >= s_acct_pls_n) {
        return;
    }
    /*
     * 两种情况分开处理：
     *   1) 缓冲里还是**别的东西**（别的歌单 / 搜索结果）→ 重建懒加载缓冲；
     *   2) 只是"这一页没取到"（缓冲就是本歌单、已经载入若干首）→ 保留已载入的，
     *      只清掉失败标记重新补页，别把前面几页再下一遍。
     */
    if (!detail_key_matches()) {
        if (!acct_lazy_begin(s_acct_pls[s_acct_sel].id,
                             (size_t)s_acct_pls[s_acct_sel].track_count)) {
            ui_toast("内存不足，打不开这个歌单", 2000);
            return;
        }
    }
    online_rebuild_page_acct();
    acct_detail_fetch_if_needed();
}

/* 昵称同理：失败（断网/服务没起来）时不能把"问过了"钉死，否则用户名永远是空的 */
static void acct_nick_kick_if_needed(void)
{
    if (s_login_name[0] || !online_has_network()) {
        return;
    }
    if (!s_acct_asked) {
        s_acct_asked        = true;
        s_acct_nick_try_tick = lv_tick_get();
        work_submit(OREQ_ACCOUNT, NULL);
        return;
    }
    if (lv_tick_elaps(s_acct_nick_try_tick) >= s_acct_backoff_ms) {
        s_acct_nick_try_tick = lv_tick_get();
        work_submit(OREQ_ACCOUNT, NULL);
    }
}

static void online_rebuild_page_acct(void)
{
    if (!s_acct_list || !s_acct_hint) {
        return;
    }
    lv_obj_clean(s_acct_list);
    /* 返回行只在详情页出现（它是页容器的固定行，不是列表行，见 make_back_label） */
    if (s_acct_back) {
        lv_obj_add_flag(s_acct_back, LV_OBJ_FLAG_HIDDEN);
    }
    /*
     * ★ "当前判定未登录"时再读一次 SD 上的 cookie：开机后 SD 可能晚挂上，
     *   那一次读不到并不代表没登录过（见 online_netease_cookie_reload 的说明）。
     *   不这么做，重启后进这一页会一直停在"登录后显示你的歌单"，歌单自然也拉不出来。
     */
    if (!online_netease_logged_in()) {
        online_netease_cookie_reload();
    }
    const bool in = online_netease_logged_in();
    login_name_sync();
    if (!in) {
        s_acct_pls_asked = false;        /* 退出/未登录：下次登录重新拉 */
        s_acct_pls_busy  = false;
        s_acct_asked     = false;
        s_login_name[0]  = '\0';
        /* 登录态没了：详情也没有意义了，退回列表（否则会停在"载入中"转圈上） */
        s_acct_detail = false;
        s_acct_sel    = -1;
        s_acct_daily  = false;
    }

    /* ---------------- 详情：每日推荐 / 某个账号歌单的歌曲 ---------------- */
    if (s_acct_detail) {
        /* 返回行固定在上方（不随列表滚动）；列表里**不再**放返回行 */
        if (s_acct_back) {
            lv_obj_remove_flag(s_acct_back, LV_OBJ_FLAG_HIDDEN);
        }

        /*
         * 头部信息：账号歌单是**懒加载**的，所以要说清"已经载入多少 / 总共多少 /
         * 第几页" —— 用户看到"已载入 100/637 首"就知道后面还能翻。
         */
        if (s_acct_daily) {
            lv_label_set_text(s_acct_hint, "每日推荐");
        } else if (s_acct_sel >= 0 && (size_t)s_acct_sel < s_acct_pls_n) {
            const online_acct_playlist_t *p = &s_acct_pls[s_acct_sel];
            if (s_acct_lazy) {
                const int pg = acct_page_count();
                if (s_songs_total) {
                    lv_label_set_text_fmt(s_acct_hint,
                                          "%.32s · 已载入 %u/%u 首 · 第 %d/%d 页", p->name,
                                          (unsigned)s_song_n, (unsigned)s_songs_total,
                                          s_acct_page + 1, pg);
                } else {
                    /* 总首数未知（服务端没给 trackCount）：只报"已载入多少" */
                    lv_label_set_text_fmt(s_acct_hint, "%.32s · 已载入 %u 首 · 第 %d/%d 页",
                                          p->name, (unsigned)s_song_n, s_acct_page + 1, pg);
                }
            } else {
                lv_label_set_text_fmt(s_acct_hint, "%.32s · %d 首", p->name, p->track_count);
            }
        }

        /*
         * ★ 只画"这一份歌确实属于当前详情"的时候（detail_key_matches）。
         *   否则点开第二个歌单时会把**上一个歌单的歌**先顶上来，
         *   用户看到的、点到的都是上一个歌单的歌（"挨个点开都是同样的"）。
         */
        if (detail_key_matches()) {
            const size_t shown = s_song_n;                    /* 已载入的**连续前缀** */
            const int pages = s_acct_lazy ? acct_page_count() : page_count(shown);
            if (s_acct_page >= pages) {
                s_acct_page = pages - 1;
            }
            if (s_acct_page < 0) {
                s_acct_page = 0;
            }
            const size_t first = page_first(s_acct_page);
            size_t rows = (shown > first) ? (shown - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            if (rows > 0) {
                fill_song_rows_range(s_acct_list, first, rows);
            }
            /*
             * 这一页**还没取满**、而且**不确认已经到歌单末尾** → 转圈 + 按需补页。
             * ⚠ 判据不能写成"总数 > 已画的"，因为总数可能未知（trackCount 缺失）——
             *   那样一首都不会去取，列表永远空着。到末尾由这两条确认：
             *   总首数已知且已载入到位，或者缓冲已经装满。
             */
            const bool at_end = (s_songs_total > 0)
                                    ? (shown >= s_songs_total)
                                    : (shown >= s_songs_cap);
            if (s_acct_lazy && rows < ONLINE_PAGE_ROWS && !at_end) {
                if (s_fetch_fail) {
                    lv_obj_t *h = lv_label_create(s_acct_list);
                    lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
                    lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
                    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(h, LV_PCT(100));
                    const char *why = online_last_error();
                    lv_label_set_text_fmt(h, "这一页没取到：%.40s", why ? why : "网络问题");
                    lv_obj_t *retry = make_row(s_acct_list, LV_SYMBOL_REFRESH, 0x3E9E86,
                                               "重试", NULL);
                    lv_obj_add_event_cb(retry, acct_reload_clicked, LV_EVENT_CLICKED, NULL);
                } else {
                    loading_rows(s_acct_list, "载入中…");
                    acct_detail_fetch_if_needed();   /* 需要哪一段就取哪一段 */
                }
            }
            page_rows(s_acct_list, PG_ACCT, s_acct_page, pages);
        } else if (s_owner == OWNER_ACCT) {
            detail_wait_rows(s_acct_list, acct_reload_clicked);   /* 转圈（超时给重试） */
        } else {
            /* s_songs 里是别处（搜索/歌单）的结果：给个重新载入的入口，
             * 别把界面卡在转圈上 */
            lv_obj_t *reload = make_row(s_acct_list, LV_SYMBOL_REFRESH, 0x3E9E86,
                                        "重新载入", NULL);
            lv_obj_add_event_cb(reload, acct_reload_clicked, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    /* ---------------- 未登录：提示 + 扫码登录 ---------------- */
    if (!in) {
        lv_label_set_text(s_acct_hint, "网易云账号");
        lv_obj_t *box = lv_obj_create(s_acct_list);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, UI_PX_H(30), LV_PART_MAIN);
        lv_obj_set_style_pad_row(box, UI_PX_H(14), LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *t1 = lv_label_create(box);
        lv_obj_set_style_text_font(t1, UI_FONT_TITLE, LV_PART_MAIN);
        lv_obj_set_style_text_color(t1, UI_COLOR_FG, LV_PART_MAIN);
        lv_label_set_text(t1, "登录后显示你的歌单");

        lv_obj_t *t2 = lv_label_create(box);
        lv_obj_set_style_text_font(t2, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(t2, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(t2, "用网易云音乐 App 扫码，\n即可获取你的账号歌单与每日推荐");

        lv_obj_t *btn = lv_button_create(box);
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, UI_PX_H(10), LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, UI_PX_H(10), LV_PART_MAIN);
        lv_obj_t *bl = lv_label_create(btn);
        lv_obj_set_style_text_font(bl, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_label_set_text(bl, "扫码登录网易云");
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, acct_login_clicked, LV_EVENT_CLICKED, NULL);
        return;
    }

    /* ---------------- 已登录：每日推荐 + 账号歌单 ---------------- */
    acct_kick_if_needed();
    acct_nick_kick_if_needed();

    /* 头部一行：昵称（NVS 里那份先用着）+ 已经拿到几个歌单 */
    if (s_acct_pls_busy && s_acct_pls_n == 0) {
        lv_label_set_text_fmt(s_acct_hint, "%s · 正在加载歌单…",
                              s_login_name[0] ? s_login_name : "网易云账号");
    } else {
        lv_label_set_text_fmt(s_acct_hint, "%s · 歌单 %u 个",
                              s_login_name[0] ? s_login_name : "网易云账号",
                              (unsigned)s_acct_pls_n);
    }

    /* 网络没通时给一句明确的话（不是"正在加载"转到天荒地老） */
    if (!online_has_network() && s_acct_pls_n == 0) {
        lv_obj_t *h = lv_label_create(s_acct_list);
        lv_obj_set_style_text_font(h, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_text(h, "Wi-Fi 未连接：进「设置」连上网络后这里会自动加载歌单");
        return;
    }

    lv_obj_t *dr = make_row(s_acct_list, LV_SYMBOL_BELL, 0xE8A33D, "每日推荐",
                            "根据你的口味每天更新");
    lv_obj_add_event_cb(dr, acct_daily_clicked, LV_EVENT_CLICKED, NULL);

    for (size_t i = 0; i < s_acct_pls_n; i++) {
        char sub[64];
        snprintf(sub, sizeof(sub), "%s · %d 首",
                 s_acct_pls[i].self_created ? "我创建的" : "收藏",
                 s_acct_pls[i].track_count);
        lv_obj_t *row = make_row(s_acct_list, LV_SYMBOL_LIST, 0x3E9E86,
                                 s_acct_pls[i].name, sub);
        lv_obj_add_event_cb(row, acct_pl_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* 列表还没到：转圈（用户要求："加载歌单这里也要转圈 + 加载出来再显示"） */
    if (s_acct_pls_busy && s_acct_pls_n == 0) {
        loading_rows(s_acct_list, "正在加载歌单…");
        return;
    }
    if (s_acct_pls_n == 0) {
        lv_obj_t *h = lv_label_create(s_acct_list);
        lv_obj_set_style_text_font(h, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(h, "还没取到歌单（检查 API 地址是否正确、网络是否通）");
    }
}

/* ---------------------------------------------------------------- 第 0 页：歌单 */

static void pl_back_clicked(lv_event_t *e)
{
    (void)e;
    s_pl_detail = false;
    s_sel_pl = -1;
    s_pl_page = 0;
    online_rebuild_page_pl();
}

/* 详情页里的"重新载入这个歌单"：当前选中的歌单再读/抓一次（详见重建函数里的说明） */
static void pl_reload_clicked(lv_event_t *e)
{
    (void)e;
    const char *id = (s_sel_pl >= 0) ? online_playlist_id((size_t)s_sel_pl) : NULL;
    if (!id) {
        return;
    }
    detail_mark_request();
    online_rebuild_page_pl();      /* 先把转圈画上，再发请求 */
    list_scroll_top(s_pl_list);
    work_submit(OREQ_LOAD, id);
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
    s_pl_page = 0;                 /* 新歌单从第 1 页看起 */
    if (s_pl_hint) {
        lv_label_set_text(s_pl_hint, "载入中…");
    }
    detail_mark_request();         /* 超时兜底计时（见 detail_wait_rows） */
    online_rebuild_page_pl();      /* 先画出"载入中"的空壳 */
    list_scroll_top(s_pl_list);    /* 新详情从第一首开始显示 */
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

/* 长按菜单的 BA 对话框句柄（s_menu 是它的 scrim，兼作"菜单开着吗"的标志） */
static ui_ba_dialog_t s_menu_dlg;
/* 待删除的歌单：确认框弹出期间菜单已经关了，s_menu_idx 会被清 —— 自己存一份 */
static int       s_del_idx = -1;
static char      s_del_id[ONLINE_ID_MAX];

static void pl_menu_build(int idx);
static void pl_menu_deferred(void *arg);

static void pl_menu_close(void)
{
    s_menu_pending = false;
    if (s_menu) {
        /* 渐出（下滑 + 缩小 + 淡出）：动画结束由 ui_popup_out 自己删 */
        ui_ba_close(&s_menu_dlg);
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

/* 「删除歌单」二次确认通过后真正下单（菜单那时已经关了） */
static void pl_menu_del_confirmed(void *user)
{
    (void)user;
    const int idx = s_del_idx;
    s_del_idx = -1;
    if (idx < 0 || !s_del_id[0]) {
        return;
    }
    if (s_sel_pl == idx) {
        s_sel_pl = -1;
        s_pl_detail = false;
    }
    ui_toast("已删除该歌单", 1500);
    work_submit(OREQ_REMOVE, s_del_id);
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

    /* ★ 删除是不可逆的（歌单要从服务器重新抓才有）：先弹二次确认 */
    s_del_idx = idx;
    snprintf(s_del_id, sizeof(s_del_id), "%s", id_copy);
    ui_ba_confirm_ex("删 除 歌 单", "确认删除这个歌单吗？\n删除后需要重新添加才能找回。",
                     "删除", "取消", true, pl_menu_del_confirmed, NULL);
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

    /* 碧蓝档案风格白卡片 + 三条选项行（左侧色条区分动作，删除用红） */
    const char *nm = online_playlist_name((size_t)idx);
    s_menu_dlg = ui_ba_open((nm && nm[0]) ? nm : "歌单", 56, LV_ALIGN_CENTER, 0);
    s_menu = s_menu_dlg.scrim;
    lv_obj_add_event_cb(s_menu, pl_menu_deleted, LV_EVENT_DELETE, NULL);
    /* 点遮罩空白处关掉（带 MENU_GRACE_MS 保险） */
    lv_obj_add_event_cb(s_menu, pl_menu_scrim_clicked, LV_EVENT_CLICKED, NULL);
    /* 点卡片自己的空白处不该关掉菜单 */
    lv_obj_add_event_cb(s_menu_dlg.card, pl_menu_panel_clicked, LV_EVENT_CLICKED, NULL);

    ui_ba_hint(s_menu_dlg.body, "选择要对这个歌单做的操作");
    ui_ba_opt_row(s_menu_dlg.body, "重命名歌单", BA_BLUE, pl_menu_btn, (void *)(intptr_t)0);
    ui_ba_opt_row(s_menu_dlg.body, "更新（重新抓取）", 0x3E9E86, pl_menu_btn, (void *)(intptr_t)1);
    ui_ba_opt_row(s_menu_dlg.body, "删除歌单", BA_DANGER, pl_menu_btn, (void *)(intptr_t)2);
}

static void online_rebuild_page_pl(void)
{
    if (!s_pl_list) {
        return;
    }
    lv_obj_clean(s_pl_list);
    /* 返回行只在详情页出现（它是页容器的固定行，不是列表行，见 make_back_label） */
    if (s_pl_back) {
        lv_obj_add_flag(s_pl_back, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 详情：某个歌单的歌曲列表 ---- */
    if (s_pl_detail) {
        if (s_pl_back) {
            lv_obj_remove_flag(s_pl_back, LV_OBJ_FLAG_HIDDEN);
        }
        const char *id = (s_sel_pl >= 0) ? online_playlist_id((size_t)s_sel_pl) : NULL;
        const char *nm = (s_sel_pl >= 0) ? online_playlist_name((size_t)s_sel_pl) : NULL;
        /*
         * ★ 关键：只有"内存里这份 s_songs 确实属于**当前这个歌单**"才画它。
         *   原先只判 `s_owner == OWNER_PL && s_song_n > 0` —— 点进第二个歌单时
         *   s_owner 还是 OWNER_PL、s_song_n 也 > 0（装的是**上一个歌单**的歌），
         *   于是列表先把上一个歌单的歌顶上来，用户看到、点到的全是它
         *   （"播放列表的歌曲我挨个点开了怎么都是同样的"就是这么来的）。
         *   现在对不上就画转圈：加载成功（online_install 把 s_cur_pl_id 换成这个歌单）
         *   才会显示歌单内容 —— 即"点进去先转圈，加载好了再显示歌单"。
         */
        const bool own = detail_owns_songs();
        if (s_pl_hint) {
            char buf[96];
            const char *title = (nm && nm[0]) ? nm : (id ? id : "歌单");
            if (own) {
                /* meting 歌单是一次抓齐的（落在 SD 上的二进制），所以只有"分页显示" */
                snprintf(buf, sizeof(buf), "%s · 共 %u 首 · 第 %d/%d 页", title,
                         (unsigned)s_song_n, s_pl_page + 1, page_count(s_song_n));
            } else {
                snprintf(buf, sizeof(buf), "%s", title);
            }
            lv_label_set_text(s_pl_hint, buf);
        }
        /* 返回行是页容器上的固定行（s_pl_back，见 make_back_label），
         * **不能**再往 s_pl_list 里塞一个 —— 那样会跟着歌曲列表滚走 */

        if (own) {
            /* ★ 分页显示：一页最多 100 行（600 首就是 3000+ 个 LVGL 对象，
             *   全建出来滚动会明显掉帧，同本地列表 §12.2 的做法） */
            s_pl_page = page_clamp(s_pl_page, s_song_n);
            const size_t first = page_first(s_pl_page);
            size_t rows = (s_song_n > first) ? (s_song_n - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            fill_song_rows_range(s_pl_list, first, rows);
            page_rows(s_pl_list, PG_PL, s_pl_page, page_count(s_song_n));
        } else if (s_owner == OWNER_SRCH || s_owner == OWNER_ACCT) {
            /* s_songs 里是别人（搜索/账号歌单）的结果：给一个"重新载入"按钮，
             * 点了才发请求（自动重发容易和结果回调互相触发） */
            lv_obj_t *reload = make_row(s_pl_list, LV_SYMBOL_REFRESH, 0x3E9E86,
                                        "重新载入这个歌单", NULL);
            lv_obj_add_event_cb(reload, pl_reload_clicked, LV_EVENT_CLICKED, NULL);
        } else {
            detail_wait_rows(s_pl_list, pl_reload_clicked);   /* 转圈（超时给重试） */
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
    /* 顶部先放"网易云账号"状态行（扫码登录 / 已登录+退出登录） */
    acct_row_build(s_pl_list);
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
    const char *k = online_history_at((size_t)idx);
    if (!k || !k[0]) {
        return;
    }
    /*
     * ★ 必须先拷一份：k 指向 s_hist[] 的内部存储，而 srch_do_search() → online_history_add()
     *   会把数组重排（同一个词"提到最前"会把后面的整体后移）。直接带指针进去，等到真正
     *   发请求时那个地址上已经是**上一条**记录了 —— 真机现象就是"点某条历史，搜出来的是
     *   另一条"（2026-10 踩过，正好错一位）。
     */
    char kw[ONLINE_KW_MAX];
    snprintf(kw, sizeof(kw), "%s", k);
    if (s_srch_ta) {
        lv_textarea_set_text(s_srch_ta, kw);
    }
    srch_do_search(kw);
}

/* 待删除的单条搜索记录（确认框弹出时列表可能已经重建，按下标删就够） */
static int s_hist_del_idx = -1;

/* 「删除这条搜索记录」二次确认通过后真正删掉 */
static void srch_hist_del_confirmed(void *user)
{
    (void)user;
    const int idx = s_hist_del_idx;
    s_hist_del_idx = -1;
    if (idx < 0) {
        return;
    }
    if (online_history_remove((size_t)idx) == ESP_OK) {
        ui_toast("已删除这条搜索记录", 1200);
        online_rebuild_page_srch();
    }
}

static void srch_row_long_pressed(lv_event_t *e)
{
    if (s_srch_has_res) {
        return;                          /* 只对历史生效 */
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const char *k = online_history_at((size_t)idx);
    if (!k || !k[0]) {
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "删除这条搜索记录？\n「%.24s」", k);
    s_hist_del_idx = idx;
    ui_ba_confirm_ex("删 除 记 录", msg, "删除", "取消", true, srch_hist_del_confirmed, NULL);
}

/* 「清除全部搜索记录」二次确认通过后真正清空 */
static void srch_clear_confirmed(void *user)
{
    (void)user;
    online_history_clear();
    ui_toast("搜索记录已清空", 1200);
    online_rebuild_page_srch();
}

static void srch_clear_clicked(lv_event_t *e)
{
    (void)e;
    /* 清空是"一次性全没了"，必须有二次确认（用户要求） */
    if (online_history_count() == 0) {
        ui_toast("还没有搜索记录", 1200);
        return;
    }
    ui_ba_confirm_ex("清 除 记 录", "确认清除全部搜索记录吗？", "清除", "取消", true,
                     srch_clear_confirmed, NULL);
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
        char buf[64];
        snprintf(buf, sizeof(buf), "正在搜索「%.40s」…", txt);
        loading_rows(s_srch_list, buf);
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
        /* 搜索结果同样分页（SEARCH_MAX=200 首 → 最多 2 页），一页最多 100 行 */
        s_srch_page = page_clamp(s_srch_page, s_song_n);
        {
            const size_t first = page_first(s_srch_page);
            size_t rows = (s_song_n > first) ? (s_song_n - first) : 0;
            if (rows > ONLINE_PAGE_ROWS) {
                rows = ONLINE_PAGE_ROWS;
            }
            fill_song_rows_range(s_srch_list, first, rows);
            page_rows(s_srch_list, PG_SRCH, s_srch_page, page_count(s_song_n));
            if (s_srch_hint) {
                lv_label_set_text_fmt(s_srch_hint, "搜索结果 · 第 %d/%d 页 · 共 %u 首",
                                      s_srch_page + 1, page_count(s_song_n),
                                      (unsigned)s_song_n);
            }
        }
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
    /*
     * ★ 咽喉处再拷一份（防御性）：调用方可能传进来指向 s_hist[] 内部的指针，而下面的
     *   online_history_add() 会 hist_move_to_front() 重排整个数组 —— 不拷的话，
     *   最后 work_submit() 取到的就是被移动后落在那个地址上的**别的关键词**。
     */
    char k[ONLINE_KW_MAX];
    snprintf(k, sizeof(k), "%s", kw);
    if (!k[0]) {
        return;
    }
    if (!online_has_network()) {
        ui_toast("Wi-Fi 未连接", 1500);
        return;
    }
    online_history_add(k);           /* 记历史（服务侧去重 + 提到最前） */
    s_srch_busy    = true;           /* 先进入"正在搜索"态（见 s_srch_busy 的说明） */
    s_srch_has_res = false;          /* 结果没回来之前不算"有结果"，避免误报"没找到" */
    s_srch_page    = 0;              /* 新结果从第 1 页看起 */
    ime_hide();
    online_rebuild_page_srch();
    work_submit(OREQ_SEARCH, k);
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
    lv_obj_set_x(s_page_acct, -v);
    lv_obj_set_x(s_page_pl, w - v);
    lv_obj_set_x(s_page_srch, 2 * w - v);
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
    if (which < 0) {
        which = 0;
    } else if (which > 2) {
        which = 2;
    }
    s_page = which;
    tab_style(s_tab_acct, s_page == 0);
    tab_style(s_tab_pl, s_page == 1);
    tab_style(s_tab_srch, s_page == 2);
    /* 离开搜索页就把键盘收了：别让它继续盖在别的页上 */
    if (s_page != 2) {
        ime_hide();
    }
    if (s_page == 0) {
        /* 进账号页刷新一次：登录态 / 歌单可能在别处变了 */
        online_rebuild_page_acct();
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

/* ⚠ 页序是三页：0 = 我的歌单（账号）、1 = 我添加的歌单、2 = 歌曲搜索。
 *   加页时这几个下标必须跟着改 —— 之前漏改就出现"点歌曲搜索跳到了我添加的歌单"。 */
static void tab_acct_clicked(lv_event_t *e) { (void)e; page_show(0, true); }
static void tab_pl_clicked(lv_event_t *e)   { (void)e; page_show(1, true); }
static void tab_srch_clicked(lv_event_t *e) { (void)e; page_show(2, true); }

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
        /* 渐出（下滑 + 缩小 + 淡出）：动画结束由 ui_popup_out 自己删。
         * 本函数可能在键盘事件回调里被调，所以这里只"开始淡出"，不阻塞。 */
        ui_ba_close(&s_modal_dlg);
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

/* 建浮层：BA 白卡片（标题 + 输入框 + 提示 + 取消/确定）+（可选拼音键盘） */
static void modal_open(int kind)
{
    if (s_modal) {
        return;
    }
    s_modal_kind = kind;
    const bool search = (kind == OREQ_SEARCH);
    const bool rename = (kind == MODAL_RENAME);

    /* 卡片停在上半屏：下半屏留给拼音键盘（IME_FIT_HEIGHT = UI_H/2） */
    s_modal_dlg = ui_ba_open(rename ? "重命名歌单" : (search ? "搜索歌曲" : "添加歌单"),
                             62, LV_ALIGN_TOP_MID, UI_PX_H(8));
    s_modal = s_modal_dlg.scrim;
    lv_obj_add_event_cb(s_modal, modal_on_delete, LV_EVENT_DELETE, NULL);
    ui_ba_on_close(&s_modal_dlg, [](void *user) { (void)user; modal_close(); }, NULL);

    lv_obj_t *ta = ui_ba_input(s_modal_dlg.body,
                               rename ? "输入新的歌单名（可留空恢复显示 ID）"
                                      : (search ? "输入歌名或歌手（可用拼音键盘）"
                                                : "请输入歌单 ID"),
                               NULL);
    s_modal_ta = ta;
    /* 重命名：把当前名字（没有就填 ID）预填进去，用户改两个字就行 */
    if (rename && s_rename_idx >= 0) {
        const char *nm = online_playlist_name((size_t)s_rename_idx);
        const char *id = online_playlist_id((size_t)s_rename_idx);
        lv_textarea_set_text(ta, (nm && nm[0]) ? nm : (id ? id : ""));
    }

    ui_ba_hint(s_modal_dlg.body, "※最多可以输入 60 个字。");

    ui_ba_btn(s_modal_dlg.btn_row, "取消", false, "Esc",
              [](lv_event_t *e) { (void)e; modal_close(); }, NULL);
    ui_ba_btn(s_modal_dlg.btn_row, rename ? "保存" : (search ? "搜索" : "添加"),
              true, "Enter", modal_submit, NULL);

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
    /* OREQ_ACCT_SONGS 的分页信息：和 songs/msg 一样，**发布标志一清就先取到本地**，
     * 免得后面被下一份结果的同名字段（工作线程会重写）串味 */
    const int page_off  = s_res_off;
    const int page_want = s_res_want;
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
            /* 内存里那份（s_songs）已经过期：把身份标记清掉，下次点进这个歌单会重新读，
             * 而不是先把旧内容顶上来 */
            if (s_cur_pl_id[0] && strcmp(s_cur_pl_id, msg) == 0) {
                s_cur_pl_id[0] = '\0';
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
        online_rebuild_page_pl();
        break;
    case OREQ_LOAD:
        if (err == ESP_OK && songs) {
            online_install(songs, count, msg, OWNER_PL);
            ui_toast("歌单已载入", 1200);
        } else {
            free(songs);
            /* 把服务里的具体原因带出来（"响应过大" / HTTP 4xx / 网络错误…）——
             * 只报一句"歌单读取失败"时根本没法判断是哪种（2026-10 踩过） */
            const char *why = online_last_error();
            char buf[128];
            snprintf(buf, sizeof(buf), "歌单读取失败：%.60s", why ? why : "网络不通");
            ui_toast(buf, 2600);
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

    /* ---------------- 网易云扫码登录（状态机：801 待扫码 / 802 待确认 / 803 成功 / 800 失效） */
    case OREQ_QR_KEY: {
        uint8_t *png = s_res_img;
        const size_t png_len = s_res_img_len;
        s_res_img     = NULL;
        s_res_img_len = 0;
        if (err != ESP_OK || !png) {
            free(png);
            qr_status_set("二维码获取失败，点「刷新」重试");
            break;
        }
        snprintf(s_qr_key, sizeof(s_qr_key), "%s", msg);
        qr_show_png(png, png_len);        /* 内部 free(png) */
        qr_status_set("请用网易云音乐 App 扫码");
        if (!s_qr_timer) {
            s_qr_timer = lv_timer_create(qr_poll_cb, QR_POLL_MS, NULL);
        }
        break;
    }
    case OREQ_QR_POLL: {
        const int code = s_res_qr;
        if (code == NET_QR_OK) {
            qr_status_set("登录成功");
            ui_toast("网易云登录成功", 1800);
            s_acct_asked = false;         /* 换账号了：重新拉昵称 */
            s_acct_nick_try_tick = lv_tick_get();
            s_acct_pls_asked = false;     /* 歌单也要重拉（绝不能显示上个账号的） */
            s_acct_pls_busy  = false;
            s_acct_pls_try_tick = lv_tick_get();
            s_acct_backoff_ms = ACCT_RETRY_MS;   /* 新账号：重试节奏从头算 */
            /* 上个账号的昵称（NVS 里那份）不能留给新账号看：清掉，
             * 等下面那次 /user/account 回来再填 */
            s_login_name[0] = '\0';
            online_netease_nickname_set(NULL);
            qr_close();
            online_rebuild_page_pl();
            online_rebuild_page_acct();
            work_submit(OREQ_ACCOUNT, NULL);   /* 立刻验证 cookie 并取昵称（成功后自动拉歌单） */
        } else if (code == NET_QR_SCAN) {
            qr_status_set("已扫码，请在手机上确认");
        } else if (code == NET_QR_WAIT) {
            qr_status_set("等待扫码…");
        } else if (code == NET_QR_EXPIRE) {
            qr_status_set("二维码已失效，点「刷新」重新获取");
            qr_stop_poll();
        }
        break;
    }
    case OREQ_LOGOUT:
        s_login_name[0] = '\0';
        s_acct_asked = true;              /* 退出后别再自动拉账号 */
        s_acct_nick_try_tick = lv_tick_get();
        /*
         * 退出必须把上一个账号的歌单清干净：否则换账号登录后会先看到**别人的歌单**，
         * 直到新列表回来才刷新 —— 既困惑又不安全。s_acct_pls_asked 一起复位，
         * 保证下次登录会重新拉。
         */
        s_acct_pls_asked = false;
        s_acct_pls_busy  = false;
        s_acct_pls_try_tick = lv_tick_get();
        if (s_acct_pls) {
            free(s_acct_pls);
            s_acct_pls = NULL;
        }
        s_acct_pls_n = 0;
        s_acct_detail = false;
        s_acct_sel = -1;
        s_acct_daily = false;
        ui_toast(err == ESP_OK ? "已退出网易云登录" : "退出失败", 1600);
        online_rebuild_page_pl();
        online_rebuild_page_acct();
        break;
    case OREQ_ACCOUNT:
        s_acct_asked = true;
        s_acct_nick_try_tick = lv_tick_get();   /* 下次重试窗口从这里开始算 */
        acct_backoff_update(err == ESP_OK);
        if (err == ESP_OK) {
            snprintf(s_login_name, sizeof(s_login_name), "%s", msg);
        } else if (err == ESP_ERR_INVALID_STATE) {
            /* 服务端明确回了"没有 profile" = cookie 真的失效了 → 回未登录态 */
            s_login_name[0] = '\0';
            ESP_LOGW(TAG, "账号信息无效：cookie 已失效，请重新扫码");
        } else {
            /*
             * ★ 网络/HTTP 失败**不等于**未登录！cookie 还在 SD 上，把界面打回未登录
             *   正是用户看到的"扫码成功后反而没反应"。这里保留登录态，只记日志。
             *   昵称会由 acct_nick_kick_if_needed() 按节流（5 s 起、失败翻倍，上限 60 s）
             *   自动重试（以前这里"问过就不再问"，断网那次失败就让用户名永远空着）。
             */
            ESP_LOGW(TAG, "账号信息获取失败(%s)：保留登录态，稍后重试", esp_err_to_name(err));
        }
        online_rebuild_page_pl();
        online_rebuild_page_acct();
        if (err == ESP_OK && !s_acct_pls_asked) {
            s_acct_pls_asked = true;      /* 登录有效 → 顺手把账号歌单拉回来 */
            s_acct_pls_busy  = true;
            s_acct_pls_try_tick = lv_tick_get();
            work_submit(OREQ_ACCT_PLS, NULL);
        }
        break;

    /* ---------------- 第 0 页「我的歌单」：账号歌单 / 每日推荐 ---------------- */
    case OREQ_ACCT_PLS: {
        s_acct_pls_busy = false;
        s_acct_pls_try_tick = lv_tick_get();   /* 失败重试的节流窗口从这里开始 */
        online_acct_playlist_t *pls = s_res_pls;
        const size_t pn = s_res_pls_n;
        s_res_pls   = NULL;
        s_res_pls_n = 0;
        if (s_acct_pls) {
            free(s_acct_pls);
            s_acct_pls = NULL;
        }
        s_acct_pls   = pls;
        s_acct_pls_n = pn;
        acct_backoff_update(err == ESP_OK && pn > 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "账号歌单拉取失败: %s（%u ms 后自动重试）", esp_err_to_name(err),
                     (unsigned)s_acct_backoff_ms);
        }
        online_rebuild_page_acct();
        break;
    }
    case OREQ_ACCT_SONGS: {
        /*
         * **懒加载的一页**：把这一页追加到 s_songs 的已载入末尾（必须连续）。
         * 注意这是"追加"而不是"安装" —— 缓冲是 acct_lazy_begin() 一次性分配好的，
         * 指针从头到尾不变，所以播放器那边的队列指针一直有效。
         */
        s_fetch_busy = false;
        const size_t off = (page_off > 0) ? (size_t)page_off : 0;
        if (err == ESP_OK && songs && count > 0) {
            if (s_acct_lazy && strcmp(s_cur_pl_id, msg) == 0) {
                acct_lazy_append(off, songs, count);
                /* 要一页却给不满一页 = 到歌单末尾了：把总数按实际修正，
                 * 免得列表一直显示"还有下一页" */
                if (count < (size_t)page_want) {
                    s_songs_total = s_song_n;
                }
            } else {
                ESP_LOGW(TAG, "drop acct page: buffer=%s req=%s lazy=%d", s_cur_pl_id, msg,
                         (int)s_acct_lazy);
            }
        } else if (off > 0 && (err == ESP_ERR_NOT_FOUND || (err == ESP_OK && count == 0))) {
            /*
             * 服务端"没有更多歌了"（空页）—— 只有总首数未知时才会走到这儿
             * （已知总数时页数算得准，不会去要点不存在的页）。
             * 这不是失败：按已载入数收尾，那个多出来的"下一页"自己就消失了。
             */
            ESP_LOGI(TAG, "acct playlist ended at %u songs", (unsigned)s_song_n);
            s_songs_total = s_song_n;
        } else {
            /* 这一页没取到：给"重试"出口（渲染时画出来），别自己死循环重发；
             * 一首都没载入时（第一次就失败）直接退回歌单列表并说明原因 */
            s_fetch_fail = true;
            if (s_song_n == 0) {
                const char *why = online_last_error();
                char buf[128];
                snprintf(buf, sizeof(buf), "歌单读取失败：%.60s", why ? why : "网络不通");
                ui_toast(buf, 2600);
                s_acct_detail = false;
                s_acct_sel = -1;
            }
        }
        free(songs);
        online_rebuild_page_acct();
        break;
    }
    case OREQ_DAILY:
        if (err == ESP_OK && songs) {
            /* 每日推荐是一整份（33 首左右），不需要懒加载 */
            online_install(songs, count, DETAIL_KEY_DAILY, OWNER_ACCT);
            s_acct_daily = true;
            s_acct_detail = true;
            s_acct_page = 0;
            ui_toast("每日推荐已载入", 1200);
        } else {
            free(songs);
            /* 同上：带出服务侧的具体原因（"响应过大" / 版权 / 网络…） */
            const char *why = online_last_error();
            char buf[128];
            snprintf(buf, sizeof(buf), "每日推荐读取失败：%.60s", why ? why : "网络不通");
            ui_toast(buf, 2600);
            s_acct_detail = false;        /* 读不出来就退回列表，别停在空壳上 */
            s_acct_sel = -1;
        }
        online_rebuild_page_acct();
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
        online_rebuild_page_acct();     /* 账号页：网络回来要重新拉歌单/昵称 */
    }
    on_result();

    /*
     * 登录态下的"补拉"：开机后 Wi-Fi 可能还没连上，那次拉歌单/昵称必然失败
     * —— 以前"问过就不再问"，用户重启后进来就是一片空。这里按 5 秒节流重试，
     * 转圈动画也跟着出现（见 acct_kick_if_needed / acct_nick_kick_if_needed）。
     */
    if (online_netease_logged_in()) {
        const bool busy_before = s_acct_pls_busy;
        acct_nick_kick_if_needed();
        acct_kick_if_needed();
        /* 转圈要跟着"这次重试"出现：只在本页可见时重画，别的页等结果回调自己重画 */
        if (s_page == 0 && s_acct_pls_busy != busy_before) {
            online_rebuild_page_acct();
        }
    }
}

/* ---------------------------------------------------------------- 页面 */

static void online_on_delete(lv_event_t *e)
{
    (void)e;
    s_page_alive = false;       /* 工作线程据此判断"这份结果还有没有人要" */
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    modal_close();
    pl_menu_close();
    qr_close();          /* 二维码浮层挂在 lv_layer_top 上，不随页面销毁，必须显式收掉 */
    /* 二次确认框同样挂在 lv_layer_top 上：退页面时它必须一起消失，
     * 否则会跟着用户到别的页面去（而且回调还指着本页的状态） */
    ui_ba_confirm_close();
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
    /* 第 0 页（账号页）的控件随 screen 一起销毁：必须清指针，
     * 否则残留的悬垂指针一旦被 rebuild 碰到就是崩溃 */
    s_page_acct = NULL;
    s_tab_acct = NULL;
    s_acct_hint = NULL;
    s_acct_list = NULL;
    s_acct_back = NULL;         /* 返回行挂在页容器上，随 screen 一起销毁 */
    s_pl_list = NULL;
    s_pl_hint = NULL;
    s_pl_back = NULL;
    s_srch_ta = NULL;
    s_srch_hint = NULL;
    s_srch_clear = NULL;
    s_srch_list = NULL;
    s_srch_has_res = false;
    s_pl_detail = false;
    s_acct_detail = false;

    /*
     * ★ 结果槽位里可能还压着一份"工作线程刚发布、本页定时器还没搬走"的结果：
     *   页面没了就没人消费它 —— 数组/二维码/歌单列表都会泄漏，而且工作线程
     *   下一轮要白等（见 online_work_task 里的等待）。这里一次清干净。
     *   （正常的 on_result 会把这些指针先搬走再置 NULL，所以这里判空即可。）
     */
    free(s_res_songs);
    s_res_songs = NULL;
    s_res_count = 0;
    free(s_res_img);
    s_res_img     = NULL;
    s_res_img_len = 0;
    free(s_res_pls);
    s_res_pls   = NULL;
    s_res_pls_n = 0;
    s_res_kind  = OREQ_NONE;
    /* 被替换下来的旧队列：交给播放器托管（它换队列时会自己放），别留在这儿 */
    online_drop_stale();
    /* 请求序号/重试节流复位：下次进来重新拉（不能沿用上一轮"问过了"的状态） */
    s_acct_pls_asked = false;
    s_acct_pls_busy  = false;
    s_acct_asked     = false;
    /* 分页/懒加载状态复位：s_songs 还在内存里（播放器可能还在放），但页码、
     * 在途标志不能留到下一次进页面 */
    s_acct_page  = 0;
    s_pl_page    = 0;
    s_srch_page  = 0;
    s_fetch_busy = false;
    s_fetch_fail = false;
}

static lv_obj_t *page_online(void)
{
    s_page_alive = true;        /* 先置位：后面任何请求发布结果时都算"有人要" */
    detail_mark_request();      /* 详情超时兜底的起点（别沿用上一轮页面留下的旧时刻） */
    s_acct_backoff_ms = ACCT_RETRY_MS;   /* 重新进 APP：重试节奏从头算（失败过也立刻再试） */
    /* 分页/懒加载：页码从第 1 页开始（静态变量跨页面实例存活，必须显式复位） */
    s_acct_page  = 0;
    s_pl_page    = 0;
    s_srch_page  = 0;
    s_fetch_busy = false;
    s_fetch_fail = false;
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

    /* 第 0 页页签：我的歌单（网易云账号）—— 必须在 pl 之前建，保证页签从左到右的次序 */
    s_tab_acct = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_acct, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_tab_acct, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_tab_acct, "我的歌单");
    lv_obj_add_flag(s_tab_acct, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_acct, UI_PX_W(10));
    lv_obj_add_event_cb(s_tab_acct, tab_acct_clicked, LV_EVENT_CLICKED, NULL);

    s_tab_pl = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_pl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_tab_pl, "我添加的歌单");
    lv_obj_add_flag(s_tab_pl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tab_pl, UI_PX_H(12));   /* 文字本身太小，扩大点击区 */
    lv_obj_add_event_cb(s_tab_pl, tab_pl_clicked, LV_EVENT_CLICKED, NULL);

    s_tab_srch = lv_label_create(tabs);
    lv_obj_set_style_text_font(s_tab_srch, UI_FONT_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_tab_srch, "搜索");
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
    pager_attach(s_host);            /* 点空白处起手横滑也能切页 */

    /* ================= 第 0 页：我的歌单（网易云账号） ================= */
    s_page_acct = lv_obj_create(s_host);
    lv_obj_remove_style_all(s_page_acct);
    lv_obj_set_size(s_page_acct, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_page_acct, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page_acct, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_page_acct, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_page_acct, 0, 0);

    /* ★ 返回行必须是页容器的**第一个**子对象（flex 列的第一行），这样它固定在
     *   列表上方、不随歌曲列表滚动（用户 2026-10 报"返回按钮跟着滑动"）。 */
    s_acct_back = make_back_label(s_page_acct, LV_SYMBOL_LEFT " 返回", acct_back_clicked);

    s_acct_hint = lv_label_create(s_page_acct);
    lv_obj_set_style_text_font(s_acct_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_acct_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_acct_hint, LV_PCT(100));
    lv_label_set_long_mode(s_acct_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_acct_hint, "");

    s_acct_list = lv_obj_create(s_page_acct);
    lv_obj_remove_style_all(s_acct_list);
    lv_obj_set_width(s_acct_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_acct_list, 1);
    lv_obj_set_flex_flow(s_acct_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_acct_list, UI_PX_H(4), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_acct_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_acct_list, LV_SCROLLBAR_MODE_AUTO);

    /* ================= 第 1 页：我添加的歌单 ================= */
    s_page_pl = lv_obj_create(s_host);
    lv_obj_remove_style_all(s_page_pl);
    lv_obj_set_size(s_page_pl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_page_pl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page_pl, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_page_pl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_page_pl, 0, 0);

    /* 同上：返回行固定在列表上方，不跟着歌曲列表滚 */
    s_pl_back = make_back_label(s_page_pl, LV_SYMBOL_LEFT " 歌单列表", pl_back_clicked);

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
