/*
 * music_source_local.cc —— 本地目录数据源（异步扫描线程 + 条目缓冲 + SD 代次）
 *
 * 搬迁自 app_music.cc 的 2896–2937、3110–3263、3357–3431（数据部分）。
 * 这里**绝不碰 LVGL**：只写条目数组与扫描标志，渲染交给 music_view_local.cc。
 */
#include "music_local_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"   /* 目录条目缓冲优先放 PSRAM（heap_caps_realloc） */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp.h"

static const char *TAG = "app_music";

#define LOCAL_SCAN_STACK  (8 * 1024)

typedef struct {
    char     dir[STORAGE_PATH_MAX];
    uint32_t seq;
} local_scan_req_t;

static char       s_cur_dir[STORAGE_PATH_MAX] = "/sdcard";
static fs_entry_t *s_entries;                 /* 目录条目缓冲（PSRAM，按需扩容） */
static size_t      s_entries_cap;             /* s_entries 的容量（条） */
static int         s_entry_n;                 /* 当前目录的条目总数 */
static uint32_t    s_local_sd_gen;            /* 上次见到的 SD 状态代次 */

static volatile bool     s_scan_busy;       /* 工作线程正在扫 */
static volatile bool     s_scan_done;       /* 有新结果等 UI 取 */
static volatile bool     s_scan_fail;       /* 这次扫描失败（读目录出错/没内存） */
static volatile uint32_t s_scan_seq;        /* UI 侧最新请求号 */
static volatile uint32_t s_scan_run_seq;    /* 已完成结果的请求号 */
static volatile int      s_scan_n;          /* 扫到的条目数（<0 = 失败） */
static volatile bool     s_scan_pend;       /* 扫描期间又来了请求，等结束再起 */
static char              s_scan_pend_dir[STORAGE_PATH_MAX];   /* 排队中的目录 */

void local_source_begin(const char *dir)
{
    if (dir && dir[0]) {
        snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", dir);
    }
    s_entry_n = 0;
}

const char *local_source_dir(void)
{
    return s_cur_dir;
}

void local_source_set_dir(const char *dir)
{
    if (dir && dir[0]) {
        snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", dir);
    }
}

int local_source_entry_count(void)
{
    return s_entry_n;
}

void local_source_set_entry_count(int n)
{
    s_entry_n = n;
}

bool local_source_last_failed(void)
{
    return s_scan_fail;
}

void local_source_view(local_dir_view_t *out)
{
    if (!out) {
        return;
    }
    out->entries = s_entries;
    out->cap     = s_entries_cap;
    out->n       = s_entry_n;
}

/*
 * 确保条目缓冲至少能装 need 条。按 64 → 翻倍 → 上限的顺序扩容，优先 PSRAM
 * （一条 fs_entry_t 264 字节，2000 条就是 ~516 KB，放内部 RAM 会把 WiFi/音频挤掉）。
 * 返回能否满足。
 */
bool local_source_reserve(size_t need)
{
    if (need <= s_entries_cap) {
        return true;
    }
    if (need > LOCAL_ENTRIES_MAX) {
        need = LOCAL_ENTRIES_MAX;
    }
    size_t cap = s_entries_cap ? s_entries_cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    fs_entry_t *p = (fs_entry_t *)heap_caps_realloc(s_entries, cap * sizeof(fs_entry_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (fs_entry_t *)realloc(s_entries, cap * sizeof(fs_entry_t));
    }
    if (!p) {
        ESP_LOGE(TAG, "local: entry buffer grow to %u failed", (unsigned)cap);
        return false;
    }
    s_entries     = p;
    s_entries_cap = cap;
    ESP_LOGI(TAG, "local: entry buffer -> %u entries", (unsigned)cap);
    return s_entries_cap >= need;
}

/* ------------------------------------------------- 本地目录扫描（工作线程）
 *
 * 需求（2026-10 用户）："点本地音乐要**立刻**进页面，用加载动画等"。
 *
 * 以前 page_local() / 进目录都是**在 LVGL 任务里同步**扫 SD：
 *   local_rebuild() 先 storage_service_browse()（列目录），再
 *   storage_service_load_dir()（再走一遍目录、建播放队列）—— 两次 full walk，
 *   几百首的目录就是"卡一会才进去"，而且转圈一动不动（LVGL 任务被占住）。
 *
 * 现在整个扫描搬到工作线程，UI 只做两件事：
 *   1) 铺加载态，**立刻返回**（页面秒进、转圈真的在转）；
 *   2) 本地页的 500 ms 定时器轮询结果，拿到就渲染列表。
 *
 * 与在线准备任务同一套约定：
 *   - 工作线程**绝不碰 LVGL**（只写 s_entries / s_scan_* 这些纯数据）；
 *   - 请求带**序号**：期间用户又换了目录就把旧结果丢掉（run_seq != seq）；
 *   - 同一时刻只有一个扫描在跑（s_scan_busy）；期间来的新请求记进 s_scan_pend*，
 *     等这次结束由轮询再起一个 —— 不能两个线程同时写 s_entries。
 *
 * ⚠ storage_service_load_dir() 内部是"先清 s_count 再重建"，存在一个短暂的
 *   "队列为空"窗口。这里敢这么用，是因为**读曲目表的只有 app_music**，而播放器页
 *   与本地列表页是路由上的**兄弟页**（不会同时存活），本地页扫描期间又只有加载态、
 *   没有任何行可点 —— 那个窗口没人看得到。将来若有别的模块（状态栏等）也去读曲目表，
 *   这条前提就不成立，得改成"在服务内部加锁 + 末尾换指针"。
 */
static void local_scan_task(void *arg)
{
    local_scan_req_t *req = (local_scan_req_t *)arg;

    /*
     * 1) 列目录：先按当前容量取一次，browse 返回的是**真实条数**（可能大于容量），
     *    装不下就扩容重来 —— 容量翻倍增长，最多几轮就够，任何规模都能列全。
     *    （旧代码把容量写死 64 条且不看返回值，目录一大就静默只剩前 64 条。）
     */
    int got = -1;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (!s_entries && !local_source_reserve(64)) {
            break;
        }
        got = storage_service_browse(req->dir, s_entries, (int)s_entries_cap);
        if (got < 0) {
            break;
        }
        if ((size_t)got <= s_entries_cap) {
            break;                                  /* 全装下了 */
        }
        const size_t cap_before = s_entries_cap;
        if (!local_source_reserve((size_t)got)) {
            break;                                  /* 分配失败：按现有容量列 */
        }
        if (s_entries_cap == cap_before) {
            break;   /* 已到 LOCAL_ENTRIES_MAX，再扫一遍目录也是同样结果 */
        }
    }

    /* 2) 播放队列 = 当前目录这一层的全部音频（上一首/下一首在整目录里轮转，与翻页无关） */
    if (got >= 0) {
        storage_service_load_dir(req->dir);
    }

    /*
     * 3) 发布。顺序要紧：**先写数据、再置 done、最后才清 busy** ——
     *    反过来的话，轮询有可能在"busy 已清、done 还没置"的缝里又起一个扫描，
     *    两个线程同时写 s_entries。
     */
    s_scan_n       = got;
    s_scan_fail    = (got < 0);
    s_scan_run_seq = req->seq;
    s_scan_done    = true;
    s_scan_busy    = false;

    ESP_LOGI(TAG, "local scan: %d entries, queue %u, dir %s (seq %u)",
             got, (unsigned)storage_service_count(), req->dir, (unsigned)req->seq);
    free(req);
    vTaskDelete(NULL);
}

/* 起一次扫描（LVGL 任务里调）。忙着就把请求排队，由轮询补起 */
void local_source_scan_start(const char *dir)
{
    if (!dir || !dir[0]) {
        return;
    }
    s_scan_seq = s_scan_seq + 1;   /* volatile 不能用 ++（C++20 起 deprecated，-Werror） */
    if (s_scan_busy) {
        snprintf(s_scan_pend_dir, sizeof(s_scan_pend_dir), "%s", dir);
        s_scan_pend = true;
        return;
    }
    local_scan_req_t *req = (local_scan_req_t *)malloc(sizeof(local_scan_req_t));
    if (!req) {
        s_scan_n       = -1;
        s_scan_fail    = true;
        s_scan_run_seq = s_scan_seq;
        s_scan_done    = true;
        return;
    }
    snprintf(req->dir, sizeof(req->dir), "%s", dir);
    req->seq    = s_scan_seq;
    s_scan_busy = true;
    s_scan_done = false;
    if (xTaskCreate(local_scan_task, "loc_scan", LOCAL_SCAN_STACK, req, 2, NULL) != pdPASS) {
        s_scan_busy    = false;
        s_scan_n       = -1;
        s_scan_fail    = true;
        s_scan_run_seq = s_scan_seq;
        s_scan_done    = true;
        free(req);
        ESP_LOGW(TAG, "local scan task create failed");
    }
}

/* 列表页销毁 / 拔卡时作废在飞的扫描 */
void local_source_invalidate(void)
{
    s_scan_seq  = s_scan_seq + 1;   /* volatile 不能用 ++（C++20 起 deprecated，-Werror） */
    s_scan_done = false;
    s_scan_pend = false;
}

void local_source_sd_sync(void)
{
    s_local_sd_gen = storage_service_sd_generation();
}

void local_source_poll(local_poll_result_t *out)
{
    if (!out) {
        return;
    }
    out->scan_ready  = false;
    out->sd_removed  = false;
    out->sd_inserted = false;

    /*
     * ① 异步扫描的结果：seq 对得上才是"当前这个目录"的结果，
     *    对不上说明期间用户又换了目录 → 直接丢掉，别把旧目录的列表画上去。
     */
    if (s_scan_done) {
        s_scan_done = false;
        if (s_scan_run_seq == s_scan_seq) {
            /*
             * 不变量：s_entry_n 必须 ≤ 缓冲容量 —— 视图按页取 entries[i] 的下标，
             * 一旦 n 大于容量就会越界读（目录条目超过 LOCAL_ENTRIES_MAX 时只列前
             * LOCAL_ENTRIES_MAX 条，并留下日志）。
             */
            if (!s_scan_fail) {
                int n = s_scan_n;
                if ((size_t)n > s_entries_cap) {
                    ESP_LOGW(TAG, "local: %d entries but buffer holds %u — listing the first %u",
                             n, (unsigned)s_entries_cap, (unsigned)s_entries_cap);
                    n = (int)s_entries_cap;
                }
                s_entry_n = n;
            } else {
                s_entry_n = 0;
            }
            out->scan_ready = true;
        } else {
            ESP_LOGI(TAG, "local scan result dropped (seq %u != %u)",
                     (unsigned)s_scan_run_seq, (unsigned)s_scan_seq);
        }
    }
    /* ② 扫描期间排队的新请求：等它结束了再起一个（同一时刻只允许一个扫描） */
    if (s_scan_pend && !s_scan_busy) {
        s_scan_pend = false;
        local_source_scan_start(s_scan_pend_dir);
        return;
    }
    /* ③ SD 热插拔代次 */
    const uint32_t gen = storage_service_sd_generation();
    if (gen == s_local_sd_gen) {
        return;
    }
    s_local_sd_gen = gen;
    if (storage_service_sd_is_mounted()) {
        out->sd_inserted = true;
    } else {
        out->sd_removed = true;
    }
}
