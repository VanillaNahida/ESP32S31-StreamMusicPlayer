/*
 * storage_service.cc —— microSD 曲库扫描
 *
 * 约定（开发指南 5.10 / 6.3）：
 *   - 只扫 /sdcard，白名单扩展名
 *   - 每扫到一批就 vTaskDelay(1) 让出 CPU，别让 LVGL 掉帧
 *   - 未插卡时优雅降级：返回 0 首，不崩
 *   - 文件名常见形如 "Artist - Title.ext"，据此推 title/artist
 */
#include "storage_service.h"
#include "media_meta.h"    /* media_meta_read_tags()：扫描时优先读内嵌 ID3/Vorbis 标签 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp.h"

static const char *TAG = "storage";

/* SD 热插拔监控（实现见下面的"SD 热插拔"一节） */
static void sd_monitor_start(void);

#define MUSIC_ROOT      "/sdcard"
#define MUSIC_ROOT_ALT  "/sdcard"
#define TRACK_MAX       8192
#define SCAN_YIELD_EVERY 32


/* 安全上限：正常不再靠它截断（按目录内实际文件数动态扩容），
 * 只在极端情况（目录里几万首）兜住内存 */
#define TRACK_MAX       8192

static track_t *s_tracks;
/*
 * 曲目表按"目录内实际文件数"动态扩容（需求：扫描不全 → 不要固定的 512 条上限）。
 *   - 先给 64 条；满了就翻倍（realloc），优先放 PSRAM（表大了不占内部 RAM）；
 *   - 真到 TRACK_MAX 或内存不足才放弃，并打日志（不再静默丢歌）。
 * storage_service_track() 返回的是表内指针，扩容会让它失效 —— 调用方都只在
 * 扫描/换目录的间隙取指针并用完即走（UI 每帧重取），所以这里不做额外保护。
 */
static size_t   s_cap;

static bool tracks_reserve(size_t need)
{
    if (need <= s_cap) {
        return true;
    }
    size_t cap = (s_cap > 0) ? s_cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > TRACK_MAX) {
        cap = TRACK_MAX;
    }
    track_t *p = (track_t *)heap_caps_realloc(s_tracks, cap * sizeof(track_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (track_t *)realloc(s_tracks, cap * sizeof(track_t));   /* 退回内部 RAM */
    }
    if (!p) {
        ESP_LOGE(TAG, "track table grow to %u failed (%u used)", (unsigned)cap, (unsigned)s_cap);
        return false;
    }
    s_tracks = p;
    s_cap    = cap;
    ESP_LOGI(TAG, "track table -> %u entries", (unsigned)cap);
    return true;
}

static bool tracks_grow(void)
{
    if (s_cap >= TRACK_MAX) {
        ESP_LOGW(TAG, "track count hit the %u safety cap, remaining files skipped", (unsigned)TRACK_MAX);
        return false;
    }
    return tracks_reserve(s_cap + 1);
}
static size_t   s_count;
static bool     s_ready;

static const char *k_ext[] = { ".mp3", ".flac", ".m4a", ".aac", ".wav", ".ogg", ".opus" };

/*
 * UTF-8 安全的拷贝/截断。
 *
 * 为什么不能直接用 snprintf：文件名是 UTF-8，中文一个字 3 字节，一旦在多字节
 * 序列中间截断，尾部就剩半个字符 —— 屏幕上是个乱码方块，fopen 也会因为路径
 * 对不上而失败。这里最多回退到最后一个完整字符的边界。
 */
static void utf8_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0 || !src) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        /* 0b10xxxxxx 是续接字节，说明正踩在字符中间，往前退到起点 */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool storage_service_is_audio(const char *name)
{
    if (!name) {
        return false;
    }
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(k_ext) / sizeof(k_ext[0]); i++) {
        size_t e = strlen(k_ext[i]);
        if (n > e) {
            const char *p = name + (n - e);
            bool eq = true;
            for (size_t k = 0; k < e; k++) {
                if (tolower((unsigned char)p[k]) != k_ext[i][k]) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                return true;
            }
        }
    }
    return false;
}

/* 去掉扩展名 */
static void strip_ext(char *s)
{
    char *dot = strrchr(s, '.');
    if (dot) {
        *dot = '\0';
    }
}

/* 由 "Artist - Title" 形式拆分；拆不出来时整串当标题 */
static void split_title_artist(const char *stem, char *title, size_t tsz,
                               char *artist, size_t asz)
{
    const char *sep = strstr(stem, " - ");
    if (sep && sep != stem) {
        size_t alen = (size_t)(sep - stem);
        if (alen >= asz) {
            alen = asz - 1;
        }
        memcpy(artist, stem, alen);
        artist[alen] = '\0';
        utf8_copy(title, tsz, sep + 3);
    } else {
        artist[0] = '\0';
        utf8_copy(title, tsz, stem);
    }
}

static void add_track(const char *full_path, const char *name, uint32_t size)
{
    if (s_count >= s_cap && !tracks_grow()) {
        return;
    }
    track_t *t = &s_tracks[s_count];
    utf8_copy(t->path, sizeof(t->path), full_path);
    t->size_bytes = size;

    char stem[STORAGE_TITLE_MAX];
    utf8_copy(stem, sizeof(stem), name);
    strip_ext(stem);
    split_title_artist(stem, t->title, sizeof(t->title), t->artist, sizeof(t->artist));

    /*
     * ★ 2026-10（用户要求）：**优先用内嵌标签**，文件名只作兜底。
     *
     *   优先级：ID3v2 / Vorbis 的 title → 文件名推出来的 title
     *             ID3v2 / Vorbis 的 artist → 文件名推出来的 artist → 空（显示端兜"未知歌手"）
     *
     *   为什么在这里做：`split_title_artist()` 只是按 "Artist - Title" 拆**文件名** ✗，
     *   文件名一旦不规范（"01 track"、"未知艺人 - xxx"、只有歌名没有歌手…），
     *   列表 / 播放器页 / 信息弹窗显示的就是错的 ✓。
     *   而 title/artist 只在**扫描时**填这一次 ✓，所以在这里覆盖等于一次修好三处 ✓
     *   （storage 的 track_t 就是全应用唯一的本地曲目数据源）。
     *
     *   `media_meta_read_tags()` 只读文件开头的标签区（MP3 = ID3v2 帧、FLAC = 元数据块链，
     *   PICTURE 块按长度跳过 ✓），每个文件是"打开 + 少量读"，扫描开销可接受 ✓；
     *   不支持的容器返回 NOT_SUPPORTED ⇒ 自然保留文件名兜底 ✓。
     */
    media_tags_t tags = {};
    if (media_meta_read_tags(t->path, &tags) == ESP_OK) {
        if (tags.title[0]) {
            utf8_copy(t->title, sizeof(t->title), tags.title);
        }
        if (tags.artist[0]) {
            utf8_copy(t->artist, sizeof(t->artist), tags.artist);
        }
    }

    s_count++;
}

static int scan_dir(const char *dir, int depth, storage_scan_cb_t cb, void *user, int scanned)
{
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGD(TAG, "opendir(%s) failed", dir);
        return scanned;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        char full[STORAGE_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) {
            /* 宁可整首跳过，也不要用截断的"短路径"（后半截文件名会全错）。
             * 日志里把目录和名字**分开完整打印**，不要再拼一个截断的短路径出来 */
            ESP_LOGW(TAG, "path too long (%d bytes), skip entry: %s / %s", n, dir, ent->d_name);
            continue;
        }

        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) {   /* 最多下钻 3 层，防止异常目录树 */
                scanned = scan_dir(full, depth + 1, cb, user, scanned);
            }
            continue;
        }

        scanned++;
        if (!storage_service_is_audio(ent->d_name)) {
            continue;
        }

        add_track(full, ent->d_name, (uint32_t)st.st_size);

        if (cb && (s_count % 8) == 0) {
            cb((int)s_count, scanned, user);
        }
        /* 分片让出 CPU */
        if (scanned % SCAN_YIELD_EVERY == 0) {
            vTaskDelay(1);
        }
    }
    closedir(d);
    return scanned;
}

esp_err_t storage_service_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    tracks_reserve(64);          /* 初始 64 条，之后按实际文件数翻倍扩容 */
    if (!s_tracks) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    sd_monitor_start();
    return ESP_OK;
}

/* ------------------------------------------------------------------ SD 热插拔
 *
 * 见 storage_service.h 的说明：板上没有卡检测脚，只能定期试探。
 *
 * 节奏（都在本任务里，优先级比 LVGL 低，卡初始化那几十毫秒不会影响刷新）：
 *   - 已挂载：每 SD_PROBE_MS 发一条 CMD13；连续 SD_PROBE_FAIL_MAX 次不应答才
 *     认定"卡拔了"（一次超时不算拔卡，SD 偶发时序抖动是会有的）；
 *   - 未挂载：每 SD_MOUNT_RETRY_MS 试挂一次，连续失败就退到 SD_MOUNT_SLOW_MS
 *     （没插卡时没必要每 1.5 秒去初始化一次卡）。
 *
 * ★ 2026-10 改：每次挂载尝试前先给卡做一次**断电重启**
 * （bsp_sdcard_power_cycle()），拔卡/卸载后立刻断电。原因是板级管理器的电源
 * 控制只在"设备 init 时"自动上电一次、失败不回退，它的电源设备引用计数一旦
 * ≥1 就再也不重新 init —— 于是连着失败几次之后卡上一直有电，插拔时卡内部的
 * 上电初始化序列根本没重跑，SDMMC/FATFS 两边就都停在半初始化状态（用户看到
 * 的就是"插拔没反应、换卡得重启"）。硬件上 SD_CTRL(GPIO39) 本来就是干这个的。
 *
 * 状态变化才 +1 代次、才打日志：UI 靠代次判断"要不要刷新"，日志也不刷屏。
 *
 * 并发：挂/卸载/断电都在 s_sd_lock 里做（监控任务 vs 设置页"重新检测"）。CMD13 探针
 * 不需要这把锁 —— IDF 的 SDMMC 驱动自己在每条事务外面就拿了控制器的 mutex
 * （sd_trans_sdmmc.c 的 sd_host_slot_sdmmc_do_transaction() 开头），所以探测和
 * 播放中的 FATFS 读不会撞车，最多互相等一会儿。
 */
#define SD_PROBE_MS           1000
#define SD_PROBE_FAIL_MAX     2
/*
 * 未挂载时的重试节奏。
 *
 * 慢档从 5000 ms 收到 **3000 ms**、门槛从 4 次放到 8 次（2026-10 改）：
 * 拔卡之后用户去电脑拷完文件再插回来，往往已经过去十几秒 —— 那时早就落进慢档了，
 * 而"插进去要等 5 秒才认"和"插进去没反应"在体感上没什么区别。
 * 慢档 3 s = 插卡最迟 ~3.2 s 被发现（快档 1.5 s），代价只是空槽多跑几次
 * 断电重启 + 探卡（每次几十毫秒，且绝大部分是 vTaskDelay）。
 */
#define SD_MOUNT_RETRY_MS     1500
#define SD_MOUNT_SLOW_MS      3000
#define SD_MOUNT_SLOW_AFTER   8
/* 栈要够 FATFS 挂载那一趟用（esp_vfs_fat_sdmmc_mount 内部走 sdmmc_card_init +
 * f_mount，IDF 例程里这类任务一般给 4 KB）。探卡的 sdmmc_card_t 是 malloc 的，
 * 不占这里。 */
#define SD_MON_TASK_STACK     4096
#define SD_MON_TASK_PRIO      2

static volatile uint32_t s_sd_gen;          /* 状态代号次（每次变化 +1） */
static volatile bool     s_sd_mounted;      /* 本服务记录的挂载状态 */
static volatile int      s_sd_state;        /* sd_state_t：给状态栏图标用 */
static TaskHandle_t      s_sd_task;
static SemaphoreHandle_t s_sd_lock;         /* 串行化 mount/unmount */

/*
 * 代次自增：监控任务写、UI 任务读，所以是 volatile。不能写成 `s_sd_gen++` ——
 * C++20 起对 volatile 用 ++ 是 deprecated，本工程 -Werror 直接编译不过；
 * 它只在锁里被改，也不需要原子读改写。
 */
static void sd_gen_bump(void)
{
    s_sd_gen = s_sd_gen + 1;
}

uint32_t storage_service_sd_generation(void)
{
    return s_sd_gen;
}

bool storage_service_sd_is_mounted(void)
{
    return s_sd_mounted;
}

sd_state_t storage_service_sd_state(void)
{
    return (sd_state_t)s_sd_state;
}

/* 状态只在**真的变了**时写 + 打一行日志（监控任务每秒都在改它，不能刷屏） */
static void sd_set_state(sd_state_t st)
{
    if ((sd_state_t)s_sd_state == st) {
        return;
    }
    s_sd_state = (int)st;
    ESP_LOGI(TAG, "SD state -> %s", st == SD_STATE_READY ? "ready"
                                : st == SD_STATE_ERROR ? "error" : "absent");
}

/* 挂/卸载的唯一入口：都在锁里做，并且只在状态真的变了时才 +1 代次。
 * 锁在 sd_monitor_start() 里建（那时还是单线程上下文），这里只管拿。 */
static bool sd_lock_take(void)
{
    return (s_sd_lock != NULL) && (xSemaphoreTake(s_sd_lock, portMAX_DELAY) == pdTRUE);
}

static void sd_lock_give(void)
{
    if (s_sd_lock) {
        xSemaphoreGive(s_sd_lock);
    }
}

/**
 * 没插卡时怎么判"卡在不在"：挂载已经失败了，再跑一次纯协议层探卡
 * （不挂 FATFS）—— 有应答说明"卡在但读不出来"（ERROR），无应答才是"没插卡"。
 */
static sd_state_t sd_classify_absent(void)
{
    const bsp_sd_presence_t p = bsp_sdcard_presence_probe();
    return (p == BSP_SD_PRESENT) ? SD_STATE_ERROR : SD_STATE_ABSENT;
}

/** 返回 true 表示状态发生了变化（代次已 +1） */
static bool sd_try_mount(bool quiet)
{
    if (!sd_lock_take()) {
        return false;
    }
    bool changed = false;
    if (!bsp_sdcard_is_mounted()) {
        /*
         * 后台轮询这条路上，没插卡时**每次**尝试挂载都会从板级管理器和 SDMMC
         * 驱动里刷好几行 ERROR（"Failed to initialize device fs_sdcard"、
         * "Failed to mount filesystem"、"send_op_cond returned 0x107"…），
         * 而这是每几秒一次的常态 —— 日志会被它填满。
         *
         * 这里在"试这一次"的窗口里把**默认**日志级别压到 NONE，试完立刻恢复
         * （这几个 tag 都没被单独设置过，所以都跟着默认级别走；窗口只有一次卡
         * 初始化的几十~一百毫秒）。成功那一次自己打一行 INFO，失败不打。
         *
         * 只压"后台重试"这一条路：设置页点「重新检测」时保留日志，那是用户主动
         * 要看的。断电重启与探卡也在这个窗口里（它们各自都会打 INFO）。
         */
        const esp_log_level_t saved_level = esp_log_get_default_level();
        if (quiet) {
            esp_log_level_set("*", ESP_LOG_NONE);
        }

        /*
         * ★ 先断电重启，再挂：新插入的卡只有这样才能稳定地重新跑一遍上电
         * 初始化（否则它带着上一次的 OCR/状态，ACMD41 经常直接超时）。
         * 断电失败（板子没配电源设备）不拦着挂载 —— 退化成老行为。
         */
        esp_err_t ret = bsp_sdcard_power_cycle();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "sd power cycle failed: %s (继续直接挂)", esp_err_to_name(ret));
        }
        ret = bsp_sdcard_mount();

        /* 挂不上时再探一次卡：把"没插卡"和"卡在但读不出来"分开（状态栏图标要用） */
        const sd_state_t st = (ret == ESP_OK) ? SD_STATE_READY : sd_classify_absent();

        if (quiet) {
            esp_log_level_set("*", saved_level);
        }

        if (ret == ESP_OK) {
            s_sd_mounted = true;
            sd_gen_bump();
            changed = true;
            sd_set_state(SD_STATE_READY);
            ESP_LOGI(TAG, "SD inserted at %s (gen %u)", bsp_sdcard_mount_point(),
                     (unsigned)s_sd_gen);
        } else {
            sd_set_state(st);
        }
    }
    sd_lock_give();
    return changed;
}

/** 返回 true 表示状态发生了变化（代次已 +1） */
static bool sd_drop_mount(void)
{
    if (!sd_lock_take()) {
        return false;
    }
    bool changed = false;
    if (bsp_sdcard_is_mounted()) {
        /* 卡已经不在了：把挂载点放掉，下一次尝试挂载才可能识别到**换的那张卡** */
        bsp_sdcard_unmount();
        s_sd_mounted = false;
        sd_gen_bump();
        changed = true;
        ESP_LOGW(TAG, "SD removed (gen %u)", (unsigned)s_sd_gen);
    }
    /*
     * 顺序要紧：**先探卡再断电**。探卡要有电（CMD8/ACMD41 是卡自己回的），
     * 而断电之后探谁都只会得到"无应答"，那就永远分不出"拔了"和"坏了"。
     * 此刻电源还开着 —— 挂载时的上电在板级电源设备里，卸载只是解除引用，
     * 引脚仍停在使能电平。
     */
    sd_set_state(sd_classify_absent());
    /* 断电：让下一次插入的卡从冷启动开始（挂载期间断电会被 bsp_sdcard_power 拒绝） */
    bsp_sdcard_power(false);
    sd_lock_give();
    return changed;
}

bool storage_service_sd_rescan(void)
{
    /* 用户主动点的：保留板级/驱动的报错，方便看为什么没挂上 */
    sd_try_mount(false);
    return s_sd_mounted;
}

static void sd_monitor_task(void *arg)
{
    (void)arg;
    int probe_fail = 0;     /* 已挂载时：连续探卡失败次数，到 SD_PROBE_FAIL_MAX 就卸载 */
    int mount_fail = 0;     /* 未挂载时：连续挂载失败次数，用来把重试间隔放慢 */

    for (;;) {
        if (bsp_sdcard_is_mounted()) {
            mount_fail = 0;
            if (bsp_sdcard_probe() == ESP_OK) {
                /*
                 * ★★ 连续失败计数**只能在探到卡时清零**（2026-10 修）。
                 *
                 * 老代码把 `fail = 0;` 写在每一轮循环的**开头**，而一轮只探一次卡 ——
                 * 于是 `++fail` 永远只到 1，`SD_PROBE_FAIL_MAX`(2) 这个阈值
                 * **永远达不到**，`sd_drop_mount()` 一次都不会被调用：
                 *   s_mounted 一直是 true → 监控任务永远走"已挂载"这一支 →
                 *   再也不会去试挂新插入的卡。
                 * 表现就是用户报的"SD 卡拔掉后没有轮询去检测新插入的卡"，
                 * 以及更早那句"每次热插拔都不能正常响应"（卡拔了界面还显示已挂载、
                 * 换卡要重启）。**清零点写错位置 = 整个热插拔形同不存在**，
                 * 动这段逻辑前先确认"计数真的能累加上去"。
                 */
                probe_fail = 0;
                sd_set_state(SD_STATE_READY);
            } else {
                probe_fail++;
                /* 先如实报 ERROR（状态栏亮 sd_card_WARN）；连错 SD_PROBE_FAIL_MAX
                 * 次才真的卸载 —— 一次 CMD13 超时可能只是时序抖动 */
                sd_set_state(SD_STATE_ERROR);
                if (probe_fail >= SD_PROBE_FAIL_MAX) {
                    probe_fail = 0;
                    sd_drop_mount();
                } else {
                    /* 拔卡路径上最多打 1 行（下一次就真的卸载了），不算刷屏 */
                    ESP_LOGW(TAG, "SD probe failed (%d/%d)", probe_fail, SD_PROBE_FAIL_MAX);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(SD_PROBE_MS));
        } else {
            probe_fail = 0;
            /* quiet = true：没插卡时每几秒一次的失败尝试不该刷日志 */
            if (!sd_try_mount(true)) {
                mount_fail++;
            } else {
                mount_fail = 0;
            }
            const uint32_t wait = (mount_fail >= SD_MOUNT_SLOW_AFTER) ? SD_MOUNT_SLOW_MS
                                                                    : SD_MOUNT_RETRY_MS;
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
    }
}

static void sd_monitor_start(void)
{
    if (s_sd_task) {
        return;
    }
    /*
     * 锁在这里建：本函数只从 storage_service_init() 里调一次，那时监控任务
     * 还没起来，是单线程上下文。放到"谁先用到谁建"就会有两个任务同时建锁的
     * 竞态（各自拿到不同的锁 = 等于没锁）。
     */
    if (s_sd_lock == NULL) {
        s_sd_lock = xSemaphoreCreateMutex();
    }
    /* 开机时 main 已经试过一次挂载：把结果记下来，否则第一次轮询会误报"插入" */
    s_sd_mounted = bsp_sdcard_is_mounted();
    s_sd_gen = s_sd_mounted ? 1u : 0u;
    s_sd_state = s_sd_mounted ? SD_STATE_READY : SD_STATE_ABSENT;
    if (xTaskCreate(sd_monitor_task, "sd_mon", SD_MON_TASK_STACK, NULL,
                    SD_MON_TASK_PRIO, &s_sd_task) != pdPASS) {
        s_sd_task = NULL;
        ESP_LOGW(TAG, "SD monitor task create failed (热插拔不可用)");
    }
}

int storage_service_scan(storage_scan_cb_t cb, void *user)
{
    if (!s_ready && storage_service_init() != ESP_OK) {
        return -1;
    }

    s_count = 0;

    if (!bsp_sdcard_is_mounted()) {
        ESP_LOGW(TAG, "sdcard not mounted, no local music");
        return 0;
    }

    /* 优先 /sdcard/music（大小写两种），没有就直接扫 /sdcard */
    const char *root = NULL;
    struct stat st;
    if (stat(MUSIC_ROOT, &st) == 0 && S_ISDIR(st.st_mode)) {
        root = MUSIC_ROOT;
    } else if (stat(MUSIC_ROOT_ALT, &st) == 0 && S_ISDIR(st.st_mode)) {
        root = MUSIC_ROOT_ALT;
    } else {
        root = bsp_sdcard_mount_point();
    }

    ESP_LOGI(TAG, "scan start: %s", root);
    int scanned = scan_dir(root, 0, cb, user, 0);
    ESP_LOGI(TAG, "scan done: found %u audio in %d entries", (unsigned)s_count, scanned);

    if (cb) {
        cb((int)s_count, scanned, user);
    }
    return (int)s_count;
}

size_t storage_service_count(void)
{
    return s_count;
}

const track_t *storage_service_track(size_t idx)
{
    if (idx >= s_count) {
        return NULL;
    }
    return &s_tracks[idx];
}

const char *storage_service_dir(void)
{
    return MUSIC_ROOT;
}

void storage_service_clear(void)
{
    s_count = 0;
}

/* ------------------------------------------------------------------ 目录浏览 */

static int cmp_entry(const void *a, const void *b)
{
    const fs_entry_t *x = (const fs_entry_t *)a;
    const fs_entry_t *y = (const fs_entry_t *)b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;      /* 目录排在文件前面 */
    }
    return strcasecmp(x->name, y->name);
}

int storage_service_browse(const char *dir, fs_entry_t *out, int max)
{
    if (!dir || !out || max <= 0) {
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "browse: opendir(%s) failed", dir);
        return -1;
    }

    int n = 0;                          /* 已写入 out 的条数 */
    int total = 0;                      /* 通过筛选的条数（可能 > max） */
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;                    /* 隐藏项，含 "." / ".." */
        }
        char full[STORAGE_PATH_MAX];
        int len = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (len <= 0 || len >= (int)sizeof(full)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        total++;
        if (n >= max) {
            continue;                    /* 缓冲装不下：只计数，调用方扩容后会重调 */
        }
        fs_entry_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        utf8_copy(e->name, sizeof(e->name), ent->d_name);
        e->is_dir   = S_ISDIR(st.st_mode);
        e->is_audio = !e->is_dir && storage_service_is_audio(e->name);
        e->size     = e->is_dir ? 0 : (uint32_t)st.st_size;
        n++;
    }
    closedir(d);

    if (n > 1) {
        qsort(out, (size_t)n, sizeof(fs_entry_t), cmp_entry);
    }
    if (total > n) {
        ESP_LOGW(TAG, "browse '%s': %d entries but buffer holds %d — caller grows and retries",
                 dir, total, n);
    }
    return total;
}

/* 排序用的临时条目（名字 + 大小），只在 load_dir 里用 */
typedef struct {
    char     name[STORAGE_NAME_MAX];
    uint32_t size;
} dir_item_t;

static int cmp_dir_item(const void *a, const void *b)
{
    return strcasecmp(((const dir_item_t *)a)->name, ((const dir_item_t *)b)->name);
}

int storage_service_load_dir(const char *dir)
{
    if (!s_ready && storage_service_init() != ESP_OK) {
        return -1;
    }
    if (!dir) {
        return -1;
    }

    s_count = 0;
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "load_dir: opendir(%s) failed", dir);
        return -1;
    }

    /*
     * 先收名字再排序，保证队列顺序和浏览页一致。
     *
     * 这里原来是两个写死 STORAGE_BROWSE_MAX(=64) 的数组：一个目录超过 64 首时，
     * 队列只剩 readdir 顺序的前 64 首（**不是**字母序前 64），表现为"歌列表/上下曲
     * 不全" —— 就是本次修的 bug。改成按实际文件数翻倍扩容，优先放 PSRAM，
     * 只有极端情况（单目录 > TRACK_MAX 首）才截断并打日志。
     */
    dir_item_t *items = NULL;
    size_t cap = 0;
    size_t n   = 0;
    bool   hit_cap = false;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' || !storage_service_is_audio(ent->d_name)) {
            continue;
        }
        char full[STORAGE_PATH_MAX];
        int len = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (len <= 0 || len >= (int)sizeof(full)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        if (n == cap) {
            if (cap >= TRACK_MAX) {
                hit_cap = true;
                break;
            }
            size_t ncap = cap ? cap * 2 : 64;
            if (ncap > TRACK_MAX) {
                ncap = TRACK_MAX;
            }
            dir_item_t *p = (dir_item_t *)heap_caps_realloc(
                items, ncap * sizeof(dir_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!p) {
                p = (dir_item_t *)realloc(items, ncap * sizeof(dir_item_t));
            }
            if (!p) {
                ESP_LOGE(TAG, "load_dir: grow to %u items failed", (unsigned)ncap);
                break;
            }
            items = p;
            cap   = ncap;
        }
        utf8_copy(items[n].name, STORAGE_NAME_MAX, ent->d_name);
        items[n].size = (uint32_t)st.st_size;
        n++;
    }
    closedir(d);

    if (n > 1) {
        qsort(items, n, sizeof(dir_item_t), cmp_dir_item);
    }
    for (size_t i = 0; i < n; i++) {
        char full[STORAGE_PATH_MAX];
        int len = snprintf(full, sizeof(full), "%s/%s", dir, items[i].name);
        if (len > 0 && len < (int)sizeof(full)) {
            add_track(full, items[i].name, items[i].size);
        }
    }
    free(items);

    if (hit_cap) {
        ESP_LOGW(TAG, "load_dir '%s': hit the %u-track safety cap, rest skipped",
                 dir, (unsigned)TRACK_MAX);
    }
    ESP_LOGI(TAG, "queue from dir '%s': %u track(s)", dir, (unsigned)s_count);
    return (int)s_count;
}

int storage_service_index_of_path(const char *path)
{
    if (!path) {
        return -1;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_tracks[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}
