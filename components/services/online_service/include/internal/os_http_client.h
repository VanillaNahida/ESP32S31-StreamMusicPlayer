/*
 * os_http_client.h —— 自管 302 的同步 HTTP（文本/二进制 GET + 下载到文件）
 *
 * 从原 online_service.cc 的 "HTTP" 那一节（291–728）整体搬出来，**逐字未改**：
 *   http_sink_t / sink_reserve / http_evt   293–379
 *   net_auth_hdr / url_for_log              406–440
 *   http_get_text                           442–573
 *   http_download                           579–728
 * 只加了 os_ 前缀，并按 §F 的清单把"相对 Location 拼回主机"抽成一个函数
 * （原来在 517–534 与 640–656 各写了一遍，逻辑完全相同）。
 *
 * ★ 为什么自己处理 302（原注释照搬）：
 *   这个实例把 http://meting.xcnahida.cn/api?... 302 到 https 同一路径，
 *   而 IDF 的自动跳转在实测里会把查询串弄丢（服务器直接回 400，
 *   日志就是 "GET status 400: http://meting.xcnahida.cn/api...playlist..."）。
 *   我们改成 disable_auto_redirect + 自己读 Location 再发一次，
 *   顺便把每一跳打进日志，方便定位。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 常量 */
#define HTTP_TIMEOUT_MS    15000
#define HTTP_BUF           2048
/* 单个 JSON 响应上限（歌单 1000 首 ≈ 300 KB），别让一个坏响应吃光内存 */
#define JSON_MAX_LEN       (1024u * 1024u)
/* 音频上限（防止下到无穷无尽的流）：250 MB */
#define AUDIO_MAX_BYTES    (250u * 1024u * 1024u)
/* 下载缓冲的起步容量：LRC 只有几十字节，不该为了它占几 MB */
#define HTTP_SINK_MIN      (16 * 1024)
/* 拼 base + path 发 GET 用的 URL 缓冲上限（见 net_api_get_ex 的注释） */
#define NET_URL_MAX        4096

/* ---------------------------------------------------------------- 响应接收 */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;              /* 当前**已分配**的容量（按需增长，不是上限） */
    size_t limit;            /* 单次响应的硬上限（超过就让上层报错） */
    bool   overflow;         /* 因为超限/没内存中止过：给上层一句能看懂的话 */
    char   location[512];    /* 301/302 的 Location：响应头只能从事件里拿
                              * （esp_http_client_get_header() 取的是**请求**头，
                              *   实测在 perform() 之后取 Location 一直是空的） */
} http_sink_t;

/*
 * 把接收缓冲扩到至少 need 字节（按需增长）。返回 false = 超上限或没内存。
 *
 * ★ 两条纪律（2026-10 修"封面 ESP_ERR_NO_MEM"）：
 *   1) **显式从 PSRAM 分配**（heap_caps_malloc + MALLOC_CAP_SPIRAM）：
 *      封面/歌单这类缓冲动辄几百 KB 到几 MB，内部 RAM 根本放不下；
 *   2) **不要一上来就 malloc(上限)**。原来的写法是 `malloc(max_len)`，
 *      封面那条路 max_len = 4 MB —— 实测直接 ESP_ERR_NO_MEM（日志里
 *      "cover ... download failed (ESP_ERR_NO_MEM, 0 bytes)"），因为 PSRAM 里
 *      已经躺着 4 MB 流式环形缓冲 + framebuffer + 封面缓存，再要一块 **4 MB 连续**
 *      空间太难，而且 200 KB 的封面白白占 4 MB。
 *      现在从 16 KB 起步、翻倍增长：300y300 的封面（~200 KB）只占 256 KB。
 */
bool sink_reserve(http_sink_t *s, size_t need);

/* esp_http_client 事件回调（抓 Location / 追加 body） */
esp_err_t http_evt(esp_http_client_event_t *e);

/* ---------------------------------------------------------------- 请求头 */

/* 拼 "Bearer xxx"（没有 token 时返回空串）。只对我们自己的 API 地址加这个头 ——
 * 绝不能把 token 泄露给 meting 那种第三方代理。 */
const char *net_auth_hdr(void);

/* 打日志用的 URL：**砍掉查询串**（账号接口把 cookie 放在 ?cookie=… 里）。
 * 返回静态缓冲，只保证"取完立刻用"（HTTP 都在同一个工作线程里跑）。 */
const char *url_for_log(const char *url);

/* ---------------------------------------------------------------- 发送原语 */

/*
 * GET 一段数据（JSON / LRC / 封面字节都走这里）。成功时 *out 为 malloc 的缓冲
 * （对文本是 NUL 结尾的字符串；对二进制则按 out_len 给长度，不保证 NUL 语义）。
 * max_len 是这一跳的单次响应上限（防止坏响应吃光内存）。
 */
esp_err_t http_get_text(const char *url, char **out, size_t *out_len, size_t max_len);

/*
 * 下载二进制到文件。自动跟随 302；成功条件是 HTTP 200 且落盘字节数 >= min_size。
 * 返回时把真实内容的前 16 字节放到 sniff（用于判断容器格式）。
 */
esp_err_t http_download(const char *url, const char *dest, size_t min_size,
                        uint8_t sniff[16]);

/* ---------------------------------------------------------------- 重定向工具 */

/*
 * 把 Location 解析成"下一跳的完整 URL"：
 *   - 绝对 URL（不是 '/' 开头）→ 原样；
 *   - 相对路径（'/' 开头）→ 用 cur 的 scheme://host 前缀拼回去。
 * 结果写进 out（cap 字节，NUL 结尾）。四条 302 路径共用这一份实现
 * （原 517–534 / 640–656 各一份，逐字等价）。
 */
void os_http_join_location(const char *cur, const char *loc, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
