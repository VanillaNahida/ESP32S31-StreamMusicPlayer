# AGENTS.md —— ESP32-S31-Korvo-1 流媒体音乐播放器

ESP32-S31 单芯片触摸音乐播放器：4.3" 800×480 屏 + LVGL 9，支持本地 SD 曲库、
在线音乐（网易云 + QQ 音乐）、蓝牙音频、USB 声卡、图库、设置、拼音搜索与语音输入。

## 1. 技术栈与版本

| 项 | 值 |
| --- | --- |
| 芯片 / IDF | ESP32-S31（RISC-V，`--preview` target），ESP-IDF 6.1 |
| UI | LVGL **9.5**（勿升 9.6），桥接用 `esp_lvgl_adapter`（不是 `esp_lvgl_port`） |
| 板级 | `esp_board_manager` + `esp_boards` 的 `esp32_s31_korvo_1` |
| 解码 | vendored 组件 `libs/espaudiocore`（不是 ESP-GMF / esp_audio_simple_player） |
| 重采样 | `espressif/esp_asrc`（S31 硬件 ASRC） |
| 拼音 | `lvgl_pinyin_ime` + `libgooglepinyin`，词库放独立 `dict` 分区 |
| 存储 | 16 MB Flash + 16 MB PSRAM；单 app 槽 11.81 MB，无 OTA A/B |
| 音频硬件 | ES8389（I2S TDM，48 kHz/16 bit）+ 扬声器 / BT A2DP / USB UAC |

## 2. 目录结构

```
main/                    入口（初始化顺序、启动图）、串口诊断命令
components/
  bsp/                   板级薄封装：显示 / 触摸 / 音频 / 存储 / 按键
  ui_core/               全局 UI：状态栏、路由栈、手势、Toast、控制中心、音量条、
                         APP 注册表（ui_app_registry）
  apps/
    app_music/           音乐：首页网格、本地/在线列表、播放器、歌词、频谱
    app_online/          在线音乐：歌单 / 搜索 / 账号（网易云与 QQ 音乐共用一套页面）
    app_settings/        设置：网络、蓝牙、输出、显示、麦克风、存储、关于
    app_gallery/         图库
    app_momotalk/        MomoTalk
  services/
    audio_service/       播放门面 + 解码会话 + 输出级 + HTTP 传输 + seek
    online_service/      在线接口（网易云 + QQ 音乐）、凭证、缓存、歌单
    media_meta/          ID3 / LRC / 封面 / 图片解码
    storage_service/     本地曲库扫描、SD 卡热插拔
    wifi_service/        Wi-Fi 连接与设置
    bt_service/          A2DP Source、AVRCP、设备名缓存
    usb_audio_service/   USB 声卡（UAC 1.0）输出
    voice_service/       讯飞 IAT 语音输入
    play_state/          音量 / 曲目 / 进度的 NVS 记忆（防抖落盘）
  ui_font/               内嵌位图字体（GB2312 + 日文，fallback 链）
libs/espaudiocore/       解码库（vendored）
docs/                    详细设计与踩坑记录（见第 7 节）
tools/                   idf.ps1、字体/图标生成、代码检查
```

## 3. 分层与线程规则

- 依赖方向：`apps` → `services` → `bsp` → `esp_board_manager`。
  `apps/` 不直接调 LCD / I2S / Wi-Fi / 蓝牙 API；`bsp/` 只做薄封装，不写寄存器序列。
- `ui_core/` 不读写 NVS / 文件系统；跨任务共享状态走 `play_state` 或各 service 的查询接口，
  不用裸全局变量。
- 所有 LVGL 调用必须在 LVGL 任务上下文；其他任务用 `esp_lv_adapter_lock()/unlock()`。
- 页面切换走 `ui_router_push()/pop()`，不要裸调 `lv_screen_load()`。
- 新增桌面 APP：定义 `app_desc_t` 并在 `ui_core_init()` 前注册，首页自动铺网格。

## 4. 音频链路

```
本地文件 / 在线 HTTP
  → libs/espaudiocore 解码（PCM 16-bit）
  → PCM ringbuf（PSRAM）
  → audio_output：esp_asrc → 48 kHz/16bit/2ch
  → ES8389(TDM) / 蓝牙 / USB / 静音
```

- 本板 I2S 是 **TDM**，espaudiocore 的 I2S 输出模式不可用，必须
  “解码到 ringbuf + 自己写 `esp_codec_dev_write()`”。
- 写硬件的 PCM 恒为 **48 kHz/16bit/2ch**，时间轴统一 `ms = 字节数 ÷ 192`。
- 重采样只能用 **S31 硬件 ASRC**（软件路径会吃满一个核并触发 WDT）。
- 会话锁 `audio_core_lock()` 只保护起播/停止/seek 重开；输出任务与事件回调绝不取锁，
  也禁止在回调里阻塞或调用 LVGL。

## 5. 构建与烧录

本机 `idf.py` 会被 PowerShell 执行策略挡住，统一用封装脚本：

```powershell
& .\tools\idf.ps1 build
& .\tools\idf.ps1 -p COMx flash monitor
```

- 首次克隆 / 换板先跑 `idf.py bmgr -b esp32_s31_korvo_1` 生成 `components/gen_bmgr_codes/`。
- 烧录波特率用环境变量 `ESPBAUD`（本机 921600），不要用命令行 `-b`
  （会连带把 monitor 也改到 921600，而 console 是 115200，刷完是乱码）。
- 改了 `partitions.csv` 必须 `idf.py fullclean` 后重编；`dict` 分区需单独烧一次，
  否则拼音输入法起不来。

## 6. 硬性约定

- **LVGL 9 API**：不要用 v8 的 `lv_obj_del` / `lv_btn_create` / `lv_disp_*` 等。
- UI 代码禁止出现 800 / 480 字面量，用 `ui_metrics.h` 的 `UI_W` / `UI_H`、
  百分比与 flex 布局。
- 页面 screen 必须调用 `ui_screen_apply_default_bg(scr)`（只设背景色会因
  `bg_opa=0` 透出上一屏内容）。
- `UI_COLOR_*` 是 `lv_color_t`，当整数用时必须
  `lv_color_to_u32(...) & 0xFFFFFF`（高字节恒 `0xFF`）。
- 图标用矢量/位图，不用 emoji；中文最小 14 px；中文用 `components/ui_font` 的位图字体，
  不要用 LVGL 内置 CJK 字体。
- 本板无电池、无背光控制：状态栏不要画电池，息屏只停 LVGL 刷新。

## 7. 详细文档

- 音频架构与线程纪律：[docs/audio-stack.md](docs/audio-stack.md)
- LVGL 布局踩坑：[docs/lvgl-pitfalls.md](docs/lvgl-pitfalls.md)
- 大文件拆分说明：[docs/refactor/README.md](docs/refactor/README.md)
- 串口诊断命令：[docs/serial-commands.md](docs/serial-commands.md)
- USB 声卡：[docs/uac-audio.md](docs/uac-audio.md)
- 图库：[docs/gallery.md](docs/gallery.md)
- QQ 音乐：[docs/qq-music.md](docs/qq-music.md)
- 开发指南：[docs/方屏流媒体音乐播放器 开发指南.md](docs/方屏流媒体音乐播放器%20开发指南.md)
