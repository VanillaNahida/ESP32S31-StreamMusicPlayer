# 06 `app_settings.cc` 拆分重构蓝图

> 审计对象：`components/apps/app_settings/app_settings.cc`
> 真实行数：**2036 行**（LF 计数，与 `read`/`grep` 行号一致；用户口头说"约 1746 行"不准，
> pwsh 的 `Get-Content | Measure-Object -Line` 也会少算）
> 头文件：`components/apps/app_settings/include/app_settings.h`（`app_settings_register_apps()`、
> `app_settings_open_bt()`）
> 本报告为只读审计产物，未修改任何源文件。

---

## 1. 文件概览

### 1.1 include（14–45 行）

| 类别 | 内容 |
| --- | --- |
| 标准库 | `stdio.h` `string.h` `stdint.h` |
| ESP-IDF | `esp_log.h` `esp_app_desc.h` `esp_chip_info.h` `esp_flash.h` `esp_partition.h` `esp_ota_ops.h` `esp_image_format.h` `esp_mac.h` `esp_heap_caps.h` `esp_timer.h` `driver/temperature_sensor.h`、`freertos/FreeRTOS.h` `freertos/task.h` |
| UI 层 | `ui_core.h` `ui_metrics.h` `ui_ba.h` `ui_app_registry.h` |
| 服务层 | `bsp.h`、`audio_service.h`、`wifi_service.h`、`bt_service.h`、`storage_service.h` |

**没有** `esp_lvgl_adapter`（grep 确认）—— 本文件**没有任何锁**；LVGL 操作全部在 LVGL 任务
（页面构建 + 事件回调 + 各页 `lv_timer`），只有无线开关的动作被丢进一个自建任务。

### 1.2 结构分区

| 区间 | 分区 |
| --- | --- |
| 1–56 | 文件头规矩（"服务只在后台任务里 init，UI 用 lv_timer 轮询，绝不在事件回调里碰 LVGL"）+ `TAG` + `s_radio_busy` |
| 58–246 | **通用控件工厂**：`make_screen` / `on_back` / `make_flat_btn` / `make_page_shell` / `make_row` / `row_user_dup` / `row_user_free` / `make_row_btn(_copy)` |
| 248–265 | 主页摘要行指针表 `s_sum[SET_N]` + `s_sum_timer` |
| 267–428 | **无线总开关**：`settings_on_delete`、`radio_toggle_task`、`on_radio_switch`、开关实例表与同步（`radio_sw_*`）、子页描述符、行点击进子页 |
| 430–550 | `make_switch_row`（图标 + 名称/状态 + 开关 + `›` 一行）+ `label_set_if_changed` |
| 552–712 | `settings_tick`（500 ms 摘要）+ `on_setting_row` + `page_settings` |
| 714–1036 | **Wi-Fi 子页**：扫描/忘记/密码浮层/列表重建/tick/on_delete/page |
| 1038–1262 | **蓝牙子页**：扫描/忘记/连接/断开/列表重建/tick/on_delete/page |
| 1264–1380 | **声音与输出子页**：输出方式选择 + 音量滑块 |
| 1399–1626 | **存储子页**：容量卡（占用条）/信息行/重新检测/热插拔 tick/on_delete/page |
| 1640–2010 | **关于本机**：温度传感器 acquire/release/update、格式化、静态信息、实时刷新、1 s tick、on_delete、page |
| 2012–2036 | 外部入口 `app_settings_open_bt()` + 注册 |

---

## 2. 全部函数清单（71 个顶层定义）

（250–253、255、258、722、1044 是前向声明，不计入；2024/2025 是单行空函数。）

| 起始 | 结束 | 行数 | 函数名 | static | 分类 | 一句话职责 |
| --- | --- | --- | --- | --- | --- | --- |
| 60 | 70 | 11 | `make_screen` | ✔ | 控件工厂 | 建无样式 screen（`pad_all` 在 `pad_top` 之前，避免状态栏被覆盖） |
| 72 | 76 | 5 | `on_back` | ✔ | 事件回调 | `ui_router_pop()` |
| 78 | 98 | 21 | `make_flat_btn` | ✔ | 控件工厂 | 药丸形小按钮（`UI_COLOR_PILL` + 小字） |
| 104 | 156 | 53 | `make_page_shell` | ✔ | 控件工厂 | 二级页外壳：返回钮 + 标题（+ 可选右上动作钮），返回可滚动 body |
| 159 | 203 | 45 | `make_row` | ✔ | 控件工厂 | 通用条目行（图标 + 名称 + 可选右侧值 label，值指针可回传） |
| 214 | 220 | 7 | `row_user_free` | ✔ | 生命周期 | 行 DELETE 时 `free(user_data)`（配合 `row_user_dup`） |
| 222 | 229 | 8 | `row_user_dup` | ✔ | 内存 | `malloc` + 拷贝 user_data（**防止列表重建后悬垂**） |
| 231 | 236 | 6 | `make_row_btn` | ✔ | 控件工厂 | 行内右侧文字按钮（直接用外部 user_data） |
| 239 | 246 | 8 | `make_row_btn_copy` | ✔ | 控件工厂 | 同上，但**自己拷一份** user_data（+ 自动 DELETE 释放） |
| 267 | 276 | 10 | `settings_on_delete` | ✔ | 生命周期 | 删主页 tick 定时器并 `memset(s_sum)`（开关由自身 DELETE 注销） |
| 285 | 295 | 11 | `radio_toggle_task` | ✔ | **后台任务** | 调 `wifi_service_set_enabled()` / `bt_service_set_enabled()`，完成后清 `s_radio_busy`，自删 |
| 297 | 329 | 33 | `on_radio_switch` | ✔ | 事件回调 | 拦冒泡、防重入（`s_radio_busy`）、**钉在 BTC 核**上起 `radio_toggle_task`（312–319 注释：否则 a2dp 注册与 deinit 抢静态标志会 abort）、Toast |
| 341 | 350 | 10 | `radio_sw_register` | ✔ | 开关同步 | 把开关实例登记进 `s_radio_sw[]`（最多 4 个） |
| 357 | 365 | 9 | `radio_sw_deleted` | ✔ | 开关同步 | DELETE 时按指针注销（**防 use-after-free**，353–356 注释） |
| 373 | 394 | 22 | `radio_sw_sync` | ✔ | 开关同步 | 把主页/子页所有开关实例拉回服务真实状态（`s_radio_busy` 时跳过） |
| 397 | 400 | 4 | `on_radio_switch_clicked` | ✔ | 事件回调 | 只拦冒泡（点开关不跳页） |
| 417 | 428 | 12 | `on_switch_row_click` | ✔ | 事件回调 | 点行（非开关区）→ `ui_router_push(subpage)`（`no_back=true`） |
| 450 | 529 | 80 | `make_switch_row` | ✔ | 控件工厂 | 开关行：图标/名称/状态/开关/`›`；开关登记 + DELETE 注销 + 事件绑定 |
| 540 | 550 | 11 | `label_set_if_changed` | ✔ | **纯逻辑** | 文本相同就不写 label（避免 500 ms 刷新的堆抖动） |
| 552 | 634 | 83 | `settings_tick` | ✔ | lv_timer 回调 | 主页摘要：Wi-Fi/蓝牙/输出/显示/**存储（节流 5 s）**/版本 |
| 636 | 672 | 37 | `on_setting_row` | ✔ | 事件回调 | 按 id 分派：输出/显示（Toast 无背光）/存储/关于 → `ui_router_push` |
| 674 | 712 | 39 | `page_settings` | ✔ | 页面构建 | 建主页：两个开关行 + 4 个条目行 + DELETE 回调 + 500 ms tick |
| 724 | 732 | 9 | `on_wifi_scan` | ✔ | 事件回调 | `wifi_service_scan_start()` + 置 `s_wifi_dirty` |
| 734 | 746 | 13 | `on_wifi_forget` | ✔ | 事件回调 | `wifi_service_forget(ssid)` + 置 dirty + Toast |
| 765 | 779 | 15 | `pw_close` | ✔ | 浮层 | 用 `ui_ba_close()`（**异步删**）关密码浮层；注释 768–773 记录"同步删会 hard fault" |
| 781 | 786 | 6 | `on_pw_overlay_deleted` | ✔ | 浮层 | DELETE 回调清指针 |
| 788 | 807 | 20 | `on_pw_ready` | ✔ | 浮层 | 取密码：**留空且已保存 → `connect_saved()`**，否则 `connect(ssid,pass,true)` |
| 809 | 813 | 5 | `on_pw_cancel` | ✔ | 浮层 | 取消 → `pw_close()` |
| 815 | 855 | 41 | `wifi_open_pw` | ✔ | 浮层 | 建密码浮层：**自己拷 SSID 到 `s_pw_ssid`**、挂当前 screen（不是 layer_top）、建 LVGL 键盘 |
| 857 | 882 | 26 | `on_wifi_ap_click` | ✔ | 事件回调 | 已保存 → 直连；开放 → 空密码连；否则弹密码浮层 |
| 884 | 943 | 60 | `wifi_rebuild_list` | ✔ | LVGL 重建 | 重建列表：已保存网络（可"忘记"）+ 附近网络（含 dBm/加密/已保存），**SSID 一律拷贝** |
| 945 | 988 | 44 | `wifi_tick` | ✔ | lv_timer 回调 | 同步总开关；刷状态行（含 `wifi_service_last_reason_str()`）；**签名变化才重建列表**（980–987） |
| 990 | 1000 | 11 | `wifi_on_delete` | ✔ | 生命周期 | 删 tick、清 `s_wifi_*` 指针与签名 |
| 1002 | 1034 | 33 | `page_wifi` | ✔ | 页面构建 | Wi-Fi 子页（顶部同一个总开关 + 状态行 + 列表 + 扫描钮） |
| 1046 | 1060 | 15 | `on_bt_scan` | ✔ | 事件回调 | `bt_service_start_discovery()`；对 `BT_ST_FAILED`/未就绪给不同 Toast |
| 1062 | 1078 | 17 | `on_bt_forget` | ✔ | 事件回调 | `bt_service_remove_bond(bda)` + 重建 |
| 1081 | 1093 | 13 | `on_bt_dev_click` | ✔ | 事件回调 | `bt_service_connect(bda)`（首次自动配对） |
| 1095 | 1103 | 9 | `on_bt_disconnect` | ✔ | 事件回调 | `bt_service_disconnect(bda)` |
| 1105 | 1180 | 76 | `bt_rebuild_list` | ✔ | LVGL 重建 | 重建：当前连接卡（断开钮）+ 已配对列表（忘记）+ 附近设备列表 |
| 1182 | 1210 | 29 | `bt_tick` | ✔ | lv_timer 回调 | 同步总开关；刷状态行；签名变化才重建（1202–1205） |
| 1212 | 1222 | 11 | `bt_on_delete` | ✔ | 生命周期 | 删 tick、清指针与签名 |
| 1224 | 1260 | 37 | `page_bt` | ✔ | 页面构建 | 蓝牙子页（顶部总开关 + 状态行 + 列表 + 扫描钮） |
| 1267 | 1312 | 46 | `out_rebuild` | ✔ | LVGL 重建 | 输出选择：静音/板载扬声器/蓝牙（蓝牙未连接时提示先连接）+ 音量子行 |
| 1314 | 1319 | 6 | `out_on_delete` | ✔ | 生命周期 | 清 `s_out_list/s_out_pct` |
| 1321 | 1379 | 59 | `page_output` | ✔ | 页面构建 | 声音与输出子页（含音量滑块：值变更 → `audio_service_set_volume`） |
| 1404 | 1418 | 15 | `storage_fmt_size_kb` | ✔ | **纯逻辑** | KB → "x.x MB/GB" 文本（整数运算） |
| 1421 | 1428 | 8 | `storage_add_info` | ✔ | 控件工厂 | 存储页信息行（名/值） |
| 1431 | 1439 | 9 | `storage_add_hint` | ✔ | 控件工厂 | 存储页提示文本 |
| 1442 | 1508 | 67 | `storage_add_gauge` | ✔ | LVGL 构建 | 容量卡：已用/共 + 大号百分比 + `lv_bar` 占用条 + 可用（≥90% 变红） |
| 1510 | 1571 | 62 | `storage_rebuild` | ✔ | LVGL 重建 | 未挂载 → 提示卡；挂载 → 容量卡 + 挂载点/卡信息（`bsp_sdcard_card_info`） |
| 1573 | 1585 | 13 | `storage_on_rescan` | ✔ | 事件回调 | `storage_service_sd_rescan()` + 记代次 + Toast |
| 1591 | 1600 | 10 | `storage_sd_tick` | ✔ | lv_timer 回调 | 代次变化（热插拔）就重建 |
| 1602 | 1610 | 9 | `storage_on_delete` | ✔ | 生命周期 | 删 tick、清 `s_storage_body` |
| 1612 | 1625 | 14 | `page_storage` | ✔ | 页面构建 | 存储子页（body + 定时器 + 记初始代次） |
| 1680 | 1699 | 20 | `about_tsens_acquire` | ✔ | 驱动 | `temperature_sensor_install`（失败置 `dead` 不再重试）+ `enable` |
| 1702 | 1708 | 7 | `about_tsens_release` | ✔ | 驱动 | 离开页面 `disable`（install 保留） |
| 1710 | 1728 | 19 | `about_tsens_update` | ✔ | 驱动+UI | 读温度（0.1 ℃ 四舍五入）写 label；不可用写"不可用" |
| 1736 | 1752 | 17 | `about_fmt_bytes` | ✔ | **纯逻辑** | 字节 → 可读字符串（整数运算，**不用 `%f`**，见 1732–1735） |
| 1755 | 1764 | 10 | `about_fmt_mac` | ✔ | **纯逻辑** | `esp_read_mac()` → `AA:BB:…` |
| 1767 | 1774 | 8 | `about_section` | ✔ | 控件工厂 | 关于页分组标题 |
| 1777 | 1790 | 14 | `about_add` | ✔ | 控件工厂 | 关于页信息行 |
| 1793 | 1848 | 56 | `about_refresh_live` | ✔ | LVGL 更新 | 实时项：SRAM/PSRAM 可用、运行时长、SD 容量、芯片温度 |
| 1850 | 1856 | 7 | `about_tick` | ✔ | lv_timer 回调 | 1 s 调 `about_refresh_live` |
| 1858 | 1871 | 14 | `about_chip_name` | ✔ | **纯逻辑** | `esp_chip_info` 型号 → 中文名 |
| 1874 | 1975 | **102** | `about_fill_static` | ✔ | LVGL 构建 | 静态信息：芯片/核心/频率、Flash 总容量、运行分区地址与大小、镜像实际长度、MAC、版本/编译时间 |
| 1977 | 1988 | 12 | `about_on_delete` | ✔ | 生命周期 | 删 tick、`about_tsens_release()`、清 `s_about_*` |
| 1990 | 2005 | 16 | `page_about` | ✔ | 页面构建 | 关于子页（body + `about_fill_static` + acquire + 1 s tick） |
| 2015 | 2022 | 8 | `app_settings_open_bt` | ✘ | **对外** | 供外部直接跳到蓝牙子页（`ui_router_push`） |
| 2024 | 2024 | 1 | `on_settings_enter` | ✔ | 钩子 | 空实现 |
| 2025 | 2025 | 1 | `on_settings_exit` | ✔ | 钩子 | 空实现 |
| 2027 | 2036 | 10 | `app_settings_register_apps` | ✘ | **对外** | 注册设置 App（`APP_FLAG_NO_BACK`：子页外壳自带返回钮） |

**最大 4 个**：`about_fill_static`(102)、`settings_tick`(83)、`make_switch_row`(80)、`bt_rebuild_list`(76)。

---

## 3. 全部全局 / 静态变量清单

| 行号 | 名字 | 类型 | 用途 / 所有权 | 访问上下文 |
| --- | --- | --- | --- | --- |
| 47 | `TAG` | `const char *` | 日志 | 只读 |
| **56** | `s_radio_busy` | `volatile int` | 0=空闲 / 1=切 Wi-Fi / 2=切蓝牙 | **LVGL 任务写、`radio_toggle_task` 写**（跨任务，只靠 volatile，见 R1） |
| 264 | `s_sum[SET_N]` | `lv_obj_t *[]` | 主页 5 行摘要 label（`SET_WIFI/BT/OUT/DISPLAY/STORAGE/ABOUT`） | LVGL 任务；`settings_on_delete` 里 `memset`(275) |
| 265 | `s_sum_timer` | `lv_timer_t *` | 主页 500 ms tick | 同上 |
| 336–339 | `s_radio_sw[RADIO_SW_MAX=4]` | `struct {int which; lv_obj_t *sw;}` | **开关实例表**（主页 + 子页共用同一状态） | LVGL 任务；登记 341、注销 357 |
| 412–414 | `SUB_WIFI` / `SUB_BT` / `SUB_STORAGE` | `static const subpage_desc_t` | 子页描述符（**必须是静态存储期**：router 按值拷指针，见 402–405） | 只读 |
| 716–720 | `s_wifi_status` `s_wifi_list` `s_wifi_timer` `s_wifi_sig` `s_wifi_dirty` | 混合 | Wi-Fi 子页控件/定时器/**重建签名**/强制重建标志 | LVGL 任务 |
| 750–752 | `s_pw_overlay` `s_pw_dlg` `s_pw_ta` | 混合 | 密码浮层与输入框 | LVGL 任务 |
| **763** | `s_pw_ssid[WIFI_SSID_MAX]` | `char[]` | **浮层自己的 SSID 副本**（753–762 注释：不能沿用行上的 `ssid_copy`，列表每 500 ms 可能重建 → 悬垂指针） | LVGL 任务 |
| 1038–1042 | `s_bt_status` `s_bt_list` `s_bt_timer` `s_bt_sig` `s_bt_dirty` | 混合 | 蓝牙子页同上 | LVGL 任务 |
| 1264–1265 | `s_out_list` `s_out_pct` | `lv_obj_t *` | 输出页列表与音量百分比 label | LVGL 任务 |
| 1399–1401 | `s_storage_body` `s_storage_timer` `s_storage_sd_gen` | 混合 | 存储页 body / tick / **SD 状态代次**（热插拔检测） | LVGL 任务 |
| 1645–1651 | `s_about_body` `s_about_timer` `s_about_lv_sram/psram/uptime/sd/temp` | 混合 | 关于页 body / 1 s tick / 5 个**值 label** | LVGL 任务 |
| 1673–1675 | `s_about_tsens` `s_about_tsens_on` `s_about_tsens_dead` | 句柄/bool/bool | 温度传感器句柄、使能态、**装不上就不再重试** | LVGL 任务（页面级） |
| 609/612 | `s_storage_tick` / `s_storage_txt[64]` | **函数级 static** | `settings_tick` 内的 5 s 节流与文本缓存（606–612 注释解释为何不每 500 ms 调 FATFS、以及为何要 64 字节） | LVGL 任务（隐藏全局，拆分时注意） |

---

## 4. 外部依赖清单

### 4.1 依赖的外部 API（按模块，均为 grep 实证）

| 模块 | 用到的符号（行号示例） | 用途 |
| --- | --- | --- |
| `wifi_service` | `wifi_service_is_enabled/set_enabled`(289/383/562)、`wifi_service_state`(564/953)、`wifi_service_conn_ssid`(566/569/957/960)、`wifi_service_scan_start`(727)、`wifi_service_forget`(743)、`wifi_service_is_saved`(798)、`wifi_service_connect_saved`(799/872)、`wifi_service_connect`(802/877)、`wifi_service_ap_count`(864/918/980)、`wifi_service_ap`(865/927)、`wifi_service_saved_count`(892/975/982)、`wifi_service_saved_ssid`(899)、`wifi_service_is_scanning`(923/981)、`wifi_service_ip/rssi`(960)、`wifi_service_last_reason_str`(965) | Wi-Fi 子页与主页摘要的全部数据/动作 |
| `bt_service` | `bt_service_is_enabled/set_enabled`(291/384)、`bt_service_state`(586/1052/1203)、`bt_service_is_discovering`(588/1160/1204)、`bt_service_bonded_count`(590/591/1124/1197/1202)、`bt_service_bonded`(1136)、`bt_service_start_discovery`(1049)、`bt_service_remove_bond`(1070)、`bt_service_connect`(1087)、`bt_service_disconnect`(1101)、`bt_service_is_connected`(1113/1190/1274)、`bt_service_is_streaming`(1117/1275)、`bt_service_peer_name/addr/bda`(1116/1119/1121/1192)、`bt_service_dev_count/dev`(1155/1164/1198/1202)、`bt_service_state_str`(1192/1196)、`bt_service_stream_rate`(1193)、`bt_service_addr_str`(1069/1170) | 蓝牙子页与主页摘要 |
| `audio_service` | `audio_service_get_output`(598/1273)、`audio_service_get_volume`(599/1354/1361)、`audio_service_set_output`(1293)、`audio_service_set_volume`(1371)、`AUDIO_OUT_NONE/BLUETOOTH` | 输出子页 |
| `storage_service` | `storage_service_sd_is_mounted`(1517/1835)、`storage_service_sd_rescan`(1581)、`storage_service_sd_generation`(1582/1594/1617) | 存储页热插拔 |
| `bsp` | `bsp_backlight_get`(602)、`bsp_sdcard_is_mounted`(615)、`bsp_sdcard_capacity_kb`(619/1524/1837)、`bsp_sdcard_mount_point`(1532)、`bsp_sdcard_card_info`(1546) | 背光占位、SD 容量/卡信息 |
| `ui_core` / `ui_metrics` | `ui_router_push`(427/671/2021)、`ui_router_pop`(75)、`ui_toast`、`UI_*` 度量与配色 | 路由与视觉 |
| `ui_ba` | `ui_ba_open_in`(830)、`ui_ba_on_close`(833)、`ui_ba_close`(775)、`ui_ba_text/hint/input/btn` | 密码浮层（BA 卡片） |
| `ui_app_registry` | `ui_app_register`(2034)、`app_desc_t`、`APP_FLAG_NO_BACK` | 注册设置 App |
| ESP-IDF | `esp_chip_info`(1881)、`esp_clk_cpu_freq`(见 1634 注释)、`esp_flash_get_size`(1903)、`esp_ota_get_running_partition`(1909)、`esp_image_get_metadata`(1917)、`esp_read_mac`(1758)、`esp_app_get_description`(633)、`esp_timer_get_time`(运行时长)、`heap_caps_get_*`、`temperature_sensor_*`(1689/1696/1705/1717)、`xTaskCreatePinnedToCore`(318)、`vTaskDelete`(294)、`lv_tick_get/elaps` | 关于页与总开关任务 |
| LVGL 9 | `lv_obj_*` `lv_label_*` `lv_button_create` `lv_switch_create` `lv_slider_create` `lv_bar_create` `lv_keyboard_create` `lv_textarea_*` `lv_timer_*` `lv_event_*` | 全部 UI |

### 4.2 对外导出 API

`include/app_settings.h`：`void app_settings_register_apps(void);`、`esp_err_t app_settings_open_bt(void);`
（后者被别处用来"从其它页面直接跳蓝牙子页"，见 2012–2014 注释）。

---

## 5. 线程 / 回调模型

| 上下文 | 干什么 | 同步 |
| --- | --- | --- |
| **LVGL 任务** | 5 个页面的构建（`page_settings`/`page_wifi`/`page_bt`/`page_output`/`page_storage`/`page_about`）、所有事件回调、**5 个 `lv_timer` 回调**（`settings_tick` 500 ms、`wifi_tick` 500 ms、`bt_tick` 500 ms、`storage_sd_tick`、`about_tick` 1000 ms） | **无锁**（同任务串行，天然安全） |
| **`radio_sw` 任务**（318：`xTaskCreatePinnedToCore(radio_toggle_task, "radio_sw", 8192, …, 3, NULL, CONFIG_BT_BLUEDROID_PINNED_TO_CORE)`） | 调 `wifi_service_set_enabled()` / `bt_service_set_enabled()`（阻塞数百 ms~1 s），完成后 `s_radio_busy = 0` 并自删 | 与 LVGL 只通过 `volatile int s_radio_busy` 交接（**没有内存屏障/信号量**） |
| 服务层回调解耦 | 文件头 9–12 行的规矩：Wi-Fi/蓝牙**状态一律靠 `lv_timer` 轮询**（`wifi_tick`/`bt_tick`），不在服务事件回调里碰 LVGL | 因此**没有跨任务改控件**的问题 |
| 服务调用上下文 | `wifi_service_*`/`bt_service_*` 的查询函数都在 LVGL 任务的 tick 里调（要求这些查询本身轻量、非阻塞）；真正的动作（scan/connect/enable）由服务内部的队列/任务执行 | — |

**唯一跨任务共享变量**是 `s_radio_busy`（56）：LVGL 任务读它决定"是否忽略这次点击/是否同步开关"，
`radio_toggle_task` 写 0。风险见 R1。

---

## 6. 拆分方案

### 6.1 目标目录树

```
components/apps/app_settings/
├── CMakeLists.txt              # 只改 SRCS
├── include/app_settings.h      # 对外契约不变
└── src/
    ├── app_settings.cc         # 注册 + 两个对外入口（~90 行）
    ├── as_widgets.cc           # 通用控件工厂（make_screen/make_row/make_flat_btn/make_row_btn*/row_user_*）（~200 行）
    ├── as_page_shell.cc        # make_page_shell + 子页描述符 + on_setting_row 分派（~150 行）
    ├── as_radio.cc             # 无线总开关：task/事件/实例表/同步（~200 行）
    ├── as_page_main.cc         # 主页：settings_tick + page_settings（~180 行）
    ├── as_page_wifi.cc         # Wi-Fi 子页 + 密码浮层（~330 行）
    ├── as_page_bt.cc           # 蓝牙子页（~240 行）
    ├── as_page_output.cc       # 声音与输出子页（~130 行）
    ├── as_page_storage.cc      # 存储子页 + 容量卡（~250 行）
    ├── as_page_about.cc        # 关于本机 + 温度传感器（~350 行）
    └── as_fmt.cc               # 纯逻辑格式化：KB/字节/MAC/芯片名（~120 行）
```

### 6.2 文件职责与接口

| 文件 | 搬走的行号 | 需要暴露的接口 | 预估行数 |
| --- | --- | --- | --- |
| `src/app_settings.cc` | 2012–2036 | `app_settings_register_apps()`、`app_settings_open_bt()` | ~90 |
| `src/as_widgets.cc` | 58–246 | `as_make_screen()`、`as_make_row()`、`as_make_flat_btn()`、`as_make_row_btn(_copy)()`、`as_row_user_dup/free()`、`as_label_set_if_changed()` | ~200 |
| `src/as_page_shell.cc` | 100–156、402–428、636–672 | `as_page_shell()`、`as_subpage_desc_t`、`as_push_subpage(const as_subpage_desc_t*)` | ~150 |
| `src/as_radio.cc` | 49–56、278–400、430–529 | `as_radio_switch_row(...)`、`as_radio_sync()`、`as_radio_busy()`、`as_radio_reset_on_delete()` | ~200 |
| `src/as_page_main.cc` | 552–712 | `as_page_settings()`、`as_settings_tick()`、`as_settings_page_delete()` | ~180 |
| `src/as_page_wifi.cc` | 714–1036 | `as_page_wifi()`、`as_wifi_tick()`、`as_wifi_page_delete()` | ~330 |
| `src/as_page_bt.cc` | 1038–1262 | `as_page_bt()`、`as_bt_tick()`、`as_bt_page_delete()` | ~240 |
| `src/as_page_output.cc` | 1264–1380 | `as_page_output()`、`as_out_rebuild()`、`as_out_page_delete()` | ~130 |
| `src/as_page_storage.cc` | 1399–1626 | `as_page_storage()`、`as_storage_sd_tick()`、`as_storage_page_delete()` | ~250 |
| `src/as_page_about.cc` | 1640–2010 | `as_page_about()`、`as_about_tick()`、`as_about_page_delete()` | ~350 |
| `src/as_fmt.cc` | 1404–1418、1736–1774、1858–1871 | `as_fmt_size_kb()`、`as_fmt_bytes()`、`as_fmt_mac()`、`as_chip_name()`（**不依赖 LVGL**） | ~120 |

### 6.3 纯逻辑（可 host 单测）

| 纯逻辑 | 行号 | 单测要点 |
| --- | --- | --- |
| `storage_fmt_size_kb` | 1404–1418 | 0 / 1023 / 1024 / 1 GB+ / 极大值 |
| `about_fmt_bytes` | 1736–1752 | 0、999、1000、1 MiB 边界、`UINT64_MAX` |
| `about_fmt_mac` | 1755–1764 | 6 字节格式、失败返回 |
| `about_chip_name` | 1858–1871 | 未知型号回退串 |
| `label_set_if_changed` 的**比较语义** | 540–550 | 可抽成 `as_text_changed(cur,next)`（表驱动） |
| 列表重建的**签名计算** | 980–982 / 1202–1205 | 抽成 `as_wifi_sig(...)` / `as_bt_sig(...)` 纯函数，单测"每种变化都会让签名变" |
| 输出项 → 文案映射 | 1267–1312 | 表驱动（含"蓝牙未连接就别让切"的判定） |
| 开关状态同步决策 | 373–394 | `as_radio_should_flip(which, real, ui_state)` |

### 6.4 迁移顺序（每步可编译、UI 不变）

1. 抽 `as_widgets.cc`（58–246、540–550）——**纯搬运**，所有页面改用新名字。
2. 抽 `as_fmt.cc`（1404–1418、1736–1774、1858–1871）并加 host 单测。
3. 抽 `as_page_shell.cc`（100–156 + `on_setting_row` 642–671）。
4. 抽 `as_radio.cc`（49–56、278–400、430–529）：`s_radio_busy` 与实例表集中到该文件，
   对外只暴露 `as_radio_*`。
5. 按页面逐个抽（wifi → bt → output → storage → about），每个文件自带
   `*_tick()` 与 `*_page_delete()`，把"建表/删表"配对收进同一个文件。
6. `app_settings.cc` 只剩注册与入口；`page_settings` 主页里改成调各页的构建函数。
7. 回归：主页摘要、Wi-Fi 扫描/连接/忘记/密码浮层、蓝牙扫描/配对/连接/断开、
   输出切换与音量、存储热插拔、关于页温度与实时刷新。

---

## 7. 风险清单

| # | 风险 | 触发场景 | 防护 / 验证 |
| --- | --- | --- | --- |
| **R1** | `s_radio_busy` 跨任务共享只靠 `volatile`（56/293/305/310/375） | `radio_toggle_task` 清 0 与 LVGL 侧读同时发生；编译器/CPU 重排 | 改用 `atomic_int`（或加 `xSemaphore`/事件组）；至少保留 `volatile` 并注释内存序假设。回归：快速连点开关 10 次 |
| **R2** | 开关实例表 `s_radio_sw[]`（336–339）**必须**在 DELETE 时注销（357–365），否则 `radio_sw_sync()` 拿已释放指针 `lv_obj_add_state` | 拆 `as_radio.cc` 时漏挂 DELETE 回调，或子页对象在其他分支被删 | 把"登记 + 注销"封在 `as_radio_attach(sw, which)` 一个函数里；回归：反复进出 Wi-Fi/蓝牙子页 |
| **R3** | `xTaskCreatePinnedToCore(..., CONFIG_BT_BLUEDROID_PINNED_TO_CORE)`（318）**必须钉核** | 改回普通 `xTaskCreate` → a2dp 注册与 deinit 抢 `g_a2dp_on_deinit` → `ESP_ERROR_CHECK` abort（312–316 记录的真机崩溃） | 保留钉核 + 注释；回归：关蓝牙再开、反复 5 次 |
| **R4** | 密码浮层的 `s_pw_ssid`（763）**必须**是浮层自己的副本 | 图省事改回行上的 `ssid_copy` → 列表重建后悬垂（AGENTS.md §11.5.4 的坑） | 保留 753–762 的长注释；回归：输密码时让 `wifi_tick` 触发一次重建（扫描中）再点"连接" |
| **R5** | `pw_close()` 必须走 `ui_ba_close()`（异步删，775） | 改成 `lv_obj_delete` → 在键盘事件回调里删自己的父树 = hard fault（768–773） | 保留 + 回归：键盘 ✓ 连接、✕ 取消都点一次 |
| **R6** | 浮层挂在**当前 screen**（829–830 `ui_ba_open_in`），不是 `lv_layer_top` | 拆文件时改成 layer_top → 页面 pop 后浮层跟着用户跑、回调指向已销毁页面 | 注释 823–828 保留；回归：开浮层后按返回键 |
| **R7** | `bt_rebuild_list` 在"断开"按钮上**直接传服务内部指针**：`make_row_btn(row,"断开",on_bt_disconnect,(void*)bt_service_peer_bda())`（1121） | 断连/重连后 `s_peer_bda` 内容变化，或行残留导致用旧 BDA 断开 | 其它行都用 `row_user_dup` 拷贝，这里应统一成拷贝 + DELETE 释放；回归：连接 A → 断开 → 立刻连 B |
| **R8** | 列表重建的**签名公式**（980–982 / 1202–1205）漏项 | 拆文件时改了签名 → 状态变了但列表不刷新（或每 tick 重建） | 抽成纯函数 + 单测；回归：扫描完成、连接成功/失败、忘记网络各看一次 |
| **R9** | 每个页面的 `lv_timer` 必须在各自的 `*_on_delete` 里删（270/993/1214/1604/1979） | 拆文件后某个 tick 漏删 → 定时器持有已释放的 label 指针 | 统一"页面对象 → tick 句柄"在一个结构体里，DELETE 时一键清理；用 LVGL 的 timer 计数/日志核查 |
| **R10** | `about_tsens` 的 install/enable/disable 配对（1680–1708）：install 保留、enable/disable 成对、失败置 `dead` | 拆 `as_page_about.cc` 时漏 `release()` → 温度传感器一直耗电；或每次进页面重装 | 回归：进出关于页 5 次，日志无重复 install；确认不留 enable 状态 |
| **R11** | 温度读数的四舍五入与负号处理（1722–1724） | 简化成 `c*10` 截断 → 负数显示错误 | 单测 `-0.05/-0.15/25.06/99.95` |
| **R12** | `settings_tick` 的存储摘要**必须节流 5 s**（605–613，`esp_vfs_fat_info` 走 FATFS） | 拆 `as_page_main.cc` 时把节流丢了 → 每 500 ms 一次 `f_getfree`，界面卡顿 | 保留节流 + 注释；用日志/耗时验证 |
| **R13** | `label_set_if_changed` 的"读当前文本比较"策略（540–550） | 改成维护独立缓存 → 与别处直接 `lv_label_set_text` 不同步（注释 538 明确不要这么做） | 保留实现；回归：状态变化时文本确实更新 |
| **R14** | 主页摘要 label 指针 `s_sum[]` 与 `memset`（264/275）：子页控件**不**在 `s_sum` 里 | 拆文件后把子页 label 也塞进 `s_sum` 或反之 → 悬垂/漏清 | 明确"`s_sum` 只属于主页"；每个页面自己的指针在自己文件里清 |
| **R15** | 子页描述符必须静态存储期（402–414）：router 按值拷 `id/create` 指针 | 改成栈上临时结构体 → 悬垂跳转 | 保留 `static const subpage_desc_t SUB_*`；回归：每个子页进出两次 |
| **R16** | `make_row` 的"值 label 回传指针"（`lv_obj_t **status_out`）与页面级指针（`s_sum[...]`）耦合 | 拆控件工厂时改了返回约定 | 保持签名；回归：主页 5 行摘要都刷新 |
| **R17** | `radio_sw_sync()` 在 `s_radio_busy` 期间**故意不刷**（375–377）：开关视觉由 LVGL 自己翻转（乐观更新），服务失败时靠 tick 回滚 | 拆文件时把 `s_radio_busy` 判断删掉 → 与手指打架/状态抖动 | 保留；回归：切 Wi-Fi 时立刻看开关位置，失败后 500 ms 内回滚 |
| **R18** | 关于页 `about_fill_static`（102 行）里的 `esp_image_get_metadata` 等调用需在分区/镜像 API 上下文 | 拆文件时把它挪到后台任务 → ESP-IDF 这些 API 的线程约束 | 保持在 LVGL 任务（一次性构建，实测可接受）；只把 `about_refresh_live` 放定时器 |

---

## 8. 附加必答

### A. 五个页面的控件树与生命周期

| 页面 | 构建函数 | 拥有的 static 指针 | tick | on_delete 清理 | 备注 |
| --- | --- | --- | --- | --- | --- |
| 主页 设置 | `page_settings`(674–712) | `s_sum[SET_N]`、`s_sum_timer` | `settings_tick` 500 ms(708) | `settings_on_delete`(267)：删 tick + `memset(s_sum)` | 开关实例由 `s_radio_sw[]` 另行管理 |
| Wi-Fi | `page_wifi`(1002–1034) | `s_wifi_status` `s_wifi_list` `s_wifi_timer` `s_wifi_sig` `s_wifi_dirty` | `wifi_tick` 500 ms | `wifi_on_delete`(990)：删 tick、清 3 项（`s_wifi_dirty` 未清，靠 `s_wifi_sig=-1` 兜） | 顶部再放一个开关实例（1012） |
| 蓝牙 | `page_bt`(1224–1260) | `s_bt_status` `s_bt_list` `s_bt_timer` `s_bt_sig` `s_bt_dirty` | `bt_tick` 500 ms | `bt_on_delete`(1212) | 同上 |
| 声音与输出 | `page_output`(1321–1379) | `s_out_list` `s_out_pct` | 无 | `out_on_delete`(1314) | 音量滑块在 `VALUE_CHANGED` 里直接 `set_volume`(1371) |
| 存储 | `page_storage`(1612–1625) | `s_storage_body` `s_storage_timer` `s_storage_sd_gen` | `storage_sd_tick` | `storage_on_delete`(1602) | 代次变化即重建（热插拔） |
| 关于本机 | `page_about`(1990–2005) | `s_about_body` `s_about_timer` + 5 个值 label + `s_about_tsens*` | `about_tick` 1000 ms | `about_on_delete`(1977)：删 tick + `about_tsens_release()` + 清指针 | install 保留、enable/disable 成对 |

### B. Wi-Fi 密码浮层完整链路

```
on_wifi_ap_click(857)                     ← 行点击（SSID 是 row_user_dup 的副本）
 ├─ 已保存 → wifi_service_connect_saved(872) + Toast
 ├─ 开放   → wifi_service_connect(ssid,"",true)(877) + Toast
 └─ 需密码 → wifi_open_pw(ssid)(815)
       ├─ snprintf(s_pw_ssid, …, ssid)(821)        ← ★ 自己存一份（防列表重建悬垂）
       ├─ ui_ba_open_in(lv_screen_active(), …, LV_ALIGN_TOP_MID)(830)   ← 挂当前 screen
       ├─ ui_ba_on_close(…, pw_close)(833)
       ├─ ui_ba_text/input(密码模式, maxlen 63)/hint(835–843)
       ├─ ui_ba_btn("取消", on_pw_cancel) / ui_ba_btn("连接", on_pw_ready, s_pw_ssid)(845–846)
       └─ lv_keyboard_create(s_pw_overlay) + READY→on_pw_ready(s_pw_ssid) / CANCEL→on_pw_cancel(848–854)
on_pw_ready(788)：
   pass = lv_textarea_get_text(s_pw_ta)
   if (!pass[0] && wifi_service_is_saved(ssid)) → connect_saved(799)   ← 需求："留空确认 = 用已保存密码重连"
   else                                        → connect(ssid, pass, true)(802)
   → pw_close(806) → ui_ba_close（异步删）(775)
```

**所有权**：`s_pw_ssid`(763) 生命周期与浮层一致（进程级静态，无需 free）；
`row_user_dup` 的 SSID 副本由 `row_user_free` 在行 DELETE 时释放（905–907、939–941）。

### C. 开关与输出选择

- **开关**：`on_radio_switch`(297) →
  ① `lv_event_stop_bubbling`（不让行点击触发跳页）；
  ② `s_radio_busy` 重入保护；
  ③ **钉核**起 `radio_toggle_task`(318)，任务里调服务的 `set_enabled(!is_enabled())` 并清 busy；
  ④ 立即 Toast（"开启中…/已关闭（资源已释放）"）。
  视觉同步全部交给 `radio_sw_sync()`(373)，由三个 tick（`settings_tick`/`wifi_tick`/`bt_tick`）一起调，
  所以主页与子页的开关永远一致；**busy 期间不刷**（让服务给出结果再对齐，避免与手指打架）。
- **输出选择**：`out_rebuild`(1267) 读 `audio_service_get_output()`(1273) 画三态
  （静音 / 板载扬声器 / 蓝牙），点蓝牙时若 `!bt_service_is_connected()` 先 Toast 拦截(1295)；
  切换走 `audio_service_set_output(o)`(1293)。音量用 `lv_slider` + `s_out_pct` 百分比 label。

### D. 关于页

- `about_fill_static`(1874–1975)：`esp_chip_info`(1881)（型号/核心数/修订 + `esp_clk_cpu_freq`）、
  `esp_flash_get_size`(1903)、`esp_ota_get_running_partition`(1909) → 分区地址/大小、
  `esp_image_get_metadata`(1917) → 镜像实际长度、`about_fmt_mac`(1755) ×2（Wi-Fi/BT）、
  `esp_app_get_description` 版本与编译时间。
- `about_refresh_live`(1793–1848)：SRAM/PSRAM 可用、运行时长（`esp_timer_get_time`）、
  SD 容量（`storage_service_sd_is_mounted` 1835 + `bsp_sdcard_capacity_kb` 1837）、芯片温度
  （`about_tsens_update` 1710）。
- `about_tick`(1850) 1 s 刷一次实时项；`about_on_delete`(1977) 必须先 `about_tsens_release()`
  再清 label 指针。
- **温度传感器**：`install` 只在首次（失败置 `s_about_tsens_dead` 不再重试，1682–1694），
  `enable`/`disable` 与页面同生命周期（1696/1705）。**不与 Wi-Fi/蓝牙共用互斥资源**
  （独立外设），但 install 失败在 S31 上是真实可能（1676–1679 注释提到依赖
  `esp_hal_ana_conv/esp32s31/temperature_sensor_periph.c`）。

---

## 9. 拆分收益小结

- 2036 行 → 11 个文件（90~350 行），把 `about_fill_static`(102)、`settings_tick`(83)、
  `make_switch_row`(80)、`bt_rebuild_list`(76) 四个大函数分别归位。
- **状态与生命周期按页面收敛**：现在 5 个 tick + 6 组页面指针散在全文件，
  拆分后"建表/删表"在同一个文件里配对（R9 类风险从"靠记性"变成"靠模块边界"）。
- **~300 行纯逻辑可 host 单测**（单位格式化、MAC/芯片名、列表签名、输出文案映射、开关决策）。
- **跨任务面收敛到 `as_radio.cc` 一个文件**（`s_radio_busy` + 钉核任务），
  这是本文件唯一的并发点，也是最该加原子/同步的地方（R1、R3）。
