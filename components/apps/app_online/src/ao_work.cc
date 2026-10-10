/*
 * ao_work.cc —— 工作线程 + 请求/结果协议（app_online 的**唯一跨任务通道**）
 *
 * ★★ 发布协议（改动前先读完这三条，见 docs/refactor/04_app_online.md 风险 R1/R14）：
 *
 *   1) **发布**：工作线程把 s_res_songs / s_res_err / s_res_msg / s_res_off /
 *      s_res_want / s_res_img / s_res_pls 这些负载**先写好**，最后才写 s_res_kind
 *      —— kind 就是"发布标志"，它一置位 LVGL 侧随时可能来读。
 *      顺序一乱 = 读到半份结果（旧的歌名配新的数组 = 悬垂指针 / 数组泄漏）。
 *   2) **消费**：ao_work_take() 把整份负载拷到调用方的 ao_result_t 里，**拷完才清
 *      槽位** —— 清早了工作线程会以为没人取，清晚了下一份就覆盖上一份。
 *   3) **背压**：工作线程每轮开头先等 LVGL 把上一份搬走（最多 3 s），
 *      否则两份结果撞在一起，被覆盖的数组既没被界面用上也没人 free。
 *
 * UI 纪律：本文件只碰静态槽位与 online_service，**绝不碰任何 lv_obj_t***
 * （唯一例外是队列满时那句 ui_toast —— 它和拆分前一样，由 LVGL 任务之外调用，
 *  ui_core 的 toast 自己做了跨任务投递）。整份文件**不用锁**。
 */
#include "ao_work.h"

#include "ui_core.h"

static const char *TAG = AO_TAG;

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

void ao_work_submit_ex(int kind, const char *arg, int off, int cnt)
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

void ao_work_submit(int kind, const char *arg)
{
    ao_work_submit_ex(kind, arg, 0, 0);
}

void ao_work_set_page_alive(bool alive)
{
    s_page_alive = alive;
}

void ao_work_take(ao_result_t *out)
{
    memset(out, 0, sizeof(*out));
    const int kind = s_res_kind;
    out->kind = kind;
    if (kind == OREQ_NONE) {
        return;                      /* 没东西可取：槽位本来就已经是空的 */
    }
    /*
     * ★ 先把整份负载拷到本地，**再**清槽位（顺序不能反）：
     *   - 拷之前清：工作线程可能已经写了负载、还没写 kind，读到半份；
     *   - 清之后才拷：读到的是下一份请求刚盖上去的字段（串味）。
     *   原来的 on_result() 是"按 case 分片拷"，拆文件后最容易漏掉某一项，
     *   所以这里一次性把 off/want/img/pls 全带上。
     */
    out->err     = (esp_err_t)s_res_err;
    out->songs   = s_res_songs;
    out->count   = s_res_count;
    snprintf(out->msg, sizeof(out->msg), "%s", s_res_msg);
    out->off     = s_res_off;
    out->want    = s_res_want;
    out->img     = s_res_img;
    out->img_len = s_res_img_len;
    out->qr      = s_res_qr;
    out->pls     = s_res_pls;
    out->pls_n   = s_res_pls_n;

    s_res_songs = NULL;
    s_res_count = 0;
    s_res_kind  = OREQ_NONE;         /* 发布标志最先清：工作线程可以立刻开始下一轮 */
    s_res_msg[0] = '\0';
    s_res_img     = NULL;
    s_res_img_len = 0;
    s_res_pls   = NULL;
    s_res_pls_n = 0;
}

void ao_work_discard_pending(void)
{
    /*
     * ★ 结果槽位里可能还压着一份"工作线程刚发布、本页定时器还没搬走"的结果：
     *   页面没了就没人消费它 —— 数组/二维码/歌单列表都会泄漏，而且工作线程
     *   下一轮要白等（见 online_work_task 里的等待）。这里一次清干净。
     *   （正常的 ao_work_take 会把这些指针先搬走再置 NULL，所以这里判空即可。）
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
}

void ao_work_init(void)
{
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
}
