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
#include "bsp.h"

static const char *TAG = "storage";

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
    return ESP_OK;
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
