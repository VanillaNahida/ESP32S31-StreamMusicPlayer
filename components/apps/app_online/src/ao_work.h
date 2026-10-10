/*
 * ao_work.h —— app_online 的**工作线程 + 请求/结果协议**（跨任务边界）
 *
 * 线程模型（照 app_music 的老规矩）：
 *   LVGL 任务只做 UI；HTTP（抓歌单/搜索/下载）全在 online_work 任务里做，
 *   结果写进一组静态槽位 + 一个"完成"标志（s_res_kind），由 200 ms 的 LVGL 定时器
 *   经 ao_work_take() 搬到界面上。
 *
 * ★ 拆分后**跨任务只允许走这两个函数**：ao_work_submit*() / ao_work_take()。
 *   任何外部代码都不许直接碰槽位 —— 顺序一乱就是"结果被覆盖 / 数组泄漏 /
 *   悬垂指针"（见 docs/refactor/04_app_online.md 的风险 R1）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "ao_common.h"

/*
 * 结果槽位的**一次性快照**。
 *
 * 为什么要把所有负载（含 off/want/img/pls）都塞进一个结构体：原来的 on_result()
 * 是"先按 kind 拷一部分到本地、再清槽位"，各种字段散落在不同 case 里；拆文件后
 * 只要有人漏拷一个字段，就会出现"读到的还是下一份请求的值"。
 * 现在 ao_work_take() 一次性全拷 + 全清，语义只有一处。
 */
typedef struct {
    int       kind;               /* OREQ_NONE = 这次没有结果 */
    esp_err_t err;
    online_song_t *songs;         /* 所有权转给消费方（free） */
    size_t    count;
    char      msg[64];            /* 歌单 ID / 昵称 / unikey */
    int       off;                /* OREQ_ACCT_SONGS：这一页从第几首开始 */
    int       want;               /* OREQ_ACCT_SONGS：本来要取多少首 */
    uint8_t  *img;                /* 二维码 PNG（所有权转给消费方，free） */
    size_t    img_len;
    int       qr;                 /* 800/801/802/803 */
    online_acct_playlist_t *pls;  /* 账号歌单列表（所有权转给消费方，free） */
    size_t    pls_n;
} ao_result_t;

/* 建请求队列（深度 4）+ online_work 任务（栈 WORK_TASK_STACK，优先级 3）。可重复调用。 */
void ao_work_init(void);

/* 页面存活标志：工作线程发布结果前看一眼（页面没了就没人消费那份结果了）。 */
void ao_work_set_page_alive(bool alive);

/* 组 oreq_t 入队（非阻塞；队列满 → Toast「上一个操作还没做完」）。 */
void ao_work_submit(int kind, const char *arg);
/* 带分页参数（off/cnt）的版本：账号歌单懒加载用。 */
void ao_work_submit_ex(int kind, const char *arg, int off, int cnt);

/*
 * 取一份结果：**先把负载全拷到 out，再把槽位置空**（顺序不能反 —— 清了才轮到工作
 * 线程发下一份）。槽位空时 out->kind = OREQ_NONE，其余字段已清零。
 */
void ao_work_take(ao_result_t *out);

/*
 * 页面销毁时清槽位：结果里压着的数组/二维码/歌单列表就地释放（否则没人消费 = 泄漏，
 * 而且工作线程下一轮要白等 3 秒）。
 */
void ao_work_discard_pending(void);
