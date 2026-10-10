/*
 * bts_state.cc —— bt_service 内部：共享状态 + 查询访问器 + 地址工具
 *
 * 从 bt_service.cc 132–158（状态定义）与 186–216 / 1210–1235 / 1264–1269 /
 * 1370–1375（工具与访问器）原样搬来：**只改名（s_xxx → bts_xxx）**，语句没动。
 *
 * ⚠ 绝不要在这里（或本组件任何地方）调 esp_bt_mem_release()：那会把控制器的内存
 *   永久归还，之后再也 init 不回来，而需求是"开关能来回切"
 *   （见 bt_service.cc 的 bt_service_deinit 注释）。
 */
#include "bts_state.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ 共享状态定义 */

volatile bt_state_t bts_state = BT_ST_UNINIT;
volatile bool       bts_inited;
volatile bool       bts_discovering;
volatile bool       bts_connected;
volatile bool       bts_streaming;

bt_dev_t        bts_dev[BT_DEV_MAX];
volatile size_t bts_dev_count;
bt_dev_t        bts_bonded[BT_DEV_MAX];
volatile size_t bts_bonded_count;

char    bts_peer_name[BT_NAME_MAX];
char    bts_peer_addr[18];
uint8_t bts_peer_bda[6];

volatile int      bts_rate;
volatile int      bts_channels;
volatile uint32_t bts_dropped;

/* ------------------------------------------------------------ 地址工具 */

void bt_service_addr_str(const uint8_t bda[6], char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return;
    }
    snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

bool bts_same_bda(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

bt_dev_t *bts_dev_find(const uint8_t *bda, bool create)
{
    for (size_t i = 0; i < bts_dev_count; i++) {
        if (bts_same_bda(bts_dev[i].bda, bda)) {
            return &bts_dev[i];
        }
    }
    if (!create || bts_dev_count >= BT_DEV_MAX) {
        return NULL;
    }
    bt_dev_t *d = &bts_dev[bts_dev_count];
    memset(d, 0, sizeof(*d));
    memcpy(d->bda, bda, 6);
    /* volatile 上不能用 ++（C++20 deprecated，IDF 又是 -Werror） */
    bts_dev_count = bts_dev_count + 1;
    return d;
}

/* ------------------------------------------------------------ 查询访问器 */

bool bt_service_is_ready(void)
{
    return bts_state != BT_ST_UNINIT && bts_state != BT_ST_FAILED;
}

bt_state_t bt_service_state(void) { return bts_state; }
bool bt_service_is_discovering(void) { return bts_discovering; }
bool bt_service_is_connected(void) { return bts_connected; }
bool bt_service_is_streaming(void) { return bts_streaming; }
int  bt_service_stream_rate(void) { return bts_rate; }
int  bt_service_stream_channels(void) { return bts_channels; }
const char *bt_service_peer_name(void) { return bts_peer_name; }
const char *bt_service_peer_addr(void) { return bts_peer_addr; }
const uint8_t *bt_service_peer_bda(void) { return bts_peer_bda; }

const char *bt_service_state_str(void)
{
    switch (bts_state) {
    case BT_ST_UNINIT:        return "初始化中…";
    case BT_ST_DISCOVERING:   return "搜索中…";
    case BT_ST_CONNECTED:     return "已连接";
    case BT_ST_STREAMING:     return "正在推流";
    case BT_ST_FAILED:        return "蓝牙不可用";
    default:                  return "就绪";
    }
}

size_t bt_service_dev_count(void) { return bts_dev_count; }

const bt_dev_t *bt_service_dev(size_t idx)
{
    return (idx < bts_dev_count) ? &bts_dev[idx] : NULL;
}

size_t bt_service_bonded_count(void) { return bts_bonded_count; }

const bt_dev_t *bt_service_bonded(size_t idx)
{
    return (idx < bts_bonded_count) ? &bts_bonded[idx] : NULL;
}
