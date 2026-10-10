# 07 `bt_service.cc` 拆分重构蓝图

> 审计对象：`components/services/bt_service/bt_service.cc`
> 真实行数：**1395 行**（LF 计数，与 `read`/`grep` 行号一致；用户口头说"约 1198 行"不准，
> pwsh 的 `Get-Content | Measure-Object -Line` 也会少算）
> 头文件：`components/services/bt_service/include/bt_service.h`（113 行）
> 本报告为只读审计产物，未修改任何源文件。

---

## 1. 文件概览

### 1.1 include（18–49 行）

| 类别 | 内容 |
| --- | --- |
| 标准库 | `string.h` `stdio.h` `stdlib.h` |
| ESP-IDF | `esp_log.h` `esp_bt.h` `esp_bt_main.h` `esp_gap_bt_api.h` |
| ESP-GMF | `esp_bt_audio.h` `esp_bt_audio_host.h` `esp_bt_audio_classic.h` `esp_bt_audio_media.h` `esp_bt_audio_stream.h` |
| 编码/重采样 | `esp_audio_enc.h` `esp_audio_enc_default.h` `esp_audio_types.h`（SBC 编码器）、`esp_asrc.h`（软件 ASRC） |
| FreeRTOS / 其它 | `freertos/FreeRTOS.h` `task.h` `stream_buffer.h`、`esp_heap_caps.h`、`esp_timer.h`、`nvs.h` |

### 1.2 结构分区

| 区间 | 分区 |
| --- | --- |
| 1–181 | 文件头契约（分工/音频通路/bitpool 决策）+ 全部宏 + **全部静态状态**（含诊断计数） |
| 186–216 | 地址工具：`bt_service_addr_str` / `same_bda` / `dev_find` |
| 218–393 | **设备名 NVS 缓存**：key 编码、真名判定、枚举、FIFO 腾位、保存/反查、三级回退取名 |
| 395–443 | **设置持久化与开机自动连**：`bt_cfg/on`、`bt_cfg/last`、`bt_auto_connect_cb` |
| 445–657 | **编码器/重采样打开**：`bt_enc_close`、`bt_asrc_open`、`bt_enc_open`（含 bitpool 封顶与预算日志） |
| 659–834 | **馈送链路**：`bt_stage_append`、`bt_send_one_frame`（编码 + acquire/release + 统计）、`feed_task` |
| 836–883 | **上游入口**：`bt_service_write_pcm`（阻塞流控）、`bt_service_dropped_bytes` |
| 885–1020 | **蓝牙栈事件回调** `bt_audio_event_cb`（发现/连接/流状态/远端媒体键） |
| 1022–1128 | **初始化**：控制器 → PCM 缓冲 + feed 任务 → `esp_bt_audio_init` → 自动连定时器 |
| 1130–1208 | 总开关：`is_enabled` / `set_enabled` / `deinit` |
| 1210–1269 | 查询访问器 + `state_str` + 发现开关 + 设备表查询 |
| 1271–1395 | 连接/断开/媒体起停/配对表刷新/移除配对/媒体键回调注册 |

---

## 2. 全部函数清单（51 个顶层定义，其中 12 个是单行访问器）

| 起始 | 结束 | 行数 | 函数名 | static | 分类 | 一句话职责 |
| --- | --- | --- | --- | --- | --- | --- |
| 186 | 193 | 8 | `bt_service_addr_str` | ✘ | **对外**/纯逻辑 | 6 字节 BDA → `AA:BB:CC:DD:EE:FF` |
| 195 | 198 | 4 | `same_bda` | ✔ | 纯逻辑 | 6 字节 BDA 比较 |
| 200 | 216 | 17 | `dev_find` | ✔ | 设备表 | 按 BDA 在 `s_dev[]` 里找（`create` = 没有就新建一条） |
| 221 | 229 | 9 | `bt_name_key` | ✔ | **纯逻辑** | MAC → 12 位大写十六进制 NVS 键（NVS 键长上限 15，带冒号会 `KEY_TOO_LONG`） |
| 237 | 245 | 9 | `bt_name_is_real` | ✔ | **纯逻辑** | 判断"名字"是不是真名（等于 MAC 串 = 假名） |
| 254 | 287 | 34 | `bt_name_enum` | ✔ | NVS | 枚举 `bt_name` 命名空间里的字符串键（最多 cap 个，`more` 表示还有） |
| 290 | 326 | 37 | `bt_name_make_room` | ✔ | NVS | 缓存满（`BT_NAME_KEEP_MAX=16`）时**FIFO 淘汰最早写入**的一条，最多 8 轮 |
| 329 | 352 | 24 | `bt_name_save` | ✔ | NVS | 真名才存；值未变则跳过写 flash；先腾位再 `nvs_set_str` + `commit` |
| 355 | 374 | 20 | `bt_name_load` | ✔ | NVS | MAC → 名字反查（未命中返回空串） |
| 382 | 393 | 12 | `make_addr_18` | ✔ | 命名 | 三级回退：事件真名 → NVS 缓存 → 地址；同时填 `s_peer_addr` |
| 397 | 408 | 12 | `bt_settings_load` | ✔ | NVS | 读 `bt_cfg/on`（默认开）与 `bt_cfg/last`（6 字节 blob） |
| 410 | 419 | 10 | `bt_settings_save_enabled` | ✔ | NVS | 存开关 |
| 421 | 432 | 12 | `bt_settings_save_last` | ✔ | NVS | 存"上次连上的设备"并同步内存 |
| 435 | 443 | 9 | `bt_auto_connect_cb` | ✔ | **esp_timer 回调** | 开机 3 s 后自动连上次设备（已连/已关/无记录则跳过） |
| 470 | 489 | 20 | `bt_enc_close` | ✔ | 编码/重采样 | 关编码器与 ASRC，释放 4 块缓冲并清零尺寸变量 |
| 504 | 555 | 52 | `bt_asrc_open` | ✔ | 重采样 | 打开 48 k→协商采样率的**软件** ASRC（刻意不用硬件，494–502 注释），分配对齐缓冲 |
| 558 | 657 | **100** | `bt_enc_open` | ✔ | 编码 | 用协商 `codec_cfg` 开 SBC 编码器：**拷贝**配置、**bitpool 封顶 40**、查帧大小、分配帧/暂存缓冲、打印链路预算、必要时开 ASRC |
| 660 | 671 | 12 | `bt_stage_append` | ✔ | 馈送 | 把重采样后的 PCM 追加到暂存（超 `STAGE_BYTES` 计丢帧） |
| 674 | 760 | 87 | `bt_send_one_frame` | ✔ | 馈送 | 编码暂存里的一帧 → `acquire_write`（最多重试 20×2 ms）→ 拷贝 → `release_write(50 ms)`；含逐帧诊断与 1000 帧统计日志 |
| 762 | 834 | 73 | `feed_task` | ✔ | **自建任务** | 主循环：流未起则收编码器；暂存够一帧就发；否则取 PCM（无 ASRC 阻塞取 5 ms / 有 ASRC 1 ms 轮询整段）、需要时 ASRC → 暂存 |
| 836 | 881 | 46 | `bt_service_write_pcm` | ✘ | **对外** | 上游 PCM 入口：未推流则**按音频时长睡**并丢弃；否则 `xStreamBufferSend` **阻塞最多 200 ms**（流控核心，856–868 注释） |
| 883 | 883 | 1 | `bt_service_dropped_bytes` | ✘ | **对外** | 累计丢字节数 |
| 887 | 1020 | **134** | `bt_audio_event_cb` | ✔ | **栈回调** | 处理 4 类事件：发现状态、发现设备（名字只增不减 + 存 NVS）、连接状态（连接时取名/存 last/刷配对；断开清流状态）、流状态（STARTED 拿流句柄 + reset 缓冲 + 读协商参数；STOPPED/RELEASED 清）、远端媒体键（转发 `s_ctrl_cb`） |
| 1024 | 1128 | **105** | `bt_service_init` | ✘ | **对外** | 幂等初始化：读开关（关着直接跳过）→ 编译期 BR/EDR 护栏 → 控制器 init/enable → PCM StreamBuffer（PSRAM 优先）+ `feed_task` → `esp_bt_audio_init`（A2DP Source、发送任务钉 core 0/prio 5/4 KB）→ 刷配对表 → 起自动连定时器 |
| 1132 | 1140 | 9 | `bt_service_is_enabled` | ✘ | **对外** | 懒加载 NVS 后返回开关 |
| 1142 | 1164 | 23 | `bt_service_set_enabled` | ✘ | **对外** | 存 NVS；开 → 等 300 ms 再 `init()`（等 BTC 收尾，1151–1157 注释）；关 → `deinit()` + 等 100 ms |
| 1174 | 1208 | 35 | `bt_service_deinit` | ✘ | **对外** | 停自动连定时器 → 清状态 → `bt_enc_close` → 删 feed 任务 → 删 PCM 缓冲 → `esp_bt_audio_deinit` → 控制器 disable/deinit（**明示不调 `esp_bt_mem_release`**，1171–1172 注释） |
| 1210 | 1213 | 4 | `bt_service_is_ready` | ✘ | **对外** | 状态既不是 UNINIT 也不是 FAILED |
| 1215 | 1215 | 1 | `bt_service_state` | ✘ | **对外** | 返回 `s_state` |
| 1216 | 1216 | 1 | `bt_service_is_discovering` | ✘ | **对外** | `s_discovering` |
| 1217 | 1217 | 1 | `bt_service_is_connected` | ✘ | **对外** | `s_connected` |
| 1218 | 1218 | 1 | `bt_service_is_streaming` | ✘ | **对外** | `s_streaming` |
| 1219 | 1219 | 1 | `bt_service_stream_rate` | ✘ | **对外** | 协商采样率 |
| 1220 | 1220 | 1 | `bt_service_stream_channels` | ✘ | **对外** | 协商声道数 |
| 1221 | 1221 | 1 | `bt_service_peer_name` | ✘ | **对外** | `s_peer_name`（三级回退后的名字） |
| 1222 | 1222 | 1 | `bt_service_peer_addr` | ✘ | **对外** | `s_peer_addr` |
| 1223 | 1223 | 1 | `bt_service_peer_bda` | ✘ | **对外** | `s_peer_bda`（断开时用；未连接全 0） |
| 1225 | 1235 | 11 | `bt_service_state_str` | ✘ | **对外**/纯逻辑 | 状态 → 中文（"搜索中…/已连接/正在推流/蓝牙不可用/就绪"） |
| 1237 | 1254 | 18 | `bt_service_start_discovery` | ✘ | **对外** | 就绪才允许；`s_dev_count=0` 重新列表 → 调栈 → 置 `discovering/state` |
| 1256 | 1262 | 7 | `bt_service_stop_discovery` | ✘ | **对外** | 未在搜就直接 OK |
| 1264 | 1264 | 1 | `bt_service_dev_count` | ✘ | **对外** | 发现列表条数 |
| 1266 | 1269 | 4 | `bt_service_dev` | ✘ | **对外** | 按索引取发现设备（越界 NULL） |
| 1271 | 1288 | 18 | `bt_service_connect` | ✘ | **对外** | 取名（三级回退）→ `esp_bt_audio_classic_connect(A2DP_SRC)`（首次自动配对） |
| 1290 | 1298 | 9 | `bt_service_disconnect` | ✘ | **对外** | 断开 A2DP |
| 1300 | 1313 | 14 | `bt_service_media_start` | ✘ | **对外** | 已连接才启动 A2DP 源流 |
| 1315 | 1321 | 7 | `bt_service_media_stop` | ✘ | **对外** | 未连接直接 OK |
| 1323 | 1368 | 46 | `bt_service_refresh_bonded` | ✘ | **对外** | `esp_bt_gap_get_bond_device_list` → 填 `s_bonded[]`（名字三级回退 + 回填发现表的 `bonded` 位） |
| 1370 | 1370 | 1 | `bt_service_bonded_count` | ✘ | **对外** | 已配对条数 |
| 1372 | 1375 | 4 | `bt_service_bonded` | ✘ | **对外** | 按索引取已配对设备 |
| 1377 | 1389 | 13 | `bt_service_remove_bond` | ✘ | **对外** | 删配对 + 刷新表 |
| 1391 | 1395 | 5 | `bt_service_set_media_ctrl_cb` | ✘ | **对外** | 注册远端媒体键回调 |

**最大 3 个**：`bt_audio_event_cb`(134)、`bt_service_init`(105)、`bt_enc_open`(100)。

---

## 3. 全部全局 / 静态变量清单

### 3.1 诊断计数（`volatile`，热路径只加不锁）

| 行号 | 名字 | 类型 | 用途 |
| --- | --- | --- | --- |
| 126 | `s_acq_retry` | `volatile uint32_t` | `acquire_write` 拿不到缓冲的次数（链路/协议栈卡） |
| 127 | `s_acq_sleep_ms` | `volatile uint32_t` | 为 acquire 睡掉的总毫秒 |
| 128 | `s_rel_timeout` | `volatile uint32_t` | `release_write` 超时（丢帧）次数 |
| 129 | `s_enc_us` | `volatile uint64_t` | SBC 编码累计耗时 |
| 130 | `s_feed_idle` | `volatile uint32_t` | feed_task "没数据"空转次数 |
| 166/167 | `s_tx_frames` / `s_tx_bytes` | `uint32_t` | 已发编码帧/字节（统计窗口） |
| 178 | `s_asrc_err_logged` | `uint32_t` | ASRC 失败计数（每 200 次打一行） |
| 179 | `s_asrc_probe` | `uint32_t` | 前 5 次 ASRC "进/出"探测日志 |
| 180 | `s_tx_t0` | `int64_t` | 吞吐统计窗口起点（us） |
| 181 | `s_last_drop_log_ms` | `uint32_t` | 环形缓冲满的限频告警时刻 |

### 3.2 状态与开关

| 行号 | 名字 | 类型 | 用途 |
| --- | --- | --- | --- |
| 132–136 | `s_state` `s_inited` `s_discovering` `s_connected` `s_streaming` | `volatile` | 对外状态（**被 BT 回调任务改、被 LVGL 任务读**） |
| 138 | `s_enabled` | `volatile bool` | 总开关（NVS `bt_cfg/on` 记忆） |
| 139 | `s_settings_loaded` | `bool` | NVS 懒加载 latch |
| 140/141 | `s_last_bda[6]` / `s_has_last` | `uint8_t[]`/`bool` | 上次连上的设备（自动连用） |
| 142 | `s_auto_timer` | `esp_timer_handle_t` | 开机自动连的一次性定时器（3 s） |

### 3.3 设备表与对端信息

| 行号 | 名字 | 类型 | 用途 | 并发 |
| --- | --- | --- | --- | --- |
| 144–147 | `s_dev[BT_DEV_MAX=24]` / `s_dev_count` / `s_bonded[24]` / `s_bonded_count` | `bt_dev_t[]` + `volatile size_t` | 发现列表与配对列表 | **写：BT 回调任务 / LVGL 任务（`refresh_bonded`）**；读：LVGL 任务 → 条目本身非原子（见 R4） |
| 149–151 | `s_peer_name[32]` `s_peer_addr[18]` `s_peer_bda[6]` | char/uint8 | 对端名字/地址 | **两处写**：BT 回调（954）与 LVGL 任务（`bt_service_connect`→1279）；对外返回的是**内部指针**（app_settings 1121 直接用） |

### 3.4 音频通路状态

| 行号 | 名字 | 类型 | 用途 | 谁碰 |
| --- | --- | --- | --- | --- |
| 153 | `s_stream` | `esp_bt_audio_stream_handle_t` | 当前 A2DP 流句柄 | 写：BT 回调（975/965/998）；读：feed_task（766）→ **非 volatile**（R2） |
| 154 | `s_pcm` | `StreamBufferHandle_t` | PCM 环形缓冲（64 KB PSRAM，≈340 ms） | 写：init/deinit（LVGL 侧任务）；用：`write_pcm`（GMF 解码任务）+ feed_task |
| 155 | `s_feed_task` | `TaskHandle_t` | feed 任务句柄 | init/deinit |
| 156/157 | `s_rate` / `s_channels` | `volatile int` | 协商采样率/声道数 | BT 回调写、LVGL 读 |
| 158 | `s_dropped` | `volatile uint32_t` | 累计丢弃字节 | 多任务累加（非原子，统计用可接受） |
| 161–167 | `s_enc` `s_enc_in_bytes` `s_enc_out_bytes` `s_enc_frame` `s_enc_registered` `s_tx_*` | 混合 | SBC 编码器与缓冲 | **注释声明"只有 feed 任务碰"（160）**，但 `bt_enc_close` 也被 `bt_service_deinit`（1189，另一任务）调用 → R3 |
| 169–177 | `s_asrc` `s_asrc_in_smp` `s_asrc_out_smp` `s_asrc_in_bytes` `s_asrc_in` `s_asrc_out` `s_asrc_out_cap_samples` `s_stage` `s_stage_len` | 混合 | 软件 ASRC 与重采样暂存 | 同上（同 R3） |
| 183/184 | `s_ctrl_cb` / `s_ctrl_user` | 回调指针 | 远端媒体键转发 | 注册在 LVGL 任务、调用在 BT 回调任务 |

---

## 4. 外部依赖清单

### 4.1 依赖的外部 API

| 模块 | 用到的符号（行号） | 用途 |
| --- | --- | --- |
| 控制器 | `BT_CONTROLLER_INIT_CONFIG_DEFAULT`(1049)、`esp_bt_controller_init`(1051)、`esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)`(1058)、`esp_bt_controller_disable/deinit`(1204/1205) | 经典蓝牙控制器生命周期 |
| `esp_bt_audio` | `esp_bt_audio_host_bluedroid_cfg_t` + `ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT`(1082)、`esp_bt_audio_config_t`(1089)、`esp_bt_audio_init`(1100)、`esp_bt_audio_deinit`(1201) | Bluedroid + A2DP/AVRCP host |
| 经典蓝牙 API | `esp_bt_audio_classic_discovery_start/stop`(1246/1261)、`esp_bt_audio_classic_connect/disconnect`(1281/1297)、`esp_bt_audio_media_start/stop`(1308/1320) | 发现/连接/媒体 |
| 流 API | `esp_bt_audio_stream_get_codec_info`(561/985)、`esp_bt_audio_stream_acquire_write`(699)、`esp_bt_audio_stream_release_write`(707) | 取协商参数 + 发包 |
| GAP | `esp_bt_gap_get_bond_device_num`(1325)、`esp_bt_gap_get_bond_device_list`(1335)、`esp_bt_gap_remove_bond_device`(1384)、`ESP_BT_COD_MAJOR_DEV_*`(924–925) | 配对表与设备类别判定 |
| SBC 编码 | `esp_audio_enc_register_default`(573)、`esp_audio_enc_open`(602)、`esp_audio_enc_get_frame_size`(608)、`esp_audio_enc_process`(684)、`esp_audio_enc_close`(473)、`esp_sbc_enc_config_t`(587) | A2DP 帧级编码 |
| ASRC | `esp_asrc_open`(516)、`esp_asrc_get_bytes_per_sample`(520)、`esp_asrc_get_out_sample_num`(528)、`esp_asrc_process`(815)、`esp_asrc_close`(477)、`ESP_ASRC_PERF_TYPE_SW_SPEED`(512) | 48 k → 协商采样率（软件路径） |
| FreeRTOS | `xStreamBufferCreateWithCaps`(1067)/`xStreamBufferCreate`(1070)、`xStreamBufferSend`(869)、`xStreamBufferReceive`(792/810)、`xStreamBufferBytesAvailable`(725/805)、`xStreamBufferReset`(978)、`vStreamBufferDelete`(1196)、`xTaskCreate`(1078)、`vTaskDelete`(1192)、`vTaskDelay`(702/772/776/807/852/1158/1162) | 环形缓冲、feed 任务、节拍 |
| 内存 | `heap_caps_aligned_alloc(64, …, MALLOC_CAP_INTERNAL)`(538/539) + `malloc/free` 兜底 | ASRC 缓冲（内部 RAM 更稳） |
| NVS | `nvs_open/get_u8/set_u8/get_blob/set_blob/get_str/set_str/erase_key/commit/close`、`nvs_entry_find_in_handle`(261)、`nvs_entry_info`(267)、`nvs_entry_next`(281)、`nvs_release_iterator`(262/285) | `bt_cfg`（开关/上次设备）与 `bt_name`（名字缓存） |
| `esp_timer` | `esp_timer_create/start_once/stop`(1118/1124/1177)、`esp_timer_get_time`(683/687/720/872) | 自动连定时器 + 编码耗时/统计 |
| 日志 | `ESP_LOGx` | 诊断（含 `a2dp tx` / `asrc probe` / `a2dp budget`） |

### 4.2 对外导出 API（`include/bt_service.h`）

| 分组 | 符号 |
| --- | --- |
| 类型/宏 | `BT_DEV_MAX 24`、`BT_NAME_MAX 32`、`bt_dev_t{bda[6], name[32], rssi, is_audio, bonded}`、`bt_state_t{UNINIT,READY,DISCOVERING,CONNECTED,STREAMING,FAILED}`、`bt_media_ctrl_cb_t(int cmd, void *user)` |
| 生命周期 | `bt_service_init`(h:53)、`bt_service_is_ready`(h:54)、`bt_service_state`(h:55)、`bt_service_state_str`(h:56)、`bt_service_is_enabled`(h:68)、`bt_service_set_enabled`(h:69)、`bt_service_deinit`(h:70) |
| 发现 | `bt_service_start_discovery`(h:73)、`bt_service_stop_discovery`(h:74)、`bt_service_is_discovering`(h:75)、`bt_service_dev_count`(h:76)、`bt_service_dev`(h:77) |
| 连接 | `bt_service_connect`(h:80)、`bt_service_disconnect`(h:81)、`bt_service_is_connected`(h:82)、`bt_service_peer_name/addr/bda`(h:83–86) |
| 配对表 | `bt_service_refresh_bonded`(h:89)、`bt_service_bonded_count`(h:90)、`bt_service_bonded`(h:91)、`bt_service_remove_bond`(h:92) |
| 音频通路 | `bt_service_media_start/stop`(h:95–96)、`bt_service_is_streaming`(h:97)、`bt_service_stream_rate/channels`(h:98–99)、`bt_service_dropped_bytes`(h:100)、`bt_service_write_pcm`(h:104) |
| 其它 | `bt_service_set_media_ctrl_cb`(h:107)、`bt_service_addr_str`(h:109) |

**外部调用方**

| 调用方 | 位置 | 用途 |
| --- | --- | --- |
| `audio_service.cc` | 输出回调（`AUDIO_OUT_BLUETOOTH` 分支）→ `bt_service_write_pcm`；另用 `bt_service_is_streaming/stream_rate/stream_channels` | PCM 推流入口（**节拍来源**） |
| `app_settings.cc` | 291/384/586–593/1049–1204/1274–1275 | 开关、扫描/连接/断开/忘记、状态显示 |
| `app_music.cc`（媒体键） | 通过 `bt_service_set_media_ctrl_cb` 注册，处理 1..5 | 远端播放/暂停/上下曲 |
| 其它 | `bt_service_init()` 在开机后台任务里调（开关为开时） | — |

---

## 5. 线程 / 回调模型

| 上下文 | 干什么 | 同步 |
| --- | --- | --- |
| **BT 栈回调任务**（Bluedroid/BTC） | `bt_audio_event_cb`(887)：改 `s_state/s_connected/s_streaming/s_stream/s_rate/s_channels/s_discovering`、写 `s_dev[]`、**写 NVS**（`bt_name_save` 921、`bt_settings_save_last` 961）、调 `bt_service_refresh_bonded`(962)、转发远端媒体键(1013) | 无锁；与 LVGL/feed 任务共享上述状态（volatile 只覆盖部分） |
| **`bt_feed` 任务**（1078：`xTaskCreate(feed_task, "bt_feed", 5 KB, NULL, **prio 6**, …)`，**未钉核**） | 读 `s_stream/s_streaming/s_pcm`；开/关编码器与 ASRC；取 PCM、ASRC、编码、`acquire/release_write`；更新诊断计数 | 与 BT 回调共享 `s_stream`（非 volatile，R2）；与 deinit 共享编码器状态（R3） |
| **A2DP 源发送任务**（组件内部，1095–1097：**core 0**、prio 5、栈 4 KB） | 组件内部打包/发送 | 由 `acquire_write/release_write` 与 feed_task 交接 |
| **`esp_timer` 任务** | `bt_auto_connect_cb`(435)（3 s 一次性） | 只读状态 + 调 `bt_service_connect`（后者会写 `s_peer_*`） |
| **调用方任务**（`bt_service_write_pcm`） | GMF 解码任务的输出回调 → **阻塞最多 200 ms** 写环形缓冲 | 这是**有意的流控**（856–868） |
| **调用方任务**（`bt_service_init/set_enabled/deinit`） | app_settings 的 `radio_toggle_task`（**钉在 `CONFIG_BT_BLUEDROID_PINNED_TO_CORE`**，见 app_settings 318） | 与 feed_task 的编码器状态竞争（R3） |
| **LVGL 任务** | 所有 `bt_service_*` 查询、`bt_service_connect/disconnect/remove_bond/refresh_bonded`、`set_media_ctrl_cb` | 与 BT 回调/esp_timer 共享 `s_peer_*`、`s_bonded[]`、`s_dev[]`（R4/R5） |

**上游节拍（本文件最重要的设计）**：`bt_service_write_pcm` 用 `xStreamBufferSend(..., 200 ms)` **阻塞**，
把 A2DP 链路的实际吞吐"回压"给 GMF 解码管线；若改成"满了丢帧并立刻返回"，
解码会全速空转、把音乐抽稀成噪音（856–865 记录了实测 8 s 灌 1.5 MB 的现场）。

---

## 6. 拆分方案

### 6.1 目标目录树

```
components/services/bt_service/
├── CMakeLists.txt              # 只改 SRCS
├── include/
│   ├── bt_service.h            # 对外契约：一行不改
│   └── internal/
│       ├── bts_state.h         #   状态与查询访问器（唯一状态 owner）
│       ├── bts_name_cache.h    #   NVS 名字缓存（纯 key 编码 + NVS 读写分离）
│       ├── bts_audio.h         #   编码/重采样/馈送
│       └── bts_events.h        #   栈事件分发
└── src/
    ├── bt_service.cc           # 对外 API 薄封装 + init/deinit（~220 行）
    ├── bts_state.cc            # 状态机与访问器（~160 行）
    ├── bts_name_cache.cc       # 名字缓存：key 编码 + 真名判定 + FIFO 淘汰（~200 行）
    ├── bts_settings.cc         # bt_cfg NVS + 自动连定时器（~110 行）
    ├── bts_audio.cc            # bt_enc_open/asrc/stage/send_frame（~300 行）
    ├── bts_feed.cc             # feed_task + write_pcm 流控（~200 行）
    ├── bts_events.cc           # bt_audio_event_cb 四个分支 + 设备表维护（~250 行）
    └── bts_scan_pair.cc        # 发现/连接/断开/配对表（~200 行）
```

### 6.2 文件职责与接口

| 文件 | 搬走的行号 | 内部接口 | 预估行数 |
| --- | --- | --- | --- |
| `src/bt_service.cc` | 1022–1128、1130–1208、1210–1269 的对外转发 | 保持 `bt_service.h` 全部签名 | ~220 |
| `src/bts_state.cc` | 132–158、1215–1235 的访问器 | `bts_state_t` + `bts_set_state()`、`bts_state_snapshot(bts_snapshot_t*)`（**一次原子快照**，替代现在散落的 volatile 读） | ~160 |
| `src/bts_name_cache.cc` | 218–393 | `bts_name_key(bda, out)`、`bts_name_is_real(bda, name)`、`bts_name_save/load()`、`bts_make_peer_name(bda, hint_name, out, cap)`（**纯 key 编码与真名判定可 host 单测**） | ~200 |
| `src/bts_settings.cc` | 395–443 | `bts_settings_load()`、`bts_save_enabled(on)`、`bts_save_last(bda)`、`bts_auto_connect_start(cb)` | ~110 |
| `src/bts_audio.cc` | 445–671 | `bts_audio_close()`、`bts_enc_open(handle)`、`bts_asrc_open(rate)`、`bts_stage_append()` | ~300 |
| `src/bts_feed.cc` | 674–834、836–883 | `bts_feed_start()`、`bts_feed_stop()`、`bt_service_write_pcm()`、`bts_dropped_bytes()` | ~200 |
| `src/bts_events.cc` | 887–1020、200–216 | `bts_events_attach(cfg)`、`bts_handle_discovered(...)`、`bts_dev_find(...)` | ~250 |
| `src/bts_scan_pair.cc` | 1237–1395 的发现/连接/配对部分 | `bts_discovery_start/stop()`、`bts_connect/disconnect()`、`bts_refresh_bonded()` | ~200 |

### 6.3 纯逻辑（可 host 单测，不依赖蓝牙栈）

| 纯逻辑 | 行号 | 单测要点 |
| --- | --- | --- |
| `bt_name_key` | 221–229 | 全 0 / 全 FF / 大小写（必须大写） / 长度恰好 12 + NUL |
| `bt_name_is_real` | 237–245 | 名字 == 格式化地址 → false；空串/NULL → false；真名 → true |
| `bt_service_addr_str` | 186–193 | cap 不足时截断不越界 |
| `same_bda` | 195–198 | 6 字节比较 |
| `bt_service_state_str` | 1225–1235 | 6 个状态全覆盖 + 越界值回退 |
| 名字缓存**淘汰策略**（把 NVS 抽象成内存表） | 290–326 | 满 16 条淘汰最早、`keep_key` 已在表中时不淘汰、删不动时不死循环（322） |
| SBC 帧率/预算计算 | 634–643 | `per_frame = in_bytes/(ch*2)`、`fps = rate/per_frame`、kbps 计算 |
| ASRC 输入字节数 | 525 | `ASRC_IN_FRAMES * s_asrc_in_smp` |
| stage 追加与余量搬移 | 660–671、752–759 | 溢出计丢帧、余量 `memmove` 正确 |
| `write_pcm` 的时长换算 | 843–844 | `len*1000/(48000*2*2)` |

### 6.4 迁移顺序（每步可编译、播放行为不变）

1. 抽 `bts_state.cc`（状态 + 访问器）与 `bts_name_cache.cc`（218–393）——纯搬运，
   先给 `bt_name_key`/`is_real`/淘汰策略加 host 单测。
2. 抽 `bts_settings.cc`（395–443）。
3. 抽 `bts_audio.cc`（445–671）+ `bts_feed.cc`（674–883）：
   **同时**把"编码器状态只允许 feed 任务碰"这条注释变成真约束（见 R3 的修法）。
4. 抽 `bts_events.cc`（887–1020）+ `bts_scan_pair.cc`（1237–1395）。
5. `bt_service.cc` 只剩 init/deinit/set_enabled + 访问器转发。
6. 回归（用 AGENTS.md §11.5.6 的日志清单验收）：
   `a2dp budget: … f/s x … B = … kbps`、`a2dp tx: … f/s (need … f/s) … acq_retry … enc … us/frame … ring … B`、
   `asrc probe #n: 128 in -> 117~118 out frames`、`first a2dp frame sent`。

---

## 7. 风险清单

| # | 风险 | 触发场景 | 防护 / 验证 |
| --- | --- | --- | --- |
| **R1** | **`bt_service_write_pcm` 的 200 ms 阻塞是设计**（869）：改成"丢帧立刻返回"会让 GMF 全速解码 → 抽稀成噪音 | 拆 `bts_feed.cc` 时"优化"成非阻塞 | 保留注释 856–868 + 回归：听感 + `pcm ring full` 日志频率 |
| **R2** | `s_stream`(153) **不是 volatile**，写方是 BT 回调（965/975/998），读方是 feed_task(766) | 断开瞬间 feed_task 拿到已失效句柄 → `acquire_write` 打到旧流 | 改成"每次循环读一次句柄 + 用 `s_streaming` 双检"（现在已是双检但顺序在 766–767）；把 `s_stream` 也标 `volatile` 或用临界区 |
| **R3** | **`bt_enc_close()` 在 `bt_service_deinit` 里被另一个任务调用（1189），而它释放的正是 feed_task 正在用的 `s_enc_frame`/`s_stage`/`s_enc`**；feed 任务在 1191 才被删 | 推流中关蓝牙 → feed_task 正在 `esp_audio_enc_process`/`memcpy` → use-after-free | 拆 `bts_audio.cc`+`bts_feed.cc` 时改为"先置停止标志 → 等 feed_task 自行退出（或 `vTaskDelete` 后不再 close）→ 再由 feed 侧 close"；或给编码器状态加互斥 |
| **R4** | `s_dev[]`/`s_bonded[]`(144–147) 条目非原子：BT 回调写、LVGL 任务读（`app_settings` 遍历） | 扫描中新设备写入的同时 UI 正在读该条 | 提供 `bts_dev_snapshot(idx, bt_dev_t *out)`（拷贝一份）；`s_dev_count` 保持 volatile |
| **R5** | `s_peer_name/s_peer_addr/s_peer_bda`(149–151) 被 BT 回调（954）与 LVGL 任务（`bt_service_connect` 1279）**同时写**；对外返回内部指针（app_settings 1121 直接把它当 user_data 用） | 连接/断开频繁时读到半更新字符串；或指针被长期持有 | 写操作加临界区；对外改成 `bt_service_peer_bda_copy(uint8_t out[6])`；app_settings 侧改拷贝（见报告 06 的 R7） |
| **R6** | `s_dropped`(158) 被多任务非原子累加（666/685/708/817/871） | 统计数字偏差（不影响播放） | 若在意精确值改 `atomic_fetch_add`；否则保持 |
| **R7** | **bitpool 必须"拷贝后再改"**（594 `memcpy(&sbc, info.codec_cfg, sizeof(sbc))` + 596–598 封顶 40） | 拆 `bts_audio.cc` 时图省事直接改 `info.codec_cfg` → 污染协议栈协商结果 | 保留拷贝 + 注释 583–586；回归日志 `bitpool 53->40 (max 40)` |
| **R8** | ASRC 刻意用**软件**（512：`ESP_ASRC_PERF_TYPE_SW_SPEED`） | 改成硬件 ASRC → GDMA/cache 一致性没对齐时"先慢放、几秒后全是噪音"（494–502 记录） | 保留；要上硬件必须整体走官方 GMF 管线 |
| **R9** | 重采样失败只告警并继续（652–655）：会变调但能出声 | 拆文件时把"失败也继续"改成"失败停流" | 保留降级策略；回归：接 44.1 kHz 耳机听音高 |
| **R10** | `STAGE_BYTES=2048`(97) 的隐含假设"一次 ASRC 输出 < 一个编码帧"（665–668 静默计丢帧） | 协商到 16 block×8 subband 的大帧时溢出 | 加断言式日志（首次溢出打一行）；单测 ASRC 输出的最大帧数 |
| **R11** | `bt_name_save` 在 **BT 回调任务**里做 NVS（含 `nvs_entry_find` 迭代 + erase + commit）：闪存写会阻塞栈任务；同时 NVS 迭代器**看不到未 commit 的写入**（251–252） | 一次扫描发现十几台设备 → 回调里连续写 NVS | 保留"先腾位再写"的顺序（347–348）；必要时把名字攒起来丢给低优先级任务写 |
| **R12** | `bt_name_make_room` 的淘汰假设"NVS 迭代器按写入顺序返回"（310–313） | NVS 版本/布局变化导致淘汰对象不是最旧 | 注释已写明是实测结论；建议把"写入序号"也存进值里，做成确定性 LRU |
| **R13** | `dev_find(d->addr, true)`（907）在回调里**无上限**新建条目，`s_dev` 仅 `BT_DEV_MAX=24` | 一次扫描超过 24 台设备 | 检查 `dev_find` 的越界处理（200–216）；超限时应有明确日志 |
| **R14** | `bt_service_start_discovery` 会 `s_dev_count=0`（1245）**清空列表**，而 UI 可能正在遍历 | 用户连点扫描 | UI 侧按下标遍历（app_settings 1155–1170）；清空与读取之间没有同步 → 建议做成"新列表双缓冲，扫完原子替换" |
| **R15** | `bt_service_set_enabled` 的 300 ms / 100 ms 延时（1158/1162）是**必须**的：立刻重注册 SBC SEP 会 `ESP_ERR_INVALID_STATE` abort | 拆文件时把延时挪走或缩短 | 保留（与 app_settings 的"任务钉 BTC 核"配套，见报告 06 R3） |
| **R16** | **绝不能**调 `esp_bt_mem_release()`（1171–1172 注释） | 有人"顺手"加上去 → 蓝牙再也起不来 | grep 确认当前**没有**该调用；在 `bts_state.cc`/`bt_service.cc` 顶部保留警示注释 |
| **R17** | 编译期护栏 `#if !CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY` 只打错误日志（1043–1046），不阻止启动 | sdkconfig 被改回 BLE_ONLY → `esp_bluedroid_init` 卡死、"扫描没反应" | 保留日志；可升级成 `#error` 或运行时 `return ESP_ERR_INVALID_STATE` |
| **R18** | 统计计数在每次 1000 帧日志后被**清零**（736–740） | 拆文件后忘了清零/或误当累计值 | 明确语义"窗口计数"；`s_dropped` 才是累计值 |

---

## 8. 附加必答

### A. 音频馈送链路全景

```
GMF 解码任务 out 回调（audio_service，AUDIO_OUT_BLUETOOTH 分支）
 └─ bt_service_write_pcm(data, len)                      bt_service.cc:836
      ├─ 未推流（!s_pcm || !s_streaming）→ 按 dur_ms 睡 + 丢（846–854，防解码空转）
      └─ xStreamBufferSend(s_pcm, data, len, 200 ms)      :869   ← ★ 流控/节拍来源
            缓冲满 → 计入 s_dropped + 限频告警（870–878）
 s_pcm：64 KB PSRAM StreamBuffer（PCM_BUF_BYTES:55，≈340 ms@48k/16b/2ch；触发水位 512:58）
 └─ feed_task                                            :762（prio 6、栈 5 KB、未钉核 :1078）
      ├─ 流未起/已停 → bt_enc_close() + 睡 20 ms（767–774）
      ├─ 编码器未开 → bt_enc_open(h)（775–778）
      ├─ 暂存已够一帧 → bt_send_one_frame(h)（781–784）
      ├─ **无 ASRC**（对端 48 k）：xStreamBufferReceive(…, enc_in_bytes, 5 ms) 阻塞取（792–799）
      │     └─ got == 0 → s_feed_idle++（不再"看一眼就睡 5 ms"的速率地板）
      └─ **有 ASRC**：够 s_asrc_in_bytes 才取（805–809）→ esp_asrc_process（815）
            → 失败：计丢帧 + 每 200 次告警（817–823）；前 5 次打 probe 日志（827–831）
            → bt_stage_append(asrc_out, out_samples*out_smp)（832）
 bt_send_one_frame                                       :674
      ├─ esp_audio_enc_process（684）：一帧 PCM(enc_in_bytes) → SBC(enc_out_bytes)
      ├─ for retry<20：acquire_write（699）— 失败 → s_acq_retry++ / 睡 2 ms / 重试
      ├─ memcpy → pkt.size → release_write(h, &pkt, TX_WAIT_MS=50)（705–707）
      │     超时 → s_dropped += out.encoded_bytes, s_rel_timeout++（708–709）
      ├─ 成功 → s_tx_frames/s_tx_bytes++；第 1 帧打 "first a2dp frame sent"（713–716）
      └─ 每 1000 帧打窗口统计（717–746）：
         `a2dp tx: N frames, X f/s (need Y f/s), dropped Z B | acq_retry … enc … us/frame, idle …, ring … B`
         然后**清零窗口计数**（736–740）
      └─ 最后把用掉的 enc_in_bytes 从 stage 前部搬走（752–759）
 组件发送任务（A2DP source，core 0 / prio 5 / 4 KB，1095–1097）
      └─ 从 data_q 收"一包一帧" → esp_a2d_source_audio_data_send()
```

**缓冲深度与丢弃策略**：环形 64 KB；stage 2 KB（溢出一律计丢帧，静默）；
acquire 重试上限 20×2 ms；release 等待 50 ms；`write_pcm` 阻塞上限 200 ms。
`bt_service_dropped_bytes()`(883) 是**累计**口径（`s_dropped`，含 stage/ASRC/编码/release/环形溢出）。

### B. 状态与事件

| `bt_state_t` | 置位点 | 迁移条件 |
| --- | --- | --- |
| `BT_ST_UNINIT`(初始) | 132、1033、1180、1187 | `init()` 成功 → READY；失败 → FAILED |
| `BT_ST_READY` | 1108、897、966、1000 | discovery 开始 → DISCOVERING；连接成功 → CONNECTED；deinit → UNINIT |
| `BT_ST_DISCOVERING` | 900、1252 | 发现结束（`!discovering`）→ READY（896–898） |
| `BT_ST_CONNECTED` | 958 | 流 STARTED → STREAMING；断开 → READY（966） |
| `BT_ST_STREAMING` | 977 | STOPPED/RELEASED → `s_connected ? CONNECTED : READY`（1000） |
| `BT_ST_FAILED` | 1054、1061、1073、1103 | 只能靠重新 `init()`（`s_inited` 只在成功时置真，1109） |

`bt_audio_event_cb` 处理的 4 类事件（行号）：
1. `DISCOVERY_STATE_CHG`(891–903)：更新 `s_discovering`/`s_state`，结束时打设备数。
2. `DEVICE_DISCOVERED`(904–942)：`dev_find(create)` → **名字只增不减**（917–922：有名字才写 + 立刻 `bt_name_save`）
   → rssi/`is_audio`（CoD 主类，924–925）→ 回填 `bonded` → 空名字时先查 NVS（932–935）再退回地址（936–939）。
3. `CONNECTION_STATE_CHG`(943–970)：只处理 CLASSIC；连接时 `make_addr_18` + 同步名字缓存（954–956）、
   memcpy `s_peer_bda`、置 CONNECTED、**写 `bt_cfg/last`(961)**、刷配对表(962)；断开清 `s_streaming/s_stream`。
4. `STREAM_STATE_CHG`(971–1008)：STARTED 取句柄、`xStreamBufferReset`(978)、读协商参数（984–991）；
   STOPPED/RELEASED 清句柄与状态（995–1003）。
5. `MEDIA_CTRL_CMD`(1009–1016)：转发给 `s_ctrl_cb`（**在 BT 任务上下文，头文件 h:106 提醒别做重活/别碰 LVGL**）。

**开机自动连**：`init()` 末尾若 `s_has_last` 就建/复用 `s_auto_timer` 并 `esp_timer_start_once(3 s)`（1112–1126）；
回调 `bt_auto_connect_cb`(435) 校验 `s_enabled && !s_connected && s_has_last` 后 `bt_service_connect(s_last_bda)`。
**discovery 一致性**：`s_discovering` 只在事件回调（893）与 `start_discovery`（1251）写；
`stop_discovery`(1256) 依赖事件回来清标志——若栈不回事件，`s_discovering` 会一直为真（**已知不对称**，见风险 R14 的延伸）。

### C. NVS 名字缓存

| 项 | 事实（行号） |
| --- | --- |
| 命名空间 | `bt_name`（78） |
| key 编码 | `bt_name_key`(221–229)：6 字节 → **12 位大写十六进制**，无冒号；原因（79–83）：NVS 键上限 15 字符，`AA:BB:…` 有 17 字符会 `ESP_ERR_NVS_KEY_TOO_LONG` |
| 真名判定 | `bt_name_is_real`(237–245)：名字非空且 **≠ 格式化地址** |
| 容量 | `BT_NAME_KEEP_MAX = 16`（86） |
| 淘汰 | `bt_name_make_room`(290–326)：枚举 → 若 `keep_key` 已在（覆盖写）直接返回；有空位返回；否则删**第一个非 keep_key**（依赖"NVS 迭代器按写入顺序"，310–313），最多 8 轮，删不动就返回 |
| 写入 | `bt_name_save`(329–352)：非真名直接跳过；值未变跳过（343–346，省 flash）；**先腾位再 set + commit**（347–348） |
| 读取 | `bt_name_load`(355–374)；`make_addr_18`(382–393) 做三级回退；`refresh_bonded`(1355–1361) 与发现回调（932–938）都用到 |
| 单测可覆盖的纯函数 | `bt_name_key`、`bt_name_is_real`、淘汰策略（把 NVS 换成内存表） |

### D. enabled / deinit 语义核实

- `bt_service_set_enabled(false)` → `bt_service_deinit()`(1174)：
  停自动连定时器（1176–1178）→ 清 `s_streaming/s_connected/s_stream` 与状态（1184–1187）→
  `bt_enc_close()`(1189) → **删 feed 任务**(1191–1194) → 删 PCM 缓冲(1195–1198) →
  `esp_bt_audio_deinit()`(1201) → `esp_bt_controller_disable()` + `deinit()`(1204–1205)。
- **确认没有调用 `esp_bt_mem_release()`**（grep 全文件无该符号；1171–1172 的注释明确禁止）
  ⇒ 重新 `set_enabled(true)` 能再 init（1150–1159，含 300 ms 等待）。
- `set_enabled(true)` → 300 ms 延时 + `bt_service_init()`；`init()` 里 `s_enabled == false`
  会直接返回 `ESP_ERR_INVALID_STATE` 且不初始化（1030–1035）。
- 复现路径已验证：`s_inited` 仅在成功时置真（1109），失败可重试；`deinit` 对"从未 init"也幂等（1179–1182）。

---

## 9. 拆分收益小结

- 1395 行 → 8 个文件（110~300 行），把 `bt_audio_event_cb`(134)、`bt_service_init`(105)、
  `bt_enc_open`(100)、`bt_send_one_frame`(87) 四个大函数按域拆开。
- **编码/重采样/馈送（`bts_audio.cc`+`bts_feed.cc`）与协议栈事件（`bts_events.cc`）分离**，
  使"发不够帧率"的定位（`acq_retry` / `enc us/frame` / `ring B` 三个数分别属于哪一段）
  在代码结构上一眼可见。
- **~250 行纯逻辑可 host 单测**：NVS key 编码与真名判定、淘汰策略、状态串、预算/帧率计算、
  stage 追加与余量搬移、`write_pcm` 时长换算。
- 最该在拆分同时修掉的两个真问题：**R3（deinit 与 feed_task 抢编码器缓冲）** 与
  **R2/R5（`s_stream`/`s_peer_*` 跨任务裸共享）** —— 它们比"文件太长"更容易造成真机崩溃。
