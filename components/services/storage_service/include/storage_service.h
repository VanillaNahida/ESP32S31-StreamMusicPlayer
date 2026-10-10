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

/* ------------------------------------------------------------------ SD 热插拔
 *
 * 板子上**没有卡检测脚**（板级 YAML 里 fs_sdcard 没有 cd），所以"卡在不在"
 * 只能靠定期试探：已挂载时发一条 CMD13（`bsp_sdcard_probe()`），没挂载时
 * 定期尝试挂载。这两件事都在 storage_service 自己的低优先级任务里做，
 * UI 只管看下面这个"代次"。
 *
 * 为什么必须有：不轮询的话，开机时挂上的那份 FATFS 会一直挂在 VFS 上 ——
 * 卡拔了界面还显示"已挂载"，换一张卡进来也读不到新内容（就是"只有重启才能
 * 识别到更换的卡"）。而且 FATFS 会把目录项缓存在内存里，普通文件操作未必
 * 报错，非得问卡本身不可。
 */

/*
 * SD 状态代号次：**每次插入/拔出各 +1**（开机时的首次挂载也算一次变化）。
 *
 * 多处 UI 同时看它没问题：各自记住上次见到的值，变了就刷新（不像"事件队列"
 * 那样只能被一个人消费）。没变化时返回值稳定。
 */
uint32_t storage_service_sd_generation(void);

/** 当前 SD 是否已挂载（与 bsp_sdcard_is_mounted() 同源，供 UI 查询） */
bool storage_service_sd_is_mounted(void);

/*
 * 立刻重试一次（"设置 → 存储"的「重新检测」按钮用）。
 *
 * 是**同步**的：会尝试挂载并更新代次，返回是否已挂载。调用方在 LVGL 任务里，
 * 阻塞时间 = 一次卡初始化（几十~几百毫秒），和这个按钮以前直接调
 * bsp_sdcard_mount() 的开销一样；不同之处是现在与监控任务之间加了锁，
 * 两者不会同时去 init/deinit 同一个 SDMMC 设备。
 */
bool storage_service_sd_rescan(void);

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

/*
 * 返回值可能**大于** max：那说明目录里条目比缓冲多、out 里只装了前 max 条
 * （排序后仍是最小的那批），真实条数就是返回值。调用方据此扩容后重调一次
 * 即可拿全 —— 这样"缓冲给多大"由调用方决定，服务不再替它砍数据。
 *
 * 旧的 STORAGE_BROWSE_MAX = 64 就是"显示不全"的根因：一个目录超过 64 首时，
 * 列表和播放队列都只剩前 64 条（而且是 readdir 顺序的前 64 条，不是字母序前 64）。
 */
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
 *
 * 队列按目录内**实际音频文件数**动态扩容（不再有 64 条的固定上限）；
 * 极端情况（单目录几万首）由服务内部的 TRACK_MAX 安全上限兜住，并打日志。
 */
int storage_service_load_dir(const char *dir);

/* 按 VFS 全路径在队列里找下标，找不到返回 -1（浏览页点文件时用） */
int storage_service_index_of_path(const char *path);

#ifdef __cplusplus
}
#endif