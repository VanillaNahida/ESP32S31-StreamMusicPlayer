/*
 * wifi_service.h —— Wi-Fi STA（扫描 / 连接 / 保存 / 忘记）
 *
 * 分层（AGENTS.md 第 6 节）：UI 只认这个头文件，不直接碰 esp_wifi。
 * 所有状态都在这里维护，UI 用 lv_timer 轮询（事件回调在 sys_evt 任务里，
 * 绝不能在里面碰 LVGL）。
 *
 * 保存的网络存自己的 NVS 命名空间（wifi_cfg），最多 5 组，支持忘记。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX   33
#define WIFI_PASS_MAX   65
#define WIFI_AP_MAX     24
#define WIFI_SAVED_MAX  5

typedef enum {
    WIFI_ST_UNINIT = 0,   /* 还没初始化（服务在后台起） */
    WIFI_ST_IDLE,         /* 就绪，未连接 */
    WIFI_ST_SCANNING,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,       /* 密码错 / 找不到 AP */
} wifi_state_t;

typedef struct {
    char    ssid[WIFI_SSID_MAX];
    int8_t  rssi;
    uint8_t authmode;     /* wifi_auth_mode_t：0 = 开放 */
    bool    saved;        /* 我们的 NVS 里存过密码 */
} wifi_ap_t;

/* 幂等；内部会 netif/event/wifi init 并（如果存过）自动连接上次的 AP。
 * 阻塞几百毫秒，**不要**在 LVGL 任务里直接调（main 里放后台任务）。
 * 开关关着时直接返回 ESP_ERR_INVALID_STATE 且不初始化（见 set_enabled）。 */
esp_err_t wifi_service_init(void);
bool      wifi_service_is_ready(void);

/* ------------------------------------------------------------------ 总开关
 *
 * 需求："给 wifi 和蓝牙添加开关，记忆开关状态，只有开启的时候才使用，
 *       不开启的话就释放资源"。
 *   - 状态存 NVS（wifi_cfg/on），开机默认开；
 *   - set_enabled(false) 会 esp_wifi_stop+deinit、销毁 netif、停 SNTP，
 *     把驱动内存与射频都还回去；set_enabled(true) 重新走一遍 init
 *     （含自动连接上次的 AP）。两者都是阻塞操作，放后台任务里调。
 */
bool      wifi_service_is_enabled(void);
esp_err_t wifi_service_set_enabled(bool on);
esp_err_t wifi_service_deinit(void);          /* = set_enabled(false) 的实现 */

/* 是否已拿到 IP（在线功能的前置条件） */
bool      wifi_service_has_ip(void);
/* SNTP 是否已同步（https 证书校验依赖系统时间） */
bool      wifi_service_time_ready(void);
/* NVS 里保存过这个 SSID 的密码？（"密码留空确认 → 用已保存的密码连"用它判断） */
bool      wifi_service_is_saved(const char *ssid);

/* 扫描（异步）：scanning 变 false 后 ap_count/ap() 就是结果 */
esp_err_t wifi_service_scan_start(void);
bool      wifi_service_is_scanning(void);
size_t    wifi_service_ap_count(void);
const wifi_ap_t *wifi_service_ap(size_t idx);

/* 连接：save=true 时把 (ssid,pass) 记进 NVS，下次开机自动连 */
esp_err_t wifi_service_connect(const char *ssid, const char *pass, bool save);
/* 用 NVS 里存的密码连（"已保存"的网络走这条；找不到返回 ESP_ERR_NOT_FOUND） */
esp_err_t wifi_service_connect_saved(const char *ssid);
esp_err_t wifi_service_disconnect(void);
/* 忘记：从 NVS 删掉；若正连着它，顺手断开 */
esp_err_t wifi_service_forget(const char *ssid);

wifi_state_t wifi_service_state(void);
const char  *wifi_service_conn_ssid(void);   /* 已连接/正在连的 SSID，空串表示无 */
const char  *wifi_service_ip(void);          /* 已连接时的点分 IP；否则空串 */
int          wifi_service_rssi(void);        /* 已连接时的信号强度，0 表示未知 */

size_t       wifi_service_saved_count(void);
const char  *wifi_service_saved_ssid(size_t idx);

#ifdef __cplusplus
}
#endif
