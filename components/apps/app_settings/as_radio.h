/*
 * as_radio.h —— 无线总开关（Wi-Fi / 蓝牙）
 *
 * 需求："给 wifi 和蓝牙添加开关，记忆开关状态，只有开启的时候才使用，
 * 不开启的话就释放资源"。
 *
 *   - 动作在**后台任务**里做：init/deinit 都是阻塞的（几百毫秒~1 秒），
 *     不能在 LVGL 任务里跑；任务必须**钉在 BTC 核**上（原因见 as_radio.cc 里的长注释）。
 *   - 开关位置由各页的 500 ms tick 按服务真实状态刷新（切换过程中先不刷，免得打架）。
 *
 * 这是本组件**唯一的跨任务点**：`as_radio_busy()` 的读写跨 LVGL 任务与 radio_sw 任务。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "as_page_shell.h"      /* subpage_desc_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 总开关的两个身份；值同时被当 user_data 传给开关回调（0 = 无效） */
enum { AS_RADIO_WIFI = 1, AS_RADIO_BT = 2 };

/*
 * 开关行（需求：开关与"进入子页"合并成一行，别再像以前那样
 * "上面一行开关 + 下面一行同名条目"）：
 *
 *     [图标] [名称 / 状态]  [开关] [›]
 *      └──────── 点这些进子页 ─────┘ └只切换┘
 *
 * @param sub        非 NULL = 点这一行进该子页；NULL = 只做开关（子页顶部用）
 * @param status_out 状态文字 label（可为 NULL）；主页两行由主页 tick 填摘要
 */
lv_obj_t *as_radio_switch_row(lv_obj_t *parent, const char *sym, uint32_t accent,
                              const char *name, int which,
                              const subpage_desc_t *sub, lv_obj_t **status_out);

/*
 * 把所有开关实例（主页一行 + 当前子页顶部一行）拉回"服务真实状态"。
 * 各页自己的 500 ms tick 都要调它 —— 这就是"主页与子页双向同步"。
 * ⚠ 正在切换（busy）时**故意不刷**：等服务给出结果再对齐，避免跟手指打架。
 */
void as_radio_sw_sync(void);

/* 1 = 正在切 Wi-Fi，2 = 正在切蓝牙，0 = 空闲 */
int as_radio_busy(void);

#ifdef __cplusplus
}
#endif
