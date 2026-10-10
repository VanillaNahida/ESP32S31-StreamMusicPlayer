/*
 * bts_name_cache.h —— bt_service 内部：设备名缓存（MAC → 名字，存 NVS）
 *
 * 名字**只能**从 ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED 里拿到，所以扫描到的名字
 * 按 MAC 存进 NVS，连接/重启后再按 MAC 反查（否则"开机自动连"那台设备只能显示 MAC）。
 * 细节见 bts_name_cache.cc 顶部注释。
 *
 * key 编码与真名判定是纯逻辑，可以 host 单测：
 *   - bts_name_key()      6 字节 → 12 位大写十六进制（NVS 键长上限 15，带冒号会 KEY_TOO_LONG）
 *   - bts_name_is_real()  名字 == 格式化地址 → 假名
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAC → NVS 键（12 位大写十六进制，无冒号；out 至少 13 字节） */
void bts_name_key(const uint8_t bda[6], char *out);

/* 这个"名字"是不是真名字（空/NULL/等于地址串都算假名） */
bool bts_name_is_real(const uint8_t bda[6], const char *name);

/* 记住一台设备的名字（不是真名就什么都不做） */
void bts_name_save(const uint8_t bda[6], const char *name);

/* 反查：MAC → 名字。命中返回 true，否则 out 为空串 */
bool bts_name_load(const uint8_t bda[6], char *out, size_t cap);

/* 三级回退取名（事件真名 → NVS 缓存 → 地址），填 bts_peer_addr / bts_peer_name */
void bts_make_peer_name(const uint8_t *bda, const char *name);

#ifdef __cplusplus
}
#endif
