/*
 * as_page_main.cc —— 设置一级页（主页）
 *
 * 原 app_settings.cc：
 *   - 一级：设置列表 一段（260–276）
 *   - settings_tick（552–634）
 *   - on_setting_row（636–672）
 *   - page_settings（674–712）→ page_settings（对外名字不变，注册表要用）
 *
 * 主页只拥有自己的 s_sum[] 与 s_sum_timer；无线开关实例归 as_radio.cc。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_app_desc.h"

#include "ui_core.h"
#include "ui_metrics.h"

#include "bsp.h"
#include "audio_service.h"
#include "wifi_service.h"
#include "bt_service.h"

#include "as_fmt.h"
#include "as_page_main.h"
#include "as_page_shell.h"
#include "as_radio.h"
#include "as_widgets.h"

/* ================================================================== 一级：设置列表 */

static const char *TAG = "app_settings";

enum { SET_WIFI = 0, SET_BT, SET_OUT, SET_DISPLAY, SET_STORAGE, SET_ABOUT, SET_N };

static lv_obj_t *s_sum[SET_N];
static lv_timer_t *s_sum_timer;

static void settings_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_sum_timer) {
        lv_timer_delete(s_sum_timer);
        s_sum_timer = NULL;
    }
    /* 开关控件随页面一起销毁：它们的 LV_EVENT_DELETE 会自己从 s_radio_sw[] 注销 */
    memset(s_sum, 0, sizeof(s_sum));
}

static void settings_tick(lv_timer_t *t)
{
    (void)t;
    /* 总开关位置跟服务真实状态对齐（主页与子页的所有实例一起刷） */
    as_radio_sw_sync();
    if (!s_sum[SET_WIFI]) {
        return;
    }
    char buf[72];

    if (!wifi_service_is_enabled()) {
        as_label_set_if_changed(s_sum[SET_WIFI], "已关闭");
    } else switch (wifi_service_state()) {
    case WIFI_ST_CONNECTED:
        snprintf(buf, sizeof(buf), "已连接 %s", wifi_service_conn_ssid());
        break;
    case WIFI_ST_CONNECTING:
        snprintf(buf, sizeof(buf), "连接中 %s", wifi_service_conn_ssid());
        break;
    case WIFI_ST_SCANNING:
        snprintf(buf, sizeof(buf), "扫描中…");
        break;
    case WIFI_ST_FAILED:
        snprintf(buf, sizeof(buf), "连接失败");
        break;
    case WIFI_ST_UNINIT:
        snprintf(buf, sizeof(buf), "初始化中…");
        break;
    default:
        snprintf(buf, sizeof(buf), "未连接");
        break;
    }
    as_label_set_if_changed(s_sum[SET_WIFI], buf);

    if (bt_service_state() == BT_ST_FAILED) {
        snprintf(buf, sizeof(buf), "不可用");
    } else if (bt_service_is_discovering()) {
        snprintf(buf, sizeof(buf), "搜索中…");
    } else if (bt_service_bonded_count() > 0) {
        snprintf(buf, sizeof(buf), "已配对 %u 台", (unsigned)bt_service_bonded_count());
    } else {
        snprintf(buf, sizeof(buf), "未配对");
    }
    as_label_set_if_changed(s_sum[SET_BT], buf);

    snprintf(buf, sizeof(buf), "%s · %d%%",
             audio_service_get_output() == AUDIO_OUT_NONE ? "静音" : "扬声器",
             audio_service_get_volume());
    as_label_set_if_changed(s_sum[SET_OUT], buf);

    /* 显示设置摘要：时钟口径 + 校时状态（原来这行写"无软件背光"，等于没信息） */
    {
        static char disp_txt[32];
        snprintf(disp_txt, sizeof(disp_txt), "%s · %s",
                 wifi_service_clock_show_seconds() ? "时钟显秒" : "时钟不显秒",
                 wifi_service_time_ready() ? "已校时" : "未校时");
        as_label_set_if_changed(s_sum[SET_DISPLAY], disp_txt);
    }
    /*
     * 存储行摘要：把"已用百分比 + 可用容量"直接摆在设置主页上（用户点进去之前就能看到）。
     * ⚠ esp_vfs_fat_info() 每次都走一遍 FATFS（f_getfree），不能每 500 ms 调一次 ——
     *   这里自己节流到 5 s（容量变化本来就慢，看不出来）。
     */
    {
        static uint32_t s_storage_tick;
        /* 64 字节：`%u` GCC 按最多 10 位算、"已用/·/可用" 是中文（3 字节/字），
         * 40 字节时 -Wformat-truncation 会报"可能被截断" —— 别改小 */
        static char     s_storage_txt[64];
        if (lv_tick_elaps(s_storage_tick) >= 5000) {
            s_storage_tick = lv_tick_get();
            if (!bsp_sdcard_is_mounted()) {
                snprintf(s_storage_txt, sizeof(s_storage_txt), "无 SD 卡");
            } else {
                uint32_t total_kb = 0, free_kb = 0;
                if (bsp_sdcard_capacity_kb(&total_kb, &free_kb) == ESP_OK && total_kb > 0) {
                    const uint32_t used_pct =
                        (uint32_t)(((uint64_t)(total_kb - free_kb) * 100u) / total_kb);
                    char freebuf[24];
                    as_fmt_size_kb(free_kb, freebuf, sizeof(freebuf));
                    snprintf(s_storage_txt, sizeof(s_storage_txt), "已用 %u%% · 可用 %.20s",
                             (unsigned)used_pct, freebuf);
                } else {
                    snprintf(s_storage_txt, sizeof(s_storage_txt), "SD 已挂载");
                }
            }
        }
        as_label_set_if_changed(s_sum[SET_STORAGE], s_storage_txt);
    }
    as_label_set_if_changed(s_sum[SET_ABOUT], esp_app_get_description()->version);
}

static void on_setting_row(lv_event_t *e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    page_desc_t p = {};
    /* 二级页外壳 as_page_shell 自带返回钮：no_back 置位，避免全局返回键重复 */
    p.no_back = true;
    switch (id) {
    /*
     * Wi-Fi / 蓝牙不在这里：它们现在走 as_radio_switch_row 自带的行回调
     * （点名称区进子页、点开关只切换），见 as_on_switch_row_click。
     */
    case SET_OUT:
        p.id = "set_out";
        p.create = page_output;
        break;
    case SET_DISPLAY:
        /*
         * 显示设置（2026-11）：以前这行点了只弹「本板没有背光器件」的 Toast，
         * 等于没有任何显示相关的设置。现在是真的子页：时钟显秒 + 手动校时，
         * 亮度那句说明挪到子页底部（见 as_page_display.cc）。
         */
        as_push_subpage(as_sub_display());
        return;
    case SET_STORAGE:
        /* 存储现在有详情页：容量 / 可用 / 占用条 / 卡的物理信息（见 page_storage） */
        as_push_subpage(as_sub_storage());
        return;
    case SET_ABOUT:
        /*
         * 关于本机：进设备信息页（芯片 / Flash / 内存 / MAC / 系统版本与运行时长）。
         * 以前这里只弹一句 Toast（版本号），信息太少 —— 需求要"点进去看详情"。
         */
        p.id     = "set_about";
        p.create = page_about;
        break;
    default:
        ui_toast("ESP32-S31-Korvo-1 · ESP-IDF 6.1 · LVGL 9.5", 2200);
        return;
    }
    ui_router_push(&p);
}

lv_obj_t *page_settings(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "设置", NULL, NULL);

    /*
     * 无线总开关 + 进入子页：**合成一行**（需求）。
     * 关掉会真正释放驱动/协议栈资源，状态记在 NVS，开机恢复。
     */
    as_radio_switch_row(body, LV_SYMBOL_WIFI, 0x3E9E86, "Wi-Fi", AS_RADIO_WIFI,
                        as_sub_wifi(), &s_sum[SET_WIFI]);
    as_radio_switch_row(body, LV_SYMBOL_BLUETOOTH, 0x4A6FA5, "蓝牙", AS_RADIO_BT,
                        as_sub_bt(), &s_sum[SET_BT]);

    static const struct {
        const char *sym;
        uint32_t    accent;
        const char *name;
        int         id;
    } rows[] = {
        { LV_SYMBOL_VOLUME_MAX, 0xE8973A, "声音与输出",   SET_OUT },
        { LV_SYMBOL_EYE_OPEN,   0x6E6E78, "显示设置",     SET_DISPLAY },
        { LV_SYMBOL_SD_CARD,    0x5B6FB5, "存储",         SET_STORAGE },
        { LV_SYMBOL_SETTINGS,   0x6E6E78, "关于本机",     SET_ABOUT },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *row = as_make_row(body, rows[i].sym, rows[i].accent, rows[i].name,
                                    &s_sum[rows[i].id]);
        lv_obj_add_event_cb(row, on_setting_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)rows[i].id);
    }

    lv_obj_add_event_cb(scr, settings_on_delete, LV_EVENT_DELETE, NULL);
    s_sum_timer = lv_timer_create(settings_tick, 500, NULL);
    settings_tick(NULL);
    ESP_LOGI(TAG, "settings page created");
    return scr;
}
