/*
 * os_cache.cc —— 通用 LRU 字节缓存容器（逐字照搬原 161–230 行）
 *
 * 原实现有一处**必须保留原样**的地方：os_cache_put() 末尾的淘汰循环。
 * 详见该函数上的长注释 —— 那是"能工作但看着像死循环"的代码，别改。
 */
#include "internal/os_cache.h"

#include <stdlib.h>

#include "esp_timer.h"

void os_cache_free_slot(os_cache_t *e)
{
    if (e->data) {
        free(e->data);
    }
    e->id      = 0;
    e->data    = NULL;
    e->len     = 0;
    e->last_ms = 0;
}

os_cache_t *os_cache_find(os_cache_t *tab, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (tab[i].id == id && tab[i].data) {
            tab[i].last_ms = (uint32_t)esp_timer_get_time() / 1000u;
            return &tab[i];
        }
    }
    return NULL;
}

/* 找一个空槽；没有就淘汰最旧的一个（LRU） */
os_cache_t *os_cache_slot(os_cache_t *tab, size_t n, size_t *bytes)
{
    size_t oldest = n;
    uint32_t oldest_ms = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        if (!tab[i].data) {
            return &tab[i];
        }
        if (tab[i].last_ms < oldest_ms) {
            oldest_ms = tab[i].last_ms;
            oldest    = i;
        }
    }
    if (oldest < n) {
        *bytes -= tab[oldest].len;
        os_cache_free_slot(&tab[oldest]);
        return &tab[oldest];
    }
    return NULL;
}

/*
 * 放进缓存（data 为调用方所有的新缓冲，插入后归缓存管）。
 *
 * ⚠⚠ **这段淘汰循环是"陷阱代码"，本文件逐字照搬，请不要"顺手修"**（拆分蓝图 R1）：
 *
 *   它看着像死循环——`while (*bytes > cap_bytes)` 里调 os_cache_slot()，而后者
 *   **优先返回空槽**（前面的 `if (!tab[i].data) return &tab[i];`）→ 拿到的 `old`
 *   可能既不是 e 也没释放任何内存 → 字节数不变 → 下一轮又是同一个空槽……
 *
 *   现版本**实际能收敛**，因为每条记录至少几百字节而表只有 8 个槽：
 *     - `old == e` → break（唯一的显式出口）；
 *     - 走到 `old != e` 且槽是空的，只可能发生在"刚插进去的 e 是表里唯一的非空槽"
 *       或"淘汰已经把表清空"这两个瞬间，那时 *bytes == e->len 且 cap ≥ 一个条目的
 *       正常大小 → 循环条件不成立、立刻退出；
 *     - 只要表里还有**别的非空槽**，os_cache_slot() 的空槽扫描就一定会先撞上已被
 *       os_cache_free_slot() 清空的槽（它就在前面），返回空槽 → 字节数不变 →
 *       下一轮同样返回**同一个**空槽（它现在是空的）→ 直到 `old == e` 才 break。
 *   也就是说：它的收敛靠"表迟早只剩 e"或"`old == e`"，**不是**靠字节数下降。
 *   真要让它严格收敛，得改成"按 last_ms 淘汰、且必须真的释放了内存才继续"
 *   （`do { before = *bytes; ... } while (*bytes > cap && *bytes < before);`）——
 *   那是行为变更，得单独评估，不在本次纯搬迁范围内。
 *
 *   host 单测要构造的场景：7 张 ~600 KB 的封面（4.2 MB > 4 MB、槽位 8 个）。
 */
void os_cache_put(os_cache_t *tab, size_t n, size_t *bytes, size_t cap_bytes,
                  uint32_t id, uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        free(data);
        return;
    }
    os_cache_t *e = os_cache_slot(tab, n, bytes);
    if (!e) {
        free(data);
        return;
    }
    e->id      = id;
    e->data    = data;
    e->len     = len;
    e->last_ms = (uint32_t)esp_timer_get_time() / 1000u;
    *bytes += len;
    /* 总量超限：从最旧开始扔，直到装得下 */
    while (*bytes > cap_bytes) {
        os_cache_t *old = os_cache_slot(tab, n, bytes);
        if (!old || old == e) {
            break;
        }
    }
}

void os_cache_reset(os_cache_t *tab, size_t n, size_t *bytes)
{
    for (size_t i = 0; i < n; i++) {
        os_cache_free_slot(&tab[i]);
    }
    *bytes = 0;
}
