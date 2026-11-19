# ESP32-S31 流媒体音乐播放器

基于 ESP32-S31 单芯片的触摸屏流媒体音乐播放器，使用 C/C++、ESP-IDF 6.1 与 LVGL 9.5 构建。

- 目标硬件：ESP32-S31-Korvo-1 V1.1（ESP32-S31-WROOM-3，16 MB Flash + 16 MB PSRAM）
- 联网、经典蓝牙、UI、解码、重采样全部运行在同一颗芯片上，无协处理器，不使用 ESP-Hosted
- UI 为手写 LVGL 9 组件，暗色卡片风格，横屏 800 x 480

> 本项目面向特定开发板。LCD 时序、Codec 引脚、功放使能与按键均来自 `esp_board_manager` 的 `esp32_s31_korvo_1` 配置；换板需要修改板级配置与 `components/bsp/`。

## 功能

- 本地曲库：递归扫描 SD 卡（`mp3` / `flac` / `m4a` / `aac` / `wav` / `ogg` / `opus`），支持 ID3v2 / FLAC 标签、内嵌封面、外挂 LRC 歌词与热插拔重扫
- 在线音乐：对接自建网易云 API 中间件，支持歌单、搜索、扫码登录、歌词与封面缓存、收藏
- 三路音频输出：板载 ES8389 + 双 NS4150B、蓝牙 A2DP（AVRCP 绝对音量）、USB 声卡（UAC 1.0）
- 播放器页：旋转封面、逐行高亮歌词、实时频谱（esp-dsp FFT）、播放队列与输出切换
- 图库：浏览 SD 卡图片，支持缩放、双击放大还原、左右滑动切换
- 设置：Wi-Fi、蓝牙、输出通道、显示、麦克风、存储、关于
- 中文拼音搜索与讯飞语音输入
- 串口诊断命令：`songinfo` / `songscan` / `playstat`

## 硬件与软件基线

| 项 | 值 |
| --- | --- |
| 主控 | ESP32-S31（RISC-V），2.4 GHz Wi-Fi 6 + 蓝牙 5.4 / 经典蓝牙 |
| 存储 | 16 MB SPI Flash + 16 MB PSRAM |
| 显示 | 4.3 英寸 800 x 480 RGB565 触摸屏（ST7262E43 / GT1151） |
| 音频 | ES8389 双声道 Codec（I2S TDM，48 kHz / 16 bit）+ 2 x NS4150B |
| 本地存储 | microSD（4-bit SDIO），挂载点 `/sdcard` |
| 输入 | 双模拟麦克风、触摸、板载按键 |

| 项 | 值 |
| --- | --- |
| 芯片 / IDF | ESP32-S31（preview target），ESP-IDF 6.1 |
| UI | LVGL 9.5，桥接组件 `esp_lvgl_adapter` |
| 板级 | `esp_board_manager` + `esp_boards` 的 `esp32_s31_korvo_1` |
| 解码 | vendored 组件 `libs/espaudiocore` |
| 重采样 | `esp_asrc`（S31 硬件 ASRC） |
| 蓝牙音频 | `esp_bt_audio`（A2DP Source + AVRCP 绝对音量） |
| 分区 | 16 MB Flash，单 app 槽 11.81 MB，`dict` 分区 3.68 MB 存放拼音词库 |

## 音频链路

```
本地文件 / 在线 HTTP
  -> libs/espaudiocore 解码（PCM 16-bit）
  -> PCM ringbuf（PSRAM，32 KB）
  -> esp_asrc 重采样（48 kHz / 16 bit / 2ch）
  -> ES8389（TDM）/ 蓝牙 A2DP / USB 声卡 / 静音
```

三条硬约束（详见 [docs/audio-stack.md](docs/audio-stack.md)）：

1. 写入硬件的 PCM 恒为 48 kHz / 16 bit / 2ch，时间轴统一为 `ms = 字节数 / 192`
2. 重采样只能使用 S31 硬件 ASRC，缓冲必须位于内部 RAM
3. 会话锁只保护起播、停止与 seek 重开；输出任务与事件回调不取锁、不调用 LVGL

## 构建与烧录

环境：Windows 10/11、ESP-IDF 6.1（`esp32s31` 为 preview target）、Python 3.10+。

依赖目录布局：

```
<工作目录>\
├── ESP32S31-StreamMusicPlayer\
│   └── libs\espaudiocore\      # 解码库（vendored，不进版本库）
└── lvgl_pinyin_ime\            # 拼音输入法（与本工程同级）
```

首次配置与构建：

```powershell
& .\tools\idf.ps1 bmgr -b esp32_s31_korvo_1   # 生成板级驱动配置
& .\tools\idf.ps1 build
& .\tools\idf.ps1 -p COMx flash monitor       # COMx 替换为实际串口
```

- 烧录速率使用环境变量 `ESPBAUD`，不要使用命令行 `-b`：`-b` 会同时改变 monitor 波特率，导致日志乱码
- 修改 `partitions.csv` 后必须 `fullclean` 并重烧分区表
- `dict` 分区（拼音词库）需要单独烧写一次，否则拼音输入法不可用
- 增量构建通过不代表从零构建通过；新增源文件时请同步更新组件的 `CMakeLists.txt`

## 在线音乐配置

在线音乐需要一个运行在 PC 或服务器上的 [NeteaseCloudMusicApi（增强版）](https://github.com/NeteaseCloudMusicApiEnhanced/api)，默认监听 `3000` 端口。

`idf.py menuconfig` -> Online music account：

| 配置项 | 说明 |
| --- | --- |
| `ONLINE_NETEASE_API_BASE` | 服务根地址。ESP32 上的 `localhost` 指设备自身；服务在 PC 上时填写 PC 的局域网地址，例如 `http://192.168.1.100:3000` |
| `ONLINE_NETEASE_API_TOKEN` | API Key，留空表示不发送鉴权头 |
| `ONLINE_NETEASE_CK_FILE` | 可选的网易云 cookie 文件，用于免扫码登录；默认路径 `assets/ncm-ck.txt`，该文件已被 `.gitignore` 忽略 |

服务地址可在运行期通过 NVS 覆盖，无需重新烧写固件。cookie 属于账号凭证，请勿提交到仓库。

## 常见问题

- 构建报 `lvgl_pinyin_ime not found`：将输入法克隆到与本工程同级的目录，或用 `-DLVGL_PINYIN_IME_DIR=...` 指定路径
- 构建报 `libs/espaudiocore` 相关错误：解码库不在版本库中，需自行放入 `libs/espaudiocore/`
- 构建只打印环境横幅：从 git-bash / MSYS 启动的 PowerShell 会继承 `MSYSTEM`，需要清除后再运行
- 蓝牙扫描一直停留在初始化：控制器模式需为 `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y`，并开启 `CONFIG_SW_COEXIST_ENABLE=y`
- 44.1 kHz 音源卡顿或变调：重采样必须使用硬件 ASRC，不要选择软件路径 `SW_SPEED`
- 在线音乐报 401 / 400：检查 API Key 与 cookie 请求头，并将 `localhost` 替换为实际服务地址

## 项目结构

```
main/                    入口、启动图、串口诊断命令
components/
  bsp/                   板级封装：显示 / 触摸 / 音频 / 存储 / 按键
  ui_core/               状态栏、路由、手势、控制中心、APP 注册表
  apps/                  app_music / app_online / app_settings / app_gallery / app_momotalk
  services/              audio / online / media_meta / storage / wifi / bt / usb_audio / voice / play_state
  ui_font/               内嵌位图字体
libs/espaudiocore/       解码库（vendored，不进版本库）
assets/                  字体、图标、背景图、启动图
docs/                    设计文档与踩坑记录
tools/                   idf.ps1、字体与图标生成、自查脚本
```

## 文档

- [docs/audio-stack.md](docs/audio-stack.md)：音频链路与线程纪律
- [docs/lvgl-pitfalls.md](docs/lvgl-pitfalls.md)：LVGL 布局与渲染踩坑
- [docs/serial-commands.md](docs/serial-commands.md)：串口诊断命令
- [docs/uac-audio.md](docs/uac-audio.md)：USB 声卡实现
- [docs/gallery.md](docs/gallery.md)：图库实现
- [AGENTS.md](AGENTS.md)：工程约束速查

## 致谢

LVGL、esp_board_manager、esp_lvgl_adapter、esp_asrc、esp-dsp、esp_bt_audio、lvgl_pinyin_ime、NeteaseCloudMusicApi（增强版）；字体来自 DreamHanSansSC 与文泉驿微米黑。

## 反馈

问题请提交 [Issue](https://github.com/VanillaNahida/ESP32S31-StreamMusicPlayer/issues)，并附上构建日志与串口日志的关键片段。

本仓库转载、二次分发或商用前请联系作者。在线音乐功能依赖第三方 API，仅供个人学习研究。
