/*
 * os_cache.h —— 通用 LRU 字节缓存容器（纯逻辑，可 host 单测）
 *
 * 原 online_service.cc 的 PSRAM 封面/歌词缓存容器（143–230）：
 *   mem_cache_t / mem_cache_free_slot / mem_cache_find / mem_cache_slot / mem_cache_put
 * 这里是**泛型化**版本：表、条数、字节计数都由调用方传进来，容器自己不持有全局状态，
 * 于是同一份实现能同时服务封面（4 MB / 8 条）与歌词（256 KB / 8 条），也能单测。
 *
 * ⚠⚠ **R1：mem_cache_put 的淘汰循环是有名的"陷阱"，本文件逐字照搬，不要"顺手修"**。
 *   现版本能工作，原因与死循环条件都记在 os_cache.cc 的 os_cache_put() 上面。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    uint8_t *data;
    size_t   len;
    uint32_t last_ms;      /* 最近使用（esp_timer 墙钟，淘汰用） */
} os_cache_t;

/* 释放一个缓存槽的 data 并清零元数据 */
void os_cache_free_slot(os_cache_t *e);

/* 按 id 查表并**更新 last_ms（LRU 触碰）**；命中返回条目指针，否则 NULL */
os_cache_t *os_cache_find(os_cache_t *tab, size_t n, uint32_t id);

/* 取空槽；没有空槽则淘汰 last_ms 最小者并递减 *bytes */
os_cache_t *os_cache_slot(os_cache_t *tab, size_t n, size_t *bytes);

/* 插入条目（接管 data 所有权），再循环淘汰直到总量 ≤ cap_bytes */
void os_cache_put(os_cache_t *tab, size_t n, size_t *bytes, size_t cap_bytes,
                  uint32_t id, uint8_t *data, size_t len);

/* 整表释放 + 计数归零（清封面/歌词缓存用） */
void os_cache_reset(os_cache_t *tab, size_t n, size_t *bytes);

#ifdef __cplusplus
}
#endif
