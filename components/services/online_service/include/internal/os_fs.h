/*
 * os_fs.h —— 依赖 ESP-IDF / PSRAM / 文件系统的"小工具"
 *
 * 与 os_util.h 的分界：这边要 heap_caps（PSRAM 优先）、stat/mkdir/opendir/unlink，
 * 所以只在目标上编译；纯逻辑仍然留在 os_util.h（可 host 单测）。
 *
 * 原文件里的名字：psram_alloc / file_ok / mkdir_if_needed / wipe_dir
 * （online_service.cc 的 254–258、273–280、282–289、1806–1823），
 * 外加 online_has_network（1798–1801，转发 wifi_service_has_ip）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 优先 heap_caps_malloc(SPIRAM)，失败退回内部 RAM malloc（PSRAM 没起来时兜底） */
void *os_psram_alloc(size_t n);

/* stat 判"是普通文件且 ≥ min_size" */
bool os_file_ok(const char *path, size_t min_size);

/* 目录不存在才 mkdir(0777)；存在但不是目录算失败 */
esp_err_t os_mkdir_if_needed(const char *path);

/* 删掉一个目录里的全部内容，并尝试 rmdir（清理历史遗留的 cover/、lrc/ 目录） */
void os_wipe_dir(const char *dir);

/* 当前网络是否可用（WiFi 拿到 IP）——在线功能的前置条件 */
bool online_has_network(void);

#ifdef __cplusplus
}
#endif
