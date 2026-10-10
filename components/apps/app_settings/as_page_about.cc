/*
 * as_page_about.cc —— 关于本机（设备信息页）+ 芯片温度传感器
 *
 * 原 app_settings.cc 1627–2005 行**整段搬迁**：
 *   - 设备信息页说明注释（1629–1644）
 *   - s_about_* 值 label（1645–1651）
 *   - 芯片温度传感器（1653–1728）→ 含 about_tsens_acquire / release / update
 *   - ABOUT_TICK_MS（1730）
 *   - about_fmt_bytes（1736–1752）→ 移到 as_fmt.cc，改名 as_fmt_bytes（纯逻辑）
 *   - about_fmt_mac（1755–1764）→ 仍在**本文件**（它要 esp_read_mac，
 *     与"纯逻辑"分册的定位不符；只此一处调用，就不搬去 as_fmt.cc 了）
 *   - about_section / about_add / about_refresh_live / about_tick /
 *     about_chip_name / about_fill_static / about_on_delete / page_about
 *
 * ⚠ 保留的语义（拆分时不许改）：
 *   1. 温度传感器 **install 一次、enable/disable 随页面生命周期**，
 *      装不上就置 `s_about_tsens_dead` 不再重试；
 *   2. 读数**不能用 %f**（本工程 newlib 是 nano 配置），整数域四舍五入到 0.1 °C；
 *   3. 离开页面 `about_on_delete` 必须先 `about_tsens_release()` 再清 label 指针；
 *   4. `about_fill_static` 里的 `esp_image_get_metadata` 等 IDF 调用**保持在 LVGL 任务**
 *      （一次性构建，不进后台任务）。
 */
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_app_desc.h"
/* 关于本机（设备信息页）要用的只读查询接口：都不需要初始化驱动 */
#include "esp_chip_info.h"      /* 型号 / 核心数 / 修订号 */
#include "esp_flash.h"          /* esp_flash_get_size()：芯片 Flash 总容量 */
#include "esp_partition.h"      /* 运行分区的地址 / 大小 */
#include "esp_ota_ops.h"        /* esp_ota_get_running_partition() */
#include "esp_image_format.h"   /* esp_image_get_metadata()：镜像实际长度 */
#include "esp_mac.h"            /* esp_read_mac()：Wi-Fi / 蓝牙 MAC（读 eFuse） */
#include "esp_heap_caps.h"      /* SRAM / PSRAM 的总量与可用量 */
#include "esp_timer.h"          /* 运行时长 */
#include "driver/temperature_sensor.h"   /* 芯片温度（需 esp_driver_tsens） */

#include "ui_core.h"
#include "ui_metrics.h"

#include "bsp.h"
#include "storage_service.h"

#include "as_fmt.h"
#include "as_page_shell.h"
#include "as_widgets.h"

static const char *TAG = "app_settings";

/* ================================================================== 关于本机 */

/*
 * 设备信息页（需求：设置 →「关于本机」点进去，看硬件与系统详情）。
 *
 * 五组：芯片 / Flash / 内存 / 网络标识 / 系统。数据全部来自 IDF 的**只读查询**接口，
 * 不需要初始化任何驱动，也不占运行时资源：
 *   - 芯片：esp_chip_info() + esp_clk_cpu_freq()
 *   - Flash：esp_flash_get_size()（芯片总容量）+ 运行分区的 address/size
 *            + esp_image_get_metadata()（镜像实际长度 → 才能算"还剩多少"）
 *   - 内存：heap_caps_*（内部 RAM 与 PSRAM 分开问）
 *   - MAC：esp_read_mac() **直接读 eFuse**，Wi-Fi / 蓝牙关着也能读到
 *   - 系统：esp_app_get_description() + esp_timer_get_time() + LVGL 版本宏
 *
 * ★ "会变的"几行（可用内存 / 运行时长 / SD 卡）挂在 1 s 的 lv_timer 上**只改文本**，
 *   不重建控件 —— 重建会把滚动位置和触摸状态一起弄乱（存储页同理，见 storage_rebuild）。
 *   一次性填完就不动的那些行，连句柄都不用留。
 */
static lv_obj_t   *s_about_body;
static lv_timer_t *s_about_timer;
static lv_obj_t   *s_about_lv_sram;     /* 值 label：SRAM 可用 */
static lv_obj_t   *s_about_lv_psram;    /* 值 label：PSRAM 可用 */
static lv_obj_t   *s_about_lv_uptime;   /* 值 label：运行时长 */
static lv_obj_t   *s_about_lv_sd;       /* 值 label：SD 卡 */
static lv_obj_t   *s_about_lv_temp;     /* 值 label：芯片温度 */

/* --------------------------------------------------------- 芯片温度传感器 */
/*
 * 需求追加：显示芯片温度。
 *
 *   - 用 IDF 的 temperature_sensor 驱动（组件 `esp_driver_tsens`）。S31 **支持**该外设
 *     （`CONFIG_SOC_TEMP_SENSOR_SUPPORTED=y`），而且 `SOC_TSENS_IS_INDEPENDENT_FROM_ADC=1`
 *     —— 它**不跟 ADC 共用寄存器**，而本板的六个按键正是 ADC1 读的，这点很关键：
 *     别换成任何"共享 ADC"的方案，否则会干扰按键。
 *   - 量程只能选芯片给定的几档（表在
 *     `esp_hal_ana_conv/esp32s31/temperature_sensor_periph.c`）：
 *     50~125(±3) / 20~100(±2) / **-10~80(±1)** / -30~50(±2) / -40~20(±3)。
 *     取 **-10~80**：日常环境温度落在这一档，且误差最小（±1 °C）。
 *     ⚠ 请求的 [min,max] 必须被某一档**完整包住**，否则 install 会报
 *     "the boundary you gave cannot meet the range of internal temperature sensor"。
 *   - **装一次、按页面生命周期开关**：enable 会给传感器上电（它在低功耗域），
 *     离开页面就 disable，不常驻耗电；install 只做一次（它内部要 malloc 一份量程表，
 *     反复装会反复分配）。
 *   - 读数**不能用 %f**：本工程 newlib 是 nano 配置，printf 不支持浮点 ——
 *     在整数域四舍五入到 0.1 °C 再拼字符串。
 */
static temperature_sensor_handle_t s_about_tsens;
static bool     s_about_tsens_on;
static bool     s_about_tsens_dead;     /* 装不上/读不了：别每进一次页面就重试 */

#define ABOUT_TSENS_DEFAULT_RANGE_MIN  (-10)
#define ABOUT_TSENS_DEFAULT_RANGE_MAX  (80)

static void about_tsens_acquire(void)
{
    if (s_about_tsens_dead) {
        return;
    }
    if (!s_about_tsens) {
        temperature_sensor_config_t cfg =
            TEMPERATURE_SENSOR_CONFIG_DEFAULT(ABOUT_TSENS_DEFAULT_RANGE_MIN,
                                              ABOUT_TSENS_DEFAULT_RANGE_MAX);
        if (temperature_sensor_install(&cfg, &s_about_tsens) != ESP_OK) {
            ESP_LOGW(TAG, "temperature sensor install failed (range %d~%d)",
                     ABOUT_TSENS_DEFAULT_RANGE_MIN, ABOUT_TSENS_DEFAULT_RANGE_MAX);
            s_about_tsens_dead = true;
            return;
        }
    }
    if (!s_about_tsens_on && temperature_sensor_enable(s_about_tsens) == ESP_OK) {
        s_about_tsens_on = true;
    }
}

/* 离开页面就断电；install 留着（下次进页面只 enable，不再 malloc） */
static void about_tsens_release(void)
{
    if (s_about_tsens && s_about_tsens_on) {
        temperature_sensor_disable(s_about_tsens);
        s_about_tsens_on = false;
    }
}

static void about_tsens_update(void)
{
    if (!s_about_lv_temp) {
        return;
    }
    float c = 0.0f;
    if (s_about_tsens_dead || !s_about_tsens_on ||
        temperature_sensor_get_celsius(s_about_tsens, &c) != ESP_OK) {
        as_label_set_if_changed(s_about_lv_temp, "不可用");
        return;
    }
    /* 0.1 °C 分辨率：整数四舍五入（负数往绝对值大的方向舍） */
    const int tenths = (int)((c >= 0.0f) ? (c * 10.0f + 0.5f) : (c * 10.0f - 0.5f));
    const int whole  = tenths / 10;
    const int frac   = (tenths % 10 < 0) ? -(tenths % 10) : (tenths % 10);
    char v[24];
    snprintf(v, sizeof(v), "%d.%d °C", whole, frac);
    as_label_set_if_changed(s_about_lv_temp, v);
}

#define ABOUT_TICK_MS   (1000)

/* MAC 六段文本；调用方保证 cap >= 20 */
static void about_fmt_mac(esp_mac_type_t type, char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, type) != ESP_OK) {
        snprintf(out, cap, "读取失败");
        return;
    }
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* 分组小标题（比行矮、颜色淡，起分隔作用） */
static void about_section(const char *title)
{
    lv_obj_t *l = lv_label_create(s_about_body);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_top(l, UI_PX_H(6), LV_PART_MAIN);
    lv_label_set_text(l, title);
}

/* 一行"名称 → 值"；out 非 NULL 时把值 label 交出去（给 tick 刷新用） */
static void about_add(const char *sym, uint32_t accent, const char *name,
                      const char *value, lv_obj_t **out)
{
    lv_obj_t *val = NULL;
    as_make_row(s_about_body, sym, accent, name, &val);
    if (val) {
        /* 比列表默认的 30% 宽：MAC 地址、容量串比"已连接"这类状态长得多 */
        lv_obj_set_width(val, UI_PCT_X(52));
        lv_label_set_text(val, value);
    }
    if (out) {
        *out = val;
    }
}

/* 每秒刷新的那几行。用 as_label_set_if_changed（原 label_set_if_changed）免得每 1 s 都重排一次 label */
static void about_refresh_live(void)
{
    char buf[40];
    char v[48];

    if (s_about_lv_sram) {
        const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t all_b  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        as_fmt_bytes(free_b, buf, sizeof(buf));
        /* %.20s 显式限长：GCC 证不出 buf 装得进 v（-Werror=format-truncation 踩过） */
        snprintf(v, sizeof(v), "%.20s · %u%%",
                 buf, (unsigned)(all_b ? ((uint64_t)free_b * 100u / all_b) : 0));
        as_label_set_if_changed(s_about_lv_sram, v);
    }
    if (s_about_lv_psram) {
        const size_t all_b  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (all_b == 0) {
            snprintf(v, sizeof(v), "未启用");
        } else {
            as_fmt_bytes(free_b, buf, sizeof(buf));
            snprintf(v, sizeof(v), "%.20s · %u%%",
                     buf, (unsigned)((uint64_t)free_b * 100u / all_b));
        }
        as_label_set_if_changed(s_about_lv_psram, v);
    }
    if (s_about_lv_uptime) {
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        const uint32_t d = sec / 86400u;
        const uint32_t h = (sec % 86400u) / 3600u;
        const uint32_t m = (sec % 3600u) / 60u;
        const uint32_t s = sec % 60u;
        if (d > 0) {
            snprintf(v, sizeof(v), "%u 天 %02u:%02u:%02u",
                     (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)s);
        } else {
            snprintf(v, sizeof(v), "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
        }
        as_label_set_if_changed(s_about_lv_uptime, v);
    }
    if (s_about_lv_sd) {
        uint32_t total_kb = 0, free_kb = 0;
        if (!storage_service_sd_is_mounted()) {
            snprintf(v, sizeof(v), "未挂载");
        } else if (bsp_sdcard_capacity_kb(&total_kb, &free_kb) == ESP_OK && total_kb > 0) {
            char f[24];
            as_fmt_size_kb(free_kb, f, sizeof(f));
            snprintf(v, sizeof(v), "可用 %.20s", f);
        } else {
            snprintf(v, sizeof(v), "已挂载");
        }
        as_label_set_if_changed(s_about_lv_sd, v);
    }
    /* 芯片温度：自己拼字符串（不能 %f，见 about_tsens_update 的说明） */
    about_tsens_update();
}

static void about_tick(lv_timer_t *t)
{
    (void)t;
    if (s_about_body) {
        about_refresh_live();
    }
}

static const char *about_chip_name(const esp_chip_info_t *info)
{
    switch (info->model) {
    case CHIP_ESP32:    return "ESP32";
    case CHIP_ESP32S2:  return "ESP32-S2";
    case CHIP_ESP32S3:  return "ESP32-S3";
    case CHIP_ESP32C3:  return "ESP32-C3";
    case CHIP_ESP32C6:  return "ESP32-C6";
    case CHIP_ESP32H2:  return "ESP32-H2";
    case CHIP_ESP32P4:  return "ESP32-P4";
    case CHIP_ESP32S31: return "ESP32-S31";
    default:            return CONFIG_IDF_TARGET;   /* 枚举里没有的就报 target 名 */
    }
}

/* 一次性把"不变"的行铺出来（会变的那四行由 about_refresh_live 周期刷） */
static void about_fill_static(void)
{
    char buf[48];
    char v[48];

    /* ---------------------------------------------------------------- 芯片 */
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    about_section("芯片");
    about_add(LV_SYMBOL_SETTINGS, 0x5B6FB5, "型号", about_chip_name(&chip), NULL);
    snprintf(buf, sizeof(buf), "%u 核 · 修订 v%u.%u",
             (unsigned)chip.cores, (unsigned)(chip.revision / 100),
             (unsigned)(chip.revision % 100));
    about_add(LV_SYMBOL_LIST, 0x6E6E78, "核心", buf, NULL);
    /*
     * CPU 频率：用 Kconfig 值而不是 esp_clk_cpu_freq() —— 后者声明在
     * `esp_private/esp_clk.h`（私有头，别碰）。本工程**没开电源管理/DFS**
     * （sdkconfig 里没有 CONFIG_PM_ENABLE），主频开机定死，配置值就是运行时值。
     * 哪天真开了 DFS，这里要换成运行时查询（`esp_pm_...` 或私有头）。
     */
    snprintf(buf, sizeof(buf), "%u MHz", (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    about_add(LV_SYMBOL_REFRESH, 0x3E9E86, "CPU 频率", buf, NULL);
    /* 芯片温度：值由 about_tsens_update() 周期填（LVGL 里没有温度计图标，
     * 用 POWER/电源符号示意"发热"这一档） */
    about_add(LV_SYMBOL_POWER, 0xE8973A, "芯片温度", "-", &s_about_lv_temp);

    /* --------------------------------------------------------------- Flash */
    about_section("Flash 存储");
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK && flash_size > 0) {
        as_fmt_bytes(flash_size, buf, sizeof(buf));
        about_add(LV_SYMBOL_DRIVE, 0xE8973A, "Flash 容量", buf, NULL);
    } else {
        about_add(LV_SYMBOL_DRIVE, 0xE8973A, "Flash 容量", "读取失败", NULL);
    }
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (part) {
        about_add(LV_SYMBOL_DIRECTORY, 0x5B6FB5, "运行分区", part->label, NULL);
        as_fmt_bytes(part->size, buf, sizeof(buf));
        about_add(LV_SYMBOL_FILE, 0x6E6E78, "分区大小", buf, NULL);
        /* 镜像实际长度：只能从引导头里读（esp_app_get_description 没有这个字段） */
        esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
        esp_image_metadata_t meta = {};
        if (esp_image_get_metadata(&pos, &meta) == ESP_OK && meta.image_len > 0) {
            as_fmt_bytes(meta.image_len, buf, sizeof(buf));
            about_add(LV_SYMBOL_DOWNLOAD, 0x5B6FB5, "固件占用", buf, NULL);
            const uint32_t free_b = (part->size > meta.image_len)
                                        ? (uint32_t)(part->size - meta.image_len) : 0;
            as_fmt_bytes(free_b, buf, sizeof(buf));
            snprintf(v, sizeof(v), "%.20s · %u%%", buf,
                     (unsigned)((uint64_t)free_b * 100u / part->size));
            about_add(LV_SYMBOL_SAVE, 0x3E9E86, "Flash 可用", v, NULL);
        }
    }

    /* ----------------------------------------------------------------- 内存 */
    about_section("内存");
    const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    as_fmt_bytes(sram_total, buf, sizeof(buf));
    about_add(LV_SYMBOL_BARS, 0x5B6FB5, "SRAM 总量", buf, NULL);
    /* 值由 tick 填（s_about_lv_sram） */
    about_add(LV_SYMBOL_SAVE, 0x3E9E86, "SRAM 可用", "-", &s_about_lv_sram);
    as_fmt_bytes(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), buf, sizeof(buf));
    about_add(LV_SYMBOL_LIST, 0x6E6E78, "SRAM 最大块", buf, NULL);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        as_fmt_bytes(psram_total, buf, sizeof(buf));
        about_add(LV_SYMBOL_BARS, 0x5B6FB5, "PSRAM 总量", buf, NULL);
    } else {
        about_add(LV_SYMBOL_BARS, 0x5B6FB5, "PSRAM 总量", "未启用", NULL);
    }
    about_add(LV_SYMBOL_SAVE, 0x3E9E86, "PSRAM 可用", "-", &s_about_lv_psram);

    /* ------------------------------------------------------------- 网络标识 */
    about_section("网络标识");
    about_fmt_mac(ESP_MAC_WIFI_STA, buf, sizeof(buf));
    about_add(LV_SYMBOL_WIFI, 0x3E9E86, "Wi-Fi MAC", buf, NULL);
    about_fmt_mac(ESP_MAC_WIFI_SOFTAP, buf, sizeof(buf));
    about_add(LV_SYMBOL_WIFI, 0x4A6FA5, "热点 MAC", buf, NULL);
    about_fmt_mac(ESP_MAC_BT, buf, sizeof(buf));
    about_add(LV_SYMBOL_BLUETOOTH, 0x4A6FA5, "蓝牙 MAC", buf, NULL);

    /* ----------------------------------------------------------------- 系统 */
    about_section("系统");
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        about_add(LV_SYMBOL_EDIT, 0xE8973A, "固件版本", app->version, NULL);
        snprintf(buf, sizeof(buf), "%.11s %.8s", app->date, app->time);
        about_add(LV_SYMBOL_LIST, 0x6E6E78, "编译时间", buf, NULL);
        about_add(LV_SYMBOL_SETTINGS, 0x5B6FB5, "IDF 版本", app->idf_ver, NULL);
    }
    snprintf(buf, sizeof(buf), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH);
    about_add(LV_SYMBOL_IMAGE, 0x6E6E78, "LVGL 版本", buf, NULL);
    about_add(LV_SYMBOL_REFRESH, 0x3E9E86, "运行时长", "-", &s_about_lv_uptime);
    snprintf(buf, sizeof(buf), "%d x %d", (int)UI_W, (int)UI_H);
    about_add(LV_SYMBOL_EYE_OPEN, 0x5B6FB5, "屏幕分辨率", buf, NULL);
    about_add(LV_SYMBOL_SD_CARD, 0x5B6FB5, "SD 卡", "-", &s_about_lv_sd);

    ESP_LOGI(TAG, "about page: flash=%u B, sram=%u B, psram=%u B",
             (unsigned)flash_size, (unsigned)sram_total, (unsigned)psram_total);
}

static void about_on_delete(lv_event_t *e)
{
    (void)e;
    s_about_body = NULL;
    s_about_lv_sram = s_about_lv_psram = s_about_lv_uptime = s_about_lv_sd = NULL;
    s_about_lv_temp = NULL;
    about_tsens_release();             /* 离开页面就给温度传感器断电（install 留着） */
    if (s_about_timer) {
        lv_timer_delete(s_about_timer);
        s_about_timer = NULL;
    }
}

lv_obj_t *page_about(void)
{
    lv_obj_t *scr  = as_make_screen();
    lv_obj_t *body = as_page_shell(scr, "关于本机", NULL, NULL);
    s_about_body = body;
    s_about_lv_sram = s_about_lv_psram = s_about_lv_uptime = s_about_lv_sd = NULL;
    s_about_lv_temp = NULL;
    about_tsens_acquire();              /* 先给温度传感器上电，下面填值才有得读 */
    about_fill_static();
    about_refresh_live();              /* 先把"会变的"那几行填上，别显示 "-" */
    if (!s_about_timer) {
        s_about_timer = lv_timer_create(about_tick, ABOUT_TICK_MS, NULL);
    }
    lv_obj_add_event_cb(scr, about_on_delete, LV_EVENT_DELETE, NULL);
    return scr;
}
