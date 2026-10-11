/*
 * console_cmds.h —— 串口诊断命令（实现见 console_cmds.cc）
 *
 * 命令：songinfo <路径> / songscan [目录] / playstat
 * 用途：把"文件本身长什么样"（采样率/位深/容器/标签/音频数据偏移）与"当前播放状态"
 *       直接打到串口，方便 AI 或人做分析与性能优化。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册命令并启动 REPL（与日志共用 console UART）。在 app_main 末尾调一次即可 */
esp_err_t console_cmds_init(void);

#ifdef __cplusplus
}
#endif
