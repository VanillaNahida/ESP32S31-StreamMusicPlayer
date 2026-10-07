/*
 * storage_service.h —— 本地曲库：扫描板载 microSD 上的音频文件
 *
 * 只做"找文件 + 从文件名推标题/歌手"，ID3/Vorbis 深度解析后续再加
 * （开发指南 5.10）。扫描分片让出 CPU，避免阻塞 LVGL。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 路径缓冲：FATFS 单个文件名最长 255 **字节**（CONFIG_FATFS_MAX_LFN），
 * 扫描最多下钻 3 层，所以 "/sdcard/" + 3×(255+1) 才是上界。
 * 原来给 256 会在长中文名（一个字 3 字节）时**截断**，截出来的就是一个
 * 打不开的"短路径"，顺带 .lrc 边文件也找不到。
 */
#define STORAGE_PATH_MAX   512
#define STORAGE_TITLE_MAX  128

typedef struct {
    char     path[STORAGE_PATH_MAX];   /* VFS 绝对路径，如 /sdcard/Music/xxx.mp3 */
    char     title[STORAGE_TITLE_MAX]; /* 由文件名解析的标题 */
    char     artist[STORAGE_TITLE_MAX];
    uint32_t size_bytes;
} track_t;

/* 扫描进度回调（在扫描任务上下文；found=已找到数，scanned=已扫描文件数） */
typedef void (*storage_scan_cb_t)(int found, int scanned, void *user);

esp_err_t storage_service_init(void);

/* 同步扫描（调用方负责放在低优先级任务里）；返回找到的曲目数，负数为错误 */
int storage_service_scan(storage_scan_cb_t cb, void *user);

size_t         storage_service_count(void);
const track_t *storage_service_track(size_t idx);
const char    *storage_service_dir(void);
void           storage_service_clear(void);

/* 按扩展名判断是否为本项目支持的音频 */
bool storage_service_is_audio(const char *name);

/* ------------------------------------------------------------------ 目录浏览
 *
 * 本地音乐页是"SD 卡文件浏览器"：只列**当前这一层**，不递归。
 *   - 目录条目排在文件前面，同类内按名字排序（不区分大小写）
 *   - 隐藏项（以 '.' 开头）不列
 * 返回条目数，负数为错误。
 */
#define STORAGE_NAME_MAX    256    /* FATFS 单个文件名最长 255 字节 + NUL */
#define STORAGE_BROWSE_MAX  64     /* 一次最多列这么多条（调用方给缓冲） */

typedef struct {
    char     name[STORAGE_NAME_MAX];
    bool     is_dir;
    bool     is_audio;
    uint32_t size;
} fs_entry_t;

int storage_service_browse(const char *dir, fs_entry_t *out, int max);

/*
 * 把某个目录**这一层**的音频文件设为播放队列（上一首/下一首就在这个目录里轮转），
 * 返回曲目数。浏览页每次进目录都会调它。
 */
int storage_service_load_dir(const char *dir);

/* 按 VFS 全路径在队列里找下标，找不到返回 -1（浏览页点文件时用） */
int storage_service_index_of_path(const char *path);

#ifdef __cplusplus
}
#endif