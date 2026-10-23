/*
 * gallery_scan.cc —— 图库：扫 SD 卡里的图片
 *
 * 只做"找文件 + 读 stat"，**不开文件内容**：
 *   - 类型按扩展名判（jpg/jpeg/png/gif，大小写不敏感）；
 *   - 大小与修改时间来自 stat()（FATFS 的 vfs_fat_stat 会把目录项里的
 *     fdate/ftime 换算成 st_mtime，见 IDF components/fatfs/vfs/vfs_fat.c 的
 *     update_stat_struct）。⚠ 必须用 **stat(路径)**，不能用 fstat(fd) ——
 *     vfs_fat_fstat 把 st_mtime 固定写成 0。
 *
 * 递归下钻 GAL_SCAN_DEPTH 层，最多收 GAL_MAX_ITEMS 张；排序在收完之后做，
 * 于是超过上限时被丢掉的是"名字靠后"的那些（顺序可预期，不是 readdir 随机序）。
 */
#include "gallery_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gallery";

bool gallery_kind_of(const char *name, gal_kind_t *out)
{
    if (!name || !out) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    const char *ext = dot + 1;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        *out = GAL_KIND_JPEG;
        return true;
    }
    if (strcasecmp(ext, "png") == 0) {
        *out = GAL_KIND_PNG;
        return true;
    }
    if (strcasecmp(ext, "gif") == 0) {
        *out = GAL_KIND_GIF;
        return true;
    }
    return false;
}

const char *gallery_kind_name(gal_kind_t k)
{
    switch (k) {
    case GAL_KIND_JPEG: return "JPEG";
    case GAL_KIND_PNG:  return "PNG";
    case GAL_KIND_GIF:  return "GIF";
    default:            return "未知";
    }
}

/* ------------------------------------------------------------------ 文件头
 *
 * 只为了"详细信息"能报**原始**分辨率（解码器会按长边折半，拿解码后的尺寸
 * 当分辨率是错的）。三个容器都只需要读前几百字节：
 *
 *   JPEG  SOF0/SOF1/SOF2 段：precision(1) height(2) width(2)，大端
 *   PNG   IHDR 固定在第 16 字节：width(4) height(4)，大端
 *   GIF   逻辑屏幕描述符：width(2) height(2)，小端（GIF 不折半，就是画布尺寸）
 * -------------------------------------------------------------------------- */

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static bool jpeg_size(FILE *f, int *w, int *h)
{
    uint8_t b[4];
    /* SOI */
    if (fread(b, 1, 2, f) != 2 || b[0] != 0xFF || b[1] != 0xD8) {
        return false;
    }
    for (int guard = 0; guard < 4096; guard++) {
        int c = fgetc(f);
        if (c != 0xFF) {
            if (c == EOF) {
                return false;
            }
            continue;   /* 找下一个标记（跳过填充/数据） */
        }
        int m = fgetc(f);
        while (m == 0xFF) {
            m = fgetc(f);    /* 标记前的填充 0xFF */
        }
        if (m == EOF) {
            return false;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            continue;        /* 无长度的标记 */
        }
        if (fread(b, 1, 2, f) != 2) {
            return false;
        }
        const uint32_t seg_len = be16(b);
        if (seg_len < 2) {
            return false;
        }
        /* SOF0..SOF15，但要排除 DHT(C4)/JPG(C8)/DAC(CC) —— 那几个不是 SOF */
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            uint8_t sof[5];
            if (fread(sof, 1, 5, f) != 5) {
                return false;
            }
            *h = (int)be16(&sof[1]);
            *w = (int)be16(&sof[3]);
            return (*w > 0 && *h > 0);
        }
        if (fseek(f, (long)seg_len - 2, SEEK_CUR) != 0) {
            return false;
        }
    }
    return false;
}

bool gallery_image_size(const char *path, gal_kind_t kind, int *w, int *h)
{
    if (!path || !w || !h) {
        return false;
    }
    *w = *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = false;
    uint8_t head[32];
    const size_t n = fread(head, 1, sizeof(head), f);
    if (n >= 10 && head[0] == 0xFF && head[1] == 0xD8) {
        rewind(f);
        ok = jpeg_size(f, w, h);
    } else if (n >= 24 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') {
        const uint32_t pw = be32(&head[16]);
        const uint32_t ph = be32(&head[20]);
        if (pw > 0 && ph > 0) {
            *w = (int)pw;
            *h = (int)ph;
            ok = true;
        }
    } else if (n >= 10 && head[0] == 'G' && head[1] == 'I' && head[2] == 'F') {
        const uint32_t gw = le16(&head[6]);
        const uint32_t gh = le16(&head[8]);
        if (gw > 0 && gh > 0) {
            *w = (int)gw;
            *h = (int)gh;
            ok = true;
        }
    } else if (kind == GAL_KIND_UNKNOWN) {
        (void)kind;
    }
    fclose(f);
    return ok;
}

/* 文件名排序（不区分大小写），用于把 scan 结果排成稳定顺序 */
static int item_cmp(const void *a, const void *b)
{
    const gal_item_t *x = (const gal_item_t *)a;
    const gal_item_t *y = (const gal_item_t *)b;
    return strcasecmp(x->name, y->name);
}

/*
 * 扫描进度：给 UI 显示"正在扫描… 已找到 N 张"（扫描线程写，LVGL 任务读）。
 * 没有它的话，扫大卡时那几百毫秒屏幕上是**一动不动**的一行字，体感像卡死。
 */
static volatile int s_scan_found;

int gallery_scan_progress(void)
{
    return s_scan_found;
}

/*
 * ⚠⚠ 这里**绝对不能**对每个目录项调 stat()。
 *
 * FATFS 的 stat/f_stat 是"从目录头开始线性查名字"（ff.c 的 dir_find），
 * 也就是说它自己就要把整个目录再扫一遍 —— 对 N 个条目各 stat 一次就是 O(N²)
 * 的目录扇区读。真机上 500 个文件的目录 = 一万多次扇区读，几秒起步，
 * 这正是"进入图库后扫描时间过长"的根因。
 *
 * 现在改成：
 *   1. 目录/文件类型直接用 readdir 的 **d_type** —— VFS 的 vfs_fat_readdir_r
 *      从 f_readdir 的 FILINFO 里填（AM_DIR 位），**不额外读卡**；
 *      只有 DT_UNKNOWN（极少数文件系统）才退回一次 stat 兜底；
 *   2. 大小/修改时间**不在这里取** —— 交给解码工作线程：它本来就要打开那个文件
 *      读文件头，顺手 stat 一次（一次 O(目录长度) 而已），随解码结果一起交回 UI。
 *   3. 非图片文件连路径都不拼（省 snprintf）。
 *
 * 结果：整趟扫描就是"每个目录顺序读一遍"，只有 O(N) 次扇区读。
 */
static void scan_dir(const char *dir, int depth, gal_item_t *items, int *count)
{
    if (*count >= GAL_MAX_ITEMS) {
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        if (depth == 0) {
            ESP_LOGW(TAG, "opendir(%s) failed", dir);
        }
        return;
    }

    struct dirent *e = NULL;
    while ((e = readdir(d)) != NULL && *count < GAL_MAX_ITEMS) {
        if (e->d_name[0] == '.') {
            continue;   /* "." ".." 以及隐藏项 */
        }

        bool is_dir;
        if (e->d_type == DT_DIR) {
            is_dir = true;
        } else if (e->d_type == DT_REG) {
            is_dir = false;
        } else {
            /* DT_UNKNOWN：退回一次 stat（FATFS 不会走到这里，留作兜底） */
            char probe[GAL_PATH_MAX];
            const int pn = snprintf(probe, sizeof(probe), "%s/%s", dir, e->d_name);
            struct stat st;
            if (pn <= 0 || pn >= (int)sizeof(probe) || stat(probe, &st) != 0) {
                continue;
            }
            is_dir = S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            if (depth + 1 >= GAL_SCAN_DEPTH) {
                continue;
            }
            char path[GAL_PATH_MAX];
            const int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (n <= 0 || n >= (int)sizeof(path)) {
                continue;
            }
            scan_dir(path, depth + 1, items, count);
            continue;
        }

        gal_kind_t kind;
        if (!gallery_kind_of(e->d_name, &kind)) {
            continue;   /* 不是图片：路径都不拼，直接下一个 */
        }

        char path[GAL_PATH_MAX];
        const int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n <= 0 || n >= (int)sizeof(path)) {
            continue;   /* 路径太长：宁可不列，也不要截断出一个打不开的路径 */
        }

        gal_item_t *it = &items[*count];
        memset(it, 0, sizeof(*it));
        snprintf(it->path, sizeof(it->path), "%s", path);
        snprintf(it->name, sizeof(it->name), "%s", e->d_name);
        it->kind  = kind;      /* size/mtime 留 0：解码线程 stat 后回填 */
        (*count)++;
        s_scan_found = *count;
    }
    closedir(d);
}

int gallery_scan(gal_item_t **out, const char *root)
{
    if (!out) {
        return -1;
    }
    *out = NULL;

    /*
     * 600 × 约 780 B ≈ 468 KB —— 一定要放 PSRAM。
     * 只写 calloc() 也行（IDF 对超过 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 的请求
     * 默认走 PSRAM），但那样一旦 PSRAM 紧张就会静默落回内部 RAM 并失败，
     * 所以这里显式指定，再把 calloc 当兜底。
     */
    gal_item_t *items = (gal_item_t *)heap_caps_calloc(GAL_MAX_ITEMS, sizeof(gal_item_t),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) {
        items = (gal_item_t *)calloc(GAL_MAX_ITEMS, sizeof(gal_item_t));
    }
    if (!items) {
        ESP_LOGW(TAG, "scan: out of memory (%u items)", (unsigned)GAL_MAX_ITEMS);
        return -1;
    }

    s_scan_found = 0;
    const int64_t t0 = esp_timer_get_time();
    int count = 0;
    scan_dir(root ? root : "/sdcard", 0, items, &count);
    if (count > 1) {
        qsort(items, (size_t)count, sizeof(gal_item_t), item_cmp);
    }
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "scan %s: %d image(s) in %u ms", root ? root : "/sdcard", count,
             (unsigned)ms);
    *out = items;
    return count;
}
