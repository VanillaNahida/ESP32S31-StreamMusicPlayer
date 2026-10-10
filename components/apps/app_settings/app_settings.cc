/*
 * 原 app_settings.cc 文件头（**逐字保留**，拆分后这些规矩仍然有效）：
 *
 * app_settings.cc —— 设置页：Wi-Fi / 蓝牙 / 声音与输出 / 显示 / 存储 / 关于
 *
 * 结构（开发指南 5.16 + 需求）：
 *   一级页：分组条目 + 当前值摘要，点进二级页
 *   二级页：Wi-Fi（扫描/连接/保存/忘记）、蓝牙（扫描/配对状态/忘记）、
 *           声音与输出（板载功放 / 蓝牙(未接入) / 静音 + 音量）
 *
 * 规矩：
 *   - 服务（wifi_service / bt_service）只在后台任务里初始化，UI 用 lv_timer
 *     轮询状态；**绝不在事件回调里碰 LVGL**
 *   - 列表只在数据"签名"变化时重建，避免每 500 ms 重铺控件
 */

/*
 * 本文件**只剩外壳**：把"设置"注册进桌面，并提供"从别处跳到蓝牙子页"的入口。
 * 页面实现按职责拆到同目录的 as_*.cc（见 docs/refactor/06_app_settings.md §6）：
 *
 *   as_widgets.cc       通用控件工厂（screen / row / 药丸按钮 / 行参数副本 / label 防抖）
 *   as_fmt.cc           **纯逻辑**格式化（KB / 字节；不依赖 LVGL，可 host 单测）
 *   as_page_shell.cc    二级页外壳 + 子页描述符 + 跳转
 *   as_radio.cc         无线总开关（Wi-Fi / 蓝牙）：跨任务面全在这里
 *   as_page_main.cc     一级页：设置列表（摘要 + tick）
 *   as_page_wifi.cc     Wi-Fi 子页 + 密码输入浮层
 *   as_page_bt.cc       蓝牙子页
 *   as_page_output.cc   声音与输出子页（输出选择 + 音量）
 *   as_page_storage.cc  存储子页（容量卡 / 占用条 / 热插拔重画）
 *   as_page_about.cc    关于本机 + 芯片温度传感器
 *
 * 所有 LVGL 调用都在 LVGL 任务上下文（页面构建 / 事件回调 / lv_timer）。
 */
#include "app_settings.h"

#include "esp_log.h"

#include "ui_core.h"
#include "ui_app_registry.h"

#include "as_page_main.h"
#include "as_page_shell.h"

static const char *TAG = "app_settings";

/* ================================================================== 注册 */

/*
 * 从别处（音乐页长按输出设备）直接跳到"设置的蓝牙页"。
 *
 * 直接 push 蓝牙子页而不是先 push 设置主页：用户的目标就是蓝牙，多一层
 * 主页只是让他多按一次返回。子页外壳自带返回钮，返回即回到来处。
 */
esp_err_t app_settings_open_bt(void)
{
    page_desc_t p = {};
    p.id      = as_sub_bt()->id;
    p.create  = as_sub_bt()->create;
    p.no_back = true;            /* as_page_shell 自带返回钮 */
    return ui_router_push(&p);
}

static void on_settings_enter(void) {}
static void on_settings_exit(void) {}

void app_settings_register_apps(void)
{
    static const app_desc_t settings = {
        "settings", "设置", LV_SYMBOL_SETTINGS, 0x6E6E78,
        page_settings, on_settings_enter, on_settings_exit, 60,
        APP_FLAG_NO_BACK      /* as_page_shell 自带返回钮，路由不再叠全局键 */
    };
    ui_app_register(&settings);
    ESP_LOGI(TAG, "settings app registered");
}
