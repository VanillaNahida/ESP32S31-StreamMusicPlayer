/*
 * ao_common.h —— app_online 拆分后的**共用定义**（请求种类 + 分页/容量常量）
 *
 * 这里只放"好几个 .cc 都要知道"的纯 POD 定义与数字常量。
 * ⚠ 本头文件**不许** include 任何 ui_* / lvgl 头：纯逻辑模块 `ao_model.cc` 也要
 *   include 它，必须保持"不依赖 LVGL"（见 docs/refactor/04_app_online.md §6.3）。
 */
#pragma once

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "online_service.h"

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

/* ---- 第 0 页「我的歌单」：网易云账号页 ---- */
#define ACCT_PL_MAX  200               /* 账号歌单最多显示多少个（自己创建 + 收藏）
                                        * 40 → 200：用户要求"列出我账号里的所有歌单"
                                        * （每个约 390 B，200 个 ≈ 78 KB，PSRAM 够） */
#define ACCT_SONGS_MAX 300             /* **每日推荐**一次最多取多少首（~33 首的量级）。
                                        * 账号歌单不再用这个上限：它走懒加载 + 分页
                                        * （见 ONLINE_SONGS_MAX / ONLINE_PAGE_ROWS） */
/* 重试间隔：失败一次翻一倍，最多 ACCT_RETRY_MAX_MS —— 服务端一直不通/ cookie 失效时
 * 不至于每 5 秒打一次（那种情况可能持续几小时）。成功或重新进页面时复位。 */
#define ACCT_RETRY_MS       5000
#define ACCT_RETRY_MAX_MS  60000

/*
 * 详情请求的"发出时刻"：转圈超过 DETAIL_TIMEOUT_MS 还没结果就换成一个"重试"入口
 * （网络很慢、或者请求根本没被工作线程收下时，不至于一直转到天荒地老）。
 * 20 秒 > HTTP_TIMEOUT_MS(15 s)，不会把"正常的慢请求"误判成超时。
 * 占位界面本体在 ao_detail_wait_rows()（ao_widgets.cc）。
 */
#define DETAIL_TIMEOUT_MS  20000

/*
 * ⚠ 十六进制转义后面紧跟 'd'（合法的十六进制字符）会被**并进同一个转义**里：
 *   `"\x01daily"` 等于 `\x01d` + "aily" —— `\x01d` 超出一个字节，GCC 直接
 *   `error: hex escape sequence out of range`（-Werror 下编译不过）。
 *   标准写法是拆成两个字符串字面量，让转义在 `\x01` 处结束。
 */
#define DETAIL_KEY_DAILY  "\x01" "daily"

/*
 * ★ 一次性推荐列表的队列 Key **必须按音源 + 种类区分**（2026-11 用户报"两家的每日推荐/
 *   私人雷达串了"）：只要 Key 相同，ao_detail_key_matches() 就会认为"这份列表就是我要的"，
 *   于是刚点开新入口的那 1~2 秒里，屏幕上画的是**上一份列表的歌**；请求失败时更是一直留着。
 *   网易云的每日推荐继续用 DETAIL_KEY_DAILY，QQ 的两个入口各用自己的 Key。
 */
#define DETAIL_KEY_QQ_DAILY  "\x02" "qq-daily"
#define DETAIL_KEY_QQ_RADAR  "\x02" "qq-radar"

/* ---------------------------------------------------------------- 工作线程请求 */

typedef enum {
    OREQ_NONE = 0,
    OREQ_ADD,        /* arg = 歌单 ID */
    OREQ_LOAD,       /* arg = 歌单 ID */
    OREQ_SEARCH,     /* arg = 关键词 */
    OREQ_REMOVE,     /* arg = 歌单 ID */
    OREQ_QR_KEY,     /* 扫码：取 identifier + 二维码 PNG（无参数；网易云/QQ 由音源决定） */
    OREQ_QR_POLL,    /* 扫码：轮询，arg = identifier（网易云是 unikey） */
    OREQ_LOGOUT,     /* 退出登录 */
    OREQ_ACCOUNT,    /* 取账号信息（昵称） */
    OREQ_ACCT_PLS,   /* 账号歌单列表（worker 内部先取 uid） */
    OREQ_ACCT_SONGS, /* arg = 歌单 ID（账号歌单的歌曲） */
    OREQ_DAILY,      /* 每日推荐 / 猜你喜欢 */
    OREQ_COLLECT,    /* 收藏到歌单：arg = dirid，arg2 = 歌曲 id，arg3 = 歌单 dissid */
    OREQ_UNCOLLECT,  /* 从当前歌单删除：arg = 当前歌单 dirid，arg2 = 歌曲 id */
} oreq_kind_t;

typedef struct {
    int  kind;
    /*
     * arg 给到 192：QQ 的二维码 identifier 实测 128 字符（网易云的 unikey 只有
     * 三四十），用 64 会把它截断，轮询就对不上了。
     */
    char arg[192];
    char arg2[32];   /* OREQ_COLLECT/UNCOLLECT：歌曲 id */
    char arg3[24];   /* OREQ_COLLECT：目标歌单的 dissid（判重用；可不填） */
    int  off;        /* OREQ_ACCT_SONGS：从歌单第几首开始取（懒加载分页用） */
    int  cnt;        /* OREQ_ACCT_SONGS：这次要取多少首 */
} oreq_t;

/* 每个 .cc 自己的日志 TAG：**统一是 app_online**（拆分前就是这个，别改，
 * 真机日志与 AGENTS 里记的排查命令都按它过滤）。 */
#define AO_TAG  "app_online"

/*
 * 在线链路自检：置 1 编译后，开机会自动跑一遍
 *   抓歌单 → 二进制落盘 → 读回 → 下载第一首音频 → 抓封面
 * 跑完把歌单从 NVS/SD 清掉（音频缓存留着），日志里打 SELFTEST。
 * 用来在没有 UI 交互的情况下验证 HTTP/重定向/磁盘/解码这条链，
 * 平时保持 0。整段自检在 ao_selftest.cc，**不要**混进业务文件。
 */
#define ONLINE_SELFTEST   0
