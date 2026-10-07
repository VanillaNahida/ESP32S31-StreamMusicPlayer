/*
 * bsp_internal.h —— bsp 组件内部接口（不对外暴露）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按需初始化一个板级设备（esp_board_manager_init_device_by_name 的薄封装） */
esp_err_t bsp_device_ensure(const char *name);

#ifdef __cplusplus
}
#endif