/*
 * os_json_mem.cc —— 把 cJSON 的节点内存改到 PSRAM
 *
 * 为什么必须改（2026-11 真机，用户报"重点优化内存占用，不要让 PSRAM 吃灰"）：
 *
 *   cJSON 是"小对象海量"型分配 —— 一个 id / 一个字符串就是一个 cJSON 节点，
 *   sizeof(cJSON) ≈ 64 B。响应文本本身早就走 PSRAM（os_http_client.cc 的 sink），
 *   但节点全走 plain malloc，而 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL(16 KB) 意味着
 *   小于 16 KB 的 malloc 一律落**内部 RAM**：
 *
 *     GET /likelist?uid=...  3 万首喜欢 → 3 万个节点 ≈ 2 MB 内部 RAM 瞬时占用
 *     GET /playlist/detail   500 首歌单 → 同样几十万个小节点
 *
 *   一次解析就能把内部 RAM 抽干，随后（哪怕已经 free）碎片也让这些必失败的分配
 *   雪崩：
 *     E esp-aes: Failed to allocate memory            <- AES DMA 对齐缓冲
 *     E esp-tls-mbedtls: write error :-0x0084
 *     E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D   <- -141 内存不足
 *     W wifi:m f null
 *
 *   ⇒ 节点显式走 PSRAM（16 MB，正好用不完），内部 RAM 只做兜底。
 *
 * 线程安全：cJSON_InitHooks() 只写两个全局函数指针，必须在**单线程阶段**调用
 *   （online_service_init 在 app_main 里、任何网络任务之前跑完）。
 *   之后所有 cJSON 分配都走同一套 hooks，不存在跨分配器的混用。
 */
#include "internal/os_json_mem.h"

#include <stdlib.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "online";

/*
 * 注意 free 用 heap_caps_free()：它按地址查所属堆区域，既能释放 heap_caps_malloc
 * 的指针，也能释放上面兜底路径的 plain malloc 指针（IDF 里 free() 本身就是它）。
 */
static void *os_json_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
    return malloc(sz);
}

static void os_json_free(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

void os_json_use_psram(void)
{
    cJSON_Hooks hooks = {};
    hooks.malloc_fn = os_json_malloc;
    hooks.free_fn = os_json_free;
    cJSON_InitHooks(&hooks);

    ESP_LOGI(TAG, "cJSON 节点内存: PSRAM 优先（internal free=%u psram free=%u）",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
