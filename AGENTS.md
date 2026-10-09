# AGENTS.md —— ESP32-S31-Korvo-1 流媒体音乐播放器

本文件给在本仓库工作的 AI Agent / 开发者提供**必须遵守的事实与命令**。与
`方屏流媒体音乐播放器 开发指南.md` 冲突时，以官方文档与本文件记录的实测事实为准。

> ⚠ **本文件已超过 64 KB 的指令预算，载入时尾部会被截断**（近期丢的是 §15.3 之后的内容）。
> 磁盘上的文件是**完整**的 —— 需要最新几节（§14.11 之后：账号歌单分页/懒加载、
> 详情页返回行、开机图片 §16）时，**直接用 read 工具打开本文件**按行号读，
> 不要以为"文件里没有"。长期解法是把深挖记录拆到 `docs/` 下分册、本文件只留索引。

---

## 1. 第一原则：官方文档优先

编程开发**严格参考** [ESP-IDF 官方文档（ESP32-S31 版）](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s31/index.html)。

涉及芯片能力、驱动 API 函数签名、Kconfig 名称与默认值、`idf.py` 用法、分区表规则、OTA
流程，一律以官方为准，**不要凭记忆或旧版本经验编码**。板级外设（LCD / 触摸 / ES8389 /
按键 / RGB LED）严格参考 `esp_board_manager` 与 `esp_boards` 的 `esp32_s31_korvo_1` 板级
YAML；拼音输入法严格参考 `lvgl_pinyin_ime` 的 `docs/`。

## 2. 版本锁定

| 项 | 值 |
| --- | --- |
| ESP-IDF | **6.1.0**（`D:\Espressif\EIM\v6.1\esp-idf`），`esp32s31` 为 **preview** target |
| LVGL | **9.5.0**（`lvgl/lvgl: "~9.5.0"`，勿升 9.6：拼音字体结构体按 9.5 修过） |
| 板级管理 | `espressif/esp_board_manager` 0.7.3 + `espressif/esp_boards` 0.6.x |
| LVGL 桥接 | **`espressif/esp_lvgl_adapter`**（**不是** `esp_lvgl_port`，见第 4 节） |
| 拼音输入法 | `lvgl_pinyin_ime`（`override_path: ../../lvgl_pinyin_ime`） |
| 工具链 | `riscv32-esp-elf`（ESP32-S31 是 RISC-V） |

**警告：不要使用 LVGL v8 API。** LVGL 9 已移除 `lv_obj_set_scrollable` /
`lv_obj_set_clickable` / `lv_obj_set_hidden` / `lv_btn_create` / `lv_obj_del` / `lv_disp_*`，
改用 `lv_obj_add_flag/remove_flag(obj, LV_OBJ_FLAG_*)`、`lv_button_create`、`lv_obj_delete`、
`lv_display_*`。

## 3. 构建 / 烧录命令（含本机必需的环境变量）

本机网络走本地代理，**组件管理器拉取依赖时必须显式设置 `https_proxy`**，否则会出现
`WARNING: Component "espressif/xxx" not found` 这类误导性的解析失败（`http_proxy` 已存在，
但 Python 的 requests 对 https 只认 `https_proxy`）。

```powershell
. "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
# 本工程是用 EIM 的 IDF python venv 配置的；export.ps1 默认指向
# D:\Espressif\python_env\idf6.1_py3.12_env，两者不一致时 idf.py 会报
# "is currently active in the environment while the project was configured with ..."。
# 统一用下面这个（与 build/ 的配置一致，免 fullclean）：
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\EIM\tools\python\v6.1\venv"
$env:PATH = "D:\Espressif\EIM\tools\python\v6.1\venv\Scripts;" + $env:PATH

$env:https_proxy = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
# 未安装 esp-bmgr-assist 时，需让 idf.py 找到 board manager 扩展：
$env:IDF_EXTRA_ACTIONS_PATH = "$PWD\managed_components\espressif__esp_board_manager"

idf.py --preview set-target esp32s31     # --preview 不能省
idf.py bmgr -b esp32_s31_korvo_1         # 生成 components/gen_bmgr_codes/
idf.py build
idf.py -p COMx flash monitor
```

- **换板 / 首次克隆后必须先跑 `idf.py bmgr -b esp32_s31_korvo_1`**，否则
  `components/gen_bmgr_codes/` 不存在，CMake 会失败。
- `components/gen_bmgr_codes/board_manager.defaults` 的优先级**高于**项目
  `sdkconfig.defaults`。改了 `sdkconfig.defaults` 没反应时，先删 `sdkconfig` 与
  `sdkconfig.old` 再重编。
- 改了 `partitions.csv` 必须 `idf.py fullclean` 后重编。

### 3.1 烧录波特率：IDF 6.1 里**没有** Kconfig 了，只能用 `ESPBAUD`

**别再往 `sdkconfig.defaults` 里写 `CONFIG_ESPTOOLPY_BAUD`** —— ESP-IDF 6.1 已经删掉了
这个符号：`sdkconfig` 里 `CONFIG_ESPTOOLPY_*` 只剩 FLASHMODE / FLASHFREQ / FLASHSIZE /
BEFORE_RESET / AFTER_RESET / **MONITOR_BAUD** / NO_STUB，整个 IDF 树里也搜不到
`ESPTOOLPY_BAUD`。写进去只会变成"未知配置项"。

现在烧录波特率只有两个入口（`tools/idf_py_actions/serial_ext.py`）：

| 入口 | 说明 |
| --- | --- |
| `idf.py -b 921600 flash` | 命令行；**默认 460800** |
| 环境变量 `ESPBAUD` | 这个工程的默认配置方式（本机已 `setx ESPBAUD 921600` 持久化） |

⚠ **务必用 `ESPBAUD`，不要用命令行 `-b`**：`serial_ext.py` 里
`global_baud_defined = ctx._parameter_source['baud'] == ParameterSource.COMMANDLINE` ——
**命令行**给的 `-b` 会**连带把 monitor 也改成同一个波特率**，而固件 console 是
115200（`CONFIG_ESP_CONSOLE_UART_BAUDRATE`），于是 `idf.py -b 921600 flash monitor`
刷完能看到的就是一片乱码。走 `ESPBAUD` 环境变量时 monitor 仍取
`CONFIG_ESPTOOLPY_MONITOR_BAUD`(115200)，一切正常。
（真要用 `-b` 就再显式补 `--monitor-baud 115200`。）

取值依据（本机实测的设备信息，2026-10）：

- 烧录口 COM20 是 **CP2102N**（`BusReportedDeviceDesc = "CP2102N USB to UART Bridge
  Controller"`，VID_10C4/PID_EA60），桥本身支持到 **3 Mbps**，不是瓶颈；
- 但本工程 `CONFIG_ESPTOOLPY_NO_STUB=y`（IDF 里**没有 esp32s31 的 flasher stub**），
  全程由 ROM 下载器写 flash，波特率拉太高容易握手失败 / 校验错。
- 所以默认取 **921600**（IDF 历史预设里的最高档，CP210x + ROM 下载器的稳妥上限）；
  想再快可以试 `ESPBAUD=1500000`（CP2102N 带得动，但 ROM 侧不保证），
  失败就退回 921600 —— 失败表现是 esptool 连不上/`Failed to connect`，不会损坏芯片。
- 提速空间顺带记一笔：真正的大头是 `NO_STUB`（ROM 写 flash 很慢）。哪天 IDF 补上
  esp32s31 的 stub，把 `CONFIG_ESPTOOLPY_NO_STUB` 关掉比继续拉波特率收益大得多。

## 4. 关键架构决策（与开发指南不同之处，务必遵守）

| 决策 | 原因 |
| --- | --- |
| **LVGL 桥接用 `esp_lvgl_adapter`，不用 `esp_lvgl_port`** | `esp_board_manager` 官方的 `display_lvgl` 示例即用该组件；它原生支持 RGB 面板的 tear-avoid 与旋转，`esp_lvgl_port` 不具备这些能力 |
| **`main/idf_component.yml` 不写 `esp_lvgl_port`** | 同上 |
| **拼音引擎 `libgooglepinyin` 走 `EXTRA_COMPONENT_DIRS`** | 它是 vendored 引擎，组件不递归搜索子目录；已在根 `CMakeLists.txt` 注册 |
| **`LV_PINYIN_IME_EMBED_DICT=OFF`** | 词库放 `dict` 分区，省 1 MB app 空间 |

## 5. 板级事实（来自官方 `esp32_s31_korvo_1` 板级 YAML，勿凭猜测写引脚）

| 外设 | 事实 |
| --- | --- |
| 主控 | 单芯片 ESP32-S31-WROOM-3，16 MB Flash + 16 MB PSRAM（OCT，200 MHz） |
| 屏幕 | 4.3" **800×480 RGB565**，`generic_rgb`，PCLK=40 / DE=43 / HSYNC=44 / VSYNC=45，数据线 GPIO8-19,33-36，**framebuffer 放 PSRAM（num_fbs=1）** |
| 背光 | **官方板级配置里没有背光设备（无 LCD_BRIGHTNESS 器件）** → 软件无法调背光；`bsp_backlight_set()` 只做记录/占位，息屏只能停 LVGL 刷新 |
| 触摸 | **GT1151**，I2C0 地址 0x14(7bit)/0x28(8bit)，**`int_gpio_num = -1`、`rst_gpio_num = -1`（没有中断脚）** → 驱动内部轮询，指南里"必须用中断"不适用于本板 |
| 音频 Codec | **ES8389**，I2C0 SDA=0/SCL=1（地址 0x20）；I2S 端口0，bclk=3 / ws=4 / dout=5 / din=6，**MCLK=-1（无 MCLK）**；**DAC/ADC 都是 TDM 格式**，48 kHz 16 bit |
| 功放 | **`PA_CTRL` = GPIO7，高电平使能**（板级 `active_level: 1`）；左右共用一个使能脚 |
| microSD | **4-bit SDMMC slot0**：clk=24 / cmd=25 / d0=20 / d1=21 / d2=22 / d3=23；`SD_CTRL`=GPIO39 **低电平使能**（`active_level: 0`）；挂载点 `/sdcard` |
| 按键 | `adc_button_group`，**GPIO42 / ADC1 ch0**。**以板级 YAML 为准**（`managed_components/espressif__esp_boards/esp32_s31_korvo_1/board_devices.yaml`）：`button_num: 6`、`max_voltage: 3000`、`voltage_range: [380, 820, 1180, 1570, 1980, 2410]`、`button_labels: ["VOLUME_UP","VOLUME_DOWN","SET","PLAY","Mode","REC"]` —— 即 **380 mV = VOLUME_UP、820 = VOLUME_DOWN、1180 = SET、1570 = PLAY、1980 = Mode、2410 = REC**。驱动按电压**升序**建句柄、窗口取相邻档位中点（`dev_button_sub_adc_multi.c`）。⚠ 旧文档里"SET=380/MODE=820/VOL-=1340/VOL+=1870"那套是错的，别再用 |
| RGB LED | WS2812，**GPIO37**，RMT |
| Boot 键 | GPIO61（低有效） |
| 供电 | USB 5 V 常供，**无电池、无电量计、无软关机**；状态栏禁止画电池 |

**已作废、禁止再写进代码的方案**：ESP32-P4 / ESP32-C5 双芯、ESP-Hosted、NT26 4G、GPS、
BQ27220、TCA9555、外挂蓝牙音频模块（UART AT）、SPI SD 模块、5 GHz Wi-Fi、Metalio Claw4、
AMOLED、ES8311。

## 6. 分层规则

- `apps/` 不直接调用 LCD / I2S / 蓝牙 / Wi-Fi 底层 API，一律走 `services/`
- `ui_core/` 不直接读写 NVS / 文件系统
- 板级外设初始化统一由 `esp_board_manager` 完成，`bsp/` 只做薄封装，**不写寄存器序列**
- 跨任务状态变更走 `event_bus`，不用裸全局变量
- 所有 LVGL 调用必须在 LVGL 任务上下文，其他任务用 `esp_lv_adapter_lock()/unlock()`

## 7. UI 规则

- **任何 UI 代码都不准出现 800 / 480 字面量**，一律用 `ui_metrics.h` 的 `UI_W` / `UI_H` 与百分比
- 布局用 flex + 百分比，不用绝对坐标
- 播放器页是**横屏左右分栏**（左封面 / 右歌词与信息）
- 图标用矢量/位图，**不用 emoji**
- 800×480 下中文最小 14 px

## 8. 字体规则

- UI 文本：**梦源黑体** `assets/fonts/DreamHanSansSC-W17.ttf`
- 数字（时钟/日期/进度/百分比）：**文泉驿等宽微米黑**
  `assets/fonts/wqy-microhei-mono-0.2.0-beta.ttc`（TTC 需先取 **face 1**）
- **`CONFIG_LV_FONT_FMT_TXT_LARGE` 必须保持关闭**（拼音字体按紧凑 8 字节 glyph 描述符生成）
- **UI 实际用的是编译进去的三套位图字体**（`components/ui_font/`），16 px 梦源黑体，
  按 LVGL 的 `fallback` 链串起来（`ui_font.c` 里的 `ui_font_text`）：

  | 字体 | 覆盖 | bpp | 真实 flash |
  | --- | --- | --- | --- |
  | `ui_font_sc16` | GB2312 全集 + ASCII + LVGL 图标 | 4 | **976 KB** |
  | `ui_font_jp1_16` | JIS X 0208 第 1 水準（2965 字）+ 日文记号（々〜等） | 4 | **146 KB** |
  | `ui_font_jp2_16` | JIS X 0208 第 2 水準（3390 字里的 1940 个 GB2312 没有的） | 2 | **137 KB** |

  **为什么必须有日文那两份**：GB2312 是简体中文字符集，日文汉字的**码位**不在其中
  （`鎖` U+9396 / `転` U+8EE2，中文是 `锁`/`转`，码位不同），
  于是"鎖那 - 夏休みの転校生"这类日文曲名会缺字成方框。日文曲库是刚需，别删。
  jp2 用 2 bpp 是为了压进 app 分区余量；**3 bpp 绝对不能用** —— LVGL 的未压缩
  字形路径只实现了 1/2/4/8 bpp（`lv_font_fmt_txt.c` 的 `LV_FONT_FMT_TXT_PLAIN`
  分支），3 bpp 只有压缩路径才有，写 `--bpp 3` 会整片空白。
  重新生成：
  ```
  tools/font/venv/Scripts/python.exe tools/font/gen_subset.py   # 先切字符集（三个 TTF）
  python tools/font/gen_lvgl_font.py                            # 再出三个 .c
  ```
  ⚠ 估体积**不要**数 `0xNN` 字面量：字形位图里还有大批十进制描述符，会少算约
  40%（实测 559 KB vs 真实 976 KB）。真实占用这样量：
  `riscv32-esp-elf-size build/esp-idf/ui_font/libui_font.a`
- 不要用 LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk` 显示中文：它只收
  "1338 most common CJK radicals"，GB2312 其余汉字会渲染成**缺字方框**。
  `lv_font_montserrat_*` 完全没有汉字，禁止用于含中文的标签。

## 9. Python 依赖

**一律用组件自带的 `tools/venv`，禁止全局 `pip install`。**

## 10. 验证方式

- 改完必须 `idf.py build` 通过（贴出结果摘要，不只说"已完成"）
- 涉及 UI 的改动要在模拟器 `simulator/index.html` 或真机截图确认
- 纯逻辑（LRC / ID3 / JSON / 路径处理）抽成不依赖硬件的模块，便于 host 侧单测
- 改完大文件（`app_online.cc` / `online_service.cc`）先跑一遍括号/引号配平自查
  （跳过注释与字符串，抓"少写一个 `}`"这类低级错）：
  `tools/font/venv/Scripts/python.exe tools/check_balance.py <文件...>`
  —— 它不是编译器，只做配平，别当编译验证用

## 11. 在线音乐 / 无线开关 / 拼音输入法（2026-10 新增）

### 11.1 分区表（app 槽现在是 7.5 MB，见 `partitions.csv`）

拼音输入法的两套字体是**编进 app** 的（`CONFIG_LV_PINYIN_IME_FONT_SRC_EMBEDDED=y`，
16 px 0.97 MB + 20 px 1.41 MB），app 从 ~5.3 MB 涨到 ~6.70 MB，原来的 6 MB app 槽
装不下了。后来又加了 UI 位图字体的日文两份（+286 KB，见第 8 节），
所以现在的布局是：

| 分区 | 大小 | 说明 |
| --- | --- | --- |
| `nvs` / `otadata` / `phy_init` / `coredump` | 24K / 8K / 4K / 64K | coredump 关了就没有崩溃转储，别随便删 |
| `dict` | 1.5 MB（`0x180000`） | 拼音**系统**词库，必须单独烧（见 11.2） |
| `usr` | 384 KB | 拼音**用户**词库（FAT，autosave） |
| `ota_0` / `ota_1` | **7.5 MB（`0x6F0000`）× 2** | `ota_0` 起于 `0x210000`，`ota_1` 起于 `0x900000` |

- `assets`(SPIFFS, 128 KB) **已在 2026-10 删掉**（整块给了 app 槽）：本工程只有
  `esp_mmap_assets` 这个传递依赖，**没有任何代码引用它**（grep 过）。
- 末尾还有 **64 KB 未分配**；再要腾地方只剩：这 64 KB、`coredump`(64 KB)、
  `usr`(384 KB) 三处。
- **改了分区表必须 `idf.py fullclean` 后重编**（AGENTS 老规矩），
  并且要重烧分区表 —— `idf.py flash` 会写它。
  `idf.py flash` 每次都会写 **空的 `otadata`**（`flash_args` 里有
  `0xf000 ota_data_initial.bin`），所以换布局后不会因为 OTA 状态指向旧偏移而起不来。

### 11.2 `dict` 分区必须单独烧一次（否则输入法起不来）

拼音系统词库（1.07 MB）放在 `dict` 分区，**`idf.py flash` 不会写它**：

```powershell
cd D:\Codes\lvgl_pinyin_ime
powershell -ExecutionPolicy Bypass -File tools\flash_dict.ps1 -Port COM20 -Dict
```

没烧过时分区里是历史残留数据（实测读出来是别的分区的旧内容），
`lv_pinyin_ime_create()` 会失败 → 搜索框的拼音键盘建不出来。

### 11.3 `LV_PINYIN_IME_EMBED_DICT=OFF` 的链接坑（已修）

组件的 `src/data/ime_dict.c` 里 `prepare_embedded()` **无条件**引用
`_binary_dict_pinyin_dat_start/_end`，而这两个符号只在 `LV_PINYIN_IME_EMBED_DICT=ON`
时由它的 CMake 注入。本工程为了省 1 MB 设成 OFF，于是**第一次真正链接这个组件**时
链接期就报 undefined reference。修法：`main/ime_dict_stub.c` 提供一对**等地址**的空符号
（blob_size 算出来是 0，`prepare_embedded()` 直接返回 false，走 dict 分区），
并在 `main.cc` 里主动引用一次把该成员从 `libmain.a` 里拉出来
（静态库只按"当前未定义符号"收成员，组件库排在 main 之后，不引用就永远拉不出来）。

### 11.4 在线音乐（`components/services/online_service`）

Meting API（`http://meting.xcnahida.cn/...`，**明文 http** 就够，省掉 TLS/校时依赖）：

| 用途 | 接口 |
| --- | --- |
| 歌单 | `/api?server=netease&type=playlist&id=<歌单ID>` |
| **搜索** | `/meting/api?server=netease&type=search&id=<关键词>`（关键词在 `id=`，内部做百分号编码） |
| 播放地址 | `type=url&id=<歌曲ID>`（302 到 https CDN） |
| 歌词 | `type=lrc&id=`（纯文本 LRC，UTF-8） |
| 封面 | `type=pic&id=`（302 到 jpg） |

⚠️ `/api?type=search&keywords=xxx` 在该实例上**忽略关键词、返回固定列表**，别用。

- 歌单落盘：`/sdcard/online/pl_<歌单ID>.bin`（magic 'OPL1' + 定长记录数组，**二进制**）
- 播放缓存：`/sdcard/online/audio/<歌曲ID>.<ext>`（扩展名按**嗅探文件头**决定，ASP 靠扩展名选解码器）
- 封面 / 歌词：**只缓存在 PSRAM，不写 SD 卡**（2026-10 改；根因是 SD 读写阻塞切歌）。
  封面字节走 `online_song_cover_data()`（返回调用方所有的 malloc 拷贝，JPEG/PNG 原样），
  歌词文本走 `online_song_lyrics_data()`（UTF-8，直接喂 `media_meta_lrc_parse`）；
  播放端解码用 `media_meta_image_mem_rgb565()`（内存版，与 file 版同一套解码器）。
  历史遗留的 `/sdcard/online/cover/`、`/sdcard/online/lrc/` 目录与 `audio/*.lrc`
  文件在 `online_service_init()` 时清掉；`online_cache_clear()` 连 PSRAM 缓存一起清。
- 歌单 ID 存 NVS `online`（`n` + `id0..id7`，最多 8 个）

**设计选择：默认流式播放**（`online_song_stream_info` 解析 302 拿到 https 音频地址，
`audio_service_play_uri()` 边下边播；流地址拿不到才回退"先下载到 SD 再当本地文件播"）。
封面 / 歌词在任何一条路径下都只进 PSRAM 缓存，不再写 SD。

### 11.5 Wi-Fi / 蓝牙开关与开机自动连

- 开关状态存各自 NVS：`wifi_cfg/on`、`bt_cfg/on`（默认开）；UI 在设置页顶部（`lv_switch`）
- 关掉会**真正释放资源**：WiFi `esp_wifi_stop+deinit` + 销毁 netif + 停 SNTP；
  蓝牙拆掉 PCM 通路 + `esp_bt_audio_deinit()` + 控制器 `disable+deinit`
  （**不要**调 `esp_bt_mem_release()`，那会让它再也起不来）
- 开机自动连：WiFi 连最近保存的 AP（原有逻辑）；蓝牙连"上次连过的设备"
  （`bt_cfg/last`，在 A2DP 连上时写入，init 后延时 3 秒发起）
- **SNTP**：WiFi 拿到 IP 后在事件回调里 `esp_netif_sntp_init("ntp.aliyun.com")`，
  顺便 setenv TZ=CST-8 —— 既让首页时钟不再"等待校时"，也让 https 证书校验有正确时间

#### 11.5.1 连不上时别乱猜：先看 reason（2026-10 修）

`wifi_service` 现在把断开原因码翻成人话打出来，例如
`connect 'X' failed: reason 15 (四次握手超时：密码错)`。
排查"配网连不上"就看这一行：**15/202/204/MIC 失败 = 密码错**，
**201 = 没找到 AP**（信号弱或只在 5G），**210/211 = 加密方式对不上**（AP 只用
WPA3 或企业级 802.1X），**8 = 本方主动离开**（不是故障）。
以前只打一句"密码错/找不到？"，看不出到底是哪种。

#### 11.5.2 三个必须守住的连接纪律

1. **不要无条件 `esp_wifi_disconnect()` 再 `esp_wifi_connect()`**。
   前者会取消正在进行的连接并**异步**投递一个 `WIFI_REASON_ASSOC_LEAVE` 事件；
   紧跟的 connect 会被这个事件当场判成失败 —— 日志特征就是
   `connecting to 'X'` 之后**几毫秒**（实测 6 ms）就 `connect failed`，然后 4 秒一轮
   永远连不上（正常的认证/握手失败至少几十到几百毫秒）。现在的做法：
   只有 `prev == CONNECTED || prev == CONNECTING` 才断开；
   `on_wifi_evt` 里**忽略** `WIFI_REASON_ASSOC_LEAVE`（自己人干的，不算失败）。
2. **自动重试绝不能走 `wifi_service_connect()` / `wifi_service_connect_saved()`**。
   这两个是"用户主动连"的入口，会调 `wifi_retry_budget_refill()` 把额度补满 ——
   原来 `wifi_retry_cb` 走的就是 `connect_saved()`，于是每重试一次额度就回到 5，
   日志里永远 `"4 retry left"`，`WIFI_RETRY_MAX` 形同虚设、无限重连。
   重试必须走 `wifi_connect_raw()`（内部实现，不碰额度）。
3. **重试节奏是两级的**：前 5 次每 4 秒（快速恢复），之后转 **60 秒一次**的慢重试
   （`WIFI_RETRY_SLOW_MS`），**不放弃**。密码错时不刷屏，AP 重启/走出盲区还能自己回来。
   补满额度只有两个时机：连上了（`on_ip_evt`）、用户主动连一次。

#### 11.5.3 `threshold.authmode` 按实际加密方式填

以前一律 `WIFI_AUTH_OPEN`（注释写"加密/开放都接受"），但只要给了密码，IDF 就会打
`Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2`
再从内部改掉 —— 这句警告正好夹在 `connecting to` 与 `connect failed` 之间，极容易被
误当成失败原因。现在：扫描结果里有这个 AP 就用它报的 `authmode`，没有（开机自动连/
重试）就"有密码 = WPA2_PSK，没密码 = 开放"。

#### 11.5.4 密码浮层的 SSID 必须自己存一份

`app_settings` 的密码输入浮层原来把列表行上的 `ssid_copy`（堆内存，行 DELETE 时
free）当作键盘 READY 事件的 `user_data`。而 `wifi_tick` 每 500 ms 就可能
`wifi_rebuild_list()` → `lv_obj_clean()` 删掉整列 —— 用户正输密码时列表一刷新，
那个指针就悬垂了，按"确认"等于拿已释放内存当 SSID/密码去连。
现在浮层用**自己的** `s_pw_ssid[WIFI_SSID_MAX]`（生命周期与浮层一致）。
凡是"把回调要用到的指针挂在别处、而那个 owner 可能被重建"的地方，都要这样copy 一份。

### 11.6 拼音搜索

搜索浮层用 `lvgl_pinyin_ime`（`lv_pinyin_ime_create()` + `attach(textarea)`，
组件初始化是**同步**的，第一次点搜索会卡约 1 秒加载词库）。
`app_online` 里的 IME 实例只建一次，关浮层时 `hide` 不销毁 —— 重复建会反复加载词库与字体。

### 11.7 ⚠ 补丁过的托管组件：`gmf_audio` 里 3 个元件（2026-10）

**这些是改在 `managed_components/` 里的**，`idf.py update-dependencies`、
删 `managed_components/` 或换 `dependencies.lock` 都会把它们冲掉。改完记得回来补。

补丁位置（同一个根因，三处）：

| 文件 | 元件 | 补丁前会崩的行 |
| --- | --- | --- |
| `esp_gmf_rate_cvt.c` | `aud_rate_cvt` | `samples_num = in_load->valid_size / ...` |
| `esp_gmf_bit_cvt.c` | `aud_bit_cvt` | 同上 |
| `esp_gmf_ch_cvt.c` | `aud_ch_cvt` | 同上 |

这三个**都在在线流式播放的管线里**（io_http → aud_dec → aud_rate_cvt → aud_bit_cvt
→ aud_ch_cvt → out），所以只补 rate_cvt 不够 —— 切歌/起播时崩在哪个元件上是随机的。

**症状**：在线流式播放时（起播或切歌）偶发整机 panic 重启：
```
E ESP_GMF_PORT: ACQ IN, there is no payload, el:0x...-aud_rate_cvt
Guru Meditation Error: Core 0 panic'ed (Load access fault).
esp_gmf_rate_cvt_process at esp_gmf_rate_cvt.c:111
   samples_num = in_load->valid_size / (rate_cvt->bytes_per_sample);
MTVAL = 0x8   ← NULL + valid_size 的偏移
```

**根因**：`esp_gmf_port_acquire_in()` 在"上游这一轮还没产出"时会把 `in_load`
留成 **NULL** 并返回 `ESP_GMF_IO_FAIL`（`esp_gmf_port.c` 里那句
"ACQ IN, there is no payload"），而这三个元件紧接着就 `in_load->valid_size` 解引用 ——
它们真正检查 `load_ret` 的代码在解引用**之后**，来不及。触发时序**完全正常**：
解码器第一轮只吃掉容器头/ID3、还没吐音频帧，同一个 GMF 任务就已经走到转换元件了
（日志 `aud_dec_open` → `aud_rate_cvt_open` → 报错 → panic 就是这个顺序）。

**修法**：`in_load == NULL` 时**返回 OK 让任务下一轮再来**（中间元件的正常空转，
不能报 FAIL —— 那会让整条管线以失败收场，用户看到"这首放不了"）；
`ABORT` 照旧上报（否则停止请求被吞）；拿到 payload 但 acquire 报错（真 IO 错）仍然 FAIL。
跳转到各自的 `__*_release` 标签是安全的：那里对 `in_load`/`out_load` 都有非空判断。

**谁没这个问题**：`audio_dec` 用了 `ESP_GMF_PORT_ACQUIRE_IN_CHECK` 宏，先检查后解引用 ✓。
`gmf_audio` 里其它效果元件（`alc`/`asrc`/`delay`/`drc`/`eq`/`fade`/`mbc`/`reverb`/
`sonic`/`deinterleave`）**同样有这个毛病**，但它们不在 ASP 的播放管线里，
用到再补（补法同上）。

另外记一条排查结论：`esp_gmf_pipeline_stop()` 是**同步**的
（`esp_gmf_task_stop()` 内部 `GMF_TASK_WAIT_FOR_STATE_BITS` 等 STOP 位），
所以 `play_uri_locked()` 里"先 stop 再 run"的顺序没问题，别去那边找原因。

---

## 12. 本地音乐列表 与 UI 帧率（2026-10 修复）

### 12.1 列表"显示不全"的根因：一个写死的 64

`storage_service_browse()` / `storage_service_load_dir()` 原来都把容量写死成
`STORAGE_BROWSE_MAX = 64`：一个目录超过 64 首时，**列表和播放队列都只剩前 64 条**，
而且那 64 条是 `readdir` 顺序的前 64（不是字母序前 64）—— 看起来就是"歌少了/顺序怪"。

现在的契约（别再改回去）：

- `storage_service_browse(dir, out, max)` 的**返回值可能大于 max**：那表示目录条目
  比缓冲多，返回的是真实条数，`out` 里装的是排序后的前 `max` 条。调用方据此扩容
  （`app_music.cc` 的 `local_reserve_entries()`，64 起翻倍，上限 `LOCAL_ENTRIES_MAX = 8192`）
  再调一次即可拿全。
- `storage_service_load_dir()` 按实际文件数翻倍扩容（优先 PSRAM），只有超过
  `TRACK_MAX = 8192` 才截断并打日志。
- 不变量：`app_music` 里 `s_entry_n <= s_entries_cap` —— 按页取 `s_entries[i]` 前必须成立，
  否则越界读（`local_rebuild()` 末尾有兜底钳制）。

### 12.2 列表分页（`LOCAL_PAGE_ROWS = 120`）

每行是 5~6 个 LVGL 对象，一个 2000 首的目录全建出来就是上万个对象：建页几百毫秒、
滚动每帧遍历它们。所以**列表按 120 行分页**（"‹ 上一页 / 下一页 ›"两行 + 顶部提示
"第 x/y 页 · 共 N 项"），而**播放队列仍是整个目录** —— 完整性问题用分页解决，不用截断。
翻页只重画（`local_render()`），不重读 SD 目录。

### 12.3 帧率：先看这三件事

1. **编译优化级别（头号因素）**。`sdkconfig.defaults` 里必须留着
   `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`。不写就是 IDF 默认的 `-Og`（"为调试优化"），
   而 LVGL 是纯软件渲染 —— 实测从 `-Og` 换到 `-Os`，app 从 7,177,312 B 瘦到
   6,861,408 B（**-308 KB**，余量从 0% 回到 5%），渲染循环也快一截。
   ⚠ 换成 `PERF`(-O2) 现在**只差一点点也装不下**（app 槽 7.5 MB，加了日文字体后
   余量只剩 ~121 KB），别直接改。
2. **绘制缓冲高度**：`bsp_display.cc` 里 `profile.buffer_height = 120` + `use_psram = true`。
   RGB 默认宏给的是 50 行且放内部 RAM → 一帧 10 次 flush、每次 80 KB，还占内部 RAM。
   120 行 = 一帧 4 条、192 KB 放 PSRAM。依据 esp_lvgl_adapter README「缓冲调优」。
3. **PPA 硬件加速**：`enable_ppa_accel = true`（`bsp_display.cc`）→ 适配器为 RGB565 注册
   自定义 blend/fill 处理器，填充与 alpha 混合走 PPA。前提是
   `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT == 1`。**不要**开 `CONFIG_LV_USE_PPA`（那是 LVGL 自带的
   另一条 PPA draw unit，9.5.0 下编译不过，见 `sdkconfig.defaults` 注释）。
   官方口径：PPA **显著降 CPU、通常不显著提 FPS**。

想再快一档的两个 A/B（都只需改一行，屏幕右下角有 LVGL 的 "xx FPS" 可对比）：

- `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`：双核并行渲染（S31 是 2 核），代价是**必须放弃 PPA**
  （适配器要求 PPA 时该值为 1）。全屏重绘型动画可能更快，但 CPU 占用上升。
- 给 LVGL 单独加 `-O2`（全局仍是 `-Os`）：在根 `CMakeLists.txt` 里对 lvgl 的 component lib
  用 `target_compile_options(... PRIVATE -O2)`；现在余量只有 ~121 KB，多半放不下，
  要动先按第 11.1 节末尾那三处腾地方。

**已知的 `-Os` 副作用**：它比 `-Og` 多做内联，因此会暴露一批
`-Werror=format-truncation`（如"512 字节路径缓冲装 560 字节流地址"、歌单 ID 按
`ONLINE_ID_MAX` 精度拷贝）。这些是**真 bug**（签名 URL 被截断会 403），改法是让缓冲
装得下或按契约显式截断，不要用 `-Wno-format-truncation` 掩盖。

---

## 13. 位图图标（`assets/icons` 下的 PNG，2026-10）

桌面图标可以不用 LVGL 的 symbol 文字，改用 PNG：`app_desc_t` **最后一个**成员
`const lv_image_dsc_t *img` 非 NULL 时，`ui_home.cc` 就建 `lv_image` 而不是画 symbol。
放在最后是刻意的 —— 已有的位置初始化少写这个成员会被零初始化成 NULL，
于是不用去动每一处描述符，只有真正要用位图的那个在末尾补上 `&netease_logo`。

**PNG 是编译期解好的，运行时没有解码器**：本工程
`LV_USE_LIBPNG` / `LV_USE_LODEPNG` / `LV_USE_TJPGD` **全是 n**，
`tools/font/venv` 里也没有 PIL（第 9 节禁止全局 pip）。所以：

```
tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \
    assets/icons/netease.png --name netease_logo --size 96
    # 默认写到 components/ui_core/ui_icons.c
```

**一次生成全部（2026-10 起，推荐）**：清单在 `assets/icons/icons.txt`（每行
`路径 符号名 尺寸`），生成整份 `ui_icons.c`：

```
tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \
    --manifest assets/icons/icons.txt -o components/ui_core/ui_icons.c
```

**素材"扩展名撒谎"的先转一次**：`tools/icons/prepare_sources.ps1`（.NET
System.Drawing，只做格式转换，不改内容）——

| 源文件 | 真实格式 | 转成 |
| --- | --- | --- |
| `assets/default.png` | **JPEG** 404x456 | `assets/icons/momotalk_avatar.png`（居中裁方 192²） |
| `assets/MomoIcon.png` | **ICO** 64x64 | `assets/icons/momotalk_icon.png` |
| `MomoTalk.html` 里的 ✕ | 内联 **SVG** | `assets/icons/momotalk_close.png`（4 倍重绘，圆头白线） |

⚠ `prepare_sources.ps1` 里全是中文注释，**文件必须带 UTF-8 BOM**：
`powershell.exe`（5.1）默认按 ANSI 读 .ps1，没 BOM 会把注释当乱码、直接语法报错
（踩过：`Unexpected token`）。

- 只用标准库 `zlib` 手解 PNG（8 bit、非隔行的灰度/真彩/调色板/带 α 都支持），
  输出 **ARGB8888**（LVGL 的字节序是 **B,G,R,A**），缩放用面积平均（按 α 加权）。
- **按显示尺寸烘**（`UI_HOME_ICON` = 96 px）：运行期缩放每帧都要重算，图标不值当。
- 加新图标：把源图加进 `assets/icons/icons.txt`，跑一次清单生成，
  `LV_IMAGE_DECLARE(...)` 加进 `components/ui_core/include/ui_icons.h`，
  再填到描述符的 `img`（或页面里 `lv_image_set_src`）。
- 体积账：96×96 ARGB8888 = **36 KB 位图**（源码 ~200 KB，编译期解析完就不占）。
  app 分区很紧，加图标前先看 12.3 的余量。
- ⚠ `-Werror=comment`：在注释里写通配路径别写成 `assets/icons/` 加 `*.png`，
  那个斜杠加星号会被当成嵌套注释而编译失败（踩过）。

---

## 14. 在线音乐大改：现状与待办（2026-10）

用户一次提了 7 项，当前状态（**接手前先看这里，别重复做**）：

| # | 需求 | 状态 |
| --- | --- | --- |
| 1 | 修崩溃（切歌时空指针 Load access fault） | ✅ 已修（**三处**元件，见 11.7），待真机确认 |
| 2 | "选新歌没顶替旧歌" | ⏳ 判断就是 #1 崩溃造成的（切歌时 panic 重启，回来还是旧状态） |
| 3 | 缓冲分片 + 显示加载百分比，多用 PSRAM | ✅ 已完成（见 14.5） |
| 4 | 图标换 `assets/icons/netease.png` | ✅ 已完成（见第 13 节） |
| 5 | 左右可滑动布局 + 歌单长按重命名/更新/删除 + 点按进歌曲列表 | ✅ 已完成 |
| 6 | 搜索页：输入框 + 历史搜索（10 条，长按删单条、右上"清除"） | ✅ 已完成 |
| 7 | 对勾/回车 触发搜索→显示结果；输入框空且关输入法才回到历史 | ✅ 已完成 |

### 14.1 两页滑动布局怎么搭的（`app_online.cc`）

第 0 页"我的歌单"、第 1 页"歌曲搜索"，顶部那两段文字既是标题也是 tab
（`s_tab_pl` / `s_tab_srch`，点击 → `page_show(0/1, true)`）。

- 两页是 `s_host` 的**并列子对象**（各 `LV_PCT(100)` 大小）。LVGL 默认把子对象
  裁剪在父对象范围内，所以不需要自己做遮罩，超出部分天然看不见。
  切页 = 一条 `lv_anim`（`PAGE_ANIM_MS` = 220 ms，`ease_out`）把两页的 x 一起平移
  （`slide_exec_cb`：`pl.x = -v`、`srch.x = W - v`）。
- ⚠ **首次定位前必须 `lv_obj_update_layout(scr)`**：页宽取自 `lv_obj_get_width(s_host)`，
  而 s_host 的宽度是父级 flex(`flex_grow`) 算出来的，只 update 它自己拿不到；
  宽是 0 时两页会叠在一起。`page_show`/`slide_exec_cb` 里还留了
  "宽 0 就用屏幕宽"的兜底。
- 歌单**点按**进"详情"不是压栈第二页，而是**同一页换个内容**（`s_pl_detail`），
  顶部补一行 "‹ 歌单列表" 返回。这样"加载结果该进哪个列表"不用跨页路由，少一层竞态；
  全局返回键仍然只负责退出本 APP。

### 14.2 两份结果共用一个数组（别改成两份）

`s_songs` / `s_song_n` 是**交给 app_music 的那一份**，用 `s_owner`
（`OWNER_PL` / `OWNER_SRCH` / `OWNER_ACCT`）记住它属于谁，用 `s_cur_pl_id`
记住"是哪个歌单的"（见 14.10）。歌单详情只在归属对得上时拿它画歌曲行，
搜索页只在 `OWNER_SRCH` 时画 —— 两个列表**永不同时**引用同一个数组，
也就不会出现"搜索结果被画进歌单详情"这种串台。
`OREQ_REMOVE` 的结果处理里**绝不能** `free(s_songs)`。

**旧队列的释放：交给播放器，不要在 app_online 里 free**（2026-10 修 use-after-free）。
`s_stale` 只表示"被替换下来的那一份"，回收一律走
`app_music_online_detach(s_stale)`：播放器还在拿它当队列（`s_on_songs == 它`，例如
用户点歌之后又连着打开两个别的歌单）就由 app_music 接管，等它自己换队列/退回本地
播放时再放；否则立刻放掉。以前 app_online 直接 `free(s_stale)`，那种序列会让
**上下曲 / 自动续播读到已释放内存**（曲名乱码、串到别的歌、甚至整机 panic）。

### 14.3 搜索页的"历史 / 结果"切换规则（需求原文）

`online_rebuild_page_srch()` 一个函数决定列表内容：

1. `s_srch_has_res` 为真 → 画**搜索结果**；
2. 否则**输入框为空**且**输入法已收起**（`ime_open()` 看键盘对象的 HIDDEN 位）→ 画**历史**；
3. 其它（正在输入）→ 只画一行提示，别把历史塞在键盘上面。

- 触发搜索：拼音键盘的 对勾/回车 → 组件在"没有未上屏拼音"时发
  `lv_pinyin_ime_event_ready()`（见 `lv_pinyin_ime.c` 的 `IME_KEY_CONFIRM` 分支），
  `ime_ready_cb()` 收到后：输入框非空 → `srch_do_search()`；空 → 收起键盘（回到历史）。
  重命名浮层开着时，同一个事件改成"提交重命名"。
- 输入框 `VALUE_CHANGED` 会把 `s_srch_has_res` 清掉（改了词，旧结果不算数）；
  搜索结果失败时**保持** `s_srch_has_res = true`，这样显示的是"没找到"而不是历史。
- 历史行：点按 = 填回输入框并立刻再搜一次；**长按 = 删这一条**；
  右上"清除"只在显示历史且有记录时出现。

### 14.4 歌单重命名 / 更新 / 删除（服务侧新 API）

`online_service` 新增（见 `online_service.h`）：

- `online_playlist_name(idx)`：显示名，**没起过名返回空串**（不是 NULL，展示决策留给 UI）；
- `online_playlist_rename(idx, name)`：存 NVS `nm0..nm7`，传 NULL/空串 = 清掉自定义名；
  名字会跟着 `online_playlist_remove()` 一起前移，不会和 ID 错位；
- 搜索历史：`online_history_count/at/add/remove/clear`，NVS `shn` + `sh0..sh9`，
  最多 `ONLINE_HISTORY_MAX`(10) 条、按关键词去重后提到最前、新的在最前。

`app_online` 的长按菜单：**更新** = `work_submit(OREQ_ADD, id)`（ADD 对已存在的 ID
本来就是"重新抓取刷新"）；**删除** = `work_submit(OREQ_REMOVE, id)`；
**重命名** = 打开 `MODAL_RENAME` 浮层（用拼音键盘，名字可能是中文）。
⚠ 长按菜单关闭时会把 `s_menu_idx` 清成 -1，所以重命名目标要先存进 `s_rename_idx`。

### 14.5 流式缓冲：4 MB PSRAM 环形缓冲 + 真实的下载百分比

实现在 `audio_service.cc` 的 `ensure_stream_io()` / `stream_evt_hook()` /
`audio_service_stream_progress()`：

- **环形缓冲 4 MB（`STREAM_RING_BYTES`）**，从默认 20 KB / 上一版 256 KB 提上来。
  GMF 的分配器在 `CONFIG_SPIRAM_BOOT_INIT=y` 时优先用 PSRAM
  （`esp_gmf_oal_mem.c` 里就是 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`），
  所以这 4 MB 落在 PSRAM。4 MB ≈ 320 kbps 的 100 秒音频 —— 相当于一口气缓冲一大段，
  网络抖动用这段顶着，不再几秒一卡。每次读 8 KB（`io_size`）分片填进去。
- **百分比是真实的下载进度**，不是估算：
  - 事件钩子 `HTTP_STREAM_FINISH_REQUEST` 时从 `esp_http_client_get_content_length()`
    拿总长度（分块传输时是 -1 → 拿不到就**不显示百分比**，退回"缓冲中，请稍后..."）；
  - 已下字节**优先用 `esp_gmf_io_get_pos()`**（IO 真正读到哪了），
    钩子里按 `ON_RESPONSE` 累加的 `s_stream_got` 只当兜底 ——
    因为 ON_RESPONSE 是在**读之前**派发的，`buffer_len` 是"这次想读多少"，
    网络慢时会虚高。
- **UI 的显示窗口**：`app_music` 里 `s_buf_show` 从"点歌/起播"置位，到
  **`audio_service_output_active()` 为真**（PCM 真的写到 codec 了）才撤掉，
  右栏说明行在这期间显示"缓冲中… xx%"，之后交回 `lyric_apply_mode()`。
  ⚠ 判据不能换成 `s_on_busy`（那只覆盖"解析地址+抓封面歌词"，那时 HTTP 流还没开始，
  进度恒为 0，百分比几乎永远显示不出来），也不能用 `s_playing`
  （pipeline 一 `run()` 状态就是 playing，那时还没声音）。
  `s_buf_show` 在本地曲目（`start_track_at`）与准备失败分支里都要清掉，
  否则下一个 tick 会把"缓冲中…"又写回歌词上。

### 14.6 ⚠ 弹层绝不能在"按压期间"建（长按菜单点不动的根因，2026-10）

**症状**：长按歌单行弹出菜单后，三个按钮点了没反应，而且点击**穿透到了下层页面**
（"点到了底部的布局"）。

**根因**（LVGL 的两条机制叠加，都在 `src/indev/lv_indev.c`）：

1. **按压期间会不断重新命中手指下的对象**。`indev_proc_press()` 每轮都调
   `pointer_search_obj()`，源码注释写得很直白：
   *"Without `LV_OBJ_FLAG_PRESS_LOCK` new widget can be found while pressing."*
   歌单行没设 `PRESS_LOCK`，所以长按回调里建出来的**全屏遮罩正好顶在手指底下** →
   下一轮 `act_obj` 就从"行"变成了"遮罩"（行收到 `PRESS_LOST`）。
2. **长按之后松手，CLICKED 照样发**。`indev_proc_release()` 里
   `if(long_pr_sent == 0) short_click(...)` 只跳过 short_click，
   紧接着的 `send_event(LV_EVENT_CLICKED, ...)` 是**无条件**的。

两者合起来：松手那一刻的 `CLICKED` 落在**遮罩**上 → 遮罩的"点空白关闭"把刚弹出的
菜单自己 `lv_obj_delete_async` 掉了 → 之后所有点击自然全部落到下层页面。
更危险的是：万一手指正压在某个按钮上，松手会**当场触发那个动作**（这里是"删除歌单"）。

**修法**（`app_online.cc` 的 `pl_row_long_pressed` / `pl_menu_deferred` / `pl_menu_build`）：

- 长按回调里**只记录目标 + `lv_async_call`**，菜单本体交给 `pl_menu_deferred()`；
- `pl_menu_deferred()` 用 `lv_indev_get_state(lv_indev_active()) == LV_INDEV_STATE_PRESSED`
  判断手指还在不在，**还按着就下一轮再看**，直到手指离开才真正建菜单 ——
  这样"长按那次按压"根本没机会碰到菜单（既不会误关，也不会误触发按钮）；
- ★★ **有长按动作的行，打开页面必须听 `LV_EVENT_SHORT_CLICKED`，不能听 `CLICKED`** ——
  这是同一条源码得出的结论：`indev_proc_short_click()`（内部在 1603 行发
  `LV_EVENT_SHORT_CLICKED`）**只在 `long_pr_sent == 0` 时被调用**，而
  `send_event(LV_EVENT_CLICKED, …)` 是无条件的。所以
  **长按之后绝不会来 SHORT_CLICKED，但一定会来 CLICKED**。
  踩过的坑：一开始想在 `pl_row_clicked` 里用"时间标志 + 手指是否离开"去吃掉那个
  补发的 CLICKED，结果挡不住（真机现象：长按既弹菜单**又进了歌单详情**，
  两块东西叠在一起看起来就是"界面卡死怎么点都没反应"）。
  现在 `app_online.cc` 里两处都改了：歌单行（打开详情）与搜索历史行（点按再搜一次）
  ——它们同时都有 `LONG_PRESSED`（弹菜单 / 删记录）。
  只挂 `CLICKED`、没有长按动作的行（点歌行 `online_row_clicked`、"‹ 歌单列表"
  返回行）保持原样即可。
- `pl_menu_build()` 开头加了 `if (s_menu) return;`：万一两个"建菜单"请求挤在一起，
  第二块全屏遮罩会把 `s_menu` 覆盖掉，**旧那块没人删、还盖在最上面吃掉所有点击**
  —— 现象同样是"卡死"。宁可少弹一次也不能留孤儿遮罩。
- 遮罩的"点空白关闭"再加一道 `MENU_GRACE_MS`(200 ms) 保险；
- 面板加 `lv_event_stop_bubbling()`：点菜单自己的空白处不该关掉菜单
  （CLICKED 会从子对象冒泡上来）。

**推论（写新弹层时照这个判）**：任何**在 `LV_EVENT_PRESSED`/`LONG_PRESSED`/`PRESSING`
里**创建的遮罩/弹窗都有这个坑。本工程已有的那几个（添加歌单、重命名、搜索浮层）
都是**从 `CLICKED` 里打开的**——那时手指已经抬起，所以一直没暴露问题。
真要在按压期间弹（想要"按住就出菜单"的手感），就得先 `lv_obj_add_state(..., LV_STATE_DISABLED)`
再等松手后解除：`indev_proc_release` 里 `is_enabled` 为假时**不发 CLICKED**
（同一段源码），这样那一下按压既不会关菜单也不会误触发按钮。

### 14.7 在线切歌的顺序：**先切界面，再显示缓冲**（2026-10 需求）

顺序固定为三步，**别再改回去**：

1. **停旧歌**（`audio_service_stop()` + 播放键切回 ▶）；
2. **界面切到目标歌**（`online_switch_ui_to(idx)`：标题 / 歌手 / 封面退回占位 /
   歌词清空 / 进度归零 / 总时长 `--:--`）；
3. **才显示缓冲信息**（`ui_toast` + `set_buffering_hint()`，随后 tick 刷"缓冲中… xx%"）。

三个入口都要走同一套：`app_music_play_online()`（歌单/搜索列表点歌）与
`play_next()` 的在线分支（上/下一首按钮 + 播完自动续播）。

**为什么**：点歌到真正起播之间要 1~3 秒（解析 302 地址 + 抓封面 + 取歌词都在工作线程），
这段时间界面上如果还是**上一首**的标题/封面/歌词，用户看到的就是"点了没反应、
切歌没生效"。先铺目标歌的信息，等待就变成"已经切过去了，正在缓冲"。

实现注意：

- `online_switch_ui_to()` 只动 UI 与 `s_elapsed_ms`/`s_dur_ms`/`s_cover_want` 等状态量，
  **不碰播放器** —— 停旧歌是调用方的事（顺序反了会变成"新歌界面 + 旧歌在响"）；
- `s_dur_ms = 0` 是安全的：tick 里对 `s_dur_ms == 0` 处处有保护（时长退回 `--:--`，
  进度条不换算百分比）；
- 封面/歌词在 `start_track_online_at()` 里会**再写一遍同一套字段**（幂等），
  所以这里清空只是为了"别留着上一首的"，不需要在这一步拿到真实数据；
- `play_next()` 的在线分支原来**不设 `s_index`**（等 `start_track_online_at()` 才设），
  现在提前设上，界面与状态从一开始就指向目标歌。

顺带清掉一处死代码：预取用的 `online_prefetch_next()` 早就没有调用点了（预取被刻意
停用，理由见 `tick_cb` 里的注释），已删除；`s_on_prefetch` 与 `online_prepare_task`
的 prefetch 分支保留，注释写明了以后要重新启用该从哪儿接回来。

### 14.8 封面：**魔数认格式 + 服务端缩图**（2026-10 修"部分歌曲封面不显示"）

用户的判断是对的（别信扩展名），但实测下来**同一个坑里还埋着一个更大的**。拿用户给的
那首歌的封面 URL 在本机直连抓了一遍（走本机代理）：

```
GET https://p2.music.126.net/0OY08L09cF4UMVnRuHK_BQ==/109951169045313754.jpg
→ HTTP 200, Content-Type: image/jpg, 文件名 .jpg
→ 实际字节：89 50 4E 47 0D 0A 1A 0A …  ← **PNG**
→ 大小：7,951,295 字节（7.6 MB！）
```

所以"部分封面不显示"有**两个**原因，只修格式判定是不够的：

| # | 原因 | 修法 |
| --- | --- | --- |
| 1 | 后缀/`Content-Type` 撒谎（`.jpg` 实为 PNG） | 解码端**本来**就是按魔数分派的（`image_to_rgb565`，见 1422 行），这一条其实没坏；现在在**下载侧也做一次**魔数校验 + 打日志（`cover_format()`），格式不认识就直接报错并说明是什么（WebP/GIF/HTML），不再让它烂到解码器里才炸 |
| 2 | **原图 7.6 MB**，远超 `COVER_MAX_BYTES`(旧 2 MB) | 拼 **`?param=NyN`** 让网易云 CDN 服务端缩图：同一条链接加 `?param=300y300` 只有 **~100 KB**（仍是 PNG），实测 200y200→100 KB、300y300→203 KB、500y500→500 KB |

实现要点（`online_service.cc` 的 `online_song_cover_data()`）：

- `ONLINE_COVER_PARAM = 300`：界面封面框 240 px、`COVER_MAX_DIM` 也是 240，300 够用
  （想省流量可以降到 200，但 200 < 显示的 240，会被放大一点点；就一个常量）。
  ⚠ **两个尺寸都必须给**：`?param=200x200` / `200y200` 都认（x、y 均可），
  但 `?param=200` 只有一维会被**静默忽略、原样返回 7.6 MB 原图** —— 别"简化"成这个写法。
  实测：200x200 → 100,539 B；200y200 → 100,539 B；**200 → 7,951,295 B**；300y300 → 202,885 B；
- **只在原 URL 没有 `?` 时**才追加 `?param=` —— 万一是带签名的 URL，多一个参数会让
  签名失效（网易云封面链接没有查询串，这条路安全）；
- 两个上限**必须对齐**：`COVER_MAX_BYTES`（下载）与 `MEDIA_META_COVER_FILE_MAX`
  （解码）现在都是 **4 MB**。之前是 2 MB / 2 MB，一旦只抬一个，就会出现
  "下下来了却解不了"这种最难查的组合。

⚠ `-Werror=format-truncation` 又一次咬人：中文全角括号 1 个字 = 3 字节，`s_last_err[64]`
里 GCC 证不出放得下。改法是 `%.20s` 显式限长（见 §12.3 的老规矩）。

### 14.9 下载缓冲必须**显式 PSRAM + 按需增长**（2026-10 修封面 NO_MEM）

**症状**（真机日志，`?param=300y300` 已经生效之后仍然失败）：

```
W online: cover 3354579942 download failed (ESP_ERR_NO_MEM, 0 bytes):
          https://p2.music.126.net/…/109951172821822423.jpg?param=300y300
```

**根因**：`http_get_text()` 是 `sink.buf = malloc(sink.cap)`，而 `sink.cap = max_len`
—— **一上来就按上限整块分配**。封面那条路 `max_len = COVER_MAX_BYTES`，上一轮我刚把它
从 2 MB 抬到 4 MB，于是它要一块 **4 MB 连续**内存：PSRAM 里已经躺着 4 MB 流式环形缓冲
（`STREAM_RING_BYTES`）+ framebuffer(768 KB) + LVGL 绘制缓冲 + 封面缓存，拿不到就
`ESP_ERR_NO_MEM`。**注意 `CONFIG_SPIRAM_USE_MALLOC=y` + `ALWAYSINTERNAL=16384` 时
plain `malloc` 本来就会去 PSRAM**，所以这不是"没写到 PSRAM"的锅，是"一次要太大一块 +
浪费"的锅。

**修法**（`online_service.cc`）：

- 新增 `sink_reserve()`：从 **16 KB 起步、翻倍增长**，上限才是 `limit`；
  分配**显式用** `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，
  失败再退回 `malloc()`。300y300 的封面（~200 KB）只占 256 KB，且不需要 4 MB 连续块；
- 好处是**三处调用一起受益**：封面（连上限 4 MB）、歌单 JSON（1 MB）、LRC（256 KB）
  以前都是按上限整块要，现在是"用多少长多少"；
- `sink.overflow` 标志 + 日志：超限/没内存时说"响应过大或内存不足(xx KB)"，
  不再只报一个笼统的 `ESP_FAIL`。

**别再改回 `malloc(max_len)`**。另外记一条：解码端（`media_meta.cc`）**已经是** PSRAM —
PNG 的整图缓冲、JPEG 的 `outbuf`、最终 RGB565 输出（`jpeg_calloc_align`，>16 KB 时
`MALLOC_CAP_DEFAULT` 也会落到 PSRAM）都在 PSRAM，这条路不用动。

**顺带记一个还没做的优化**：封面/歌词是在"准备任务"里下载的，而**起播要等它们下完**
（`online_prepare_task` 先 `online_song_prepare_stream()` 抓封面+歌词，才置 ready）。
封面晚几百毫秒出来没关系，但它拖慢了起播。要再快一档就把封面挪到 `app_music` 的
封面工作线程按需下（`cover_kick` 那条路），让"解析到流地址"就能起播。

### 14.10 详情页"这份歌是谁的" / 账号页转圈与重试 / 昵称持久化（2026-10 用户报）

这一轮用户报了三件事，对应四个改动，**都别再改回去**：

#### (1) 歌单详情必须先"认出自己那一份"，否则一直转到加载完成

**症状**："播放列表的歌曲我挨个点开了怎么都是同样的" —— 点进第二个歌单，看到的、
点到的仍然是**上一个歌单**的歌。

**根因**：详情页原来只判 `s_owner == OWNER_PL && s_song_n > 0`。而点进第二个歌单时
`s_songs` 里还是上一个歌单的数组（`s_owner` 也还是 `OWNER_PL`），于是列表**先把上一个
歌单的歌顶上来**；那时点行播的就是老歌单里的歌。`s_cur_pl_id` 早就存在，但**从来没被
读过**。

**修法**：`online_install()` 把"这份数据属于谁"写进 `s_cur_pl_id`（歌单 ID /
`DETAIL_KEY_DAILY` / 搜索时为空串），详情页用 `detail_owns_songs()` 比一次：
对得上才 `fill_song_rows()`，对不上就画转圈（`detail_wait_rows()`）。
于是行为变成用户要的"**点进去先转圈，加载好了再显示歌单**"。
- 账号歌单/每日推荐走同一套（`DETAIL_KEY_DAILY` 是"每日推荐"的专用标记，
  它没有歌单 ID）；
- `s_songs` 里是别处（搜索/账号页）的结果时给一个"重新载入"行 —— **不要**在 rebuild 里
  自动重发请求（结果回调会再触发 rebuild，容易形成循环）；
- `detail_wait_rows()` 另有 `DETAIL_TIMEOUT_MS`(20 s) 兜底：超时就把转圈换成"重试"行。
  请求万一没被工作线程收下（队列满），界面不会永远转下去。

#### (2) 重启后账号歌单/昵称加载不出来：一次性标志改成节流重试

**症状**："重启后进入在线播放页面，歌单什么的都没成功加载出来"。

**根因**：`s_acct_pls_asked` / `s_acct_asked` 是**一次性**标志。开机后进页面时 Wi-Fi
可能还没连上，那一次请求失败之后标志仍然是 true —— `poll_cb` 里"网络变了就重建页面"
也拉不动它，整轮开机就停在"还没取到歌单/网易云账号"。

**修法**：`acct_kick_if_needed()` / `acct_nick_kick_if_needed()`：
- 没网就什么都不发（等网络事件触发重建，不给服务器空打）；
- 没拉过就拉；失败过就重试，间隔从 `ACCT_RETRY_MS`(5 s) 起**翻倍**、上限
  `ACCT_RETRY_MAX_MS`(60 s)（服务端一直不通 / cookie 失效时不至于每 5 秒打一次）；
  `poll_cb` 每 200 ms 检查一次，进入 APP 时节奏复位；
- **已经有列表（`s_acct_pls_n > 0`）就不再自动拉** —— 否则每次进页面都闪一下转圈；
- 请求在路上的这段区间由 `s_acct_pls_busy` 表示，账号页列表区显示
  `loading_rows()` 的转圈 + "正在加载歌单…"（用户要求：这里也要转圈）。
- 结果处理里 `s_acct_pls_busy = false` 并把 `s_acct_pls_try_tick` 重置为"结束时刻"，
  所以节流窗口从**失败那一刻**开始算。

#### (3) 已登录昵称不再"重启就丢"

`online_service` 新增 `online_netease_nickname()` / `online_netease_nickname_set()`：
NVS `online`/`nick`，`online_netease_account()` 成功时自动写，`online_service_init()`
和首次查询时懒加载，**退出登录时清掉**（换账号登录也不该看到上一个账号的名字）。
`app_online` 侧 `s_login_name` 由 `login_name_sync()` 从 NVS 补齐 —— 重启后立刻显示
"已登录：XX"，不用等 `/user/account` 的网络往返（没网时那个往返根本不会成功）。

#### (4) 顺带修的两处"结果/内存"问题

- **`s_page_alive`**：工作线程发布结果前看一眼页面还在不在。不在就直接把结果释放
  （否则数组/二维码/歌单列表都会泄漏，而且下一轮要白等）；`online_on_delete()` 里也把
  槽位清一次（那是"结果已发布、人走了"的情况）。
- **工作线程的 3 秒闸门**：结果槽位只有一份，`online_work_task` 每次开始处理请求前
  先等 LVGL 侧的 200 ms 轮询把上一份搬走（最多 3 秒）。否则两份结果会互相覆盖 ——
  被覆盖的那个数组既没人用也没人 free，表现是偶发的"点歌单没反应 / 搜索卡在转圈"。
- **`online_netease_cookie_reload()`**：cookie 存在 SD 上，而开机后马上进在线页时 SD
  可能还没挂上；`net_cookie_load()` 一旦读到"没有"就会 latch，界面于是永远停在未登录
  （用户看到的"重启后歌单全没了"也有这一份）。现在账号页重建时、以及每个账号类请求
  发出前都会再读一次。

⚠ 两个**编译期**坑（都真机踩过，-Werror 下直接不过）：

- `#define DETAIL_KEY_DAILY "\x01daily"` 编译失败：`\x01d` 里的 `d` 是合法十六进制位，
  会被并进同一个转义 → `error: hex escape sequence out of range`。
  正确写法是拆字面量：`"\x01" "daily"`。凡是 `\xNN` 后面紧跟 `[0-9a-fA-F]` 都要拆。
- NVS 键名拷贝（`bt_name_enum()` 里 `snprintf(keys[n], 13, "%s", info.key)`）会报
  `-Werror=format-truncation`：`info.key` 是 `char[16]`（`NVS_KEY_NAME_MAX_SIZE`），
  GCC 证得出"最多 16 字节进 13 字节"。改法：`"%.*s", BT_NAME_KEY_LEN - 1, info.key`
  —— 显式精度既消警告又符合"这个命名空间里只有我们写的 12 位键"的事实。

---

### 14.11 账号歌单取歌**必须分页**（2026-10 修"歌单读取失败"）

**症状**：点"每日推荐"能出来，点某些**账号歌单**报"歌单读取失败"（同一页面上有的歌单
点得开、有的点不开）。

**根因**：`online_netease_playlist_songs()` 原来是一次
`/playlist/track/all?id=X&limit=<max>`（max = `ACCT_SONGS_MAX` = 300）。而服务端
`module/playlist_track_all.js` 的实现是"取 playlist.trackIds → slice → 请求
`/api/v3/song/detail`"，也就是说**每首歌都是完整的 song detail 对象**
（ar/al/dt/h/m/l/privileges…），不是精简字段：

| 口径 | 数值 |
| --- | --- |
| 每日推荐（真机日志实测） | 33 首 = **75,965 字节** → 约 **2.3 KB/首** |
| `limit=300` 估算 | ≈ 700~900 KB |
| 而 `NET_JSON_MAX`（`http_get_text` 的单响应上限） | **512 KB** |

于是 `sink_reserve()` 置 `overflow` → 请求失败 → 界面报"歌单读取失败"。
按 2.3 KB/首算，**超过 ~190~220 首的歌单必炸**；短歌单（≤100 首）一直正常，
所以看起来像"随机失败"。`/playlist/detail` 那条兜底也一样（它一次返回整个歌单，更大）。

**修法（第一步）**：服务层 `online_netease_playlist_page(pl_id, offset, out, max, out_n)`
按 `limit=100&offset=N` 循环取（服务端 `playlist_track_all.js` 明确支持 `limit`/`offset`），
每页 ≈250 KB 稳稳在限内；不足一页即到末尾。单页失败但已有数据时把已拿到的交出去，
第一页就失败才走 `/playlist/detail` 兜底。

**另外**：`OREQ_LOAD` / `OREQ_ACCT_SONGS` / `OREQ_DAILY` 的失败 Toast 现在带
`online_last_error()`（"响应过大或内存不足(512 KB)" / "服务器返回 HTTP 400" / 网络错误…）
—— 以前只报一句"歌单读取失败"，根本没法判断是哪一类。

⚠ 分页只是第一步；**"一次把所有歌都拉回来"本身在 600+ 首的歌单上还是太重**
（JSON 总量 ≈1.4 MB、解析峰值 + 一张 700 KB 的歌数组、再加上 600 行 × 5~6 个 LVGL 对象）。
真正的做法是懒加载 + 按页渲染，见 §14.12。

---

### 14.12 在线列表**分页渲染 + 懒加载**（2026-10 需求："600 多首，别一次全加载"）

用户原话："歌曲一共有 600 多个，建议你做懒加载和按需加载，不要一次把所有的列表内容
加载出来。" 两件事都做了（宏定义在 `app_online.cc` 头部，工作线程也要用）：

| 常量 | 值 | 作用 |
| --- | --- | --- |
| `ONLINE_PAGE_ROWS` | 100 | **屏幕上一页**画多少首（+ 2 行"上一页/下一页"） |
| `ONLINE_SONGS_MAX` | 1000 | 单个账号歌单最多载入多少首（≈1.2 MB PSRAM） |
| `ACCT_FETCH_MAX` | 200 | **单次补页**最多取多少首（= 2 个 HTTP 分页） |
| `ONLINE_PL_PAGE_SONGS` | 100 | 服务侧一个 HTTP 请求取多少首（响应 ≈250 KB） |

**1) 渲染分页**：`fill_song_rows_range(list, first, count)` 只画当前页；
`page_rows()` 负责"上一页/下一页"，`page_row_clicked()` 用
`user_data = (which << 1) | dir` 区分三个列表（账号歌单 / 我添加的歌单 / 搜索结果）。
为什么必须分页：每行 5~6 个 LVGL 对象，600 首 = 3000+ 对象，滚动每帧都要遍历
—— 同本地列表 §12.2 的结论。搜索（≤200 首）也一起分页了。

**2) 网络懒加载（只对账号歌单）**：

- 打开歌单时 `acct_lazy_begin(pl_id, trackCount)`：**按歌单总首数一次性分配好缓冲**
  （cap = min(trackCount, ONLINE_SONGS_MAX)，trackCount 缺失时按 ONLINE_SONGS_MAX），
  `s_song_n = 0`，**一首都不取**。
- `acct_detail_fetch_if_needed()` 是唯一入口：算"当前页要显示到第几首"（`need`），
  只补 `need - s_song_n`、且一次不超过 `ACCT_FETCH_MAX`，用
  `work_submit_ex(OREQ_ACCT_SONGS, id, off = s_song_n, cnt)` 下单。
  翻到第 N 页才取到第 N 页需要的那一段；翻页时如果数据没到，列表里显示转圈并自动补。
- 结果在 `OREQ_ACCT_SONGS` 分支里走 **`acct_lazy_append(off, page, n)`**
  （用 `memcpy` 追加到已载入末尾）。`oreq_t` 因此多了 `off`/`cnt` 两个字段，
  `work_publish` 配套的 `s_res_off`/`s_res_want` 也必须在发布前写好。
- **指针从头到尾不变**（缓冲一次分配好、只往里追加），所以播放器（app_music 存的是
  同一个指针）永远有效 —— 不需要任何 realloc/换指针的危险操作。这也是为什么
  歌单详情只画"已载入的连续前缀"：队列语义要求连续，跳页会按顺序把中间那些页补齐。
- 取不满一页 = 到歌单末尾：把 `s_songs_total` 按实际修正，列表就不会一直显示"还有下一页"。
- 取页失败 → `s_fetch_fail` + 一行"这一页没取到：<原因>" + "重试"（重试只补页，
  **不重建缓冲**，免得把前面几页重下一遍）；首次（一首都没载入）就失败才退回歌单列表。

⚠ 两个要留意的点：

- **播放队列 = 已载入的那一段**。用户点某一行时 `app_music_play_online(s_songs, s_song_n, idx)`
  会把当时的 `s_song_n` 交给播放器；之后即使又载入了更多页，也要等用户再点一次才会
  更新播放器的队列长度（`s_on_count`）。这是刻意的取舍：不动 app_music 的队列结构，
  就不会引入新的 use-after-free 面。
- **`trackCount` 为 0 的退化情况**：按 `ONLINE_SONGS_MAX` 分配（宁可多占点 PSRAM，
  也不能让用户只能看到前 100 首），真实总数由"取不满一页"修正。

---

### 14.13 详情页的"返回上一级"行**必须固定在列表之外**（2026-10 用户报）

**症状**："歌单的返回按钮为什么会跟着滑动？导致无法快速地返回上一级菜单" —— 进歌单详情后
往下翻歌，返回入口跟着滚出屏幕，只能滑回顶部才能点。

**根因**：返回行（账号歌单的 `返回` 标签、我添加的歌单的 `‹ 歌单列表` 行）原来是
**可滚动列表容器**（`s_acct_list` / `s_pl_list`，都是 `flex_grow` + `LV_DIR_VER`）的
子对象，属于列表内容，自然跟着滚。

**修法**：把它提升成**页容器**（`s_page_acct` / `s_page_pl`）的**第一个** flex 子对象：

- `make_back_label(parent, text, cb)` 建一行"‹ 返回 / ‹ 歌单列表"，默认 `HIDDEN`，
  进详情时 `remove_flag(HIDDEN)`，退回列表时 `add_flag(HIDDEN)`；
- **隐藏时 flex 会自动跳过它**（`lv_flex.c:341/478` 里
  `LV_OBJ_FLAG_HIDDEN` 的对象不参与布局），所以列表状态下不会留一条空白；
- 创建顺序 = flex 布局顺序，所以必须在 `*_hint` **之前**建（`page_online()` 里）；
- 换页/关页时 `online_on_delete()` 要把 `s_acct_back` / `s_pl_back` 一起置 NULL
  （它们随 screen 销毁，留着就是悬垂指针）。

**顺带**：内容整段重建之后要 `list_scroll_top(list)` 归零滚动位置 ——
`lv_obj_clean()` + 重画不会重置滚动偏移，进详情/翻页后新内容会从**中间**开始显示
（看起来像"漏了前几首"）。三处入口（`pl_row_clicked`、`acct_pl_clicked`、
`acct_daily_clicked`）与 `page_row_clicked()` 都调了。

**教训**：凡是"必须一直看得见"的操作入口（返回、标题、页签、翻页），都要挂在
**不滚动的容器**上；只有"列表内容"才放进可滚动列表。

---

## 15. 设置页无线开关行 / 蓝牙名缓存 / 长按输出设备跳蓝牙（2026-10）

### 15.1 设置页：开关与"进入子页"合并成**一行**

以前 Wi-Fi / 蓝牙各有**两行**（一行 `lv_switch`、下面一行同名条目用来进子页）。现在合成一行：

```
[图标] [名称 / 状态]  [开关] [›]
 └──────── 点这些进子页 ─────┘ └只切换┘
```

- 点**开关图案** → 只切开关；点行里其它任何地方（图标 / 名称 / 状态 / › / 空白）→ 进子页；
- 实现只有一处：`app_settings.cc` 的 `make_switch_row()`，三处共用（设置主页 Wi-Fi 行、
  设置主页蓝牙行、子页顶部那行）。`sub` 参数传 `NULL` = "只做开关"（子页里已经在子页，
  不再需要"进入"），`status_out` 传 `NULL` = 不建状态行；
- Wi-Fi / 蓝牙的 `s_sum[]` 摘要 label 现在就是那一行里的状态 label，`settings_tick` 照旧写它。

**两个必须守住的点：**

1. **`lv_switch` 被点会发 `CLICKED`，而 LVGL 默认让事件冒泡到父对象** —— 不挡的话
   "点开关"会连带触发行的"进子页"。挡法：给开关自己挂一个 `LV_EVENT_CLICKED` 回调，
   里面 `lv_event_stop_bubbling()`（值变化那条 `VALUE_CHANGED` 也顺手挡了）。
   顺带记一条 LVGL 9.5 的实现：`LV_OBJ_FLAG_CHECKABLE` 的控件是在 **`LV_EVENT_RELEASED`**
   里改状态、再补发 `VALUE_CHANGED`（`lv_obj.c` 的 `lv_obj_event()`），所以
   `stop_bubbling()` **不会**影响开关自己切换 —— 别被"挡住了事件"误导。
2. **开关实例表 `s_radio_sw[]`**：主页一行 + 子页顶部一行是**两个 `lv_switch`、一个逻辑开关**，
   由 `radio_sw_sync()` 按服务真实状态一起拉齐；三个页面的 500 ms tick
   （`settings_tick` / `wifi_tick` / `bt_tick`）都会调它一次 —— 所以"子页里改、返回主页立刻一致"
   不依赖哪个页面活着。子页会被反复 push/pop，**`LV_EVENT_DELETE` 里必须注销**，
   否则 tick 会拿已释放的对象 `add/remove_state`（use-after-free）。

### 15.2 蓝牙设备名缓存（NVS `bt_name`）—— 修"名称变成 MAC 地址"

**根因**：设备名**只能**从 `ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED` 里拿到。
`esp_bt_audio_event_connection_st_t`（`esp_bt_audio_event.h`）**没有名字字段**，
组件也没给 `read_remote_name` 之类的接口（`esp_bt_audio_classic.h` 只有
discovery_start/stop、connect/disconnect、set_scan_mode）。于是"开机自动连上次那台设备"
这条路（`bt_cfg/last`）**永远**拿不到名字，`make_addr_18()` 只能退回 MAC。

**做法**（`bt_service.cc`）：

- 扫描拿到真名（`d->name[0]` 且不等于 MAC 兜底串）→ 写 NVS `bt_name/<键>`（`bt_name_save()`）；
- 反查三级回退：事件里的真名 → NVS → MAC（`make_addr_18()`）；
  `DEVICE_DISCOVERED` 里"这一包还没带名字"、`bt_service_refresh_bonded()` 里
  "已配对但本次没扫到"也各查一次 NVS —— 后者让开机没扫描时"已配对"列表也有名字；
- **键名不能直接用 `AA:BB:CC:DD:EE:FF`**：NVS 键名上限是 **15 个字符**
  （`NVS_KEY_NAME_MAX_SIZE=16`，含结尾 0，见 `nvs.h`），17 字符的 MAC 串会
  `ESP_ERR_NVS_KEY_TOO_LONG`。所以键是**去掉冒号的 12 位大写十六进制**（`A1B2C3D4E5F6`）。
- **条数上限 `BT_NAME_KEEP_MAX`(16)**：写之前 `bt_name_make_room()` ——
  用 `nvs_entry_find_in_handle(h, NVS_TYPE_STR, &it)` 数条数，满了就 `nvs_erase_key()`
  删枚举到的第一条（NVS 按条目索引顺序枚举，先写的先出来 = FIFO 淘汰最旧的）。
  ⚠ "腾地方"必须在 `nvs_set_str` **之前**做：**迭代器不保证看得到同一次事务里尚未 commit
  的写入**，写在后面会少数一条、上限形同虚设。
- 名字没变就不写（先 `nvs_get_str` 比一次），避免同一次扫描反复上报时擦写 flash。

### 15.3 长按输出设备 → 设置蓝牙页

- `app_settings` 对外新增 `app_settings_open_bt()`（`app_settings.h`）：直接
  `ui_router_push()` 蓝牙子页（**不先 push 设置主页** —— 目标就是蓝牙，多一层主页只是
  让用户多按一次返回）。`app_music` 因此要在 `CMakeLists.txt` 里 `REQUIRES app_settings`。
- ⚠ **短按必须挂 `LV_EVENT_SHORT_CLICKED`，不能挂 `CLICKED`**：`indev_proc_release()` 里
  SHORT_CLICKED 只在 `long_pr_sent == 0` 时才发，而 CLICKED 是**无条件**发的 —— 用 CLICKED
  的话"长按之后松手"会**既跳设置页、又开出输出选择弹窗**（同 §14.6 的结论）。
  所以 `make_round_btn_ex()` 多了一个 `code` 参数，只有输出按钮传 `LV_EVENT_SHORT_CLICKED`。
- ⚠ **长按回调里不能直接 push 页面**：按压期间 LVGL 会不断重新命中手指下的对象
  （`lv_indev.c` 的 `pointer_search_obj`），松手补发的那个 CLICKED 会落到**新页面**里
  手指位置的对象上 —— 蓝牙页那里正好可能是设备列表的一行，会莫名其妙去连一台设备。
  所以长按只置 `s_open_bt_req` + `lv_async_call`，等
  `lv_indev_get_state(lv_indev_active()) != LV_INDEV_STATE_PRESSED` 再 push
  （与 `app_online` 的 `pl_menu_deferred` 是同一套手法）。

### 15.4 存储详情页（`page_storage`，2026-10 需求）

需求："存储需要设置为可点开，点击后显示 SD 卡的全部详细信息，包括总容量、可用容量，
并绘制一个存储状态占用条。"

- 设置主页那一行（`on_setting_row` 的 `SET_STORAGE`）从"弹个 Toast"改成 push 子页
  （`SUB_STORAGE`，`no_back = true` 由 `make_page_shell` 自带返回钮）；
- 页内：状态 / 挂载点 / 文件系统 + **容量卡**（`已用 x / 共 y` + `lv_bar` 占用条 +
  大号百分比 + `可用 z`）+ 明细（总容量 / 已用 / 可用 / 卡类型 / 数据线宽度 / 工作频率 /
  卡容量(CSD) / 卡片名称 / 序列号）。占用 ≥90% 时占用条与百分比变红（0xB5504E）。
- **容量卡第一行的高度必须给常量**，不能用 `LV_SIZE_CONTENT`：行内两个 label 是
  `lv_obj_align()` 按父容器尺寸定位的，而 `LV_SIZE_CONTENT` 又要反过来由子对象算 ——
  互相依赖会算出 0 高（LVGL 经典坑）。
- 卡信息来自 BSP 新增的 `bsp_sdcard_card_info()`（`bsp_storage.cc`）。**解码口径照抄
  IDF 的 `sdmmc_card_print_info()`**（`sdmmc/sdmmc_common.c:314-361`）：容量 =
  `csd.capacity × csd.sector_size`，类型靠 `ocr` 的 CCS/S18R 位，数据线宽度取
  `ssr.cur_bus_width`（**当前**宽度）。CID 的 `name` 是 8 字节**空格填充**，要截 + 去尾空格。
- 右上角"重新检测"：`bsp_sdcard_mount()` 幂等，**板子只在启动时 mount 一次**，
  开机后才插卡必须再挂一次才认（否则永远显示"未检测到"）。
- 设置主页那行的摘要是"已用 38% · 可用 18.3 GB"，**自己节流到 5 s** ——
  `esp_vfs_fat_info()` 每次都走一遍 FATFS（`f_getfree`），不能每 500 ms 调一次。
- ⚠ LVGL 9 的 `lv_color_t` 是**结构体**，别用 `uint32_t` 存颜色（`UI_COLOR_ACCENT` 这类
  宏就是 `lv_color_hex(...)`）；要动态选色就声明 `lv_color_t tint = cond ? lv_color_hex(a)
  : UI_COLOR_ACCENT;`，别套 `uint32_t` + 再 `lv_color_hex()` 一次。

---

## 16. 开机图片：启动图 + 首页背景图（2026-10 需求）

### 16.1 开机启动图（`assets/init.jpg` + Toast，main/splash.cc）

需求原文："使用 assets/init.jpg 图片作为启动加载初始化图，开机后初始化完屏幕就显示
这个图片和 Toast，Toast 内容为：正在初始化... 初始化完成后，经过一个过渡动画，加载首页。"

实现：`main/splash.cc`（+ `main/splash.h`），时序：

| 时刻 | 动作 |
| --- | --- |
| `app_main` 第一件事（`bsp_init()` 之后） | `boot_splash_decode_start()`：后台任务把 JPEG 软解成 RGB565（≈770 KB PSRAM），**与后面的初始化并行**，钉 core 0（core 1 是 LVGL） |
| 显示链路 ready 且 `ui_core_init()` 之后（**同一个 LVGL 锁里**） | `boot_splash_show()`：在 `lv_layer_top()` 最上层盖全屏图 + `ui_toast("正在初始化...")` |
| `radio_init_task` 结束（WiFi + 蓝牙初始化完） | `boot_splash_init_done()` 只置一个标志 |
| LVGL 定时器每 100 ms 检查 | `(init_done && 已显示 ≥ 900 ms) \|\| 已显示 ≥ 8000 ms` → 淡出（opa）+ 上滑 28 px，360 ms → 删对象、还回 PSRAM，露出首页 |

几个**别改回去**的点：

1. **图片用 BINARY EMBED 进 app**（`main/CMakeLists.txt` 的 `target_add_binary_data ...
   BINARY RENAME_TO boot_splash_jpg`）。别用 TEXT（JPEG 里随便一个 `0x00` 会被当字符串
   结尾）；也别编译期烘成 C 数组（没有图片编码工具，且 355 KB → ~6 MB 源码）。
   IDF 6.1 的 `data_file_embed_asm.cmake` 会生成三个符号：
   `_binary_boot_splash_jpg_start/_end` 与 `boot_splash_jpg_length` ✓ 可预测。
2. **运行时解码走 `media_meta_image_mem_rgb565(data, len, UI_W, ...)`** —— 与在线封面
   同一套解码器（JPEG/PNG 按魔数分派，输出 RGB565 + PSRAM）。`max_dim` 传 `UI_W`：
   图正好 800×480，解码器在 `long_side <= max_dim` 时**不缩放**（`media_meta.cc` 的判定）。
   像素用 `media_meta_cover_free()` 释放（内部 `jpeg_free_align`），描述符用堆上的
   `lv_image_dsc_t`（照抄 `app_music` 的 `cover_show()`）。
3. **Toast 必须在启动图之后弹**：`ui_toast()` 内部 `lv_obj_move_foreground()` 把 Toast
   提到最前，于是"图盖住状态栏/返回键/首页、Toast 盖住图"。顺序反了 Toast 就被图盖住。
4. **启动图与 Toast 之间不能松 LVGL 锁**（`main.cc` 里与 `ui_core_init()` 同一个锁）：
   松锁会让 LVGL 先渲染一帧首页，用户看到"首页闪一下再被盖住"。
5. 启动图容器设 `CLICKABLE` + 全屏：开机那几秒的触摸不该点到底下的首页。
6. **释放顺序**（`splash_out_done_cb`）：`lv_image_set_src(img, NULL)` →
   `lv_obj_delete_async(scr)` → `lv_async_call(free)`。`lv_obj_delete_async` 与
   `lv_async_call` 是**同一条 FIFO 异步队列**（前者就是后者包的），所以这样写能保证
   "对象真删掉之后"才 free 描述符与像素。
7. 新增 `ui_toast_hide()`（`ui_core.h`）：过渡一开始就收掉"正在初始化..."，
   不让它飘在首页上；`ui_core_init()` 里原来那句 `ui_toast("系统就绪")` 已删
   —— 开场时机现在由 app 层（启动图）决定。

⚠ 由此带来的体积：app 从 7.23 MB → ≈7.59 MB。分区表当前是**单个 14.61 MB 的
`factory` 槽**（`partitions.csv`，见那里的注释：为了给后续功能腾地方，砍掉了 ota_1、
放弃了 A/B OTA），所以完全放得下 —— §11.1 里"两个 7.5 MB app 槽"那段是**旧布局**，
已过时。

### 16.2 首页背景图（`assets/background.jpg`，ui_core/ui_home_bg.cc）

- 资源**嵌在 ui_core**（`components/ui_core/CMakeLists.txt` 的 `target_add_binary_data`
  → `_binary_home_bg_jpg_start/_end`）：画首页的组件自己带首页的图，换壁纸只改图片文件。
- **直接当 screen 的 `bg_image` 用**（不建铺满屏幕的子对象）：LVGL 在
  `bg_image_tiled = false` 时按原尺寸居中画在对象自己的区域上（`lv_draw_rect.c` 的
  `draw_bg_img`），图正好 800×480 = 整屏；也就避开了 screen 的 `pad_top` 让子对象
  坐标偏移那件事。
- **必须压暗**：这张壁纸是亮色天空，而首页时钟（0xEDE6D8）与图标名（`UI_COLOR_FG`）
  都是浅色 —— 用 `bg_image_recolor` = 黑 + `bg_image_recolor_opa = HOME_BG_DIM_OPA`
  (120/255 ≈ 47%) 做一层暗色滤镜，一个常量调浓淡。
- **后台解码**：`ui_core_init()` 里 `ui_home_bg_start()` 先把解码任务发出去（800×480
  JPEG，几百毫秒），再建首页；解完在任务里按 AGENTS §6 **加 `esp_lv_adapter_lock()`
  贴图**（首页可能还没建/已销毁，两种顺序都兜住：`ui_home_bg_bind()` 双向绑定）。
  像素（≈770 KB PSRAM）常驻不释放。
- 首页 `LV_EVENT_DELETE` 里除了解除绑定，还要**清掉时钟 label 指针并删掉 1 s 定时器**：
  否则下一个 tick 会写到已释放的对象上（use-after-free，见 `home_deleted()`）。


