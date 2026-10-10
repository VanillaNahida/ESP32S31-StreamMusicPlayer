# 01 —— `components/services/audio_service/` 只读审计报告

- 审计对象：`components/services/audio_service/`（全部 3 个文件）
- 审计方式：`read` 全文逐行核对（**不修改任何源文件**，本报告是唯一新建文件）
- 交叉核对：`managed_components/espressif__esp_audio_simple_player/`、`espressif__gmf_core/`、
  `espressif__gmf_io/`、`espressif__esp_codec_dev/`、`espressif__esp_board_manager/`、
  `libs/espaudiocore/{include/espaudiocore.h,API.md,README.md,src/}`、`sdkconfig`、
  `components/gen_bmgr_codes/gen_board_periph_config.c`、`components/bsp/bsp_audio.cc`、
  `components/services/{bt_service,media_meta,play_state}`、`components/apps/{app_music,app_online,app_settings}`、
  `components/ui_core/{ui_volume_panel,ui_control_center}`、`ESP-IDF v6.1` 的 `esp_driver_i2s/i2s_std.c`
- 结论口径：行号全部来自本次 `read`，**没有再引用任何"凭记忆"的行号**

> ⚠ **任务书里的行数与实际不符**：任务描述写"`audio_service.cc` 约 989 行"，
> `read` 全文确认**实际 1269 行**（`(End of file - total 1269 lines)`）。
> 本报告一律以 1269 行为准。

---

## 1. 文件清单与每个文件的职责

| # | 文件 | 行数 | 职责 |
| --- | --- | --- | --- |
| 1 | `components/services/audio_service/audio_service.cc` | **1269** | 播放服务**唯一**实现文件。包含：GMF/ASP 会话生命周期、out 回调与输出分流、事件回调与状态机、MUSIC_INFO 元数据兜底探测、HTTP IO 定制（4 MB 环形缓冲）、流缓冲进度/下载速度查询、时间轴与实测码率、seek 换算与落点核对、音量/静音/输出通道、对外 API 全部实现 |
| 2 | `components/services/audio_service/include/audio_service.h` | **165** | 对外契约（业务层只认它）：`audio_evt_t`(18-24)、`audio_event_cb_t`(26)、16 个 API 声明(29-161)、`audio_out_t`(91-95)、`audio_info_t`(116-122) |
| 3 | `components/services/audio_service/CMakeLists.txt` | **16** | 组件注册：`SRCS "audio_service.cc"`、`INCLUDE_DIRS "include"`、`REQUIRES bsp / esp_audio_simple_player / esp_codec_dev / gmf_core / media_meta / bt_service / gmf_io / mbedtls / play_state`；额外关掉 `-Wno-missing-field-initializers` |

**目录下没有第四个文件**（`glob components/services/audio_service/**/*` 只返回上述 3 个）。

职责之外的"隐形职责"（都写在这个 `.cc` 里，是拆分的主要动因）：

1. **两套并存的 IO 定制**：HTTP（生效，行 696-729）与本地 file（**已回退，只剩注释**，行 731-766），
   而 `#include "esp_gmf_io_file.h"`（行 24）现在**已无任何调用点**（死包含）。
2. **UI 支撑面**：`stream_progress` / `stream_speed` / `output_active` 三个函数是"给界面画缓冲转圈与速度"的，
   属于 UI 语义混在播放服务里。
3. **诊断面**：`audio_stats_log`（行 149-186）每 5 s 打一行播放健康度，是 2026-10 一系列卡顿定位的落点。

---

## 2. 完整函数清单（函数名 / 行号范围 / 职责 / 是否 static）

按文件内出现顺序，共 **36 个函数定义 + 2 个前置声明**。

| # | 函数 | 行号范围 | 一句话职责 | static |
| --- | --- | --- | --- | --- |
| — | `seek_bitrate(void)` 前置声明 | 124 | 为 `audio_stream_measure` 提前声明"本曲平均码率" | static |
| — | `stream_io_update(void)` 前置声明 | 128 | 为 `play_uri_locked` 提前声明"取管道在用的输入 IO" | static |
| 1 | `audio_stats_log` | 149-186 | 每 5 s 汇总一行播放健康度（PCM KB/s、IO 位置、codec 耗时、回调最大间隔） | ✅ |
| 2 | `timeline_now` | 190-193 | 当前播放时间轴 = `s_tl_base_ms + s_pcm_bytes/192` | ✅ |
| 3 | `audio_stream_measure` | 199-261 | 每写一块 PCM：累计时间轴、做 seek 落点核对、按 1 s 窗口采样实测码率（3:1 IIR + 两道合理性检查） | ✅ |
| 4 | `pcm16_scale` | 267-278 | 16-bit PCM 就地软件音量缩放（带 ±32767 削波保护），**只给蓝牙路用** | ✅ |
| 5 | `out_write_cb` | 287-340 | GMF 输出回调：置"已出声"标志 → 计时/统计 → 按 `s_out` 分流（蓝牙软件缩放 + `bt_service_write_pcm` / 扬声器 `esp_codec_dev_write`） | ✅ |
| 6 | `notify` | 344-349 | 把内部事件转给上层注册的 `audio_event_cb_t` | ✅ |
| 7 | `player_event_cb` | 351-452 | ASP 事件回调：MUSIC_INFO 填 `s_info`（含 media_meta 兜底探测）+ STATE 五态驱动 PA/静音/`s_running`/notify | ✅ |
| 8 | `audio_service_init` | 456-522 | 建锁、读 NVS 音量、`bsp_audio_init`+开 codec(48k/2ch/16bit)、建 ASP 实例（core 0 / prio 5 / 6 KB 栈）、注册事件回调 | ❌ 对外 |
| 9 | `play_uri_locked` | 530-586 | 内部统一起播入口：停旧曲 → 记录 `s_cur_path` → **清零 12 项跨曲状态** → 解析容器偏移 → `run(uri)` → `stream_io_update()` | ✅ |
| 10 | `audio_service_play_path` | 588-607 | 本地播放：`/sdcard/...` → `file:///sdcard/...`，持锁调 `play_uri_locked` | ❌ 对外 |
| 11 | `stream_evt_hook` | 643-668 | HTTP IO 事件钩子（只旁观、恒返回 0）：累加"想收字节"，读方向下 `FINISH_REQUEST` 永不触发 | ✅ |
| 12 | `stream_total_bytes` | 684-694 | 流总字节：优先 `esp_gmf_io_get_size()`，钩子的 `s_stream_total` 只当兜底 | ✅ |
| 13 | `ensure_stream_io` | 696-729 | 一次性把自定义 HTTP IO 注册进 ASP 池（4 MB PSRAM 环 + 8 KB 读 + 证书包 + 事件钩子） | ✅ |
| 14 | `stream_io_update` | 783-810 | 起播后取"管道真正在用的输入 IO"到 `s_stream_io`，并在**那个实例**上开 GMF 速度统计 | ✅ |
| 15 | `audio_service_stream_progress` | 812-845 | 在线缓冲进度（已下载/总长，0~100），算不出返回 false | ❌ 对外 |
| 16 | `audio_service_stream_speed` | 860-875 | 在线下载速率（**Kbps**，GMF 速度统计原样交出不换算） | ❌ 对外 |
| 17 | `seek_byte_for` | 893-916 | "时间 → 字节"：VBR 向前拖走相对映射，其余走绝对映射 | ✅ |
| 18 | `audio_service_output_active` | 918-921 | 是否已经真的出声（= `s_out_flowing`） | ❌ 对外 |
| 19 | `audio_service_play_uri` | 923-942 | URI 起播：本地路径转发 `play_path`，`http(s)://` 先 `ensure_stream_io` 再持锁起播 | ❌ 对外 |
| 20 | `audio_service_pause` | 944-951 | 暂停（薄封装 `esp_audio_simple_player_pause`） | ❌ 对外 |
| 21 | `audio_service_resume` | 953-964 | 恢复 + 主动补发 `AUDIO_EVT_RESUMED` | ❌ 对外 |
| 22 | `seek_bitrate` | 984-996 | 本曲平均码率：在线=总字节×8000÷时长（8 kbps~2 Mbps 合理性），否则用 `s_info.bitrate` | ✅ |
| 23 | `audio_service_set_duration_ms` | 998-1004 | 上层告知本曲时长（在线 seek 与 UI 总时长用） | ❌ 对外 |
| 24 | `audio_service_seek_supported` | 1006-1024 | 现在能不能拖；拒绝时打一行自诊断（`info=/io=/total=/dur=/avg=/meas=/gmf_br=`） | ❌ 对外 |
| 25 | `audio_service_seek_landing_ms` | 1026-1036 | 取"上一次 seek 实际落到哪"（**一次性**，读到即清标志） | ❌ 对外 |
| 26 | `audio_service_seek_ms` | 1054-1126 | 拖动定位：时间→字节 → 必要时 pause → `esp_gmf_pipeline_seek(byte)` → resume → 清出声标志 → 重设时间轴基准 + 挂落点核对 | ❌ 对外 |
| 27 | `audio_service_toggle` | 1128-1142 | RUNNING↔PAUSED 切换，其它状态返回 `ESP_ERR_INVALID_STATE` | ❌ 对外 |
| 28 | `audio_service_stop` | 1144-1154 | 停止 + 静音 + 关 PA + `s_running=false` | ❌ 对外 |
| 29 | `audio_service_is_playing` | 1156-1164 | `get_state()==RUNNING`（**不含 PAUSED**） | ❌ 对外 |
| 30 | `audio_service_set_output` | 1168-1210 | 三路输出切换（静音/扬声器/蓝牙）+ 防爆音时序 + BT 流的起停 | ❌ 对外 |
| 31 | `audio_service_get_output` | 1212-1215 | 读当前输出通道 | ❌ 对外 |
| 32 | `audio_service_set_volume` | 1217-1244 | 音量 0~100：写 ES8389 **寄存器** + 0/非 0 自动静音 + 唯一落盘点（`play_state_volume_set`） | ❌ 对外 |
| 33 | `audio_service_get_volume` | 1246-1249 | 读 `s_volume` | ❌ 对外 |
| 34 | `audio_service_set_mute` | 1251-1254 | 静音（直接转 `bsp_audio_set_mute`） | ❌ 对外 |
| 35 | `audio_service_set_event_cb` | 1256-1260 | 注册上层事件回调 | ❌ 对外 |
| 36 | `audio_service_get_info` | 1262-1269 | 取当前曲目流信息（`s_info_valid` 为假则 false） | ❌ 对外 |

统计：**static 14 个**（2 个前置声明不计入），**对外 22 个**（这 22 个全部在 `audio_service.h` 里有声明，
已逐个核对：h 的 29/36/44/46/47/48/49/50/54/65/74/87/103/104/107/108/109/112/124/138/152/161 行）。
`player_event_cb` / `out_write_cb` 是 GMF 回调（static + 函数指针），**其余 static 都是内部工具函数**。

---

## 3. 完整全局变量清单（变量名 / 行号 / 类型 / 用途）

### 3.1 文件作用域静态变量（31 个 + TAG）

| # | 变量 | 行号 | 类型 | 用途 / 说明 |
| --- | --- | --- | --- | --- |
| 0 | `TAG` | 34 | `const char *` | 日志标签 `"audio_svc"` |
| 1 | `s_player` | 41 | `esp_asp_handle_t` | ASP 播放器实例；非 NULL 即"服务已初始化"（行 458 用它做幂等判断） |
| 2 | `s_lock` | 42 | `SemaphoreHandle_t` | 起播互斥（只在 `play_path` 603 / `play_uri` 938 取，保护 `play_uri_locked`） |
| 3 | `s_evt_cb` | 43 | `audio_event_cb_t` | 上层事件回调 |
| 4 | `s_evt_user` | 44 | `void *` | 上层回调 user |
| 5 | `s_running` | 45 | `volatile bool` | **状态机标志**：RUNNING 事件置真(426)、FINISHED/STOPPED/ERROR 置假(434/439/443)、`stop()` 也清(1152)；`set_output` 用它决定要不要重开 PA(1185) |
| 6 | `s_volume` | 47 | `int` | **跨任务共享状态**：LVGL 任务写(1222)、out 回调读(321)；开机取 NVS(469)，默认 `PLAY_STATE_VOL_DEFAULT` |
| 7 | `s_info` | 48 | `audio_info_t` | 当前曲目流信息（采样率/码率/声道/位深/时长），MUSIC_INFO 填(362-411) |
| 8 | `s_info_valid` | 49 | `volatile bool` | MUSIC_INFO 已到；换曲清零(560)、seek 前置条件(1059/1011) |
| 9 | `s_data_offset` | 50 | `size_t` | 音频数据起始偏移（MP3 = ID3v2 长度），seek 绝对映射用(911)、落点核对扣它(216) |
| 10 | `s_out` | 51 | `audio_out_t` | **输出通道路由标志**：默认 SPEAKER；out 回调据此分流(313)，`set_output` 写(1172/1182/1204) |
| 11 | `s_cur_path` | 54 | `char[600]`（`AUDIO_PATH_MAX`=53） | 本曲路径/URI；MUSIC_INFO 时用它做 media_meta 兜底探测(382-392) |
| 12 | `s_out_flowing` | 57 | `volatile bool` | **"已经真的出声"标志**：out 回调置真(291)、换曲清(545)、seek 清(1095)；UI 判缓冲结束用(918-921) |
| 13 | `s_stream_total` | 65 | `volatile uint32_t` | HTTP Content-Length（钩子填）——**读方向恒为 0**，只当兜底(686) |
| 14 | `s_stream_got` | 66 | `volatile uint32_t` | ON_RESPONSE 累加的"想收字节"（第二兜底，836-840） |
| 15 | `s_stream_io` | 67 | `esp_gmf_io_handle_t` | **管道真正在用的输入 IO**（不是注册进池子的模板）：总长(689)/位置(172,214,238,833,898)/速度(866)全问它 |
| 16 | `s_dur_hint_ms` | 75 | `volatile uint32_t` | 上层告知的本曲时长；换曲清零(548)；用于在线平均码率(987-993) |
| 17 | `s_pcm_bytes` | 92 | `volatile uint64_t` | **时间轴基准量**：自基准点以来写出的 PCM 字节；192 B/ms 换算(192) |
| 18 | `s_tl_base_ms` | 93 | `volatile uint32_t` | 时间轴基准（换曲 0、seek 后=目标时刻）(551/1115) |
| 19 | `s_stats_calls` | 94 | `volatile uint32_t` | PCM 写入次数（健康度日志）(205) |
| 20 | `s_stat_codec_us` | 96 | `volatile uint64_t` | 窗口内写 codec/蓝牙累计耗时(330/338)，日志后清零(181) |
| 21 | `s_stat_last_ms` | 97 | `volatile uint32_t` | 上次 out 回调时刻，用于算块间隔(306-312) |
| 22 | `s_stat_gap_max_ms` | 98 | `volatile uint32_t` | 两次回调最大间隔（"被饿住"判据）(308-309/182) |
| 23 | `s_meas_bps` | 117 | `uint32_t`（**非 volatile，跨任务读写**） | 实测平均码率；seek 相对映射用(901-906)、合理性检查用(251-255) |
| 24 | `s_meas_anchor_ms` | 118 | `uint32_t` | 上一个采样点的时间轴位置(258) |
| 25 | `s_meas_anchor_pos` | 119 | `uint64_t` | 上一个采样点的 IO（解码器）位置(259) |
| 26 | `s_meas_anchor_ok` | 120 | `bool` | false = 下个采样点只锚定不算码率（起播抢读期 231、seek 后 1116） |
| 27 | `s_seek_req_ms` | 131 | `volatile uint32_t` | 用户请求的定位时刻（落点核对基准）(1117/221) |
| 28 | `s_seek_req_bps` | 132 | `volatile uint32_t` | 换算落点用的码率（必须与映射同一个）(1118/217) |
| 29 | `s_seek_landing_pending` | 133 | `volatile bool` | 等"seek 之后第一块 PCM"(1120 → 211) |
| 30 | `s_seek_landing_ready` | 134 | `volatile bool` | 落点已算好、等上层取走(218/1031) |
| 31 | `s_seek_landing_ms` | 135 | `volatile uint32_t` | 按字节位置反推的真实落点(217/1033) |

### 3.2 函数内 `static`（**拆分时必须升级成模块级状态**）

| 变量 | 行号 | 类型 | 用途 |
| --- | --- | --- | --- |
| `last_ms` | 151 | `static uint32_t` | `audio_stats_log` 上次打印时刻 |
| `last_pcm` | 152 | `static uint64_t` | 上次打印时的 `s_pcm_bytes` |
| `last_calls` | 153 | `static uint32_t` | 上次打印时的 `s_stats_calls` |
| `done` | 698 | `static bool` | `ensure_stream_io` 一次性开关（"自定义 HTTP IO 只注册一次"） |

### 3.3 关键宏常量（跨模块隐式契约）

| 常量 | 行号 | 值 | 被谁用 |
| --- | --- | --- | --- |
| `OUT_RATE_HZ` / `OUT_CHANNELS` / `OUT_BITS` | 37/38/39 | 48000 / 2 / 16 | init(478/520)、日志 |
| `AUDIO_PATH_MAX` | 53 | 600 | `s_cur_path`、`play_path` 的 uri 缓冲(596) |
| `OUT_PCM_BYTES_PER_MS` | 84 | 192 | `timeline_now`(192)、日志 audio 时长(180)、**app 层看门狗/进度也靠它** |
| `SEEK_MEAS_WINDOW_MS` | 113 | 1000 | 实测码率采样窗口(234) |
| `SEEK_MEAS_WARMUP_MS` | 114 | 2000 | 跳过起播抢读期(230) |
| `SEEK_REL_MIN_MS` | 115 | 2000 | 小于它的向前拖动不值得走相对映射(901) |
| `STREAM_RING_BYTES` | 632 | 4 MB | 自定义 HTTP IO 环形缓冲(709) |

### 3.4 审计发现（状态管理层面）

1. **并发可见性不完整**：`s_out`(51)、`s_volume`(47)、`s_info`(48)、`s_meas_bps`(117)、
   `s_meas_anchor_ms/pos/ok`(118-120) 都是**非 volatile**，却分别在 LVGL 任务与 GMF 解码任务
   之间读写（`s_meas_*` 在 out 回调写、在 `seek_byte_for` 读，`s_out`/`s_volume` 反向）。
   RISC-V 上 32 位读写本身原子，所以现实表现只是"偶发读到旧值"，但这是**跨模块拆分时最容易
   被改成真 bug 的地方**——一旦拆成不同编译单元 + LTO 边界变化，可见性假设就变了。
2. **单一标志承担多种语义**：`s_running`(45) 既表示管线在跑、又被 `set_output` 用来决定是否重开 PA(1185)；
   `s_out_flowing`(57) 既表示"出声了"、又被 seek 复用为"重新缓冲中"(1095)。
3. **换曲清零分散在一处但覆盖 5 个模块的语义**（543-557 + 560 + 563 + 568）：共 12 个变量，
   任何新增状态忘了加进去，就会变成"上一首的脏数据带进下一首"（seek 采样被带偏就是这类）。
4. `s_data_offset` 在**每次** MUSIC_INFO 都会被 `media_meta_audio_data_offset()` 之外的路径影响：
   实际只在 `play_uri_locked`(563) 赋值，但落点核对(216) 与绝对映射(911) 都读它 —— 在线流恒 0（已知误差源）。

---

## 4. 所有用到的 ESP-GMF / esp_audio_simple_player API 清单

格式：**API —— 出现行号 —— 用它做什么 —— 换成自研解码库后会失去什么**。

### 4.1 `esp_audio_simple_player`（门面，含 `_advance`）

| # | API / 符号 | 行号 | 用它做什么 | 换自研后失去什么 |
| --- | --- | --- | --- | --- |
| 1 | `esp_asp_cfg_t`（`in.cb`/`out.cb`/`out.user_ctx`） | 487-491 | 配置"输出回调 + 回调 ctx（= `bsp_audio_codec()`）" | 失去"解码→输出回调"这条**通用注入点**；自研库要自己定义等价回调（espaudiocore 只有 I2S/ringbuf 两种输出） |
| 2 | `cfg.task_prio` / `task_stack` / `task_core` | 492-504 | 播放管线独占 **core 0**、prio 5、6 KB 栈（core 1 留给 LVGL） | 失去"整条解码链单任务 + 指定核"的确定性调度；espaudiocore 有 `task_core/priority/stack`（默认 core 1，**必须改 0**，否则与 LVGL 抢核） |
| 3 | `esp_audio_simple_player_new` | 506 | 建实例（内部建 pipeline 任务、注册默认解码器与 IO 池） | 失去"解码器/IO 池 + 管线自动装配"；自研库要自己装配解码器、端口、任务 |
| 4 | `esp_audio_simple_player_set_event` | 513 | 注册 `player_event_cb`（状态 + MUSIC_INFO 的唯一来源） | 失去 MUSIC_INFO 事件（采样率/位深/声道/码率）；espaudiocore 只有 `get_format()`（无码率）与 `on_meta`（标签/tlen），**码率要自己算** |
| 5 | `esp_audio_simple_player_run` | 570 | 按 URI 起播（内部按 scheme 选 IO、按 URI 配解码器） | 失去"URI → IO/解码器"路由；自研库无 URI 概念，**HTTP 必须自研** |
| 6 | `esp_audio_simple_player_stop` | 536（换曲）、1149（对外） | 停旧管线（换曲前必须，否则复用旧管线会崩） | 失去"停到干净状态"的语义；espaudiocore `stop()` 阻塞至任务退出，**语义更强**（可直接替换） |
| 7 | `esp_audio_simple_player_pause` | 949 | 暂停（seek 前也用它，满足 pipeline 状态要求 1072） | 失去"暂停即停喂"的中间态；espaudiocore `pause()` 等价（I2S 模式有 DMA 余音） |
| 8 | `esp_audio_simple_player_resume` | 958 | 恢复（seek 后也调 1081/1099） | 同上，espaudiocore `resume()` 可直接替换 |
| 9 | `esp_audio_simple_player_get_state` | 534, 1069, 1134, 1162 | 换曲前判状态 / seek 前判是否 RUNNING / toggle / is_playing | 失去"RUNNING/PAUSED/STOPPED/FINISHED/ERROR"五态查询；espaudiocore 只有 `is_running()`（**含暂停态**）→ 状态机要自己按事件维护 |
| 10 | `esp_audio_simple_player_get_pipeline`（advance） | 790, 1078 | 取 pipeline：一个为拿输入 IO(794)、一个为 seek(1097) | 失去"拿到管线去 seek"这条路；espaudiocore 直接给 `seek_ms(ms)`，**反而更简单** |
| 11 | `esp_audio_simple_player_register_io`（advance） | 717 | 把我们自建的 HTTP IO 登记进 ASP 池（覆盖默认 20 KB 缓冲那份） | 失去 IO 池覆盖机制；自研库需要我们自己建/管环形缓冲（ringbuf 输入模式） |
| 12 | `esp_asp_event_pkt_t` / `esp_asp_music_info_t` / `esp_asp_state_t` | 351 / 359 / 420 | 事件包、曲目信息、状态枚举 | 对应 `espaudiocore_event_t` / `espaudiocore_meta_t` / `is_running()`；**信息量不对等**（无码率、无 MUSIC_INFO） |
| 13 | `ESP_ASP_EVENT_TYPE_MUSIC_INFO` | 358 | 拿采样率/位深/声道/码率，并在此做 media_meta 兜底探测(382-407) | 失去"曲目参数一次性到达"的时机；espaudiocore 要轮询 `get_format()` + `get_duration_ms()`，码率另算 |
| 14 | `ESP_ASP_EVENT_TYPE_STATE` | 416 | 五态分发：RUNNING→开 PA+STARTED(425-429)、PAUSED(430)、FINISHED→关 PA+FINISHED(433-437)、STOPPED(438)、ERROR→ERROR 事件(442-447) | 对应 `ESPAUDIOCORE_EVT_{STARTED,PAUSED,RESUMED,SEEKED,EOS,STOPPED,ERROR}`；**语义更全**（多了 SEEKED），但没有"五态查询" |
| 15 | `ESP_ASP_STATE_{NONE,RUNNING,PAUSED,STOPPED,FINISHED,ERROR}` | 420/425/430/433/438/442 等 | 见上 | 见上 |
| 16 | `esp_gmf_err_t` / `ESP_GMF_ERR_OK` | 506-517, 571, 689, 713, 717, 790, 794, 806, 833, 866, 898, 950, 959, 1078-1101, 1149 | 统一返回码 | 换成 `esp_err_t`（IDF 语义）——espaudiocore 用的就是 IDF 码，`esp_err_to_name()` 直接可用 |

### 4.2 ESP-GMF core（pipeline / IO 基类）

| # | API / 符号 | 行号 | 用它做什么 | 换自研后失去什么 |
| --- | --- | --- | --- | --- |
| 17 | `esp_gmf_pipeline_handle_t` | 789, 1077 | pipeline 句柄 | 无对应；自研库内部自己持有 |
| 18 | `esp_gmf_pipeline_get_in` | 794 | **取"管道真正在用的输入 IO"**（无锁赋值）——这是 `pos/size/speed` 的唯一正确来源 | 失去"IO 位置/总长/速度"这条统一的观测口；换自研后要自己维护下载计数与文件偏移 |
| 19 | `esp_gmf_pipeline_seek(pipe, byte)` | 1097 | 按**字节**定位；要求 PAUSED/STOPPED/FINISHED（`esp_gmf_pipeline.c:584-601`），内部走 `esp_gmf_io_seek()`（`esp_gmf_io.c:396-440`，会**阻塞等 IO 任务完成**，并先试 `seek_in_cache`） | 失去"字节级 seek + 缓存内瞬移 + HTTP Range 重开"三件套；espaudiocore 提供的是**毫秒级 seek**（内部解码器复位），本地文件反而更强，**在线 ringbuf 输入则完全不支持 seek** |
| 20 | `esp_gmf_io_handle_t` | 67, 712, 793 | IO 句柄类型 | 无对应 |
| 21 | `esp_gmf_io_get_pos(io, &pos)` | 172（健康度日志）、214（落点核对）、238（码率采样）、833（缓冲进度兜底）、898（相对映射起点） | 位置口径 = **消费者已 release 的字节**（`release_read` 推进），即"解码器吃到哪" | 失去"解码器消费进度"这个量；espaudiocore 给的是 `get_position_ms()`（时间），要字节得自己按码率折 |
| 22 | `esp_gmf_io_get_size(io, &size)` | 689 | 流总长（= HTTP Content-Length，`_http_open` 里 `set_size(info.size)`） | 失去"总长"；自研 HTTP 要自己解析 Content-Length |
| 23 | `esp_gmf_io_get_speed_stats(io, &st)` | 866 | 下载速度（`current_speed_kbps`/`average_speed_kbps`，**Kbps**，IO 任务 1:9 平滑） | 失去"网络速率统计"；自研 HTTP 要自己在读循环里做滑动窗口 |
| 24 | `esp_gmf_io_speed_stats_t` | 865 | 速度结构体 | 同上 |
| 25 | `esp_gmf_io_enable_speed_monitor(io, true)` | 806 | 在**管道在用的那个 IO 实例**上开速度统计（模板上开无效） | 失去"打开统计"的开关；自研方案里统计是我们自己的代码 |
| 26 | `ESP_GMF_ERR_OK` / `ESP_GMF_ERR_*` | 见 #16 | 返回码 | 见 #16 |

### 4.3 GMF HTTP IO（`esp_gmf_io_http.h`）

| # | API / 符号 | 行号 | 用它做什么 | 换自研后失去什么 |
| --- | --- | --- | --- | --- |
| 27 | `http_io_cfg_t` + `HTTP_STREAM_CFG_DEFAULT()` | 704 | 以默认配置为底改我们的参数 | 失去整套 HTTP reader（含 TLS、Range、分块、重连续传） |
| 28 | `cfg.dir = ESP_GMF_IO_DIR_READER` | 705 | 读方向（诊断里"FINISH_REQUEST 只对 writer 派发"就是它决定的） | 见 #31 |
| 29 | `cfg.crt_bundle_attach = esp_crt_bundle_attach` | 706（配合 `esp_crt_bundle.h` 行 27） | 音频流走 **https 也要证书校验** | 自研要自己挂 crt_bundle / 校时（SNTP 已由 wifi_service 负责） |
| 30 | `cfg.io_cfg.buffer_cfg.io_size=8 KB / buffer_size=4 MB / thread.stack_in_ext=true` | 708-710 | 每次读 8 KB、**4 MB 环形缓冲（PSRAM）**、IO 任务栈放外部 RAM | 失去"深缓冲抗抖动"能力 → 必须自己在 ringbuf 输入模式里重建（4 MB 放 PSRAM 可行，但"预读满就停读网络"的策略要自己写） |
| 31 | `cfg.event_handle = stream_evt_hook` + `http_stream_event_msg_t` + `HTTP_STREAM_FINISH_REQUEST`(649) / `HTTP_STREAM_ON_RESPONSE`(659) | 643-668, 707 | 旁观响应：读 Content-Length（**读方向根本不触发**）、累加"想收字节" | 失去"数据到达事件"；自研 HTTP 天然在读循环里，反而更好统计 |
| 32 | `esp_gmf_io_http_init(&cfg, &io)` | 713 | 建自定义 HTTP IO | 失去 GMF 自带的重连（`HTTP_MAX_CONNECT_TIMES=5` 连续失败才放弃）、Range 续传、`Server ignored Range` 兜底 |
| 33 | `esp_gmf_io_file.h`（`#include`） | 24 | **只被注释引用**（预读缓冲方案已回退 731-766），无任何调用点 | 无（死包含，可直接删） |

### 4.4 非 GMF 但与本模块强耦合的 API（同样要跟着改）

| # | API | 行号 | 用途 |
| --- | --- | --- | --- |
| 34 | `esp_codec_dev_write`（+ `esp_codec_dev_handle_t`） | 333, 337 | 扬声器路把 PCM 写进 ES8389（**唯一真正出声的调用**） |
| 35 | `bsp_audio_init` / `bsp_audio_open_output` / `bsp_audio_codec` | 471, 478, 491 | 打开 48k/2ch/16bit 输出并取 codec 句柄当 out ctx |
| 36 | `bsp_audio_set_volume` / `_set_mute` / `_pa_enable` | 482,483,484,564,565,573,574,1150,1151,1173,1174,1183,1184,1186,1199,1200,1224,1231,1233,1253 | 音量/静音/功放时序（PA 只在本文件被开关） |
| 37 | `bt_service_write_pcm` | 329 | 蓝牙路推 PCM（非阻塞，满了丢帧；**必须返回全长**） |
| 38 | `bt_service_is_connected` / `_is_streaming` / `_media_start` / `_media_stop` / `_peer_name` | 1191,1201,1202,1180,1205 | 蓝牙路切换与状态 |
| 39 | `media_meta_probe_audio` | 392 | MUSIC_INFO 里用**自研探测**覆盖 GMF 的码率/时长/采样率（FLAC/WAV/ADTS 全靠它） |
| 40 | `media_meta_audio_data_offset` | 563 | 容器头部偏移（seek 绝对映射用） |
| 41 | `play_state_volume_get` / `_set` | 469, 1241 | 音量 NVS 记忆（读在 init 之前、写在唯一音量入口） |
| 42 | `esp_timer_get_time` | 155, 305, 330, 338 | 毫秒/微秒级计时（健康度、codec 耗时） |

### 4.5 换自研解码库后的"能力净损失"汇总（第 4 节的结论）

| 能力 | 现在靠什么 | 自研后 |
| --- | --- | --- |
| HTTP/HTTPS 传输 + Range 续传 + 重连 | `esp_gmf_io_http` | **必须自研**（最大缺口） |
| 4 MB 深缓冲 | `buffer_cfg.buffer_size` | 自研 ringbuf（可保留 4 MB/PSRAM） |
| 解码器自动路由（URI → 解码器） | ASP 池 | 自研按扩展名/魔数分派 |
| 重采样 44.1k→48k | GMF `aud_rate_cvt` | **必须自研**（或改输出时钟） |
| 声道/位宽转换 | GMF `aud_ch_cvt`/`aud_bit_cvt` | 部分可替代（espaudiocore `output_bits`） |
| 字节级 seek / 缓存内瞬移 | `esp_gmf_pipeline_seek` + `seek_in_cache` | espaudiocore 给**毫秒级** seek（本地更强、在线更弱） |
| 位置/总长/速度三个观测值 | `io_get_pos/get_size/get_speed_stats` | **必须自研**（下载计数 + 滑动窗口） |
| 曲目参数事件 | MUSIC_INFO | 轮询 `get_format()`；码率自算 |
| 播放健康度日志 | 本文件 149-186（数据源是 GMF 的 pos/speed） | 保留框架，换数据源 |

---

## 5. 对外能力清单（`audio_service.h` 里每个函数）

约定：**任务上下文**一列是**实际调用点**（不是猜测），依据是 `grep` 到的调用位置。

| # | 对外函数（h 行号） | 实现方式（.cc 行号） | 依赖哪些 GMF 能力 | 依赖网络 | 线程约束 | 实际调用上下文 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `audio_service_init` (29) | 456-522 | `esp_asp_cfg_t`/`new`/`set_event` | ✗ | 幂等（`s_player` 判空 458）；建锁；**必须在任何播放前**调用 | `main.cc:238`（`app_main` 任务，开机一次） |
| 2 | `audio_service_play_path` (36) | 588-607 | `run`(570)、`stop`(536)、`get_state`(534)、`get_in`(794) | ✗ | **持 `s_lock` 阻塞**（603-605）；内部会 `stop()` 旧曲；`media_meta_probe_audio` 在事件回调里同步读 SD | `app_music.cc:586`（`start_track_at`）← LVGL 定时器/按键；`app_music.cc:3319`（本地列表点歌）← LVGL |
| 3 | `audio_service_play_uri` (44) | 923-942 | 同上 + `io_http_init`/`register_io`（首次流式） | **✓**（http/https） | 同上（**持锁阻塞**，首次流式还会建 4 MB 缓冲 + TLS） | `app_music.cc:2232`（`start_track_online_at`）← `online_prepare_task`（"on_fetch" 工作线程）与 LVGL；`app_online.cc:140`（selftest） |
| 4 | `audio_service_pause` (46) | 944-951 | `pause`(949) | ✗ | 无锁；非阻塞（底层等状态同步，最长 `task_timeout` 15 s） | `app_music.cc:1284`（BT 媒体键，LVGL 定时器）、`1579`（看门狗） |
| 5 | `audio_service_resume` (47) | 953-964 | `resume`(958) | ✗ | 同上；**成功会同步回调 `AUDIO_EVT_RESUMED`**（在 GMF 任务上下文） | `app_music.cc:1273/1581/1591`（LVGL 定时器） |
| 6 | `audio_service_toggle` (48) | 1128-1142 | `get_state`(1134)+ 上两者 | ✗ | 只有 RUNNING/PAUSED 有效，其它状态返回错误（上层据此走"从记忆进度起播"） | `app_music.cc:676`（播放键，LVGL） |
| 7 | `audio_service_stop` (49) | 1144-1154 | `stop`(1149) | ✗ | 阻塞（等管线停）；内部静音 + 关 PA | `app_music.cc:630/1288/1564/2350/3362`（LVGL 定时器） |
| 8 | `audio_service_is_playing` (50) | 1156-1164 | `get_state`(1162) | ✗ | 纯查询；**返回 RUNNING，不含 PAUSED**（与 espaudiocore 语义不同） | 多处 tick/看门狗（`app_music.cc:145/685/847/1544/1589/1625/1632`） |
| 9 | `audio_service_seek_ms` (54) | 1054-1126 | `get_state`/`pause`/`get_pipeline`(1078)/`pipeline_seek`(1097)/`resume`/`io_get_pos` | ✓（在线重连） | **必须在 app 层白名单之后调用**；内部 pause→seek→resume；`esp_gmf_io_seek` 会**阻塞等 IO 任务**（`esp_gmf_io.c:432`）→ 在 LVGL 任务里可能卡几十~几百 ms（长距离在线拖动要重连） | `app_music.cc:852`（拖动释放，LVGL）、`1581/1649/1712`（看门狗/断点续播，LVGL 定时器） |
| 10 | `audio_service_set_duration_ms` (65) | 998-1004 | 无（写 `s_dur_hint_ms`） | ✗ | **必须在 `play_uri()` 之后调用**（换曲会清零 548） | `app_music.cc:2243`（起播在线曲目后，工作线程/LVGL） |
| 11 | `audio_service_seek_supported` (74) | 1006-1024 | `seek_bitrate`→`io_get_size`/`get_pos` | 在线需要 Content-Length | 纯查询（但内部会打 WARN 日志）；拒绝原因全打在日志里 | `app_music.cc:835`（拖动前）、`1646`（断点续播轮询） |
| 12 | `audio_service_seek_landing_ms` (87) | 1026-1036 | 无（读标志） | ✗ | **一次性**：读到即清；只能在 seek 后第一块 PCM 之后调 | `app_music.cc:1373`（tick 轮询，最多 10 个 tick） |
| 13 | `audio_service_set_output` (103) | 1168-1210 | 无（改 `s_out` + 板级时序） | ✗ | 无锁；蓝牙未连接返回 `ESP_ERR_INVALID_STATE`(1193)；切蓝牙会 `bt_service_media_start()`(1202) | `app_music.cc:988`（输出按钮，LVGL）、`app_settings.cc:1293`（设置页，LVGL） |
| 14 | `audio_service_get_output` (104) | 1212-1215 | 无 | ✗ | 纯查询 | `app_music.cc:1010/1132/2830`、`app_settings.cc:598/1273`（LVGL） |
| 15 | `audio_service_set_volume` (107) | 1217-1244 | 无（`bsp_audio_set_volume` 寄存器 + NVS） | ✗ | 无锁；0/非 0 会连带静音；**唯一 NVS 写入点**(1241) | `app_music.cc` 音量路径、`ui_volume_panel.cc:160/215`、`ui_control_center.cc:189`、`app_settings.cc:1371`（全在 LVGL） |
| 16 | `audio_service_get_volume` (108) | 1246-1249 | 无 | ✗ | 纯查询 | `ui_volume_panel.cc:107/212/266`、`ui_control_center.cc:326`、`app_settings.cc:1354/1361`（LVGL） |
| 17 | `audio_service_set_mute` (109) | 1251-1254 | 无 | ✗ | 直通 `bsp_audio_set_mute` | （本次 grep 未见 app 调用点，仅内部/预留） |
| 18 | `audio_service_set_event_cb` (112) | 1256-1260 | 无 | ✗ | **开机注册一次**；回调在 GMF 任务上下文同步执行，禁止重活/禁止 LVGL | `app_music.cc:3579`（`app_music_register_apps`，开机） |
| 19 | `audio_service_get_info` (124) | 1262-1269 | MUSIC_INFO 的产物 | 在线才有值 | 纯查询（`s_info_valid` 门禁） | `app_music.cc:143/1687/1739`、`app_online.cc:143` |
| 20 | `audio_service_stream_progress` (138) | 812-845 | `io_get_pos`(833) + `io_get_size`(689) | ✓ | 纯查询（注释声明"任何任务都能调"） | `app_music.cc:1398/1576/2503`（tick / 缓冲提示，LVGL） |
| 21 | `audio_service_stream_speed` (152) | 860-875 | `io_get_speed_stats`(866) | ✓ | 纯查询 | `app_music.cc:2502`（缓冲提示，LVGL） |
| 22 | `audio_service_output_active` (161) | 918-921 | 无（读 `s_out_flowing`） | ✗ | 纯查询，由 out 回调置位(291) | `app_music.cc:1388/1536/1544/1625/1651`（tick，LVGL） |

**契约层面的审计发现**：

1. **阻塞调用出现在 LVGL 任务里**：`play_path`/`play_uri`（持锁 + `stop()` + 首轮 `run()` 最长等 15 s）、
   `seek_ms`（pause→IO seek→resume）。app 层用"工作线程 + tick 轮询"部分规避了，但 `start_track_at()`
   仍可能直接跑在 LVGL 任务（`app_music.cc:3319`、`681`、`1277`）。拆分/替换时**必须把这条约束写进头文件**。
2. **事件回调上下文**：`notify()`(344-349) ← `player_event_cb` ← `esp_audio_simple_player.c:73-113`
   的 `_pipeline_event`，即 **GMF 管线任务**。头文件 111 行已写"不要在里面做重活"，但没写"**禁止 LVGL 调用**"。
3. **`audio_service_set_mute` 与 `audio_service_set_volume(0)` 语义重叠**：前者不改 `s_volume`，
   后者会写 NVS；拆分时若把静音状态散到两个模块，很容易出现"静音了但 `s_volume` 还是 55"的不一致。
4. **`audio_service_get_info()` 的 `bitrate` 是"探测平均值"而非 GMF 报值**（382-399），
   头文件(118)没写清这点；上层若拿它算文件大小会与 GMF 的语义混淆。

---

## 6. 输出通道路由与 PCM 完整路径

### 6.1 三条路的代码位置与调用链

**（一）扬声器（ES8389 → NS4150B ×2 → 喇叭）**

```
GMF 管线任务: aud_bit_cvt → out 端口(asp_func_release_write)
  → out_write_cb (audio_service.cc:287)
      ├ s_out_flowing = true                        (291)
      ├ audio_stream_measure(data_size)             (293) → 时间轴/码率/落点核对
      ├ 间隔统计 t_out0 / s_stat_gap_max_ms          (305-312)
      └ s_out == SPEAKER 分支                       (333-339)
          ctx = bsp_audio_codec()（init 时经 cfg.out.user_ctx 注入，491）
          esp_codec_dev_write(dev, data, size)       (337)
            → esp_codec_dev 的 data_if->write
            → i2s_channel_write (TDM, I2S_NUM_0)
            → GPIO3/4/5 → ES8389 DAC → 板载功放(PA_CTRL=GPIO7, 高有效)
```
- 音量走**ES8389 数字音量寄存器**：`audio_service_set_volume`(1217-1244) → `bsp_audio_set_volume`(1224)
  → `bsp_audio.cc:123 esp_codec_dev_set_out_vol`。**输出通路上不做任何软件缩放**。
- PA 只在状态事件里开关：RUNNING 开(427)、FINISHED/STOPPED/ERROR 关(435/440/444)；
  `set_output` 切回扬声器时按 `s_running` 补开(1185-1187)。

**（二）蓝牙（A2DP Source）**

```
out_write_cb (287)
  └ s_out == BLUETOOTH 分支                          (313)
      ├ vol = clamp(s_volume,0,100)                  (321)
      ├ vol <= 0 → 直接 return data_size（不推数据，但仍报全长）(322-324)
      ├ vol < 100 → pcm16_scale(data, size/2, vol)   (325-328)  ← ★ 蓝牙路的软件音量
      └ bt_service_write_pcm(data, size)             (329)      ← 非阻塞，满即丢帧
          → bt_service 内部 PCM 环形缓冲 → feed_task (bt_service.cc:1078)
          → （对端不是 48k 时）esp_asrc 重采样 48k→对端率 (bt_service.cc:646-657, 786-826)
          → SBC 编码 → esp_bt_audio_stream_acquire_write/release_write → A2DP → 耳机
      最后 return data_size（永远报"写满"，绝不短返回）(331)
```
- 切换：`audio_service_set_output(AUDIO_OUT_BLUETOOTH)`(1190-1206) → 先静音 + 关 PA(1199-1200)
  → `bt_service_media_start()`(1202，异步) → `s_out = out`(1204)。
- ⚠ **板载与蓝牙不会同时出声**（if/else 分流），AGENTS §11.5.7 已核实。

**（三）静音（AUDIO_OUT_NONE）**

```
audio_service_set_output(AUDIO_OUT_NONE) (1170-1176)
  → s_out = NONE (1172) → bsp_audio_set_mute(true) (1173) → bsp_audio_pa_enable(false) (1174)
```
- 此时 out 回调仍会走**扬声器分支**（因为 `s_out != BLUETOOTH`，313 判断为假）→
  `esp_codec_dev_write`(337) 照写，只是 codec 处于 mute 寄存器态。**"静音"不是"停止喂数据"**，
  时间轴照常推进（tick 上进度条会继续走）。
- 另有一次性静音点：`play_uri_locked` 起播前 `bsp_audio_set_mute(false)`(564)；
  `run()` 失败时静音 + 关 PA(573-574)；`stop()` 静音 + 关 PA(1150-1151)。

### 6.2 完整 PCM 路径与"格式转换发生在哪一段"

管线元素顺序在 ASP 里写死（`esp_audio_simple_player.c:45-56` 的 `el_names`）：

```
输入 IO(file/http) → aud_dec → aud_rate_cvt → aud_ch_cvt → aud_bit_cvt → out 端口 → out_write_cb
```

- **解码**：`aud_dec`（GMF audio decoder，按 URI/魔数选 mp3/aac/flac/wav…）
- **44.1k → 48k 重采样**：`aud_rate_cvt`，目标率来自
  `sdkconfig: CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE=48000`
  （`audio_simple_player_pool.c:57-58`）
- **声道转换**：`aud_ch_cvt`，目标 2ch（`CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST=2`，同上 69）
- **24→16 bit 位宽转换**：`aud_bit_cvt`，目标 16 bit
  （`CONFIG_AUDIO_SIMPLE_PLAYER_BIT_CVT_DEST_BITS=16`，同上 78）
- 因此 **送到 `out_write_cb` 的 PCM 恒为 48 kHz / 16 bit / 2ch**，这正是
  `OUT_PCM_BYTES_PER_MS = 192`（行 84）成立的依据；也是"蓝牙路 `data_size/2` 就是样本数"(327) 的依据。
- ⚠ 若按 AGENTS §11.5.12 把 DAC 改成 24-bit，**`sdkconfig` 的 BIT_CVT 目标、`OUT_BITS`(39)、
  `OUT_PCM_BYTES_PER_MS`(84)、`pcm16_scale`(267-278)、BT 路的 `data_size/2`(327) 五处要同步改** ——
  这是本文件最容易回归的地方。
- **蓝牙路的"第二次重采样"**不在本文件：`bt_service.cc:652` 在对端协商率 ≠ 48k 时开 ASRC（48k→对端率），
  本模块只管把固定 48k/16bit/2ch 推过去。

### 6.3 输出通道相关的状态与陷阱

| 项 | 位置 | 说明 |
| --- | --- | --- |
| `s_out` 默认值 | 51 | 每次开机回 SPEAKER（**不持久化**，无 NVS） |
| out ctx | 491 | 初始化时 **一次性**捕获 `bsp_audio_codec()`；切输出**不会**换 ctx |
| 蓝牙软件音量 | 321-328 | 唯一覆盖蓝牙路的音量点（ES8389 寄存器对 A2DP 无效） |
| 静音返回全长 | 322-324 | "不推数据但照旧返回全长"——返回短了 GMF 会当写失败/结束 |
| 蓝牙缓冲区满 | 329 | `bt_service_write_pcm` 非阻塞、满了丢帧（宁可丢帧不停管线） |
| `s_running` 的复用 | 426/434/439/443/1152/1185 | 既表管线状态，又决定 `set_output(SPEAKER)` 要不要补开 PA |

---

## 7. seek / 缓冲进度 / 下载速度 / 落点核对 / 断点续播 的实现位置与 GMF 依赖

| 机制 | 本文件实现行号 | 依赖的 GMF 具体能力 | 依赖的 GMF 具体行为/坑 |
| --- | --- | --- | --- |
| **seek 时间→字节** | `seek_byte_for` 893-916；平均码率 `seek_bitrate` 984-996；容器偏移 `s_data_offset`(50/563) | `esp_gmf_io_get_pos`(898)、`esp_gmf_io_get_size`(689 经 `stream_total_bytes`) | 相对映射需要"解码器消费位置"（`release_read` 推进）；绝对映射需要平均码率（本地=media_meta 探测，在线=总字节÷时长） |
| **seek 执行** | `audio_service_seek_ms` 1054-1126 | `esp_audio_simple_player_get_state`(1069)、`esp_audio_simple_player_get_pipeline`(1078)、`esp_gmf_pipeline_seek`(1097)、`pause`/`resume`(1072/1081/1099) | pipeline 必须 PAUSED/STOPPED/FINISHED（`esp_gmf_pipeline.c:587-592`）；`esp_gmf_io_seek` 内部先试 `seek_in_cache`（`esp_gmf_io.c:412`，只处理向前、命中则就地丢字节、零网络），未命中则 `abort` data bus + 由 IO 任务执行 seek 并**阻塞等 `IO_EVT_TASK_SEEK_DONE_BIT`**（`esp_gmf_io.c:418-432`）→ 在线表现为"关连接 + 带 Range 重开" |
| **seek 后的时间轴重建** | 1114-1120（`s_pcm_bytes=0`、`s_tl_base_ms=pos_ms`、`s_meas_anchor_ok=false`、挂落点核对） | 无（自有状态） | 锚点不清会被差分算出天文数字（1111 注释） |
| **seek 出声标志清零** | 1095（`s_out_flowing=false`） | 无 | UI 靠 `audio_service_output_active()` 撤"缓冲中…"，不清零则提示刚挂上就被撤 |
| **缓冲进度** | `stream_total_bytes` 684-694；`audio_service_stream_progress` 812-845；钩子 `stream_evt_hook` 643-668 | `esp_gmf_io_get_size`(689)、`esp_gmf_io_get_pos`(833)、HTTP IO 事件钩子(707) | ⚠ 读方向 `HTTP_STREAM_FINISH_REQUEST` **永不派发**（`io_http` 只在 `dir==WRITER` 分支 dispatch）→ `s_stream_total` 恒 0，总长只能走 `io_get_size`；`ON_RESPONSE` 的 `buffer_len` 是"想读多少"不是"读到多少"(824-829)，只能当兜底 |
| **下载速度** | `audio_service_stream_speed` 860-875 | `esp_gmf_io_get_speed_stats`(866)；**必须** `esp_gmf_io_enable_speed_monitor`(806) 且开在 `esp_gmf_pipeline_get_in`(794) 拿到的实例上 | 单位是 **Kbps**（源码头文件注释与 AGENTS §11.4 均确认），UI ÷8 才是 KB/s；在池子"模板"上开无效（`esp_gmf_pool_new_io` 走 `esp_gmf_obj_dupl` 复制实例） |
| **落点核对** | 采样点 `audio_stream_measure` 211-224（第一块 PCM 反推）；取值 `audio_service_seek_landing_ms` 1026-1036；状态 131-135、1117-1120 | `esp_gmf_io_get_pos`(214) | 用**与映射同一个码率** `s_seek_req_bps`(217) 反推；`body = pos - s_data_offset`(216)；上层偏差 >3 s 时对齐进度/歌词（`app_music.cc:1373`） |
| **断点续播（底座）** | 本文件只提供三件：时间轴 `timeline_now` 190-193、`seek_supported` 1006-1024、`seek_ms` 1054-1126、`set_duration_ms` 998-1004 | `get_pos`/`get_size`/`pipeline_seek` | **策略全在 app 层**（`app_music.cc` 的 `s_on_resume_ms` / `online_url_refresh()` / `audio_service_seek_ms(s_elapsed_ms)` 1649/1712）：本文件不知道"重刷地址"，也不知道"断点" |
| **实测有效码率** | 状态 117-120；采样 `audio_stream_measure` 226-261 | `esp_gmf_io_get_pos`(238) | 只在 `s_stream_io != NULL`（有流式 IO）时采；跳过 2 s 抢读期(230)、1 s 窗口(234)、8 kbps~2 Mbps + 与全曲平均相差 ≤4 倍两道检查(252-253)、3:1 IIR(255) |

**seek 相关的两处已知误差（代码注释已声明，审计确认属实）**：

1. 在线流 `s_data_offset = 0`(563)，而 `total`/`duration` 把 ID3v2 标签也算作音频 → **中段定位早到几秒**(980-982)。
2. `s_meas_anchor_ok` 由 out 回调写、`seek_ms` 清(1116)，两者并发；代码用"与全曲平均相差 4 倍"这道
   相对检查兜住跨 seek 脏采样(246-249)。

---

## 8. 拆分建议

### 8.1 目标与行数预算

目标：**`audio_service.cc` ≤ 400 行**（当前 1269）。注释密度极高（约 45% 是注释），
拆分时要**把注释跟着函数一起搬**，不要为了压行数删注释 —— 这些注释是踩坑记录，删了必然复发。

建议 7 个文件（都在 `components/services/audio_service/` 内，**`audio_service.h` 对外 ABI 保持不变**）：

| 新文件 | 预算行数 | 负责什么 | 搬过去的函数（原行号） |
| --- | --- | --- | --- |
| `audio_service.cc` | **≈250** | **对外门面**：只做参数校验、取锁、转发到内部模块；不出现任何 GMF 类型 | `audio_service_init`(456-522 的**板级部分保留**，ASP 部分外移)、`play_path`(588-607)、`play_uri`(923-942)、`pause`(944-951)、`resume`(953-964)、`toggle`(1128-1142)、`stop`(1144-1154)、`is_playing`(1156-1164)、`set_mute`(1251-1254)、`set_event_cb`(1256-1260)、`get_info`(1262-1269) |
| `audio_service_internal.h`（新） | ≈140 | 内部共享类型 + **状态访问函数**声明（见 8.3）；常量 `OUT_*`/`OUT_PCM_BYTES_PER_MS`/`STREAM_RING_BYTES` 单一来源 | — |
| `audio_engine.cc` | ≈300 | **GMF/ASP 会话**：建实例、事件回调、状态机、起播流程、out 回调外壳 | `player_event_cb`(351-452)、`notify`(344-349)、`play_uri_locked`(530-586)、`out_write_cb`(287-340)、`audio_service_init` 的 cfg/new/set_event 段(487-517) |
| `audio_source_io.cc` | ≈200 | **输入 IO 定制与观测**：HTTP IO 注册、总长/位置/进度/速度 | `ensure_stream_io`(696-729)、`stream_evt_hook`(643-668)、`stream_total_bytes`(684-694)、`stream_io_update`(783-810)、`stream_progress`(812-845)、`stream_speed`(860-875) |
| `audio_timeline.cc` | ≈150 | **时间轴 + 健康度统计 + 出声标志** | `timeline_now`(190-193)、`audio_stream_measure` 的时间轴/日志部分(199-206)、`audio_stats_log`(149-186)、`output_active`(918-921) |
| `audio_seek.cc` | ≈230 | **seek 换算与执行 + 落点核对 + 时长提示** | `seek_bitrate`(984-996)、`seek_byte_for`(893-916)、`seek_supported`(1006-1024)、`seek_landing_ms`(1026-1036)、`seek_ms`(1054-1126)、`set_duration_ms`(998-1004)、`audio_stream_measure` 的落点/码率采样部分(211-261) |
| `audio_output.cc` | ≈190 | **输出通道 / 音量 / 静音 / PA / 软件缩放** | `pcm16_scale`(267-278)、`out_write_cb` 的分流与计时部分(305-339)、`set_output`(1168-1210)、`get_output`(1212-1215)、`set_volume`(1217-1244)、`get_volume`(1246-1249) |

> `out_write_cb` 与 `audio_stream_measure` 会被切成两半（引擎外壳 + 时间轴 + 输出 + seek）：
> **这是本方案唯一的"函数级拆分"**。具体做法：
> `out_write_cb` 留在 `audio_engine.cc`，函数体变成 4 行委托：
> `timeline_flowing(true) → timeline_account_pcm(size) → stats_on_write(t0) → output_write(data,size)`，
> 原来 40 行注释与细节分别搬到 `audio_timeline.cc` / `audio_output.cc` 的同名函数里，
> 并在两处都留一句"原 audio_service.cc:287-340"的出处注释。

### 8.2 全局变量的搬迁映射

| 变量（原行号） | 新归属 | 对外暴露方式 |
| --- | --- | --- |
| `s_player`(41)、`s_lock`(42)、`s_evt_cb`(43)、`s_evt_user`(44) | `audio_engine.cc` | `engine_session_start()/stop()`、`engine_set_event_cb()`、`engine_take_lock()/give_lock()` |
| `s_running`(45) | `audio_engine.cc` | `engine_is_running()`（`set_output` 的 PA 补开改调它） |
| `s_volume`(47) | `audio_output.cc` | `output_volume()` / `output_set_volume_state()`（out 回调**必须**能无锁读到） |
| `s_info`(48)、`s_info_valid`(49) | `audio_engine.cc`（元数据面） | `engine_info_get(audio_info_t*)`、`engine_info_valid()` |
| `s_data_offset`(50) | `audio_seek.cc` | `seek_set_data_offset()` / `seek_data_offset()` |
| `s_out`(51) | `audio_output.cc` | `output_current()` / `output_set_current()` |
| `s_cur_path`(54) | `audio_engine.cc` | `engine_cur_path()`（MUSIC_INFO 探测用） |
| `s_out_flowing`(57) | `audio_timeline.cc` | `timeline_set_flowing()` / `timeline_flowing()` |
| `s_stream_total`(65)、`s_stream_got`(66)、`s_stream_io`(67) | `audio_source_io.cc` | `source_io_total_bytes()`、`source_io_handle()`、`source_io_reset_counters()` |
| `s_dur_hint_ms`(75) | `audio_seek.cc` | `seek_set_duration_hint()` / `seek_duration_hint()` |
| `s_pcm_bytes`(92)、`s_tl_base_ms`(93)、`s_stats_calls`(94) | `audio_timeline.cc` | `timeline_account_pcm()`、`timeline_now_ms()`、`timeline_reset()`、`timeline_set_base()` |
| `s_stat_codec_us`(96)、`s_stat_last_ms`(97)、`s_stat_gap_max_ms`(98) | `audio_timeline.cc` | `timeline_stats_note_write(us)`、`timeline_stats_take()` |
| `audio_stats_log` 的 `static last_ms/last_pcm/last_calls`(151-153) | `audio_timeline.cc`（**升级为模块级**） | 无（模块内部） |
| `s_meas_bps`(117)、`s_meas_anchor_ms/pos/ok`(118-120) | `audio_seek.cc` | `seek_measured_bps()`、`seek_measure_reset()` |
| `s_seek_req_ms/bps`(131-132)、`landing_pending/ready/ms`(133-135) | `audio_seek.cc` | `seek_note_request()`、`seek_take_landing()`、`seek_landing_pending()` |
| `ensure_stream_io` 的 `static done`(698) | `audio_source_io.cc`（**升级为模块级**） | 无（模块内部） |

### 8.3 必须"升级成模块级状态 + 配访问函数"的清单（按风险排序）

1. **`ensure_stream_io` 的 `static bool done`(698)** —— 最危险的一个：拆到别的文件后如果还留在函数里，
   而"注册 IO"的入口被换掉（例如换成重新 init），就会出现"以为注册过、其实没注册"或"注册两次"。
   → 升级为 `audio_source_io.cc` 的 `static bool s_io_registered` + `source_io_ensure()`。
2. **`audio_stats_log` 的三个 static**(151-153) —— 一旦日志函数被别处调用（例如 seek 模块），
   静态量就变成了"两个模块各有一份"，日志会算错窗口。
3. **`s_out_flowing`(57)** —— 三个写入点（out 回调 291、换曲 545、seek 1095）分属三个新文件，
   必须走同一个访问函数，否则拆分后极易漏掉 seek 那次清零（直接表现：拖动后 UI 的"缓冲中"提示不出现）。
4. **`s_pcm_bytes`/`s_tl_base_ms`(92-93)** —— 四个写入点（换曲 550-551、seek 1114-1115、测量 204）；
   必须只经 `timeline_*` 访问，且 `OUT_PCM_BYTES_PER_MS` 只允许在 `audio_service_internal.h` 定义一次。
5. **`s_stream_io`(67)** —— 换曲清空(568) → 起播后绑定(583/795) → 查询函数读(172/214/238/689/833/866/898)。
   绑定与清空**必须同属 `audio_source_io.cc`**，否则"模板句柄/管道句柄"那个坑（行 768-782 的血泪注释）
   会在拆分中被重新引入。
6. **`s_volume`(47)** —— 写方是 LVGL（1217-1244），读方是 out 回调(321)。跨模块后要么保持
   `volatile int` + 单独访问函数（低成本），要么评估是否上 `_Atomic`。
7. **`s_seek_req_bps`(132) 与 `seek_bitrate()`(984)** —— 落点核对要求"同一码率"，所以
   `seek_ms` 必须把**已算出的**那个值记下来，而不是在落点核对时重算（当前实现是对的；
   拆分时要保证 `audio_seek.cc` 同时拥有"算"和"记"两件事）。
8. **`s_data_offset`(50)** —— 跨 seek 模块（映射 911/216）与引擎模块（赋值 563）：给 setter/getter。

### 8.4 拆分中最容易出错的耦合点（逐条给出位置与后果）

| # | 耦合点 | 位置 | 拆错会怎样 |
| --- | --- | --- | --- |
| 1 | **换曲清零必须"原子地一次性做完"** | 543-557 + 560 + 563 + 568（共 12 个变量，跨 5 个新模块） | 漏一个就是"上一首的脏状态进下一首"：漏 `s_meas_anchor_ok` → 首次采样记一个跨曲的假码率（seek 落点漂）；漏 `s_seek_landing_ready` → 上层读到上一首的落点 |
| 2 | **out 回调是热路径且不能取锁** | 287-340（每 ~46 ms 一次，单任务） | 拆成模块后若任何一段加了 `s_lock` 或阻塞 I/O，就会与 `play_uri` 持锁互等；`bt_service_write_pcm` 非阻塞是硬约束(329) |
| 3 | **`s_stream_io` 的句柄身份** | 568 / 583 / 794（"池子模板 vs 管道实例"） | 拆到两个文件后若任一处改回用 `ensure_stream_io` 里那个句柄，`total` 恒 0 → "定位失败：码率未知" + 缓冲百分比永远不显示（此坑现场踩过两轮） |
| 4 | **`stream_io_update` 的调用时机**（必须在 `run()` 成功之后、且先清旧句柄） | 567-568、570-583 | 提前调 → 拿到旧管线的 IO；不先清 → 上一首的句柄被当成本曲的 |
| 5 | **seek 的 pause→seek→resume 顺序**（要跨 3 个新模块：engine/seek/output） | 1068-1100 | 顺序错/漏 resume → 管线停在 PAUSED；`was_running` 判断错 → 播完状态下 seek 会把管线留在 PAUSED 又去 resume |
| 6 | **`s_out_flowing=false` 与落点核对的挂接** | 1095 + 1120 + 211 | 漏 1095 → UI"缓冲中"秒消失；漏 1120 → 上层永远读不到落点（`app_music.cc:1373` 白等 10 个 tick） |
| 7 | **时间轴基准（`s_tl_base_ms`/`s_pcm_bytes`）与 `OUT_PCM_BYTES_PER_MS` 必须同源** | 84 + 192 + 1114-1115 + 180 | 常量复制两份后，改采样率/位深时只改了一处 → 进度条与音频时长系统性偏移 |
| 8 | **`seek_bitrate()` 被 4 处调用**（251 合理性、895 映射、1011 支持性、1019/1124 日志） | 251/895/1011/1019/1124 | 它每次都会问 IO（`stream_total_bytes`→`esp_gmf_io_get_size`）；拆开后若在别处再算一次，会出现"映射用一个码率、核对用另一个"（1124 那行现在就已经多算了一次） |
| 9 | **事件回调上下文**：`notify`→app 回调运行在 **GMF 管线任务** | 344-349 / 351-452 | 拆模块后在事件里调 `output_*`（尤其 `set_output`/`set_volume`）可能引入 LVGL 调用或锁竞争；app 层 `on_audio_evt`(`app_music.cc:1142`) 只置标志是对的，这个约束要写进 `audio_service_internal.h` |
| 10 | **`media_meta_probe_audio` 在事件回调里同步读 SD** | 392 | 拆分后若搬进 `audio_source_io.cc`，很容易被当成"IO 模块的活"而在别的线程再调一次 → 音频任务被 SD 阻塞（首轮 `Run timeout` 的诱因之一，见 ASP 源码 302-322 的补丁注释） |
| 11 | **out ctx 是 init 时一次性捕获的 codec 句柄** | 491 | 拆到 `audio_output.cc` 后容易改成"每次回调现取 `bsp_audio_codec()`"（多一次函数调用，且如果将来支持"播放中重开 codec"会拿错句柄） |
| 12 | **`audio_service_set_volume` 的 NVS 落盘唯一性** | 1240-1242 | 拆分后若把"改音量"复制到输出模块内部（例如蓝牙路自己算 vol），会出现第二条写 NVS 的路径 → 长按 VOL+ 狂写 flash |
| 13 | **`s_running` 被 `set_output` 复用** | 1185 | 拆模块后 `set_output` 在输出模块、`s_running` 在引擎模块 → 要么暴露 getter，要么改成"由引擎提供 PA 恢复回调"，否则切输出后 PA 不开（=没声音） |
| 14 | **`stop()`/`pause()` 的阻塞语义与 15 s 超时** | ASP 源码 `esp_gmf_task_set_timeout(15000)`（该文件 302-322） | 拆分后在更多线程调用这些阻塞 API（例如从事件回调里）→ 死等 15 s |

### 8.5 建议的实施顺序（每步都能编译 + 可回归）

1. **步 1（无行为变化，纯搬迁）**：建 `audio_service_internal.h` + `audio_timeline.cc` + `audio_output.cc`，
   先把"时间轴/统计/输出分流"搬出去，`out_write_cb` 变成委托壳。
   回归点：进度条、`play stats` 日志、扬声器/蓝牙出声与音量。
2. **步 2**：搬 `audio_seek.cc`（含 `seek_bitrate`/`seek_byte_for`/落点核对）。
   回归点：本地/在线拖动、日志 `seek -> …` 与 `seek landed: …`、落点偏差对齐。
3. **步 3**：搬 `audio_source_io.cc`（HTTP IO + 进度 + 速度）。
   回归点：`缓冲中 xx KB/s · xx%`、`seek not ready:` 自诊断不再出现 `total=0`。
4. **步 4**：搬 `audio_engine.cc`（ASP 会话 + 事件 + `play_uri_locked`）；`audio_service.cc` 只剩门面。
   回归点：换曲、播完自动下一首、错误事件、开机恢复进度。
5. **步 5**：清理死代码：`#include "esp_gmf_io_file.h"`(24)、`#include "esp_http_client.h"`(26)、
   731-766 的"已回退的本地预读"大段注释（或压缩成 5 行并注明"曾回退，见 AGENTS §12.4.4"）。
6. **步 6**：把 `OUT_PCM_BYTES_PER_MS`、`OUT_*`、`STREAM_RING_BYTES`、`SEEK_MEAS_*`、`SEEK_REL_MIN_MS`
   全部收敛到 `audio_service_internal.h`，并在 `audio_service.h` 里补三条线程约束的注释（阻塞、
   事件回调上下文、`is_playing` 不含 PAUSED）。

---

## 9. 换用 `libs/espaudiocore` 的适配点

依据：`libs/espaudiocore/include/espaudiocore.h`（320 行，逐行读过）、`libs/espaudiocore/API.md`（488 行，逐行读过）、
`libs/espaudiocore/README.md`（144 行）、`src/audio_player.cpp:552-608`、`src/audio_sink_i2s.cpp:372-500/641-651`、
`src/audio_sink_ringbuf.cpp:55-215`。

### 9.0 先决条件（两件必须先做，否则接不上）

| # | 事实 | 证据 | 影响 |
| --- | --- | --- | --- |
| 1 | **`libs/` 目前没有被纳入构建** | 根 `CMakeLists.txt:22-27` 的 `EXTRA_COMPONENT_DIRS` 只有 pinyin 引擎、`components/services`、`components/apps`；除本报告外，全工程源码与构建文件里 `grep espaudiocore` **0 命中**（`*.txt`/`*.yml` 均无） | 接它必须先登记 `libs`（或在 `main/idf_component.yml` 里 `override_path`），并在 `CMakeLists.txt` 的 `REQUIRES` 里加 `espaudiocore`、去掉 `esp_audio_simple_player`/`gmf_core`/`gmf_io` |
| 2 | **板载 I2S 是 TDM，不是 STD；espaudiocore 的 I2S 输出用 STD API** | `components/gen_bmgr_codes/gen_board_periph_config.c:49` → `esp_bmgr_i2s_audio_out_cfg.mode = I2S_COMM_MODE_TDM`（48000/16bit/TDM SLOT0\|SLOT1/I2S_NUM_0）；`libs/espaudiocore/src/audio_sink_i2s.cpp:402` 用 `i2s_channel_reconfig_std_clock()`、`:476` 用 `i2s_channel_reconfig_std_slot()`；IDF `i2s_std.c:394/444` 明确 `ESP_GOTO_ON_FALSE(handle->mode == I2S_COMM_MODE_STD, ESP_ERR_INVALID_ARG, …"this handle is not working in standard mode")` | **`espaudiocore_begin(path, tx, …)`（文件→I2S）在本板会失败**：44.1k 源触发 `set_rate` → `reconfig_std_clock` 返回 `ESP_ERR_INVALID_ARG` → 变调（44.1k 数据按 48k 时钟放，快 8.8%）；`set_format` 同理。**结论：本板必须走 ringbuf 输出模式**，由我们自己按现有方式写 `esp_codec_dev_write()`（TDM 通道保持板级管理器所有） |

### 9.1 四种模式 ↔ 本工程需求的对应关系

| espaudiocore 模式 | 原型 | 本工程能不能用 | 替代现在的哪条路 |
| --- | --- | --- | --- |
| 文件 → I2S | `espaudiocore_begin(path, tx, cfg, &dec_err)` | ❌ 受 9.0#2 阻挡（TDM） | `play_path` 的"本地文件"路 |
| 文件 → ringbuf | `espaudiocore_begin_rb(path, out_rb, cfg, &dec_err)` | ✅ **推荐** | `play_path`（本地文件），PCM 由我们取走再分发给 codec/蓝牙 |
| ringbuf → I2S | `espaudiocore_begin_stream(in_rb, fmt, tx, cfg, &dec_err)` | ❌ 同 9.0#2；且 ringbuf 输入**不支持 seek** | `play_uri` 的在线流式路 |
| ringbuf → ringbuf | `espaudiocore_begin_stream_rb(in_rb, fmt, out_rb, cfg, &dec_err)` | ✅ **推荐**（在线流的唯一可行形态） | `play_uri`（在线流式），HTTP 完全自研 |

### 9.2 逐能力映射（现有能力 → espaudiocore 对应 → 缺口）

| # | 现有能力（本文件行号） | espaudiocore 对应 | 差距 / 需要我们在上层实现什么 |
| --- | --- | --- | --- |
| 1 | 建会话 + 起播 `new`(506)/`set_event`(513)/`run`(570) | `espaudiocore_begin()` / `begin_rb()` / `begin_stream(_rb)()` | 单会话（重复 begin 前必须 `stop()`，API.md §5）；没有 URI/pipeline/IO 池概念；**`task_core` 默认 1 必须显式改 0**（本工程 LVGL 在 core 1） |
| 2 | 停止 `stop`(536/1149) | `espaudiocore_stop()` | 更强（阻塞至解码任务退出）；`ESP_ERR_INVALID_STATE`=没有会话。**换曲流程可直接替换** |
| 3 | 暂停/恢复 `pause`(949)/`resume`(958) | `espaudiocore_pause()` / `resume()` | 等价；I2S 模式有 DMA 余音（本板走 ringbuf，无此问题） |
| 4 | 播放状态 `get_state`(534/1069/1134/1162)、`is_playing`(1156-1164) | `espaudiocore_is_running()` + 事件 | ⚠ **语义不同**：`is_running()` **含暂停态**，现在的 `is_playing()` 不含 → 要么自己按 `EVT_PAUSED/RESUMED` 维护一个 `s_running`（保留现有语义，推荐），要么改上层判断 |
| 5 | 事件 `ESP_ASP_EVENT_TYPE_STATE`(416-451) | `ESPAUDIOCORE_EVT_{STARTED,PAUSED,RESUMED,SEEKED,EOS,STOPPED,ERROR}`（头文件 69-77） | 语义**更全**（多了 SEEKED），映射：STARTED→RUNNING 分支(425)、PAUSED(430)、RESUMED→`AUDIO_EVT_RESUMED`(960 现在是自己补发的)、EOS+STOPPED→FINISHED(433)、ERROR(442)。回调上下文同样"解码任务、禁止阻塞"(头文件 80-86) |
| 6 | 曲目参数 `ESP_ASP_EVENT_TYPE_MUSIC_INFO`(358-414) | `espaudiocore_get_format(&rate,&ch,&bits)`(313) + `get_duration_ms()`(301) + `on_meta` 的 `"tlen"` | **码率没有 API** → 必须自己算（本地仍可用 `media_meta_probe_audio`；在线用 Content-Length÷时长）。MUSIC_INFO 里那份"本地优先用探测值"的逻辑(369-407)要原样保留，只是触发时机改成 begin 之后轮询 |
| 7 | 时长 `s_info.duration_ms` + `set_duration_ms`(998-1004) | `espaudiocore_get_duration_ms()`（`<0` 未知）+ `"tlen"` 元数据 | 本地普遍**更好**（MP3 有 Xing/FLAC 有 STREAMINFO/WAV 有 data 长度）；**在线 ringbuf 流仍无时长** → `audio_service_set_duration_ms()` 这个 hint 接口要保留（上层在线元数据依旧是唯一来源） |
| 8 | 时间轴 `timeline_now`(190-193) + `s_pcm_bytes`(92) | `espaudiocore_get_position_ms()`（`audio_player.cpp:579-608`：已写帧数 − sink 未播字节） | ⚠ **ringbuf 输出模式下 `audio_sink_pending_bytes()` 恒为 0**（`audio_sink_i2s.cpp:648-651` 的注释明确"I2S 有 DMA 残留，ringbuf 输出没有"）→ position 会**领跑**"真正送到 codec 的位置"，差值 = 我们 ringbuf 里的积压。建议：**保留现有"按写到 codec 的字节计时"机制**（`s_pcm_bytes`/192），把 `get_position_ms()` 只当辅助/校验 |
| 9 | seek `seek_byte_for`(893-916)/`seek_bitrate`(984-996)/`pipeline_seek`(1097) | `espaudiocore_seek_ms(int64 ms)`（阻塞至生效，内部"seek 源 → 复位解码器 → 继续解码"，头文件 260-269 / API.md 3.2.4） | **净收益**：不再需要"平均码率/实测码率/字节换算/容器白名单"，`audio_service_seek_supported()` 对**文件输入恒真**。⚠ **ringbuf 输入模式 `seek_ms()` 返回 `ESP_ERR_NOT_SUPPORTED`** → 在线流的拖动与"断点续播"要么放弃，要么自研（见 #14）。另外 M4A 要求可 seek 的文件输入（README 136） |
| 10 | 落点核对(211-224/1026-1036) | 无对应 | 不需要（seek 由库内解码器保证帧对齐）。若仍想要，只能用 `get_position_ms()` 反推 —— 但 ringbuf 模式 position 领跑（#8），误差反而更大。**建议：保留接口 `audio_service_seek_landing_ms()` 恒返回 false，等上层停止依赖后再删** |
| 11 | 缓冲进度(812-845) | 无 | **自研**：Content-Length 自己解析、已下载字节自己累加（HTTP 是我们写的） |
| 12 | 下载速度(860-875) | 无（`esp_gmf_io_get_speed_stats` 是 GMF 的） | **自研**：在读循环里做滑动窗口（仍以 Kbps 对外，保持 UI 不改） |
| 13 | 4 MB 环形缓冲(632/709) | 无（ringbuf 输入模式的 buffer 由我们建） | **自研**：`xRingbufferCreate` 4 MB（PSRAM）继续可用；但"预读满就停止读网络"的策略要自己写（这是抗抖动的关键） |
| 14 | **HTTP 传输**（含 TLS、Range、重连、206/200 兜底） | **没有** | **最大的自研项**：`esp_http_client`（工程已依赖 `esp_http_client.h`）+ `esp_crt_bundle` + 自己的环形缓冲 + Range 续传 + 断线续传 + 403 重刷（后者已在 app 层） |
| 15 | 解码器路由（URI→解码器） | 文件输入按"扩展名→魔数"，含 ID3v2 前缀跳过（README 13） | ringbuf 输入**必须显式给 `espaudiocore_format_t`**（README 135）→ 我们要在流的前几 KB 上自己嗅探容器（`media_meta_probe_audio` 的魔数分派逻辑可复用） |
| 16 | 重采样 44.1k→48k（GMF `aud_rate_cvt`，sdkconfig=48000） | **没有重采样**；I2S 模式"采样率变化时重配时钟"，ringbuf 模式"只上报 `get_format()`，由应用处理"（头文件 303-313） | **自研二选一**：(a) 接 `espressif__esp_asrc`（已在 managed_components，`bt_service.cc:652` 已在用它做 48k↔对端率）；(b) 放弃 48k 固定输出 —— 但蓝牙路（SBC/A2DP，`bt_service` 固定按 48k 取数）会连带受影响 → **推荐 (a)** |
| 17 | 声道转换（GMF `aud_ch_cvt`，目标 2ch） | **没有声道转换**：I2S 模式单声道走 `I2S_SLOT_MODE_MONO`(`audio_sink_i2s.cpp:463-464`)；ringbuf 模式原样输出单声道 PCM | 单声道源（部分 AAC/WAV）→ 必须自己复制成 2ch（否则蓝牙路与"192 B/ms"时间轴全错） |
| 18 | 位宽转换（GMF `aud_bit_cvt`，24→16bit） | `cfg.output_bits`：`0` 跟随 PCM；`16/24/32` 固定（源更窄左移补齐、更宽右移截断） | 可替代 24→16；注意它是**简单移位**（无抖动），且 AGENTS §11.5.12 指出 ES8389 本就支持 24-bit → 可以借这次机会不再做 24→16（但 `OUT_PCM_BYTES_PER_MS`(84)、`pcm16_scale`(267-278)、BT 路 `data_size/2`(327)、sdkconfig BIT_CVT 五处要一起改） |
| 19 | 音量 `set_volume`(1217-1244)，走 ES8389 寄存器 | `espaudiocore_set_volume_db(float)` / `get_volume_db()`（整型域缩放 + 饱和；0 dB 零开销；I2S 与 ringbuf 语义一致） | **百分比↔dB 换算要自研**（0~100% → 建议 -60~0 dB）；也可以继续用 `bsp_audio_set_volume()` 走 ES8389 寄存器（推荐：与本板现有"数字音量"一致），把 `set_volume_db()` 只用于蓝牙路的软件音量替换 `pcm16_scale`(267-278)。注意：库的音量是**全局单一会话**的，两条输出路（扬声器/蓝牙）语义不同，仍需我们在 `out_write_cb` 分流 |
| 20 | 静音 `set_mute`(1251-1254) / 音量 0 自动静音(1230-1234) | **没有 mute API** | 保留 `bsp_audio_set_mute()`；或者用 `set_volume_db(-60)`（API.md 3.3.1：-60 及以下视为静音）—— 但那样扬声器路的 ES8389 寄存器态与库内增益态会打架，**建议只用 bsp_audio_set_mute** |
| 21 | 输出通道三路（1168-1210 + 313-339） | **没有输出路由**（模式在 begin 时定死） | 保住三路的**唯一办法**：用 `begin_rb`/`begin_stream_rb`，PCM 进我们的 ringbuf，再由现有 `out` 分发逻辑（扬声器 `esp_codec_dev_write` / 蓝牙 `bt_service_write_pcm`+软件音量 / 静音不写）消费。**PA 时序、静音时序、`s_running` 语义全部保留在本模块** |
| 22 | 曲目结束/错误 → app（`AUDIO_EVT_*`） | `ESPAUDIOCORE_EVT_EOS` / `EVT_ERROR`(msg 仅回调期有效) | 映射到现有 `audio_evt_t`（`app_music.cc:1142` 只置标志，无需改） |
| 23 | 健康度日志(149-186) | 无 | 保留框架；`io=` 一项数据源从 `esp_gmf_io_get_pos` 换成"我们 HTTP 层的下载字节/文件偏移"，`br=/meas=` 换 `seek_bitrate`/无（ringbuf 流无实测码率需求时可直接删） |
| 24 | 封面/歌词（现由 `media_meta` 读文件，`app_music.cc:544` 等） | `on_meta` 直接给 `APIC`/`PICTURE` 原始字节（零拷贝，`espaudiocore_meta_t`） | 可选收益：**在线/流式场景**下不用先落盘再解析封面；注意 `meta->data` **仅回调期有效**(头文件 108-111)，要留必须立刻拷贝（现有 `media_meta_image_mem_rgb565` 正好吃内存缓冲） |
| 25 | 断点续播（`s_on_resume_ms`，`app_music.cc`） | 依赖 seek | 文件模式：`seek_ms()` 直接可用，**比现在更可靠**；ringbuf 流模式：seek 不可用 → 只能"重开 HTTP + 丢弃到目标位置 + 解码器从头对齐"，等价于把 GMF 的 seek 重写一遍（**建议第一版先放弃在线续播定位**，保留"从头重刷地址播放"） |
| 26 | 卡死看门狗 / URL 重刷（app 层） | 依赖 `output_active`/`is_playing`/`seek_supported`/`seek_ms` | `output_active` 需要新的"已出声"判据（建议：我们写 codec 的字节数 > 0，即现有 `s_out_flowing`）；`seek_supported` 对文件恒真、对 ringbuf 流恒假 → app 层 `track_seek_unsupported()` 白名单可以退休（文件模式），但在线流要新增"不支持拖动"的提示 |

### 9.3 净收益（值得换的理由）

1. **seek 从"码率推测"变成"库内精确 seek"**：本文件 893-996、211-224、1006-1036 共约 250 行
   （含 600+ 字注释）可以整体退休；`定位失败：码率未知` 这条失败模式在本地文件上彻底消失。
2. **格式覆盖更宽**：WAV/MP3/AAC/FLAC/OGG Vorbis/Opus/WavPack（README 24-32），
   比 GMF 默认 ASP 池的覆盖面更适合"用户的 SD 卡什么都有"。
3. **高分辨率保留**：24/32-bit 源不再被迫降到 16-bit（README 16），配合 §11.5.12 的
   "ES8389 支持 24-bit"可以顺带解决"24→16bit 转换"这一环。
4. **位置 API 与错误分层**：`get_position_ms()`（已扣 DMA 残留）、`get_duration_ms()`、
   错误分 `esp_err_t` + `dec_err` 两层（头文件 170-181）。
5. **元数据零拷贝**（封面不进堆）+ `"tlen"` 时长估算。
6. **代码体量**：删掉 GMF 相关约 500 行（IO 定制/seek 换算/落点核对/缓冲进度兜底），
   配合 §8 的拆分，`audio_service.cc` 有望落到 250 行上下。

### 9.4 迁移风险清单（按严重度）

| # | 风险 | 证据 | 建议 |
| --- | --- | --- | --- |
| 1 | I2S 输出模式与 TDM 不兼容（**阻塞性**） | 见 9.0#2 | 一律用 `begin_rb`/`begin_stream_rb`，PCM 由我们写 `esp_codec_dev` |
| 2 | 重采样缺失 → 44.1k 文件变调 | espaudiocore 无 rate cvt；`bt_service` 固定按 48k 取数(`bt_service.cc:91`) | 先接 `esp_asrc`（工程已在用）把 44.1k/其他率统一到 48k，再进输出分发 |
| 3 | 单声道源未转 2ch → 时间轴与蓝牙全错 | 无 ch cvt；`audio_sink_i2s.cpp:463` 只在 I2S 模式处理单声道 | 在输出分发前做 mono→stereo 复制（并据此修正 `OUT_PCM_BYTES_PER_MS` 的假设） |
| 4 | ringbuf 模式下 `get_position_ms()` 领跑 | `audio_sink_i2s.cpp:648-651` | 保留自研时间轴（写 codec 的字节数），或在分发后自己扣"ringbuf 待播字节" |
| 5 | 在线流（ringbuf 输入）失去 seek / 时长 / 格式探测 | API.md 3.1.3、README 134-136 | 第一版明确"在线不支持拖动"，UI 侧沿用 `audio_service_seek_supported()` 但内部改为"文件=真、流=假" |
| 6 | HTTP 全靠自研，工作量最大且是听感风险点 | 无对应 API | 单独一个模块 + 单独一轮真机验证（Range 206/200、CDN 掐断续传、403 重刷） |
| 7 | 同一 I2S 通道"双主人" | 板级管理器创建 TDM 通道并交给 `esp_codec_dev`（`dev_audio_codec.c:237` 用 `esp_board_periph_ref_handle`） | 不要用 `esp_board_periph_ref_handle("i2s_audio_out", …)` 把通道再交给 espaudiocore；**保持 esp_codec_dev 独占** |
| 8 | `is_running()` 含暂停态、`stop()` 阻塞 | 头文件 245/315-316 | 自维护 `s_running`；不要在事件回调里调 `stop()` |
| 9 | build 集成 | 根 `CMakeLists.txt:22-27` | 把 `libs` 加进 `EXTRA_COMPONENT_DIRS`（或 `override_path`），并在组件 `REQUIRES` 里替换 GMF 依赖 |

### 9.5 一句话结论

> 现有能力里，**解码 / seek（本地）/ 位宽 / 元数据 / 位置** 可以干净地映射到 espaudiocore；
> 而 **HTTP 传输、进度、速度统计、断点续播（在线）、重采样、声道转换、输出三路路由、PA 与静音时序、
> 音量百分比语义、事件到 `audio_evt_t` 的映射** 必须在 `audio_service` 这一层自己实现。
> 又因为本板 I2S 是 **TDM** 而 espaudiocore 的 I2S 输出走 **STD** API，**落地形态只能是
> "espaudiocore 出 PCM 到 ringbuf + 我们自己的输出分发"**（即 §8 拆分后的 `audio_output.cc` 原样保留）。
