/*
 * os_json_mem.h —— 把 cJSON 的节点内存改到 PSRAM
 *
 * 只在目标上编译（要 heap_caps）；纯逻辑的 os_json.h 不动，host 单测照旧。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 装上"PSRAM 优先"的 cJSON_Hooks（失败退回内部 RAM malloc）。
 *
 * ⚠ 必须在**第一次调用 cJSON 之前**装：cJSON 的分配/释放函数是全局的，
 *   装晚了就会出现"前面用 malloc 分配、后面用 heap_caps_free 释放"的混用。
 *   在线服务里就是 online_service_init() 的最开头。重复调用无害。
 */
void os_json_use_psram(void);

#ifdef __cplusplus
}
#endif
