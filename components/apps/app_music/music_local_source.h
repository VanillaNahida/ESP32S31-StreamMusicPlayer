/*
 * music_local_source.h —— 本地目录的**数据源**：异步扫描线程 + 条目缓冲 + SD 代次
 *
 * 搬迁自 app_music.cc 的 2896–2937（条目缓冲扩容）、3110–3263（扫描线程与结果消费）、
 * 3357–3431 的数据部分（SD 热插拔代次）。这里**绝不碰 LVGL**：
 *   - 工作线程只写条目数组与音乐 APP 状态的扫描标志；
 *   - 渲染与"进目录/翻页/点行"由 music_view_local.cc 负责，通过下面这几个入口拿数据。
 *
 * ★ 发布顺序（原注释点名，不可改）：**先写数据 → 再置 done → 最后才清 busy**。
 *   反过来的话，tick 有可能在"busy 已清、done 还没置"的缝里又起一个扫描，
 *   两个线程同时写 s_entries。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 数据部分一眼可见的容量上限：条目缓冲最多这么多条（一条 fs_entry_t 264 字节） */
#define LOCAL_ENTRIES_MAX  8192
/* 一页最多画这么多行的**数据侧**口径在视图里（LOCAL_PAGE_ROWS）；这里只管缓冲 */

/* SD 热插拔轮询周期（视图的定时器用它） */
#define LOCAL_SD_POLL_MS 500

/* 扫描结果的可见状态（给视图渲染用） */
typedef struct {
    fs_entry_t *entries;          /* 条目数组（PSRAM） */
    size_t      cap;              /* 数组容量（条） */
    int         n;                /* 本次渲染的条目总数 */
} local_dir_view_t;

/* 初始化当前目录（挂载点；没挂载时由调用方先判） */
void local_source_begin(const char *dir);

/* 当前目录（".."/换目录都改它） */
const char *local_source_dir(void);
void        local_source_set_dir(const char *dir);

/* 确保条目缓冲至少能装 need 条。按 64 → 翻倍 → 上限的顺序扩容，优先 PSRAM。
 * 返回能否满足。 */
bool local_source_reserve(size_t need);

/* 取当前快照（渲染用）。entries 可能为 NULL（还没扩容过）。 */
void local_source_view(local_dir_view_t *out);
/* 条目总数（视图的 current_dir 快照 part） */
int  local_source_entry_count(void);
/* 扫描成功后由视图写回"实际要渲染多少条"（不变量：必须 ≤ cap） */
void local_source_set_entry_count(int n);

/* 起一次扫描（LVGL 任务里调）。忙着就把请求排进队列，由下一次轮询补起。 */
void local_source_scan_start(const char *dir);

/* 作废在飞的扫描（页面销毁 / 拔卡）：结果不该留给下一个页面 */
void local_source_invalidate(void);

/* 上一次扫描是不是失败了（视图据此画"读取目录失败"） */
bool local_source_last_failed(void);

/*
 * SD 热插拔代次：记下当前代次（建页面时调一次）。
 * 之后由 local_source_poll() 比对，代次变了才报插/拔。
 */
void local_source_sd_sync(void);

typedef struct {
    bool scan_ready;              /* 有新目录数据要渲染（seq 对得上） */
    bool sd_removed;              /* 卡拔了 */
    bool sd_inserted;             /* 卡在（含刚插上/换卡） */
} local_poll_result_t;

/*
 * 由本地页的 500 ms 定时器调用：
 *   ① 取异步扫描结果（seq 对得上才算当前目录）→ scan_ready；
 *   ② 扫描期间排队的新请求 → 起一个；
 *   ③ 比对 SD 代次 → 变化时置 sd_removed / sd_inserted。
 */
void local_source_poll(local_poll_result_t *out);

#ifdef __cplusplus
}
#endif
