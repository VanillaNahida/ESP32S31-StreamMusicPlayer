/*
 * bts_name_cache.cc —— bt_service 内部：设备名缓存（MAC → 名字，存 NVS）
 *
 * 从 bt_service.cc 218–393 原样搬来（含全部注释），只把函数名加上 bts_ 前缀
 * （原来是文件内 static，现在跨文件，加前缀避免和别的组件撞符号）。
 */

#include "bts_name_cache.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "bts_state.h"      /* bt_service_addr_str / bts_peer_addr / bts_peer_name */

static const char *TAG = "bt_svc";   /* 与拆分前一致：日志里仍显示 bt_svc */

/*
 * 设备名缓存（MAC → 名字）。
 *
 * 为什么要持久化：名字**只能**从 ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED 里拿到 ——
 * esp_bt_audio_event_connection_st_t 根本没有名字字段，组件也没给
 * read_remote_name 之类的接口（见 esp_bt_audio_classic.h）。于是
 * "开机自动连上次那台设备"这条路**永远**拿不到名字，bts_peer_name 只能退回 MAC，
 * 用户看到的就是"设备名变成 MAC 地址"。把扫描时拿到的名字按 MAC 存起来，
 * 连接/重启后再按 MAC 反查即可。
 */
#define NVS_NS_NAME      "bt_name"
/*
 * NVS 键名上限 15 个字符（NVS_KEY_NAME_MAX_SIZE = 16，含结尾 0），
 * 而 "AA:BB:CC:DD:EE:FF" 有 17 个字符 —— 拿它当键会直接
 * ESP_ERR_NVS_KEY_TOO_LONG。所以键用去掉冒号的 12 位大写十六进制（如 "A1B2C3D4E5F6"）。
 */
#define BT_NAME_KEY_LEN  13
/* 名字缓存条数上限：满了淘汰最早写进去的那条，别让 NVS 被无限撑大 */
#define BT_NAME_KEEP_MAX 16

/* MAC → NVS 键：去掉冒号的 12 位大写十六进制（原因见 BT_NAME_KEY_LEN 的注释） */
void bts_name_key(const uint8_t bda[6], char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = hex[(bda[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[bda[i] & 0x0F];
    }
    out[12] = '\0';
}

/*
 * 这个"名字"是不是真名字。
 *
 * 扫描表里的 name 拿不到真名时会被填成 MAC 串（DEVICE_DISCOVERED 里的兜底），
 * 那不是名字：既不该存进 NVS（会把真名覆盖掉），也不该当作已解析出的名字。
 */
bool bts_name_is_real(const uint8_t bda[6], const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    char addr[18];
    bt_service_addr_str(bda, addr, sizeof(addr));
    return strcmp(name, addr) != 0;
}

/*
 * 枚举 bt_name 里的字符串键：返回实际拷进 keys 的条数（最多 cap 个），
 * *more 为真表示还有没装下的。
 *
 * ⚠ 迭代器不保证看得到同一次事务里**尚未 commit** 的写入，所以"腾地方"必须在
 * nvs_set_str **之前**做（见 bts_name_save 的顺序）。
 */
static size_t bts_name_enum(nvs_handle_t h, char keys[][BT_NAME_KEY_LEN],
                            size_t cap, bool *more)
{
    if (more) {
        *more = false;
    }
    nvs_iterator_t it = NULL;
    if (nvs_entry_find_in_handle(h, NVS_TYPE_STR, &it) != ESP_OK || !it) {
        nvs_release_iterator(it);          /* 允许传 NULL */
        return 0;
    }
    size_t n = 0;
    nvs_entry_info_t info;
    while (nvs_entry_info(it, &info) == ESP_OK) {
        if (n < cap) {
            /*
             * ⚠ 必须显式给出精度：`info.key` 是 `char[NVS_KEY_NAME_MAX_SIZE]`（16 字节，
             *   NVS 键名上限 15 字符），而 keys[] 的每个元素只有 BT_NAME_KEY_LEN(13)。
             *   直接 "%s" 会被 GCC 证明"最多写 16 字节进 13 字节"，在 -Werror 下直接
             *   编译失败（真机踩过）。本命名空间里的键**只有我们自己写的 12 位十六进制**，
             *   按 12 字节截断既安全又能让编译器证得出来。
             */
            snprintf(keys[n], BT_NAME_KEY_LEN, "%.*s", BT_NAME_KEY_LEN - 1, info.key);
            n++;
        } else if (more) {
            *more = true;
        }
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
    }
    nvs_release_iterator(it);
    return n;
}

/* 为即将写入的 keep_key 腾位置：满了就删最早写进去的那条（FIFO 淘汰） */
static void bts_name_make_room(nvs_handle_t h, const char *keep_key)
{
    for (int round = 0; round < 8; round++) {
        char keys[BT_NAME_KEEP_MAX + 1][BT_NAME_KEY_LEN];
        bool more = false;
        const size_t n = bts_name_enum(h, keys, BT_NAME_KEEP_MAX + 1, &more);

        bool have = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(keys[i], keep_key) == 0) {
                have = true;
                break;
            }
        }
        if (have) {
            return;                        /* 覆盖写：不占新条目，不用腾 */
        }
        if (n < BT_NAME_KEEP_MAX && !more) {
            return;                        /* 还有空位 */
        }
        /*
         * NVS 按条目索引顺序枚举（实测先写的先出来），所以第一个非 keep_key
         * 就是最旧的一条 —— 淘汰它。
         */
        size_t victim = n;                 /* n = 没找到可删的 */
        for (size_t i = 0; i < n; i++) {
            if (strcmp(keys[i], keep_key) != 0) {
                victim = i;
                break;
            }
        }
        if (victim == n || nvs_erase_key(h, keys[victim]) != ESP_OK) {
            return;                        /* 删不动就别在这儿死循环 */
        }
        ESP_LOGI(TAG, "bt name cache full: dropped '%s'", keys[victim]);
    }
}

/* 记住一台设备的名字（传进来的不是真名字时什么都不做） */
void bts_name_save(const uint8_t bda[6], const char *name)
{
    if (!bts_name_is_real(bda, name)) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NAME, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[BT_NAME_KEY_LEN];
    bts_name_key(bda, key);

    char cur[BT_NAME_MAX];
    size_t len = sizeof(cur);
    if (nvs_get_str(h, key, cur, &len) == ESP_OK && strcmp(cur, name) == 0) {
        nvs_close(h);                      /* 名字没变：别反复擦写 flash */
        return;
    }
    bts_name_make_room(h, key);
    if (nvs_set_str(h, key, name) == ESP_OK && nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "bt name cached: %s = %s", key, name);
    }
    nvs_close(h);
}

/* 反查：MAC → 名字。命中返回 true（out 里是名字），否则 out 为空串 */
bool bts_name_load(const uint8_t bda[6], char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NAME, NVS_READONLY, &h) != ESP_OK) {
        return false;                      /* 还没缓存过任何名字 */
    }
    char key[BT_NAME_KEY_LEN];
    bts_name_key(bda, key);
    size_t len = cap;
    const bool ok = (nvs_get_str(h, key, out, &len) == ESP_OK) && out[0];
    nvs_close(h);
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

/*
 * 对端显示名 + 地址。
 *
 * 三级回退：事件里带来的真名 → NVS 里缓存过的名字 → 地址。
 * 中间那一级就是"名称变成 MAC"的修法：连接事件里从来没有名字，只有扫描才有。
 */
void bts_make_peer_name(const uint8_t *bda, const char *name)
{
    bt_service_addr_str(bda, bts_peer_addr, sizeof(bts_peer_addr));
    if (bts_name_is_real(bda, name)) {
        snprintf(bts_peer_name, sizeof(bts_peer_name), "%s", name);
        return;
    }
    if (bts_name_load(bda, bts_peer_name, sizeof(bts_peer_name))) {
        return;
    }
    snprintf(bts_peer_name, sizeof(bts_peer_name), "%s", bts_peer_addr);
}
