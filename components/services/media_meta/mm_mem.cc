/*
 * mm_mem.cc —— 见 mm_mem.h 的长注释（为什么大块缓冲必须先要 PSRAM）。
 */
#include "internal/mm_mem.h"

#include <stdlib.h>

#include "esp_heap_caps.h"

void *mm_alloc_big(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    /* 先 PSRAM：封面/歌词/探测缓冲都不参与 DMA，也不需要低延迟 */
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
    /* PSRAM 拿不到（或该芯片没有 PSRAM）再退回内部 RAM —— 与改造前行为一致 */
    return malloc(size);
}
