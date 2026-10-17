/*
 * bt_service.h —— 蓝牙音频（ESP-GMF esp_bt_audio：经典蓝牙 A2DP Source）
 *
 * 本模块是**唯一**碰蓝牙栈的地方：
 *   - 控制器由本模块初始化（esp_bt_controller_init/enable(CLASSIC_BT)）
 *   - Bluedroid + A2DP/AVRCP 由 esp_bt_audio 初始化（host_config 传进去）
 *   - 搜索/连接/断开走 esp_bt_audio 的经典蓝牙 API
 *   - 对端连上后，用 esp_bt_audio_stream_acquire_write/release_write 把 PCM
 *     推给 A2DP 源（内部用 esp_audio_codec 的 SBC 编码器打包发送）
 *
 * 数据通路：
 *   audio_simple_player out 回调 → bt_service_write_pcm()（非阻塞，满了丢帧）
 *   → 环形缓冲 → 本模块的喂数据任务 → acquire_write/release_write → A2DP
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_DEV_MAX   24
#define BT_NAME_MAX  32

typedef struct {
    uint8_t bda[6];
    char    name[BT_NAME_MAX];
    int8_t  rssi;
    bool    is_audio;    /* Class of Device 主类 = 音频/视频 */
    bool    bonded;      /* 已配对 */
} bt_dev_t;

typedef enum {
    BT_ST_UNINIT = 0,
    BT_ST_READY,          /* 已使能，空闲 */
    BT_ST_DISCOVERING,    /* 搜索中 */
    BT_ST_CONNECTING,     /* ★ 正在连 A2DP（含"准备/停查询/等重试"阶段，最长 ~15 s） */
    BT_ST_CONNECTED,      /* A2DP 已连接 */
    BT_ST_STREAMING,      /* A2DP 源流已启动（正在推 PCM） */
    BT_ST_CONNECT_FAILED, /* ★ 最近一次连接失败（目标与原因还在，UI 显示"重试"） */
    BT_ST_FAILED,         /* 蓝牙不可用（控制器/协议栈初始化失败） */
} bt_state_t;

/* 远端媒体按键（AVRCP Target 收上来的 play/pause/next/prev）。
 * cmd 取值见 esp_bt_audio_media_ctrl_cmd_t（1=PLAY 2=PAUSE 3=STOP 4=NEXT 5=PREV） */
typedef void (*bt_media_ctrl_cb_t)(int cmd, void *user);

/* 幂等；阻塞（控制器 + Bluedroid + A2DP 使能），放后台任务调。
 * 开关关着时直接返回 ESP_ERR_INVALID_STATE 且不初始化。
 * 初始化成功后若有"上次连过的设备"，会延时数秒自动连它（记忆 + 开机自动连）。 */
esp_err_t bt_service_init(void);
bool      bt_service_is_ready(void);
bt_state_t bt_service_state(void);
const char *bt_service_state_str(void);

/* ------------------------------------------------------------------ 总开关
 *
 * 需求："给 wifi 和蓝牙添加开关，记忆开关状态，只有开启的时候才使用，
 *       不开启的话就释放资源"。
 *   - 状态存 NVS（bt_cfg/on），开机默认开；
 *   - set_enabled(false) 会拆掉 PCM 通路、esp_bt_audio_deinit()（Bluedroid 一起
 *     释放）、控制器 disable+deinit；set_enabled(true) 重新初始化。
 *     （**不**调 esp_bt_mem_release：那会让它再也起不来）
 *   - 阻塞操作，放后台任务里调。
 */
bool      bt_service_is_enabled(void);
esp_err_t bt_service_set_enabled(bool on);
esp_err_t bt_service_deinit(void);

/* ---- 搜索 ---- */
esp_err_t bt_service_start_discovery(void);
esp_err_t bt_service_stop_discovery(void);
bool      bt_service_is_discovering(void);
size_t          bt_service_dev_count(void);
const bt_dev_t *bt_service_dev(size_t idx);

/* ---- 连接（A2DP Source；首次连接会自动走配对） ----
 *
 * 2026-10 起这是一次**有生命周期**的操作（实现：bts_conn.cc）：
 *   - 发起前会自动**停掉正在进行的搜索**（inquiry 与 page/AVDTP 抢射频，实测会
 *     直接失败并报 BTA_AV_FAIL_STREAM）；
 *   - 已连着别的设备时会**先断开旧对端**再连新的；
 *   - 失败/超时（15 s）自动重试 2 次（间隔 1.5 s），仍失败才进 BT_ST_CONNECT_FAILED；
 *   - 正在连同一台时返回 ESP_ERR_INVALID_STATE（UI 提示"正在连接，请稍后再试"）。
 * 全程用 bt_service_state() / bt_service_is_connecting() / bt_service_connect_target()
 * 轮询即可，UI 不需要自己算超时。 */
esp_err_t bt_service_connect(const uint8_t bda[6]);
esp_err_t bt_service_disconnect(const uint8_t bda[6]);
bool        bt_service_is_connected(void);
/* 是否在"连接中"（含准备/等待重试阶段） */
bool        bt_service_is_connecting(void);
/* 正在连（或刚连失败）的目标 BDA；没有则 NULL。UI 用它给那一行画转圈/重试 */
const uint8_t *bt_service_connect_target(void);
/* 上次连接失败的原因短句（"连接超时"/"连接失败"），非失败态是空串 */
const char *bt_service_connect_error(void);
const char *bt_service_peer_name(void);
const char *bt_service_peer_addr(void);
/* 已连接对端的 6 字节地址（断开时用）；未连接时返回全 0 */
const uint8_t *bt_service_peer_bda(void);

/* ---- 已配对（bond 表，查询式；配对结果由 A2DP 连接过程产生） ---- */
esp_err_t       bt_service_refresh_bonded(void);
size_t          bt_service_bonded_count(void);
const bt_dev_t *bt_service_bonded(size_t idx);
esp_err_t       bt_service_remove_bond(const uint8_t bda[6]);

/* ---- 音频通路 ---- */
esp_err_t bt_service_media_start(void);      /* 启动 A2DP 源流（连接成功后才有意义） */
esp_err_t bt_service_media_stop(void);
bool      bt_service_is_streaming(void);
int       bt_service_stream_rate(void);      /* 协商到的采样率，0 = 未知 */
int       bt_service_stream_channels(void);  /* 协商到的声道数 */
uint32_t  bt_service_dropped_bytes(void);    /* 缓冲满丢掉的字节数（排查用） */

/* 推 PCM（A2DP 源要求：16bit 交错，采样率 = bt_service_stream_rate()）。
 * **非阻塞**：缓冲满就丢这一帧并返回 ESP_ERR_NO_MEM，绝不卡住解码线程。 */
esp_err_t bt_service_write_pcm(const uint8_t *data, size_t len);

/* ---- 音量：AVRCP 绝对音量（esp_bt_audio_vol.h） ----
 *
 * 蓝牙路的音量**不再靠软件缩放 PCM**（那是旧做法，音质与 CPU 都吃亏）：
 * 0~100 直接以 AVRCP SetAbsoluteVolume 发给对端，由耳机/音箱自己衰减，
 * 对端的音量条也会跟着动 —— 手机当 A2DP 源时就是这么干的。
 *
 * ⚠ 只有 AVRCP 不可用（AVRCP Controller 还没连上、或对端不支持绝对音量）时,
 *   audio_output 才退回逐样本软件缩放。两者**绝不会同时生效**（否则音量为平方）。
 */
esp_err_t bt_service_set_volume(int percent);
int       bt_service_get_volume(void);
/* 上一次 AVRCP 绝对音量下发是否成功；false ⇒ 输出级会退回软件缩放 */
bool      bt_service_volume_is_avrcp(void);
/* 对端（耳机按键/手机）改了音量时的回调（**蓝牙栈任务上下文**，别做重活/别碰 LVGL） */
typedef void (*bt_volume_cb_t)(int percent, void *user);
void bt_service_set_volume_cb(bt_volume_cb_t cb, void *user);

/* 远端媒体按键回调（在蓝牙栈的任务上下文调用，**别做重活、别碰 LVGL**） */
void bt_service_set_media_ctrl_cb(bt_media_ctrl_cb_t cb, void *user);

void bt_service_addr_str(const uint8_t bda[6], char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
