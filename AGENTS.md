# AGENTS.md —— ESP32-S31-Korvo-1 流媒体音乐播放器

本文件给在本仓库工作的 AI Agent / 开发者提供**必须遵守的事实与命令**。与
`方屏流媒体音乐播放器 开发指南.md` 冲突时，以官方文档与本文件记录的实测事实为准。

> ⚠ **本文件已超过 64 KB 的指令预算，载入时尾部会被截断**（近期丢的是 §15.3 之后的内容）。
> 磁盘上的文件是**完整**的 —— 需要最新几节（§14.11 之后：账号歌单分页/懒加载、
> 详情页返回行、开机图片 §16）时，**直接用 read 工具打开本文件**按行号读，
> 不要以为"文件里没有"。长期解法是把深挖记录拆到 `docs/` 下分册、本文件只留索引。

---

## 0. ⚠ 音频链路已在 2026-10 换成 libs/espaudiocore（先读这一节）

**解码库不再是 `esp_audio_simple_player` / ESP-GMF**。`esp_audio_simple_player`、
`gmf_audio`、`gmf_io`、`gmf_core` 都已退出构建；音频链路的当前架构、线程纪律、
对外语义变化、我们对 vendored 库做的补丁、排查用日志，**全部在
[`docs/audio-stack.md`](docs/audio-stack.md)**，改音频相关代码前必须读它。

本文件 §11.4 及其之后关于 GMF 的记录（`esp_gmf_io_http` 的坑、`seek_byte_for` 码率换算、
落点核对、`esp_gmf_obj_dupl` 的句柄身份、`Run timeout`……）现在是**历史记录**：
现象与排查思路仍有参考价值，但**对应的代码已经不存在了**，别照着它改代码。

三条最容易踩的现行约定，先记在这里：

1. 本板 I2S 是 **TDM**，espaudiocore 的 I2S 输出模式（STD API 重配时钟）**不可用** ——
   一律"解码到 ringbuf + 我们自己写 `esp_codec_dev_write()`"。
2. 写硬件的 PCM **恒为 48kHz/16bit/2ch**（由 `audio_output.cc` 用 esp_asrc 统一），
   所以时间轴只有一个口径：`ms = 字节数 ÷ 192`。
3. 会话锁（`audio_core_lock()`）只保护"起播/停止/seek 重开"；**输出任务与事件回调绝不取锁**。

构建入口也变了：本机 `idf.py` 会被 PowerShell 执行策略/沙箱挡住，统一用
`& .\tools\idf.ps1 build`（封装了 EIM profile、代理与 bmgr 扩展）。

**同一轮还做了大文件拆分**：`audio_service` / `app_music` / `online_service` /
`app_online` / `media_meta` / `app_settings` / `bt_service` 七个组件已从单文件
（1269~3619 行）拆成 5~17 个 `.cc`（都 ≤640 行）+ 各自的内部头，
**对外头文件一个字没改**。拆分清单、验收方式、以及"增量绿而从零红"这类坑，
见 [`docs/refactor/README.md`](docs/refactor/README.md)；
拆分前每个文件的逐行审计报告在 `docs/refactor/01~07_*.md`（行号对应当初的旧文件）。

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
- ⚠ **LVGL 真机崩溃的坑集中在 [`docs/lvgl-pitfalls.md`](docs/lvgl-pitfalls.md)** ——
  改"全局复用的控件"和"挂 `lv_layer_top()` 的浮层/键盘"之前**必读**。
  最容易踩的一条：`lv_keyboard_set_textarea()` / `lv_pinyin_ime_attach()` 只存指针、
  textarea 删除时**不清理**，比 textarea 活得久的键盘必须显式解绑（2026-10 因它崩过一次）。

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
- **串口诊断命令**（`songinfo <路径>` / `songscan [目录]` / `playstat`，实现见
  `main/console_cmds.cc`、用法见 [`docs/serial-commands.md`](docs/serial-commands.md)）：
  想知道"这个文件到底是什么参数 / 当前播放状态如何"，**先跑命令拿事实**，别靠猜。
  与日志共用 console UART（115200），提示符会和日志交错，属正常。
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

**★ 播放地址来自"自建中间件 + 当前登录 CK"，不是 meting**（2026-10 用户要求改）。
meting 退居**辅助**：歌词 / 封面 / 歌单列表；并且在"没登录 / CK 失效 / 中间件不可达"
时仍然当**兜底**（能力只增不减）。

| 用途 | 接口 |
| --- | --- |
| **播放直链** | **自建中间件 `GET /song/url/v1?id=<歌曲id>&level=standard`（带 `cookie=`）**，失败再试 `level=exhigh`；仍失败才退回 meting 的 `type=url`。见 `os_account.cc: online_netease_song_url()` + `os_stream_url.cc` |
| 歌单 | `/api?server=netease&type=playlist&id=<歌单ID>` |
| **搜索** | `/meting/api?server=netease&type=search&id=<关键词>`（关键词在 `id=`，内部做百分号编码） |
| 播放地址（兜底） | `type=url&id=<歌曲ID>`（302 到 https CDN）—— **匿名请求**，会员/数字专辑歌只给 30 秒试听片段 |
| 歌词 | `type=lrc&id=`（纯文本 LRC，UTF-8） |
| 封面 | `type=pic&id=`（302 到 jpg） |

**为什么必须带 CK 取直链**：meting 的 `type=url` 是匿名请求 —— 会员歌只下发
**30 秒试听片段**（真机实测 Content-Length 439 KB ≈ 128 kbps 的 27 秒，而元数据时长
是整首），连带把 seek 时间→字节映射、进度条、"提前结束"判据全带偏。带 CK 走中间件
拿到的才是**完整**直链（`/song/url/v1` 还顺手给准确的 `size` + `br`，时长因此是准的）。

判读日志：`online: song url <id>: N bytes, M ms`（成功）/
`... **试听片段(freeTrialInfo)**：该账号对这首没有完整版权`（登录了但没 VIP/无版权）/
`stream: 账号直链(CK) 拿不到 id=…，退回 meting`（未登录或中间件不可达）。

⚠ **同一个 id 的老缓存可能是试听片段**：`online_song_prepare()` 命中缓存时会拿
"服务端报的完整大小"比一比，明显偏小就 unlink 重下（日志 `疑似试听片段缓存，重下`）。
"先下载再播"的 m4a/ogg/opus 与手动缓存都走这条体检。

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

**播放地址会过期（403），app 自动重刷**（2026-10 加，`app_music.cc`）：

```
I ESP_GMF_HTTP: HTTP Open, URI = http://m701.music.126.net/20261010132238/...mp3?vuutv=...
E ESP_GMF_HTTP: Invalid HTTP stream, status code = 403
E ESP_GMF_IO: esp_gmf_io_open failed   →   E audio_svc: player error state
```

网易云的流地址带**时间戳 + 签名**（`.../20261010132238/...?vuutv=...`），放一阵、或过一会儿
再点回来听就 403。到 app 这层只剩一个 `AUDIO_EVT_ERROR`（**拿不到状态码**），所以策略是
"在线流媒体出错先重刷一次地址再放"：`online_url_refresh()` 弹
`音乐链接已过期，正在刷新链接...` → 复用点歌的准备链路（解析 302 → `s_on_ready` → 起播），
**每首最多 `ONLINE_URL_RETRY_MAX`(2) 次**，仍然失败才按原来的"这首放不了，已跳过"处理
（本地曲目不走这条路）。计数在"起播成功 3 秒后"与"点新歌 / 切回本地"时清零。
⚠ 出错那一刻如果 `s_on_busy`（用户刚点了别的歌，在跑的准备是新的那首），只"别 advance"、
不重复下单、也不再弹提示。

**拖动定位（seek）**（2026-10 加：`audio_service.cc` + `app_music.cc`。传输层是 GMF 现成的，
我们只负责"时间 → 字节"与 seek 前后的收尾）：

| 环节 | 事实 |
| --- | --- |
| 传输 | `esp_gmf_io_http` 的 `_http_open()` 在 `info.pos != 0` 时**自动**加 `Range: bytes=N-`（不用自己调 `esp_http_client_set_header`）；`_http_seek()` = 关连接 + 重开 |
| 缓存内瞬移 | `esp_gmf_io_seek()` 先试 `seek_in_cache()`：**只处理向前**、且目标落在"已预读、还没喂给解码器"的数据里 → 就地丢字节，零网络（日志 `Seek within buffer, drop N bytes`）。4 MB 环形缓冲 ≈ 320 kbps 的 100 秒 |
| 服务端不支持 Range | 回 200 时 `range_not_supported=true` + `skip_bytes=info.pos` → 本地丢掉前 N 字节（慢，但**不是"不能 seek"**） |
| 实测 | 网易云 CDN `m801.music.126.net` 对 `Range: bytes=1000000-` 回 **206 + `Content-Range: bytes 1000000-1000103/3195693`** ✓ |
| 时间 → 字节 | 码率优先级：① 本地 `media_meta` 探测的平均码率（MUSIC_INFO 处理时已换过）② **在线 `Content-Length ÷ 时长`**（就是"(目标时间/总时长)×总字节数"）③ 兜底解码器第一帧码率。②的时长由 `audio_service_set_duration_ms()` 告知（GMF 的 MUSIC_INFO 里 duration 恒为 0），**必须在 `play_uri()` 之后调**（换曲会清零） |
| 出声标志 | `audio_service_seek_ms()` 里把 `s_out_flowing` 清零：UI 靠 `audio_service_output_active()` 判"缓冲结束、撤掉'缓冲中…'"，不清零的话提示刚挂上就被撤掉，用户只看到"进度条跳过去了但没声音" |
| 卡死看门狗 | seek 后必须复位 `s_stall_kicks/s_stall_ticks`：重新缓冲正是它盯的特征（声称在播但没 PCM），不复位它 10 s 后会去 pause/resume 搅局 |
| 容器白名单 | ⚠ **已改（2026-10）**：现在**不按扩展名挡拖动** —— 本地文件恒可拖（各解码器都实现了 `seek_ms()`：WAV 字节率 / Vorbis `ov_pcm_seek` / Opus `op_pcm_seek` / M4A mp4 demuxer / FLAC / MP3），权威判据是 `audio_service_seek_supported()`。那张名单改名成 `music_container_needs_seekable_source()`，**只**用于"在线歌要不要先下载再播"（m4a/mp4/ogg/opus 不能从流中间打开）。在线可拖的容器只有 MP3/ADTS-AAC（`fmt_stream_seekable()`） |
| 已知误差 | 流上 `s_data_offset = 0`（不扣 ID3v2 头）→ 中段定位会**早到几秒**（标签越大越明显）；网易云下发的流标签很小（封面走单独接口），实测可忽略 |
| 暂停时拖动 | 不挂"缓冲中"提示（那时不会有 PCM，提示会永远留在屏上）；在线拖动的进度**不写进度记忆**（URL 带时效签名，存了没用） |

验证用日志：`ESP_GMF_IO: Seek within buffer, drop N bytes, seek to M`（瞬移）/
`ESP_GMF_HTTP: HTTP Open, URI = ...`（Range 重连）/ `W ESP_GMF_HTTP: Server ignored HTTP
Range (status 200)`（兜底，慢）/ `audio_svc: seek -> NNNms (byte N, bitrate N bps, stream
size/duration)` / `app_music: seek rebuffered, stream at N%`（落点核对）。

**seek 的两个进阶机制**（2026-10 补，全在 `audio_service.cc` 里）：

1. **实测有效码率**（`s_meas_bps`）：`esp_gmf_io_get_pos()` 的推进点在
   `esp_gmf_io_release_read()`（消费者 release 一块数据时才 `+valid_size`），也就是
   **"解码器吃到哪了"**，**不是"网络读到哪"** —— 所以 `Δpos ÷ Δ已播时长` 就是这段音频的
   真实码率（VBR 也成立）。采样在 `out_write_cb` 里顺带做：跳过起播抢读期
   （`SEEK_MEAS_WARMUP_MS` = 2 s，那会儿解码器会抢读一大段，测出来能飙到几 Mbps）、
   每 `SEEK_MEAS_WINDOW_MS`(1 s) 采一个窗口、两道合理性检查（绝对值 8 kbps~2 Mbps +
   与全曲平均相差不超过 4 倍，后者专防**跨 seek 的脏采样**），结果做 3:1 IIR。
2. **相对映射**（`seek_byte_for()`）：向前拖、且实测码率与全曲平均差 20% 以上
   （VBR，或压根没有全曲平均）时用
   `byte = 解码器当前位置 + Δ时间 × 实测码率 ÷ 8000`；其余情况用绝对映射
   `byte = offset + 时间 × 平均码率 ÷ 8000`。**向后拖只能用绝对映射**（相对没法往回推）。
   时间轴 = `s_pcm_bytes ÷ 192`（输出固定 48 kHz/16bit/2ch ⇒ 192 B/ms），
   seek 成功时把基准挪到目标时刻、PCM 计数清零（避免逐块取整的漂移）。
3. **落点核对**（`audio_service_seek_landing_ms()`）：seek 后**第一块 PCM** 出来时，用
   `esp_gmf_io_get_pos()` 按**同一个码率**把字节反推回时间并打日志；`app_music` 轮询它
   （最多 10 个 tick），偏差 > 3 s 就把 `s_elapsed_ms` 对齐过去 + `lyric_resync()`
   —— 防的是"服务端悄悄忽略 Range / 按别的范围给数据"，那种情况不校正就永远差一截。

对应日志：`audio_svc: seek -> NNNms (byte N, N bps, average|measured(relative))`、
`audio_svc: seek landed: byte N -> NNN ms (asked NNN ms, diff N ms)`、
`app_music: seek landing off by N ms ... realign`（只有偏差大时才打）。

⚠ **GMF 的坑：`HTTP_STREAM_FINISH_REQUEST` 只在 writer 方向派发**（2026-10 真机踩过）。
`esp_gmf_io_http.c` 的 `_http_open()` 里是
`if (http->is_open && (http->base.dir == ESP_GMF_IO_DIR_WRITER)) { … dispatch_hook(FINISH_REQUEST) }`
—— 流式播放是 **READER**，这个事件**永远不来**，所以钩子里的 `s_stream_total` 恒为 0。
真机现象：在线拖动定位弹"定位失败：码率未知" —— 总长为 0 就算不出平均码率，而那条流
GMF 报的 MUSIC_INFO 码率又是 **0 bps**（`music info: 48000 Hz 2ch 16bit 0 bps, 200884 ms`），
连兜底都没有。**总长度一律走 `stream_total_bytes()`**：问 `esp_gmf_io_get_size()` ——
`_http_open()` 里 `esp_gmf_io_set_size(self, info.size)` 就是 Content-Length，读侧也照设
（"缓冲中 xx%" 一直能显示，靠的也是这条兜底）。

⚠⚠ **别拿"注册进池子的 IO 句柄"当管道在用的那个**（2026-10 真机踩了**两轮**才定位到）。
`esp_audio_simple_player_register_io()` 只是把句柄登记进池子；建管道时
`esp_gmf_pool_new_io()` 走的是 **`esp_gmf_obj_dupl()`**（`tmp->new_obj(tmp->cfg, new_obj)`）
—— 池子按 cfg **新建一份实例**给管道用，所以注册进去的那个"模板"永远不会被 open，
它的 `esp_gmf_io_get_pos()` / `esp_gmf_io_get_size()` **恒为 0**。
症状：在线拖动定位弹"定位失败：码率未知"（总长取到 0 → 算不出平均码率）；**同一个原因**
也让"缓冲中 xx%"一直显示不出来（一直停在"缓冲中，请稍后..."）。
**正确来源：`esp_gmf_pipeline_get_in(pipe, &io)`**（源码里就是个赋值 + 无锁，随便调）——
`audio_service` 里封成 `stream_io_update()`，在 `play_uri_locked()` 成功 `run()` 之后调一次；
`play_uri_locked()` 开头先把旧句柄清空（管道里的实例可能已经重建）。
⚠ cfg 是**共享**的，所以 4 MB 环形缓冲、事件钩子这些都照旧生效 —— 只有**句柄身份**是错的，
别被"日志里 ring 4096 KB 明明生效了"误导。

排查这类"算不出来"的问题，先看这行自诊断日志（`audio_service_seek_supported()` 拒绝时打）：
`audio_svc: seek not ready: info=1 io=0x… total=0 dur=114770 avg=0 meas=0 gmf_br=0`
—— `total=0` 就是上面这个坑；`info=0` 是 MUSIC_INFO 还没来；全都有值却还是失败，
那就是 app 侧的容器白名单挡的（`track_seek_unsupported()`）。

**CDN 掐断长连接 → GMF 自己重连续传（别被 ERROR 吓到）**（2026-10 真机日志）：

```
E transport_base: tcp_read error, errno=Connection reset by peer      ← 服务端/中间设备掐了长连接
W HTTP_CLIENT: esp_transport_read returned:-2 and errno:5
E ESP_GMF_HTTP: The error is happened in reading data, return value: -1, error msg: I/O error
I ESP_GMF_HTTP: HTTP Open, URI = …（同一条 URI）                       ← GMF 自己重开
I ESP_GMF_HTTP: The total size is 2576344 bytes                       ← 仍是**整首**总长（有意保留）
W ESP_GMF_HTTP: Reconnect to peer successful                          ← ★ 成功了
```

- 这组是**恢复成功**的样本：`_http_read_data()` 读到错误 → `connect_times++` →
  `_http_reconnect()`（= `_http_close()` + `_http_open()`）。
- **续传按位置来**：`_http_open()` 从 IO 的 `info.pos` 拼 `Range: bytes=<pos>-`；
  `info.size` 只在 `info.pos <= 0` 时取本次 Content-Length，否则**保留原总长**
  （源码注释：*"the total byte of content has been changed after seek, set
  info.total_bytes at beginning only"*）—— 所以日志里是整首大小、不是剩余长度。
- 服务端**不支持** Range 才走兜底：`W Server ignored HTTP Range (status 200) … skip N bytes`
  （日志里没这行 = 这次是正常的 206 部分续传 ✓）。
- 计数是"**连续**失败"：`if (rlen > 0) http->connect_times = 0;` —— 成功读到一次就清零，
  所以 `HTTP_MAX_CONNECT_TIMES(5)` 是"连着 5 次重连都拿不到数据"才放弃。
- **什么时候才算真出问题**：`Reconnect times more than 5, disconnect http stream` +
  `Failed to reset http connection` → `ESP_GMF_IO_FAIL` → `audio_svc: player error state`
  → 这时才轮到 app 那套（403 重刷 / 换下一首）。
- ⚠ 4 MB 环形缓冲会**长时间不读网络**（预读满就等消费），空闲长连接正是 CDN/NAT 最爱掐的
  —— 这类 reset 出现得频繁是正常的，缓冲够深时听感不受影响。

**⚠ 已知未修：重连连传锚点偏保守（会重复缓冲里那段）**。
`info.pos` 的推进点是 `esp_gmf_io_release_read()`（**消费者已取走**的字节），
而重连路径**不 flush 环形缓冲** —— 所以 `Range: bytes=<pos>-` 发出去后，
服务端重发的 `[pos, N)` 会和缓冲里**还没消费**的那段重叠，被播两遍。
重叠长度 = 重连瞬间缓冲里的存量，**与 `STREAM_RING_BYTES` 成正比**：
GMF 默认 20 KB（≈0.5 s，没人会注意）→ 我们的 4 MB（可能整首重放）。
正确锚点是"网络前沿" `anchor = info.pos + 缓冲已填字节数`
（`esp_gmf_db_get_filled_size()`；首次 open 与 seek 后都为 0，所以对那两个调用点是 no-op）。
真要修就改 `_http_open()` 里 Range 与 200 兜底 `skip_bytes` 用的那个值（托管组件补丁，同 §11.7/§11.8）。

**在线流断了之后的三种收尾**（2026-10 用户报"卡了、甚至会没声音"，全在 `app_music.cc`）：

| 现象 | 处理 |
| --- | --- |
| GMF 报错（403 / open 失败） | `online_url_refresh()` 重解析地址 → **从断点续播**（`s_on_resume_ms`，见下） |
| **提前结束**（TCP reset 后 GMF 把"读不到"当成整体结束，`done` 在歌只放了一小半时到） | `tick` 里判 `s_dur_ms > 30 s && s_elapsed_ms + 15 s < s_dur_ms` → 不当成放完，改走重刷 + 续播；`s_eof_retry` 每首上限 3 次（**只在真换曲时清零**，否则"放了 3 秒"就把额度补满 = 没上限） |
| **彻底卡死**（GMF 既不出错也不返回，所以 app 永远收不到事件） | 20 s 无 PCM 的看门狗：先 pause/resume 踢 3 次；**踢完必须确认 `audio_service_is_playing()` 为真**（pause 成功而 resume 失败会把管线留在 PAUSED，而看门狗只盯"声称在播"的流 → 再也没人来救 = 彻底无声）；3 次无效后**升级成重刷地址 + 断点续播**，刷新额度也用完才 `s_err_req = true` 交给错误处理 |

- **断点续播**（`s_on_resume_ms`）：`online_url_refresh()` 在重刷前把 `s_elapsed_ms`
  记下来（>5 s 才记），起播后由 `tick` 等 `audio_service_seek_supported()`
  就绪再 `audio_service_seek_ms()` 定位回去 —— 复用的是 §11.4 那套"时间 → 字节"，
  所以进度条/歌词会跟着对齐、落点核对还会纠偏差。以前是**从头放**（听了 40 秒一出错回 0 秒）。
  `s_elapsed_ms` 只在**真的出声**期间累加，所以卡住/无声那段不会让它虚涨，它天然就是断点。
- 续播超时（`Content-Length` 拿不到 / 时长未知）会放弃并从头放，日志 `online resume: not ready`。

**播放器页的缓冲提示：转圈 + 实时下载速度**（2026-10 需求："只写一行字像卡死"）：

```
[◜] 缓冲中 20 KB/s · 45%
```

- **速度来源是 GMF 自己的速度统计**：`esp_gmf_io_get_speed_stats()`（头文件里的
  `esp_gmf_io_speed_stats_t.current_speed_kbps / average_speed_kbps`）。
  `audio_service_stream_speed(int *kbps)` 把它包出来（**纯查询**，任何任务都能调）。
  - 统计是在 IO 任务每次真读到数据后 `esp_gmf_io_update_speed_stats(io, 实际字节, 耗时)`
    更新的（1:9 平滑，≥100 ms 一次），所以它反映的是**网络下载速率**；
  - ⚠ 单位是 **Kbps（千比特/秒）**（源码 `interval_bytes * 8 / interval_time_ms`）。
    UI 要 KB/s 得 **÷8**（`app_music` 的 `buf_hint_text()` 就是这么干的）——
    直接把 Kbps 当 KB/s 印，320 kbps 的流会显示成 "320 KB/s"，差 8 倍；
  - ⚠⚠ **必须在这个句柄上开**：`esp_gmf_io_enable_speed_monitor(io, true)` 要调在
    `stream_io_update()` 拿到的、**管道真正在用的**那个 IO 上 —— 池子里的"模板"开了没用
    （同 §11.4 那个 `esp_gmf_obj_dupl()` 的坑）。`enable(true)` 只在没分配时分配、
    重复调用只是 memset 清零，不会泄漏。
  - 拿不到速度（本地文件 / 没开统计 / 还没读）时函数返回 false，UI **不显示速度**，
    别显示 "0 KB/s"。
- **转圈**用 `lv_spinner`（本工程 `CONFIG_LV_USE_SPINNER=y`，app_online 的"载入中"也是它；
  它自带无限循环动画，不用自己起 timer）。样式抄 app_online：暗底圈 `UI_COLOR_PILL` +
  强调色转弧 `UI_COLOR_ACCENT`，`arc_width` 3 px，尺寸 22 px。
- **布局**：提示行是一个 `LV_SIZE_CONTENT` 的 **flex row**（转圈 + 文字）居中放在歌词框里。
  用 flex 而不是绝对定位，是因为转圈藏起来时（"暂无歌词"/"轻点封面显示歌词"）
  文字会自动居中，不用额外调。
- ⚠ **转圈的可见性只在 `buf_hint_text()` 里对齐**（"有文字就有转圈"）。
  为什么不能只在 `set_buffering_hint()` 里显示：起播流程是
  `app_music_play_online()` → `set_buffering_hint()`（显示转圈）→
  `start_track_online_at()` → `lyric_rebuild()` → `lyric_apply_mode()`
  （**把转圈藏起来**）—— 而这时 HTTP 流才刚开始缓冲（`s_buf_show` 仍为真），
  只在第一处开的话，最需要转圈的那一段反而没转圈。
  `lyric_apply_mode()` 负责关（出声/换曲时调用）。

**本地文件"没时长 / 不能拖 / 卡死"三件事**（2026-10 用户报，真机日志）：

```
I AUD_Dec_Parse: FLAC sample_rate 44100 duration:268384 max_frame:3635
E AUD_Dec_Parse: Not found frame in max frame size 3635      ← 反复刷
...
I audio_svc: music info: 48000 Hz 2ch 16bit 0 bps, 0 ms      ← ★ 码率/时长都是 0
I media_meta: cover decoded 240x240                          ← 说明 media_meta 能读这个文件
```

- **根因①（时长 / 拖动）**：`media_meta_probe_mp3()` **只认 `.mp3`**（注释里写着"其它扩展名
  返回 `ESP_ERR_NOT_SUPPORTED`"）→ FLAC / WAV / AAC 这些本地文件的 `s_info` 永远是
  `0 bps, 0 ms` → 界面总时长 `--:--`、进度条不动，拖动直接报"定位失败：码率未知"。
  **修法**：新增 `media_meta_probe_audio()`，**按魔数**分派（不看扩展名）：

  | 容器 | 取值 | 时长怎么算 |
  | --- | --- | --- |
  | MP3 | 直接转给原有 `media_meta_probe_mp3()`（ID3 + 帧头 + Xing/Info） | Xing 帧数，或大小÷码率 |
  | FLAC | `fLaC` → 块链找 **STREAMINFO**（34 B）：20bit 采样率 / 声道 / 位深 / **36bit 总采样数** | 总采样数 ÷ 采样率（**准确**） |
  | WAV | `RIFF/WAVE` → `fmt ` 的字节率 + `data` 长度 | data ÷ byte_rate |
  | ADTS AAC | 帧头 7 B：采样率索引 + 13bit 帧长（每帧 1024 采样） | 大小 ÷ 平均码率（估） |

  - FLAC/WAV **没有码率字段** → 码率按 `音频字节数 × 8 ÷ 时长` 补（这正是 seek 要的平均值，
    VBR 也成立）。
  - ⚠ **按块长度跳，绝不逐字节扫**：FLAC 的内嵌封面（PICTURE 块）动辄几百 KB~几 MB，
    逐字节扫又慢、又正是 ESP 那个解析器刷 `Not found frame in max frame size` 的来源。
    我们只读几十字节 + 少量 `fseek`。
  - 顺手比 ESP 解析器宽容：允许 FLAC 前面带 **ID3v2** 标签（先按 synchsafe 长度跳过）。
  - **还没做的容器**：M4A（要走 moov/mvhd）、OGG/Opus（要末页 granule）→ 仍走原逻辑
    （时长显示 `--:--`）。这两类本来就在 seek 白名单之外（见上面的容器表）。
  - 调用点在 `audio_service.cc` 的 MUSIC_INFO 处理里（本地文件优先用探测值，
    顺带把 GMF 漏报的采样率也补上）。
- **根因②（卡死 / 刷屏）**：那个 FLAC 解码器**一帧都吐不出来**，但管线状态仍是 RUNNING
  → 界面显示"在播"、没有任何声音、日志一直刷，而**旧看门狗只看在线流**
  （注释里甚至写着"本地文件不会这样卡"，已被真机打脸）→ 本地这种情况**永远没人管**。
  **修法**：看门狗改成在线/本地都盯，阈值分开 —— 在线 20 s（重新缓冲可能真要十几秒）、
  **本地 8 s**（`LOCAL_STALL_TICKS`，SD 读 + 解第一帧只要 2~3 s）。
  本地超时 → `s_fail_streak++` + Toast「这个文件解不出来，已跳过」+ **`play_next(+1)`**
  （**不能**置 `s_advance_req`：那个走 done 分支，单曲循环下会把同一个坏文件再放一遍 =
  死循环）；连续 3 个都放不了就停下并提示。
- **仍未定位**：那个 FLAC 为什么解不出帧。解析器在**预编译库**里
  （`libesp_audio_simple_dec.a` 的 `es_parse_default.c` / `esp_es_parse.c`；
  反汇编 `flac_frame_verify` 看到它只是个 CRC-16 校验，报错来自它的调用方），
  而且**没有配置入口**（`esp_flac_dec.h` 的 open 是 `cfg=NULL, cfg_sz=0`）——
  §11.8 那个 `set_search_limit` 补丁管的是"首次同步头搜索"的总上限，不是这条
  按 `max_frame` 分块的搜索。**要定位必须拿到那个文件本身**（看它的 STREAMINFO、
  元数据块链和首帧位置）。

**循环模式：在线只支持 列表循环 / 单曲循环**（2026-10 用户报"在线单曲循环失效"）：

- **根因（两个都修了）**：`tick_cb` 里"放完 → 下一首"那块原来只有**本地**那套逻辑：
  ① `PLAY_MODE_ONE` 分支调的是本地的 `start_track(s_index)` —— 在线曲目不在本地库里，
  要么报"曲目不存在"、要么拿**同下标的本地曲目**顶上（这就是"单曲循环失效"）；
  ② 整块被 `storage_service_count() > 0` 挡着 —— 没插卡 / 本地为空时，在线歌放完
  **停住不续播**。现在按 `on_queue = (s_on_mode && s_on_songs && s_on_count > 0)` 分开：
  在线单曲循环走 **`play_next(0)`**（重新解析地址 → 起播，顺带换掉可能过期的签名地址），
  在线列表循环走 `play_next(+1)`，本地那两条逻辑不变。
- **在线禁用随机循环**：`on_mode_click()` 在线时只在 列表 ↔ 单曲 之间切，想切随机就拦下并
  Toast「在线播放不支持随机循环」；切到在线队列时（`app_music_play_online()`）若当前是随机，
  **兜底落回列表**（这里不弹 Toast —— 会被紧接着的"缓冲中"顶掉），并且 `play_next()` 的
  在线分支里**删掉了随机分支**，不给"在线也能随机"的错觉。
- **播放器页显式标注来源**（需求）：标题上方一个小药丸 `apply_src_badge()` ——
  **在线播放**（强调色底、深色字）/ **本地播放**（灰底、暗字）。
  它由 `tick_cb` 每 500 ms 兜底刷新（内部比较上次画的状态，没变就直接返回，零开销），
  所以换队列/切歌的路径再多也不会漏。页面销毁时记得把 `s_p_src_badge` 置空 +
  `s_src_badge_state = -1`（否则重进页面不重画）。

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

#### 11.5.5 重试必须**轮询已保存的列表**，不能死磕一个（2026-10 用户报）

**现象**（用户原话："为什么要一直对一个 wifi 重试？不能根据已保存的列表轮询检测吗？"）：

```
W wifi_svc: connect '蔚蓝档案千禧科技学院' failed: reason 201 (没找到这个 AP（信号弱/在 5G 频段？）)
W wifi_svc: quick retries exhausted, retry '蔚蓝档案千禧科技学院' every 60 s from now on
I wifi_svc: slow retry '蔚蓝档案千禧科技学院'      ← 每 60 秒刷一次，永远是同一个
W wifi_svc: connect '...' failed: reason 201
```

**根因**：`wifi_retry_cb()` 里是 `store_find(s_conn_ssid)` —— 永远重连"当前目标"
（开机取 `s_store.list[count-1]`，即**最近保存的那个**）。存了多个网络时，
只有一个在范围内的情况下，它会把 60 秒一次的重试**全部**花在那个连不上的 SSID 上，
另一个明明可用却永远轮不到。

**修法**（都在 `wifi_service.cc`）：

| 点 | 做法 |
| --- | --- |
| 轮询游标 | `s_cred_idx` = **下一次重试**要试的已保存凭据下标；`wifi_retry_cb()` 试完当前这个就 `(idx+1) % count` |
| 发起连接 | `cred_cursor_next(ssid)` 把游标指向**目标的下一个**（开机自动连 / `wifi_service_connect()` / `connect_saved()` 三处都调）→ 它要是失败，下次重试自然换一个 |
| 连上了 | `WIFI_EVENT_STA_CONNECTED` 里把游标**指回它自己** —— 掉线先重连刚刚还好用的那个，再往后面轮 |
| 快速额度 | `wifi_retry_budget_refill()` 取 `max(WIFI_RETRY_MAX, s_store.count)` —— **每个已保存的网络至少被快速试一次**（否则存 8 个、额度 5，后面的永远轮不到） |
| 日志 | `connect 'X' (try, N retry left, saved 2/3)` / `slow retry 'X' (saved 2/3)`，轮询到第几个一眼可见 |
| forget 之后 | `s_cred_idx >= s_store.count` 时在 `wifi_retry_cb` 开头夹回 0（列表会变短，游标不能越界） |

⚠ 注意区分"轮询"和"扫描"：`reason 201` = `WIFI_REASON_NO_AP_FOUND`，**那个 AP 此刻确实不在范围内**
—— 所以**只有一个已保存网络时轮询也救不了**（那是环境问题，不是代码问题），日志里会一直刷。
轮询解决的是"存了好几个网络、只有一个可用"的场景。
更彻底的一步（**未做**）：重试前先 `esp_wifi_scan_start()`，只在**扫描可见**的已保存网络里挑，
这样每个不在范围的 AP 能省掉一次 2~4 秒的失败连接，还能顺手在 UI 上提示
"你保存的网络当前都不在范围内"。

#### 11.5.6 ⚠ "放歌一卡一卡"的真凶是**蓝牙 A2DP 链路带宽**，不是 SD/解码（2026-10 真机定位）

用户报"放一些歌曲会一卡一卡"，日志（**本地** FLAC → A2DP 耳机）：

```
I AUD_Dec_Parse: FLAC sample_rate 44100 duration:194931 max_frame:20323
E AUD_Dec_Parse: Not found frame in max frame size 20323      ← 9 次后自己恢复了，与卡顿无关
I media_meta: audio probe: 0 bps, 194931 ms, 44100 Hz, 2ch     ← ⚠ 见下面的码率 bug
I bt_svc: a2dp tx: 8000 frames, 289 f/s (need 345 f/s), dropped 0 B    ← ★ 只发了需求的 84%
I audio_svc: play stats: PCM 157 KB/s (820312 B in 5076 ms)            ← 满速应 192 KB/s
E BT_L2CAP: l2cab is_cong_cback_context                                ← L2CAP 拥塞回调
```

**判读**（这套日志组合起来就把范围锁死了）：

| 观测 | 含义 |
| --- | --- |
| `need 345 f/s` vs 实际 289 f/s | A2DP 每秒少发 16% 的编码帧 → 对端欠载 → **一卡一卡** |
| `PCM 157 KB/s`（应 192） | 整条解码链被**拖慢到 82%**（不是它自己慢） |
| `dropped 0 B` + 环形缓冲没满 | **不是我们丢帧**，是下游持续只吃得下这么多 |
| `l2cab is_cong_cback_context` | L2CAP 缓冲被填满 = 链路/对端给不出更多发送缓冲 |
| CPU 极低 | 任务都在等射频，不在算 |

所以最初"CPU 低 → 是不是没在解码"的怀疑方向是错的：**解码是异步且跟得上，是蓝牙拖住了它**。
`bt_service_write_pcm()` 的阻塞发送（上限 200 ms）本来就是**故意**把链路节拍交还给解码管线的，
所以链路一慢，PCM 速率就跟着掉 —— 这是设计使然，不是 bug。

**这一轮改的三件事**（都对症，但**根治要看下一次日志**）：

1. **SBC bitpool 封顶**（`A2DP_SBC_BITPOOL_MAX = 40`）：码率需求由 bitpool 决定，
   joint-stereo/16block/8subband 下 44.1 kHz 的 bitpool 53 ≈ 328 kbps、64 ≈ 400 kbps，
   廉价耳机 + 2.4 GHz 共存时经常吃不下。压到 40（≈250 kbps）给链路留余量。
   **合法性**：SBC 每帧帧头都带 bitpool 字段，对端按帧头解码，不需要重新协商
   （各家"音质/稳定性"档位就是这么做的）。代价是听感略糊。恢复原样改成 250 即可。
   ⚠ 必须**拷贝**协商结构体再改（`info.codec_cfg` 是协议栈的内存，不能就地改）。
2. **馈送速率地板拆掉**：`feed_task` 原来"没数据就睡 5 ms"，等于给馈送速率设了
   512 B/5 ms ≈ 100 KB/s 的地板（< 需要的 192 KB/s）—— 缓冲一旦被抽空就再也追不上。
   现在无重采样路径改成**带超时的阻塞取**（有数据立刻返回），有重采样路径的等待从 5 ms 降到 1 ms。
3. **诊断计数**（下次日志一眼定位）：`a2dp tx` 那行加了
   `acq_retry N (M ms slept), rel_to N, enc N us/frame, idle N, ring N B` ——
   - `acq_retry`/`ms slept` 大 ⇒ 卡在**协议栈/链路**（给不出发送缓冲）；
   - `enc us/frame` 大 ⇒ 卡在我们自己的 SBC 编码；
   - `ring B` 一直贴 0 ⇒ 上游（解码）供不上；一直满 ⇒ 下游（链路）吃不下。
   另外启动时会打 `a2dp budget: 345 f/s x NNN B = NNN kbps`，一眼看出**链路得扛多少**。

**顺带修的真 bug**：`media_meta` 对 FLAC 的码率算成了 `0 bps`（`audio probe: 0 bps`）——
成因是元数据块链走完后的 `data_off` 偶尔算飞，`file_size - data_off` 不合法就**静默**不填码率
（这个 FLAC 就是）→ 后果是这些文件**拖不动进度条**（报"定位失败：码率未知"）。
现在加了兜底（`file_size - 42`）+ 一行 `flac: data_off N, audio N B, bitrate N bps (avg)`
诊断日志，不会再悄悄变 0。

**还没做但更根本的两件事**（按性价比排序）：

1. **WiFi/BT 共存**：2.4 GHz 上 WiFi 与 BT 共享射频。用户那台机器当时 WiFi 正在
   **每 60 秒重试一个连不上的 AP**（见 §11.5.5），每次连接尝试都会抢走射频时间。
   最快的验证：**关掉 WiFi 再放同一首歌**；若明显好转，就该做成"BT 正在放音时不主动
   scan/连接，只做轻量重试"。
2. **重采样路径的取数改成"整段阻塞"**（`xStreamBufferReceive` 配 trigger level 或加一级
   中间缓冲），彻底去掉轮询；现在是 1 ms 轮询，已经不是瓶颈。

### 11.5.7 ⚠ 板载输出也卡 ⇒ 瓶颈在**解码链**，蓝牙只是又一个受害者（2026-10 用户追报）

用户补充："这个音频文件不止蓝牙卡，**板载音频输出也卡**。" —— 这句话把上一节的结论修正了一半：

- **板载 I2S 是硬件时钟**（DMA 按 48 kHz 精确取数），**不存在"带宽不够"**；出现卡顿
  只有一个解释：**解码链产不出实时 PCM**（每 10 秒少 ~1.6 秒的音频）。
- 所以蓝牙侧的 `289 f/s (need 345)` 很可能**不是链路带宽问题**，而是**上游本来就只喂了 84%**：
  `bt_service_write_pcm()` 的阻塞发送只是把"上游慢"如实反映出来。
  （bitpool 封顶与馈送地板那两个改动仍然合理 —— 它们各自是独立的问题 —— 但**不是**这个的根因。）

**顺带排除的一个怀疑**（查过了，代码是对的）：选了扬声器输出时，PCM **不会**同时喂蓝牙 ——
`out_write_cb()` 里是 if/else 分流（`AUDIO_OUT_BLUETOOTH` → BT；否则 → `esp_codec_dev_write`），
所以"板载卡是蓝牙拖的"不成立。

**这个文件的特征**（下一条线索）：`FLAC 44100 Hz 24 bit, max_frame 20323` ——
位深 24 bit、`max_frame` 高达 20 KB（常见 FLAC 只有 3~7 KB）。整条链是
`FLAC(24bit) 解码 → 44.1k→48k 重采样 → 24→16bit → 48k/2ch 输出`，
其中**解码**与**重采样**都可能吃满 CPU（单核 320 MHz，且 LVGL/WiFi/蓝牙都在抢）。

**这一轮加的分级耗时统计**（托管组件补丁，同 §11.7/§11.8 的补丁一起会丢）：
`esp_gmf_audio_dec.c` 与 `esp_gmf_rate_cvt.c` 各加了一个 5 秒窗口的统计：

```
I ESP_GMF_ASMP_DEC: aud dec: 117 calls, 3200000 us cost, 4012 ms PCM out -> 79.8% of realtime
I ESP_GMF_RATE_CVT: rate cvt: 117 calls, 890000 us cost, 4012 ms audio (src 44100 Hz) -> 22.2% of realtime
```

判读（`% of realtime` = 处理这段音频花掉了多少实时时间）：
- **解码**那一行接近或超过 **100%** ⇒ FLAC 解码本身跟不上（24 bit + 大帧的字形，CPU 硬伤）；
- **重采样**那一行占比很大（>40%）⇒ `44.1k→48k` 重采样是主因；
- 两行都低（合计 <70%）⇒ 不在计算上，得往管线调度/IO 阻塞方向查。

**要定位下一步，需要**（真机日志）：
1. **板载输出**（设置里把输出切到扬声器）放这个文件 15 秒的日志，重点是：
   `play stats: PCM x KB/s`、上面两行新的耗时统计、以及**播放中**是否还有
   `AUD_Dec_Parse: Not found frame in max frame size ...`；
2. 如果方便，**把这个 FLAC 拷到电脑上**发我 —— 有文件就能离线看它的 STREAMINFO /
   元数据块链 / 帧结构，把"解不出帧"（§11.8 遗留）和这次的"跟不上"一起定死。
   没有文件只能靠上面两行占比来二分。

#### 11.5.8 CPU 假说被数据否掉：解码 7~13%、重采样 20%，整条链却只跑 73% ⇒ 在**等**（2026-10）

用户提供了**分级耗时统计**的实机日志（本地 FLAC，板载/蓝牙都一样卡），关键几行：

```
I ESP_GMF_ASMP_DEC: aud dec: 1296 calls, 387611 us cost, 5248 ms PCM out -> 7.4% of realtime
I ESP_GMF_RATE_CVT: rate cvt: 41 calls, 754673 us cost, 3808 ms audio (src 44100 Hz) -> 19.8% of realtime
I audio_svc: play stats: PCM 139 KB/s (731152 B in 5106 ms), io=2610688 B, calls=41, flow=1
```

| 观测 | 结论 |
| --- | --- |
| 解码 **7.4%**、重采样 **19.8%** of realtime | **CPU 完全不是瓶颈**（合计 1/3 个核，还剩 2/3 空闲） |
| 整条链 PCM **139~142 KB/s**（应 192）= 73% | 稳态就是**缺 27%**，且**很稳定**（不是偶发抖动） |
| `io=` 每 5 s 涨 ~692 KB = 135 KB/s，而文件平均 199 KB/s | 连**读文件**的字节速率都只有 68% ⇒ 卡在"等"，不在"算" |
| 解码 105%（5248 ms/5 s）而重采样 75%（3808 ms/5 s） | 两者的 5 s 窗口**不是同一段**（各自从首次调用起算），不能逐窗口对比；长期稳态两边必然相等 |

**另一个更严重的现象**：`E ESP_GMF_TASK: Run timeout` + `audio_svc: run '...' failed: -3`
—— 文件放到 ~5 s 被**整个掐死**（`esp_gmf_task_run()` 等 `GMF_TASK_RUN_BIT` 超过
`api_sync_time` 就返回 `ESP_GMF_ERR_TIMEOUT`，见 `esp_gmf_task.c:684`）。
首轮任务里有 FLAC 解析器**连续 9 次** `Not found frame in max frame size 20323`
（每次都要扫最多 20 KB）＋ 24→16bit 输出缓冲重分配（`Not enough memory for out,
need:27648, old: 24576`），首轮很容易超过同步超时 ⇒ 被当成致命错误、直接跳过这首。
**这是"有些歌点进去放不出来/刚放就跳下一首"的直接原因之一。**

**还有一条强线索**：`media_meta` 的块链遍历在这两个文件上**都算飞了**：

```
W media_meta: flac: bad data offset 94542957 (file 38834495), fall back to 38834453 B
W media_meta: flac: bad data offset 138083655 (file 36897355), fall back to 36897313 B
```

数据偏移算到了 94 MB / 138 MB，而文件只有 38 MB / 36 MB —— 说明**这两个 FLAC 的元数据
块链不正常**（某个块的长度字段被读成了巨大的值，或者块结构与常规不同）。
同一个异常结构很可能就是 ESP 解析器**一直在 `Not found frame`** 的原因
（它按 `max_frame` 分块找同步字，遇到非标准结构就找不到帧）。

**这一轮加的"等待点"统计**（托管组件补丁）：

| 位置 | 日志 | 判读 |
| --- | --- | --- |
| `esp_gmf_io_file.c` 的 `_file_acquire_read` | `ESP_GMF_FILE: file read: N calls, M ms cost, B B in W ms -> K KB/s, P% of the window spent reading` | `P%` 大 ⇒ **卡在读 SD**（这也是当初"给 io_file 加预读缓冲"那个方向，但**这次要先看数据再动手**，别再盲改） |
| `audio_service.cc` 的 `out_write_cb` | `play stats: ... \| out M ms in codec, gap max G ms, audio A ms` | `gap max` 正常 ≈ 一个块的长度（~46 ms）；出现几百毫秒 ⇒ **被饿住**（解码/任务调度）；`out` 明显大于音频时长 ⇒ **codec/I2S 慢** |

判读组合（下一次日志直接定位）：
- `file read` 的 `P%` > 40% ⇒ SD 读是主因 ⇒ 才轮到做预读缓冲 / 换卡测试（用电脑读同一张卡对比速度）；
- `gap max` 很大而 `file read` 很小 ⇒ 任务被抢占或上游没数据 ⇒ 查任务优先级/核心亲和；
- `out` ≫ `audio` ⇒ codec 写阻塞异常 ⇒ 查 I2S/DMA 配置；
- 三者都不高 ⇒ 时间花在 GMF 内部（端口/队列）或**解析器空转**（`Not found frame` 的代价）。

#### 11.5.9 块链留证：那两个 FLAC 的元数据块链确实不正常（2026-10）

§11.5.8 的 `bad data offset 94542957 (file 38834495)` 只说了"偏移算飞"，没说**是哪一块**。
现在 `probe_flac()` 会把走到的每一块（type / len / pos，最多 16 块）留证，并在
**块长度 > 文件大小**时立刻停下（继续加只会越飞越远，真机到过 138 MB）：

```
W media_meta: flac: block type 4 len 4294967295 exceeds file 38834495, stop walking
W media_meta: flac: bad data offset 94542957 (file 38834495, 3 blocks seen:
W media_meta: flac:   block[0] type 0, len 34, pos 4
W media_meta: flac:   block[1] type 3, len ..., pos 42
W media_meta: flac:   block[2] type 4, len 4294967295, pos ...
W media_meta: flac: fall back to 38834453 B
```

判读：
- `type 0` = STREAMINFO、`3` = SEEKTABLE、`4` = VORBIS_COMMENT、`6` = PICTURE ✓ 正常；
- 某个块的 `len` 是 **0xFFFFFFFF / 几十 MB** ⇒ 块头被读错位了（前一块的长度算错、或
  文件里嵌了非 FLAC 数据段）⇒ 与 ESP 解析器一直 `Not found frame` 高度相关；
- 如果块链看起来**正常**而偏移仍算飞 ⇒ 是我们这边（`be24`/`ftell`）的问题，得改代码。

⚠ 顺带说明：这条诊断只影响**码率/时长**（`media_meta` 自己算的），
**不改变播放**——播放走的是 ESP 的解析器，它有自己的一套。

#### 11.5.10 ✅ 用户给了文件：**文件完全合规**，"偏移算飞"是我们自己的 bug（2026-10 定案）

用户把文件拷到 PC（`C:\Users\VanillaNahida\Videos\蔚蓝档案 RE Aoharu 电吉他版.flac`），
离线解剖（`tools/font/venv` 里的 `build/flac_check.py` + ffprobe）：

```
block 0: pos=4        type=0 (STREAMINFO) len=34
    STREAMINFO: 44100 Hz, 2 ch, 24 bit, 8,596,480 采样 (194.932 s), block size 4096..4096
block 1: pos=42       type=4 (VORBIS_COMMENT) len=111
block 2: pos=157      type=6 (PICTURE) len=423,169
block 3: pos=423,330  type=1 (PADDING) len=4,096
音频数据起点: 427,430        ← 设备端曾算出 94,542,957（差 94 MB）
逐帧校验: 2099 帧，帧头 CRC-8 通过 2098 / 失败 1（仅最后一帧）、同步字错 0
块大小码 12（= 4096 采样，固定值）、位深码 6（= 24 bit）全部一致
```

**结论**：

1. **文件是标准的** —— ffprobe 与自写解析器都毫无困难，帧头 CRC-8 全过。
   `Not found frame in max frame size 20323` 是 **ESP 侧解析器**的问题，不是文件坏了。
2. **"bad data offset 94 MB" 是我们的 bug**（`probe_flac` 走块链时**没有 fseek 跳过块数据**，
   于是下一轮把"上一块的数据"当成块头读 → 长度变垃圾 → 偏移越滚越大）。
   这个 bug 的**唯一后果**是 `media_meta` 的**平均码率/时长**不准（→ 那些 FLAC 拖不动进度条），
   **不影响播放**。修法已在工作区（每步都真的 `fseek(nlen, SEEK_CUR)`），
   修好后算出来正好是 **427,430**，与 PC 完全一致。
3. **`Not found frame` 只出现在起播那 ~900 ms 内**（9 次），稳态播放期间**没有**它 ——
   所以它**不是**"只跑到 73%"的原因，别把它当主因。

**三个文件是同一套参数**（都是 24-bit / 4096 采样块 / 大帧，应该是同一个压制工具出来的）：

| 文件 | 采样率 | 位深 | 块大小 | 帧数 | 帧头 CRC-8 |
| --- | --- | --- | --- | --- | --- |
| 蔚蓝档案 RE Aoharu 电吉他版 | 44100 | 24 | 4096 | 2099 | 全过 |
| 最孤独的音色——正弦波 | 48000 | 24 | 4096 | 3275 | 全过 |
| …准备出发！ | 48000 | 24 | 4096 | 1067 | 全过 |

⇒ 下一步的关键是**把"24-bit 特有环节"的耗时也量出来**：链上只有 `aud_bit_cvt`
（24→16 bit 逐样本转换）是"只在 24-bit 文件里才真干活"的元件，而它一直没被统计。
本轮给它和 `aud_ch_cvt` 都加了同样的 5 秒窗口统计（`ESP_GMF_BIT_CVT: stage: ...`）：

- 若它的 `% of realtime` 很大 ⇒ **24→16bit 的软件转换**就是缺口 ⇒ 应当让解码器直接吐
  16-bit（避免整条 24-bit 链路），或换更省的做法；
- 若它也很小（各段加起来仍 ≪100%）⇒ 时间不在计算上 ⇒ 看 `file read`（SD）与
  `play stats` 的 `gap max`（被饿住）那两行。

#### 11.5.11 ✅ 拿到文件后的三处收尾（2026-10）

**① 我们自己的 bug 修掉了**（§11.5.10 已定案：文件合规、是 `probe_flac` 走块链时漏了 fseek）。
修好后同一份文件应打出与 PC 完全一致的数：

```
I media_meta: flac: data_off 427430, audio 38407065 B, bitrate 1576232 bps (avg)
```

（以前是 `bad data offset 94542957` + 兜底 38834453 B → 码率偏高 1.1%，
seek 的时间→字节映射也跟着偏；现在准确了。）

**② `Run timeout` 的真凶是 ASP 那一行，不是 GMF 的宏**。

- `esp_gmf_task.c` 的 `DEFAULT_TASK_OPT_MAX_TIME_MS(2000)` 会被
  **`esp_audio_simple_player.c:302` 的 `esp_gmf_task_set_timeout(player->work_task, 5000)` 覆盖**
  —— 所以只改宏**没有任何效果**（我先改了宏，发现这个后又撤回）。
- 真机 `Run timeout` 出现在文件打开后**正好 5.0 s**（1704260 → 1709295），
  正是这 5000 ms：首轮要走过 423 KB 封面（含 9 次假同步字试探）＋24→16bit 输出缓冲
  重分配＋重采样器首次分配，超时就被当成**致命错误**直接跳过这首。
- 现在把**这一行**改成 **15000 ms**（只放宽 API 同步等待，不改变音频行为）。
  ⚠ 要调就调这一行；`esp_gmf_task.c` 里的宏对本工程无效。

**③ 四级阶段耗时统计齐了**，下一份日志可以**逐段相加**：

| 段 | 日志 | 说明 |
| --- | --- | --- |
| FLAC 解码 | `ESP_GMF_ASMP_DEC: aud dec: ... -> x% of realtime` | 实测 7~13% |
| 44.1k→48k | `ESP_GMF_RATE_CVT: rate cvt: ... -> x% of realtime` | 实测 ~20% |
| **24→16bit** | `ESP_GMF_BIT_CVT: stage: ... -> x% of realtime` | ★ **只有 24-bit 文件才真干活**，之前从没量过 |
| 声道转换 | `ESP_GMF_CH_CVT: stage: ... -> x% of realtime` | 2ch→2ch 基本是直通 |
| 读文件 | `ESP_GMF_FILE: file read: ... P% of the window spent reading` | SD 等待 |
| 写输出 | `play stats: ... \| out M ms in codec, gap max G ms, audio A ms` | codec 等待 / 被饿住 |

判读总原则：**把各段 % 加起来**。
- 合计 ≈ 100% ⇒ 就是计算/IO 本身慢，找占比最大的那段下手；
- 合计 ≪ 100%（而整条链就是跑不满）⇒ 时间花在 GMF 的**端口/队列等待或任务调度**上，
  重点看 `gap max`（几百毫秒 ⇒ 被饿住，查任务优先级/核心亲和）。

#### 11.5.12 ES8389 支持 24-bit，**是我们按 16-bit 打开的**（2026-10，用户提示后核实）

用户提示"板载的 DAC 是 ES8389"。核实结果：

| 事实 | 位置 |
| --- | --- |
| ES8389 驱动支持 **16 / 18 / 20 / 24 / 32-bit** | `es8389_set_bits_per_sample()`（`case 24: ES8389_S24_LE` 等，寄存器位见 `es8389_reg.h`） |
| 字长由 `fs->bits_per_sample` 决定，驱动自己会配寄存器 | `es8389_config_sample(codec, fs->sample_rate, fs->bits_per_sample)` |
| **我们打开时写死 16-bit** | `bsp_audio_set_sample_rate()` 里 `.bits_per_sample = 16`（通用打开路径有 `bits` 参数，但目前传的就是 16） |

⇒ 链路上那次 **24→16bit 转换是我们自己的选择**，不是 ES8389 的限制。
（另一路蓝牙：SBC 本身就是 16-bit，天然要转。）

**但这修不了"只跑到实时 73%"** —— 先把两件事分清：

- **73% 是"等待"问题，不是算力问题**：实测解码 7~13% + 重采样 20% ≈ 1/3 个核，
  而解码管线是**单线程**的 → 任务 27% 在算、**73% 在阻塞等待**。
  `aud_bit_cvt`（24→16，纯逐样本运算）最多让 CPU 再多几个百分点，
  **不会**把 73% 变成 100%。
- 真正要抓的是那个**阻塞点**（读 SD / 写 codec / 端口等待）—— 也就是已经埋好的
  `ESP_GMF_FILE: file read ... P%` 与 `play stats: ... out M ms, gap max G ms` 三行。

**若日志确认 `ESP_GMF_BIT_CVT` 占比真的大**，这条 24-bit 路线的代价也要先摆清楚：

1. **时间基要变成按格式计算**：现在是写死的 `192 B/ms`（48k/2ch/16bit）；
   24-bit 是 **288 B/ms**。这个常量被进度条、seek 的时间→字节映射、看门狗、
   `play stats` 的音频时长共用 —— 属于**易回归区**（上次 seek 的教训）。
2. **蓝牙那一路仍是 16-bit**（SBC）⇒ 要么按输出设备建不同管线，要么在 BT 侧转换。
**补充核实（同一轮，决定"要改的话改哪里"）**：

| 事实 | 位置 / 含义 |
| --- | --- |
| codec **只在 init 打开一次**，此后不重开 | `bsp_audio_open_output(OUT_RATE_HZ, OUT_CHANNELS, OUT_BITS)`（`audio_service.cc:477`）；`bsp_audio_set_sample_rate()`（会 mute+重开）**全工程没有调用者** ⇒ 切歌不会有 codec 断音，格式改动也只影响这一处 |
| 输出格式常量就三个 | `OUT_RATE_HZ 48000` / `OUT_CHANNELS 2` / `OUT_BITS 16`（`audio_service.cc:36-38`） |
| 时间基只在**两处**用 | `OUT_PCM_BYTES_PER_MS (192)`（`audio_service.cc:83`）→ `timeline_now()`（191）与诊断日志（179）⇒ 改成"按格式算"是小改动 |
| 24→16 那次转换的来源是 **Kconfig**，不是硬件 | `CONFIG_AUDIO_SIMPLE_PLAYER_BIT_CVT_DEST_BITS`（`audio_simple_player_pool.c` 建 `aud_bit_cvt` 用它）；同理 `CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE`（48k）、`..._CH_CVT_DEST_CH` |
| ⚠ 但**全局**改成 24-bit 会打断蓝牙 | `bt_service_write_pcm()` 把缓冲当 **int16** 处理（`A2DP_PCM_BITS 16`），24-bit 数据会被当作错位的 16-bit ⇒ 噪音。要 24-bit 就得**按输出设备建不同管线**（扬声器 24 / 蓝牙 16），这是这条路线真正的工作量 |

⇒ 结论不变：**先看那 6 行日志**。若 `ESP_GMF_BIT_CVT` 占比确实大 *且* 卡顿被证明是算力问题
（现有数据恰恰说明不是），再按"按输出设备分管线 + 时间基按格式算"来做。
3. 既然 ES8389 也支持 44.1k/48k，**同样的取舍也适用于重采样那 20%**：
   让板载输出跟随源采样率（44.1k）可以省掉重采样，但时间基变成 176.4 B/ms ——
   和上面是同一类改动，同样要先有数据再决定。

#### 11.5.13 ✅ 根因定案：**SD 每次只读 512 字节**，读卡限流到实时 71%（2026-10）

板载输出 + 24-bit FLAC 的完整 `play stats` 日志（稳态六个窗口一致）：

| 每 5 秒窗口 | 实测 | 占窗口 |
| --- | --- | --- |
| FLAC 解码 | 386 ms（**7.9% of realtime**） | 8% |
| 44.1k→48k 重采样 | 695 ms（**19.7%**） | 14% |
| **24→16bit** | 22 ms（**0.6%**） | 0.4% |
| 声道转换 | 0.4 ms（**0.0%**） | ~0 |
| **读文件（阻塞）** | **1207 ms / 1394 次调用** | **24%** |
| 写 codec（阻塞） | 2548 ms | 51% |
| 实际送出音频 | **3529 ms / 5000 ms = 71%** | — |

**关键两点**：

1. **`bit cvt` 只占 0.6%** ⇒ 24-bit 链路**实测排除**（解码+重采样+位深+声道合计 ~28% of realtime，
   CPU 绝不是瓶颈）。ES8389 支持 24-bit 是真的，但没有必要为性能去动它。
2. **`file read: 1394 calls, 713728 B` ⇒ 平均每次只读 512 字节** ⇒ 每秒 ~275 次读，
   其中 **1.2 s 花在等 SD** 上；读卡只给到 **138 KB/s**，而文件平均 **199 KB/s** ⇒
   整条链被读卡限流在 **71%**。这就是"卡"的唯一原因（与蓝牙、与 24-bit、与 CPU 都无关）。

**修法**：给 `io_file` 加**预读环形缓冲**（`ensure_file_io()`）：
`thread.stack = 4096` + `prio = 5` + `io_size = 8 KB` + `buffer_size = 64 KB`
⇒ 每次读 8 KB（16 倍于 512 B）、每秒 ~25 次，等待时间应降到 ~0.12 s / 5 s。

⚠ **上次翻车的地方**：只设了 `buffer_size`、漏了 `thread.stack` ⇒ `io->type` 不是 BLOCK、
没有 IO 任务 ⇒ `esp_gmf_io_seek()` 的异步分支（abort data bus → IO 任务执行 seek）
**永远没人执行** ⇒ 本地拖动失效。所以：

- 两个字段**必须一起给**（§12.4.4 已记录）；
- **烧录后第一件事就是验拖动定位**（`audio_svc: seek -> ...` + `seek landed: ... diff 0 ms`
  这两行应正常出现）；
- 顺带把 `file read` 日志改成显示 **B/read 与 µs/read**（旧版整数毫秒会把
  1394 次共 1207 ms 显示成 `0 ms/call`，正是它把"512 字节/次"这条线索藏了起来）。

**另外记两个这次日志顺带确认的事**：

- **拖动定位已经好了**：`seek -> 21000ms (byte 4137609, 1576232 bps, average)` +
  `resumed at 21000 ms` + `seek landed: byte 4137609 -> 21000 ms (diff 0 ms)`
  —— 码率 1576232 正是 `probe_flac` 修好 `fseek` 之后的正确值（§11.5.10/11.5.11）。
- **WiFi 轮询策略生效**：`connect ... (try, 3 retry left, saved 2/2)` → 失败后
  `... (try, 2 retry left, saved 1/2)` → 连上了第二个 AP（`4B103-蒲公英X1-Pro-2.4G`，
  `got ip 10.168.1.210`）⇒ §11.5.5 的轮询确实按预期工作。

#### 11.5.14 ❌ 预读尝试导致整机 panic（已回退）+ 补上 GMF `_file_seek` 判空（2026-10）

**先说结论**：§11.5.13 的预读（`ensure_file_io()`）**已回退**。它不是"慢"，而是**崩**。

**崩溃现场**（真机，第一个 panic 的栈）：

```
Guru Meditation Error: Core 1 panic'ed (Load access fault)
  fseek(stream=0x0)                                   ← NULL
   _file_seek (esp_gmf_io_file.c:244)
    esp_gmf_pipeline_seek
     audio_service_seek_ms(pos_ms=0)
```

**触发链（两个 bug 叠加）**：

1. **我们的 bug**：进度记忆里存的是 **197000 ms**，而这个文件只有 **194931 ms** →
   恢复时按字节 seek 正好落到**文件尾** → 解码器立刻 EOF → 管线收尾关闭。
2. **GMF 的 bug**：`_file_close()` 会把 `file_io->file` 置 **NULL**，而
   **`_file_seek()` 没有判空**（同一个文件里的 `_file_reset()` 一直是有判空的）→
   收尾逻辑再发一次 seek 就是 `fseek(NULL)` → 整机 panic。

预读只是把 IO 变成"有自己的任务/数据总线"，让这条"关闭后再 seek"的路径更容易被走到
（日志顺序：`ESP_GMF_FILE: CLose, pos = 38834495/38834495` → 紧接着 panic）。

**本轮修的两处**：

| 修哪 | 内容 |
| --- | --- |
| **GMF 判空**（托管组件补丁，同 §11.7/11.8 会随重装依赖丢失） | `esp_gmf_io_file.c` 的 `_file_seek()`：`file_io == NULL \|\| file_io->file == NULL` 时打一条 WARN 并返回 `ESP_GMF_ERR_INVALID_STATE`（`esp_gmf_pipeline_seek()` 会当普通失败上报，不再崩） |
| **进度恢复越界夹取**（`app_music.cc`） | 新增 `RESUME_END_GUARD_MS (5000)`：恢复位置落在"离总长不到 5 秒"的范围内就**从头放**（`resume pos N ms is at the end of M ms track, start from 0`）。既堵住触发条件，听感上也更合理 |

**教训（写给自己）**：这类"改了 IO 配置"的改动，**验证顺序必须是**
① 拖进度条 → ② 放到底自然结束 → ③ **用一个"进度记忆超过总长"的曲子起播**（本轮就是第③条漏了）。
第二条在改完之后要重跑一遍。

### 11.6 拼音搜索

搜索浮层用 `lvgl_pinyin_ime`（`lv_pinyin_ime_create()` + `attach(textarea)`，
组件初始化是**同步**的，第一次点搜索会卡约 1 秒加载词库）。
`app_online` 里的 IME 实例只建一次，关浮层时 `hide` 不销毁 —— 重复建会反复加载词库与字体。

### 11.7 ⚠ 补丁过的托管组件：`gmf_audio` 里 3 个元件（2026-10）

**这些是改在 `managed_components/` 里的**，`idf.py update-dependencies`、
删 `managed_components/` 或换 `dependencies.lock` 都会把它们冲掉。改完记得回来补。
（同一目录下还有 §11.8 的 FLAC 搜索上限补丁，也在 `esp_gmf_audio_dec.c` 里。）

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

### 11.8 ⚠ 补丁：FLAC 内嵌大封面导致整首放不出来（2026-10 真机踩过）

**症状**（本地 FLAC，41 MB）：

```
I (641945) audio_svc: playing file:///sdcard/024-Shoujo Delight_38762618.flac
I (642184) AUD_Dec_Parse: FLAC sample_rate 48000 duration:138172 max_frame:6942
E (643192) ESP_ES_PARSER: Search overlimited 512000
E (643193) ESP_GMF_ASMP_DEC: Failed to decode data, ret: -7
I (643203) ESP_GMF_FILE: CLose, pos = 513024/41671543
E (643248) audio_svc: player error state
```

**根因**：FLAC 的元数据块（尤其内嵌封面 PICTURE）可以有好几百 KB 到几 MB，**音频首帧在
它后面**；而 `esp_audio_simple_dec` 里那个 es_parser 在"找首帧"时默认只肯扫 **512000**
字节。反汇编 `libesp_audio_simple_dec.a` 的 `esp_audio_simple_dec_open` 看得很清楚：

```asm
190: li   a5,50          ; 内部参数 = 50
194: lui  a1,0x7d        ; ★ a1 = 0x7D000 = 512000
198: lw   a0,4(s0)       ; 解析器句柄
19a: sh   a5,40(s0)
1a0: jalr ...            ; esp_es_parse_set_search_limit(parser, 512000)
```

**修法**（`managed_components/espressif__gmf_audio/esp_gmf_audio_dec.c`，
和 §11.7 一样是托管组件补丁，重装依赖会丢）：

```c
/* 组件头文件里没声明，符号是导出的（nm 里是 T） */
extern esp_audio_err_t esp_audio_simple_dec_set_search_limit(void *handle, uint32_t search_limit,
                                                             uint16_t reserved);
static esp_audio_err_t aud_dec_open_patched(esp_audio_simple_dec_cfg_t *cfg, void **handle)
{
    esp_audio_err_t ret = esp_audio_simple_dec_open(cfg, handle);
    if (ret == ESP_AUDIO_ERR_OK && *handle != NULL) {
        esp_audio_simple_dec_set_search_limit(*handle, 4 * 1024 * 1024, 50);   /* 50 = 库默认 */
    }
    return ret;
}
```

文件里**三处** `esp_audio_simple_dec_open()`（open / 探测后 open / reopen）都改成走这个
包装，任何一条打开路径都会放宽上限。

- **签名是反汇编确认的**：`handle` + 32 位字节上限 + 16 位内部参数（库里默认
  `(512000, 50)`）；`esp_audio_simple_dec_set_search_limit` 内部把第 3 个参数以 `sh`
  写进 handle 偏移 40，把第 2 个参数转交给 `esp_es_parse_set_search_limit`（那里是
  `sw a1,104(a0)`，确认是 32 位字节数）。第三个参数保持库默认值 50，别乱动。
- **不额外吃内存**：搜索是"边读边丢"的流式扫描（只推进位置），所以放宽到 4 MB 只是允许
  它多读一段元数据；普通文件首帧就在开头，零影响。
- 升级 `esp_audio_codec` 后如果这个符号没了，**链接会失败**（正好能发现），
  那时去看新版本有没有公开的配置入口。

---

## 12. 本地音乐列表 与 UI 帧率（2026-10 修复）

### 12.0 ★★ 布局自反馈 = LVGL 空转 + 看门狗超时（2026-10 真机踩过）

真机日志长这样（BT 在放歌，`lvgl` 一直跑、IDLE1 饿死）：

```
E task_wdt: Task watchdog got triggered. ... - IDLE1 (CPU 1)
E task_wdt: CPU 1: lvgl
--- 0x401db2de: calc_content_height at .../lv_obj_pos.c:1464
```

根因是 **`lv_obj_update_layout()` 里的 `while(scr->scr_layout_inv)` 没有次数上限**
（`lv_obj_pos.c:393`）：只要一轮布局里又有对象把布局标脏，它就再来一轮。最常见的
制造方式是**内容尺寸自反馈**：

```c
lv_obj_t *card = lv_obj_create(parent);          /* lv_obj_set_height(card, LV_SIZE_CONTENT) */
lv_obj_t *ovl  = lv_obj_create(card);
lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));  /* 高度 = 卡片高度 */
lv_obj_add_flag(ovl, LV_OBJ_FLAG_IGNORE_LAYOUT); /* ← 以为这样就"不影响布局"了 */
```

**`IGNORE_LAYOUT` 只让子对象不参与 flex/grid 布局，并不会让它不算进父对象的
`calc_content_height()`** —— 那里只跳过 `HIDDEN` 和 `FLOATING`
（`lv_obj_pos.c:1482`）。于是"卡片多高取决于覆盖层、覆盖层多高又取决于卡片"，
来回震荡就永远出不来。同理，`lv_obj_align_to()`（会记住 base 并在 base 移动时重新
对齐）放在 `LV_SIZE_CONTENT` 的父对象里也是同一个坑。

**规则**：覆盖层/装饰层/浮动提示用 **`LV_OBJ_FLAG_FLOATING`**（布局与内容尺寸
计算都会跳过它）；`IGNORE_LAYOUT` 只在"父对象尺寸已经定死"时才够用。

**已经这么改过的**：`ui_ba.cc` 的卡片装饰层与右上角关闭按钮、`lvgl_pinyin_ime`
的拼音 chip（改成候选栏内的普通 flex 子对象）与"左滑清空"提示条。
**另外**：条件允许就别在最外层套 `LV_SIZE_CONTENT`；固定高度能让整类问题消失。

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

### 12.2.2 本地目录扫描必须走工作线程（2026-10 需求："点了立刻进，用加载动画等"）

**以前**：`page_local()` / 进目录 / 上一级都在 **LVGL 任务里同步**读 SD ——
`local_rebuild()` 先 `storage_service_browse()`（列目录）再 `storage_service_load_dir()`
（**又走一遍目录**建播放队列），两次 full walk；几百首的目录就是"卡一会才进去"，
而且 `local_show_loading()` 里那个转圈**一动不动**（LVGL 任务被占住，动画没机会刷）。

**现在**（`local_scan_task`：内核 8 KB 栈、优先级 2、一次性任务）：
- 列目录 + 建播放队列**整个**搬到工作线程，UI 只铺加载态后**立刻返回** → 页面秒进、
  转圈真的在转（不再需要 `lv_refr_now()` 去抢那一帧）；
- 结果由 `local_sd_tick`（已有的 500 ms 定时器）轮询消费：`s_scan_done` 且
  `s_scan_run_seq == s_scan_seq` 才 `local_apply_scan()`，否则丢弃并打一行日志；
- 同一时刻**只允许一个扫描**（`s_scan_busy`）；期间来的请求记进 `s_scan_pend(_dir)`，
  由 tick 补起 —— 两个线程同时写 `s_entries` 就完了；
- 发布顺序必须是 **写数据 → 置 `done` → 清 `busy`**：反过来 tick 会在
  "busy 已清、done 还没置"的缝里再起一个扫描；
- ⚠ **`seq` 校验不是可选项**：页面销毁（`local_page_watch`）与新建（`page_local`）
  都**必须** `s_scan_seq = s_scan_seq + 1`，否则上一次"在飞"的扫描结果会被画到新页面上
  —— 列的是旧目录的文件名，点下去却按新的 `s_cur_dir` 拼路径，**放到别的文件去了**。
  插卡/拔卡分支同理（拔卡时也要作废在飞的扫描）。
- ⚠ `volatile` 计数器**不能写 `++`**（C++20 起 deprecated，本工程 `-Werror=volatile`）
  → 一律写 `x = x + 1;`。

**为什么敢在工作线程里调 `storage_service_load_dir()`**（服务内部是"先清 `s_count`
再重建"，存在短暂的"空队列"窗口）：读曲目表的只有 `app_music`，而播放器页与本地列表页
是路由上的**兄弟页**（不会同时存活），本地页扫描期间又只有加载态、没有任何行可点 ——
那个窗口没人看得到。将来若有别的模块（状态栏等）也去读曲目表，这条前提就不成立，
得改成"在服务内部加锁 + 末尾换指针"。

### 12.4 三个 2026-10 用户报的问题（IME 收起 / 后台续播 / 本地文件卡顿）

#### 12.4.1 虚拟键盘不会自动收起 + 光标是方块（组件侧修，`lvgl_pinyin_ime`）

- **键盘不收起**：`lv_pinyin_ime_attach()` 原来只把 `ctx->ta` 记下来（外加设字体），
  **没挂任何焦点事件**；各页面只在少数几个动作里显式 `hide()`，所以点别处键盘一直挂着。
  ⚠ **不能用 textarea 的 `LV_EVENT_DEFOCUSED`**：LVGL 只在点到**另一个
  `CLICK_FOCUSABLE` 对象**时才给旧对象发 DEFOCUSED（见 `lv_indev.c` 的
  `indev_click_focus`），点普通行/遮罩根本不触发 —— 那正是用户遇到的场景
  （顺带记一条：`lv_button` **不是** click-focusable，所以点键盘按键不会让输入框失焦，
  这也是官方 keyboard 示例能安心用 READY/CANCEL 的原因）。
  **修法**：在**屏幕**与 **`lv_layer_top()`**（模态框挂那儿）上挂一个 CLICKED 回调，
  看这一击落在谁身上：落在键盘或绑定的输入框上（沿 `lv_obj_get_parent` 往上找）就算
  "还在输入"，否则 `hide()`。
  - ⚠ **冲突保护**：应用自己的点击回调可能顺手 `show()`（例如点搜索历史行会重跑搜索
    并重新拉起键盘），冒泡到 dismiss 会立刻又收起来 → 刚 `show()` 过的
    `IME_DISMISS_GRACE_MS`(100 ms) 内不理会（同本工程 `MENU_GRACE_MS` 的思路）。
  - ⚠⚠ **销毁时必须摘回调**：屏幕/顶层 layer 比键盘活得久，不摘的话键盘销毁后回调还在，
    一旦被点到就会用已释放的 ctx 去 `lv_obj_get_user_data()` → 悬垂指针崩溃。
    `ime_root_delete_cb()` 里用 `lv_obj_remove_event_cb_with_user_data()` 摘掉（此时 ctx 还活着）。
- **光标是方块**：LVGL 的 textarea 光标是 `LV_PART_CURSOR`，默认用**背景色填满整个字符格**
  → 看起来就是实心方块。改成"背景透明 + 只留左边框 2px"就是细竖线光标；颜色取
  `LV_PART_MAIN` 的文字色（深浅主题都不会看不见）。同样在 `attach()` 里做，一处修好
  WiFi 密码 / 歌单输入 / MomoTalk / BA 弹窗全部输入框。

#### 12.4.2 后台播完不续播：tick 必须常驻（`app_music.cc`）

- **症状**：后台放歌，一首放完**什么都不发生**（像暂停了），**进一次播放页**才继续。
- **根因**：`tick_cb`（消费 `s_advance_req`/`s_err_req`、跑循环模式、卡死看门狗、
  断点续播、进度落盘）原来由 `page_player()` 创建、并在页面 `LV_EVENT_DELETE` 里**删除**
  —— 不在播放器页时**没有任何人消费 `AUDIO_EVT_FINISHED` 置的那个标志**，于是歌放完就
  挂在那里等用户进页面。错误处理 / 耳机按键（`media_cmd_tick`）同理全停摆。
- **修法**：两个定时器改成**开机就建、永不删除**（在 `app_music_register_apps()` 里，
  它跑在 `play_state_init` / `audio_service_init` 之后），`player_on_delete()` **不再删**，
  页面里的 `if (!s_tick)` 创建退化成兜底。
  - `tick_cb` 里所有碰 UI 的地方**本来就有空指针保护**（`s_p_seek` / `s_p_cur` / `s_p_tot` /
    `s_p_btn` / `s_p_lyric_col`（`lyric_resync` / `update_lyric_highlight` 内部判）/
    `s_p_cover_img`（`cover_show` 内部判）/ `s_out_btn_icon`（`out_btn_sync` 内部判）），
    所以它可以脱离页面一直跑 —— **页面只负责显示，状态机跑在页面之外**。
  - 附带好处：后台播放的进度记忆现在也会按时落盘（以前只在播放器页里存）。

#### 12.4.3 部分 FLAC 卡顿但 CPU 极低：本地文件 IO **没有预读缓冲**（`audio_service.cc`）

- **先回答"解码器在不在解码"**：GMF 管线跑在**独立任务**里（IO 任务 + 元件任务），
  解码是异步的、不占 LVGL 任务；"卡顿 + CPU 极低"这个组合本身就说明
  **任务在等 IO，不是在算**（解码在 CPU 上是有实打实开销的）。
- **根因**：ASP 池子里默认的 `io_file` 是 `FILE_IO_CFG_DEFAULT()` ——
  **`buffer_size = 0` / `io_size = 0`**，而 GMF 的 IO 基类在 `buffer_size == 0` 时
  **根本不建 data bus**（`esp_gmf_io.c`：`if (io->data_bus == NULL && buffer_size > 0)`）。
  没有 data bus 就没有 IO 任务预读，**解码器每要一块数据就同步读一次 SD**
  （raw `acquire_read`）。对帧小帧多的文件（真机那个 FLAC `max_frame` 才 3635 字节）
  就是"一帧一次读卡"，每次都等 FATFS —— 正是"断断续续 + CPU 很低"。
- **修法（⚠ 第一版已撤回，见 §12.4.4）**：本想照抄 HTTP 那条路给 `io_file` 加预读缓冲，第一版**漏了 `thread.stack`**、把**本地拖动定位**搞死了 → 已撤回。要用就按 §12.4.4 的正确写法（两个字段一起给，真机先验拖动）：
  注册一份 **ring 64 KB + 每次读 8 KB** 的 file io（在 `audio_service_play_uri()` 的
  本地分支里调；失败就什么都不改，池子里的默认那份还在，本地播放照旧可用）。
- **自证用的日志**（`audio_service.cc` 的 `audio_stats_log()`，每 5 s 一行）：
  ```
  I audio_svc: play stats: PCM 96 KB/s (480000 B in 5000 ms), io=1234567 B, calls=117, flow=1, br=128000/130000
  ```
  判读：满速是 **192 KB/s**（48 kHz/16bit/2ch）；`PCM` 明显低于 192 而 `io=` **几乎不动**
  ⇒ 卡在读 SD/网络；`io=` 一直涨而 PCM 上不去 ⇒ 解码/转换环节的问题；`flow=0` ⇒ 当前没在出 PCM。

#### 12.4.4 ⚠ 教训：给 `io_file` 加缓冲**必须同时给线程栈**，否则本地 seek 直接失效

2026-10 踩的坑（自己引入又撤回）：为了治"部分 FLAC 卡顿"，照 HTTP 那条路注册了一份
`buffer_size = 64 KB` 的 file io —— **本地拖动进度条随即完全失效**。

**根因**（`esp_gmf_io.c`）：

```c
/* esp_gmf_io_init() */
if ((io->io_cfg.thread.stack > 0) && (io->io_cfg.buffer_cfg.buffer_size > 0)) {
    io->type = ESP_GMF_IO_TYPE_BLOCK;          /* ← 两个条件都要满足 */
}
...
/* esp_gmf_io_open() */
if (io->data_bus == NULL && io_cfg->buffer_cfg.buffer_size > 0) {
    esp_gmf_db_new_block(1, buffer_size, &io->data_bus);   /* ← 只看 buffer_size */
}
```

`FILE_IO_CFG_DEFAULT()` 给的是 **`thread.stack = 0`**，所以只设 `buffer_size` 的结果是：
**建了 data bus，却没有 IO 任务、`type` 也不是 BLOCK** —— 半吊子状态：

- **放音正常**：读路径判的是 `io->data_bus && io->task_hd`（`esp_gmf_io_acquire_read`），
  task 为空 → 退回 **raw** 路径，照旧能读；
- **seek 死掉**：`esp_gmf_io_seek()` 的异步分支是"abort data bus → **由 IO 任务执行
  seek**"，没有任务就永远没人执行 → 拖动毫无反应。

（HTTP 那份没事，是因为 `HTTP_STREAM_CFG_DEFAULT()` **自带非零 stack** ——
照抄时只改了 buffer，把这点漏了。）

**正确写法**（将来真要再做，两个字段一起给，且真机上**先验拖动**）：

```c
file_io_cfg_t cfg = FILE_IO_CFG_DEFAULT();
cfg.dir = ESP_GMF_IO_DIR_READER;
cfg.io_cfg.thread.stack           = 4096;        /* ★ 缺了它就不是 BLOCK IO */
cfg.io_cfg.thread.prio            = 5;
cfg.io_cfg.buffer_cfg.io_size     = 8 * 1024;
cfg.io_cfg.buffer_cfg.buffer_size = 64 * 1024;
```

**同时补的一个防误伤**（`app_music.cc`）：`audio_service_seek_ms()` 会清掉出声标志，
而**本地看门狗阈值只有 8 s** —— 一次慢一点的本地定位（大 FLAC 重新填缓冲）会被判成
"这个文件解不出来"直接跳过。现在 `on_seek_release()` 会置 `s_seek_in_flight`，
定位期间看门狗不介入；出声（`audio_service_output_active()`）就清标志，
真卡死超过 `SEEK_STALL_MAX_TICKS`(20 s) 才放手交给看门狗兜底。
顺带把本地 seek 也纳入落点核对（`s_seek_landing_wait` 不再 `streamed ? 10 : 0`——
本地 `s_stream_io` 就是文件 IO，同样能按字节反推落点）。

### 12.2.1 SD 卡热插拔（2026-10 加，2026-10 补电源控制与三态）

**板子上没有卡检测脚**（板级 YAML 的 `fs_sdcard` 没有 `cd`），所以"卡在不在"
只能定期试探。下面几件事合起来才成立，缺一个就会变成"插拔没反应 / 换卡要重启"：

1. **探测**：`bsp_sdcard_probe()` 用 **CMD13(SEND_STATUS)** 问卡还在不在。
   ⚠ 不能用 `stat()`/`opendir()` 代替 —— FATFS 把目录项与 FAT 表缓存在内存里，
   卡拔了它照样能回答，界面于是永远显示"已挂载"。
2. **卸载**：探测连续失败 `SD_PROBE_FAIL_MAX`(2) 次才 `bsp_sdcard_unmount()`。
   ⚠ 卸载**必须**走板级 `esp_board_manager_deinit_device_by_name("fs_sdcard")`：
   它的 deinit 会 `esp_vfs_fat_sdcard_unmount()` → 内部 `call_host_deinit()`，
   把 SDMMC 也一起拆干净（S31 上 `SDMMC_HOST_FLAG_DEINIT_ARG` 置位，走的是
   `sdmmc_host_deinit_slot()`，它内部 `sd_host_del_controller()` 成功后同样把
   `s_ctlr` 置 NULL）。不拆干净的话，重新挂载会在 `sdmmc_host_init_slot()` 上
   拿到 "slot already initialized" —— 这就是"换了卡也得重启才认"的根因。
   `sdmmc_host_init()` 自己是幂等的（已建就返回旧句柄），关键在 **slot** 有没有被拆。
3. **轮询任务**：`storage_service` 里 `sd_monitor_task`（4 KB 栈、优先级 2）——
   已挂载时每 1 s 探一次，未挂载时每 1.5 s 试着挂一次（连续失败退到 5 s，
   免得没插卡时每 1.5 秒初始化一次卡）。挂/卸都过一把互斥锁，设置页的
   「重新检测」（`storage_service_sd_rescan()`）走同一个入口，两边不会同时
   init/deinit 同一个设备。
4. **★ 电源控制（2026-10 补，热插拔真正生效的关键）**：`SD_CTRL`=GPIO39 是
   **低电平使能**的卡供电，板级 `sdcard_power_ctrl` 设备管着它。问题是板级管理器
   **只在"设备 init 时"自动上电一次**，而它的电源设备**引用计数一旦 ≥1 就再也
   不会重新 init**、init 失败也不回退 —— 于是连续失败几次之后卡上一直有电，
   插拔时卡内部的上电初始化序列根本没重跑，SDMMC/FATFS 两边都停在半初始化状态。
   现在由存储服务自己控制：
   - `bsp_sdcard_power(bool)`：唯一入口（`esp_board_device_power_ctrl()` 的薄封装）。
     **已挂载时拒绝断电**（会把文件系统从底下抽走），返回 `ESP_ERR_INVALID_STATE`。
   - `bsp_sdcard_power_cycle()`：断电 → 等 60 ms → 上电 → 再等 60 ms。
     **每次挂载尝试前都做一次**（`sd_try_mount()` 里）—— 新插入的卡只有这样才能
     稳定地重跑 ACMD41；卸载后立刻断电（`sd_drop_mount()` 里），让下一张卡冷启动。
5. **三态（状态栏 SD 图标的数据源）**：`storage_service_sd_state()`
   —— `ABSENT`(探卡无应答=没插卡) / `READY`(已挂载) / `ERROR`(卡有应答但挂不上，
   或已挂载的卡不再应答)。区分"没插卡"和"卡坏了"靠
   `bsp_sdcard_presence_probe()`：不挂 FATFS，自己按板级配置起一次 SDMMC 主机 →
   `sdmmc_card_init()`（400 kHz）→ 立刻拆主机；**`ESP_ERR_TIMEOUT`/`NOT_FOUND`
   才算没插卡**，其它错误是"卡在但读不出来"。
   ⚠ 只在**未挂载**时调；⚠ 卸载路径里必须**先探卡再断电**（断电后探谁都无应答）。
   ⚠ `sdmmc_card_t` 一千多字节，探卡那次是 `calloc` 的，别放栈上（监控任务栈 4 KB）。

**UI 怎么跟上**：状态每变一次 `storage_service_sd_generation()` **+1**（多处 UI
各记自己上次见到的代次，比"事件队列"好，不存在谁消费掉了）。用它的有三处：

| 位置 | 行为 |
| --- | --- |
| `ui_status_bar.cc` | 500 ms 轮询三态 + 代次 → SD 图标（没插卡不画 / OK / WARN）+ Toast（插入还报容量） |
| `app_music` 本地列表页 | 拔卡 → 列表换"未检测到"，**正在放本地歌就停播**并提示；插卡 → 从挂载点重新列一遍（换卡也能认出来） |
| `app_settings` 存储页 | 代次变了就 `storage_rebuild()` 重画容量/卡信息 |

Toast 只在**状态栏**那一处弹（插入/拔出/插进来读不出来的卡各一句），页面不重复弹，
否则两句会互相覆盖。"已挂载的卡突然不应答"（READY→ERROR）**故意不弹** ——
单次 CMD13 超时是会有的抖动，连续两次才真卸载，那种情况只让图标变橙。

**轮询的日志必须自己管住**（2026-10 用户报"每秒一行 `BOARD_DEVICE: Device handle
fs_sdcard found`"）—— 热插拔是每秒/每几秒一次的常态动作，任何 INFO/ERROR 都会刷屏：

| 噪声来源 | 修法 |
| --- | --- |
| `esp_board_manager_get_device_handle()` 每次都打一行 INFO | `bsp_sdcard_probe()` / `bsp_sdcard_card_info()` 改用**挂载时缓存的卡句柄**（`s_card = fat->card`，卸载时先清空，生命周期与 `s_mounted` 一致），不再每次去问 |
| 没插卡时每次重试挂载都会从板级管理器 / SDMMC 驱动刷好几行 ERROR（"Failed to initialize device"、"Failed to mount filesystem"、"send_op_cond returned 0x107"…） | 后台重试那一次把**默认日志级别**临时压到 `ESP_LOG_NONE`（`esp_log_level_set("*", ...)` + `esp_log_get_default_level()` 保存/恢复，窗口只有一次卡初始化的几十~一百毫秒），试完立刻恢复；成功那次自己打一行 `SD inserted at /sdcard (gen N)` |
| 用户主动点的「重新检测」 | **不压日志**（`sd_try_mount(false)`）：那是用户要看失败原因的场景 |
| `bsp_sdcard_mount()` 自己的失败 WARN | 一个"失败连续段"里只打一次（`s_mount_fail_logged`，成功时清零）；开机那次是显式调用，正常打出来 |

⚠ 别用 `esp_log_set_default_level()`：它虽然是"只改默认级别、不动 per-tag 表"的正解，
但声明在 **`esp_private/log_level.h`** 里（私有头）。工程里没有任何地方设过 per-tag 级别，
所以 `esp_log_level_set("*", ...)` 的"清空 linked list"副作用没有代价 —— 用它就够，
而且是公开 API。将来真用上 `esp_log_level_set("<tag>", ...)` 的话，这里的保存/恢复要
跟着改成按 tag 逐个处理。

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

**SVG 源直接进清单（2026-10 加）**：`assets/icons/` 里那批状态栏图标是 SVG，
不需要 cairosvg / PIL / Inkscape —— 同目录的 `tools/icons/svg_raster.py`
是个纯标准库的极简光栅化器（解析 `d` → 拍平二/三次贝塞尔 → 扫描线 **nonzero**
填充；`A/a` 圆弧不支持，要用先转曲线）。gen_icon.py 见到 `.svg` 就按
`SVG_SUPERSAMPLE`(4) 倍渲染，再 box filter 缩到清单里的尺寸
（= 每边 4 级灰阶、边沿不掉渣）。

- SVG 一律按**白色**烘（`SVG_FILL`）—— 状态栏那几个是**单色位图**，
  显示颜色由 `lv_obj_set_style_image_recolor()` + `image_recolor_opa = LV_OPA_COVER`
  决定（`ui_status_bar.cc` 的 `icon_set()`），所以"同一个图标画成暗色/强调色"
  不用烘两份。⚠ LVGL 的 ARGB8888 是**直混**（非预乘），gen_icon 输出的就是直混，
  别自己乘 α。
- 想肉眼看烘出来什么样：`--preview build/icons_preview` 会把每张位图另存成 PNG
  （`svg_raster.write_png`），拿图片查看器看一眼再进固件。

**常驻状态栏的图标逻辑全在 `ui_status_bar.cc`（2026-10 重做）**：四个图标
（Wi-Fi / 蓝牙 / 输出通道 / microSD）由一个 500 ms 的 `lv_timer` 从
`wifi_service` / `bt_service` / `storage_service` 读状态后画出来，
**没有任何 `ui_status_set_*` 入口**（老版本那四个 setter 是"永远没人调"的占位，
所以图标常年一动不动，已删）。

| 图标 | 显示规则 |
| --- | --- |
| Wi-Fi | 开关关着 → **不显示**；开着没连上 → `wifi_off`（暗）；已连接 → `wifi_s1..s4` 按 RSSI 分档（`rssi_to_level()`，未知(0) 按满格） |
| 蓝牙 | 开关关着 → **不显示**；已连接/推流 → `bt_connected`（强调色）；搜索中 → `bt_searching`；其余 → `bt_idle`（暗） |
| SD | `storage_service_sd_state()`：`ABSENT` → 不显示；`READY` → `sd_ok`；`ERROR` → `sd_warn`（强调色） |

- 图标排在一条 **flex row** 里，隐藏用 `LV_OBJ_FLAG_HIDDEN` —— LVGL 的 flex
  **跳过隐藏子对象**，所以关掉 Wi-Fi 时右边几个会自动收拢，不会留空洞
  （`align_to` 链做不到，会留洞）。
- 行宽 `LV_SIZE_CONTENT`、高取 `UI_STATUSBAR_H`：**交叉轴不能是 CONTENT**
  （`lv_flex.c` 会把 CONTENT 的交叉轴对齐强制成 START，文字符号就贴上边缘了）。
- 图标边长固定 `UI_STATUS_ICON`(22)，**不随 UI_H 缩放** —— 位图就是按 22 烘的，
  改这里必须同步改 `icons.txt` 并重新生成。
- **右侧左滑功能抽屉已按需求整体移除**（`s_spr_drawer` / 右边缘热区 / `UI_DRAWER_W`
  全删）：右边缘那条 24px 热区会把页面自己的横滑手势全吃掉，现在直接落给控件。

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

### 15.5 设备信息页（"关于本机"，2026-10 需求）

设置 →「关于本机」以前只弹一句 Toast（版本号），现在点进去是完整的设备信息页
（`page_about`，`app_settings.cc`）：**芯片 / Flash 存储 / 内存 / 网络标识 / 系统** 五组。

| 项 | 取法 | 坑 |
| --- | --- | --- |
| 型号 · 核心数 · 修订 | `esp_chip_info()` | 枚举里**有** `CHIP_ESP32S31`；认不出的型号退回 `CONFIG_IDF_TARGET` |
| CPU 频率 | **`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`** | `esp_clk_cpu_freq()` 声明在 `esp_private/esp_clk.h`（私有头，别碰）。本工程没开 `CONFIG_PM_ENABLE`/DFS，主频开机定死，配置值就是运行时值；哪天真开了 DFS 要换运行时查询 |
| Flash 容量 | `esp_flash_get_size(NULL, &sz)` | 外挂 flash 的总容量（本板 16 MB） |
| 分区大小 / Flash 可用 | 运行分区 `address/size` − **镜像长度** | 镜像长度只能问 `esp_image_get_metadata()`（`esp_app_get_description()` **没有** size 字段）。该 API 已验证是**只读且静默**的（内部 `do_load=false, do_verify=false, silent=true`），LVGL 任务里调用安全 —— **别换成 `esp_image_verify`**（那个会读整个镜像） |
| SRAM / PSRAM 总量与可用 | `heap_caps_get_total_size / get_free_size(MALLOC_CAP_INTERNAL / MALLOC_CAP_SPIRAM)` | 两者分开问；PSRAM 总量为 0 时显示"未启用" |
| Wi-Fi / 热点 / 蓝牙 MAC | `esp_read_mac(mac, ESP_MAC_WIFI_STA / _SOFTAP / ESP_MAC_BT)` | **直接读 eFuse**，Wi-Fi/蓝牙驱动关着也能读到（不用等无线服务起来） |
| 版本 / 编译时间 / IDF / 运行时长 / 屏幕 / SD 卡 | `esp_app_get_description()` + `esp_timer_get_time()` + LVGL 版本宏 + `UI_W/UI_H` + `storage_service_*` | — |

- 页面由 `about_section()`（分组小标题）+ `about_add()`（"名称 → 值"行）+ 1 s 的
  `about_tick` 组成。值 label 宽度是 **`UI_PCT_X(52)`**（不是列表默认的 30%）——
  MAC 串、容量串更长。
- **只有"会变的"四行**（SRAM 可用 / PSRAM 可用 / 运行时长 / SD 卡）交给 tick，
  用 `label_set_if_changed()` 就地改文本；**绝不重建控件** —— 重建会把滚动位置和
  触摸状态一起弄乱（同 §15.4 的 `storage_rebuild`）。
- `app_settings/CMakeLists.txt` 为此补了 `esp_hw_support / heap / spi_flash /
  esp_partition / app_update / bootloader_support / esp_timer`。
  ⚠ **改这个 CMakeLists 会触发整个 app 组件树重编**：2026-10 就因此把别处"正在编辑器里
  改到一半"的文件一起编了，报出一串看着像本次改动引起的错 —— 先看那个文件**当前磁盘
  内容**再判断，别顺着错误信息去改自己的新代码。
- `-Werror=format-truncation`：把 `about_fmt_bytes()` 的结果再拼进更短的缓冲时必须写
  `%.20s` 显式限长（同 §12.3 的老规矩）。

**芯片温度**（追加需求，`about_tsens_*`）：用 IDF 的 `temperature_sensor`
（组件 **`esp_driver_tsens`**；本 target 有 `CONFIG_SOC_TEMP_SENSOR_SUPPORTED=y`）。

- **量程只能从芯片给定的几档里选**（表在
  `esp_hal_ana_conv/esp32s31/temperature_sensor_periph.c`）：
  `50~125(±3)` / `20~100(±2)` / **`-10~80(±1)`** / `-30~50(±2)` / `-40~20(±3)`。
  取 **-10~80**：日常环境温度落在这一档，误差最小（±1 °C）。
  ⚠ 请求的 `[min,max]` 必须被某一档**完整包住**，否则 `temperature_sensor_install()`
  会报 *"the boundary you gave cannot meet the range of internal temperature sensor"*。
- **装一次、按页面生命周期开关**：`install` 只做一次（它内部要 malloc 一份量程表，
  反复装会反复分配）；`enable`/`disable` 跟着页面走 —— 传感器在低功耗域，
  离开页面就别让它一直耗电。
- **它和 ADC 是独立模块**（`SOC_TSENS_IS_INDEPENDENT_FROM_ADC=1`）—— 这点很关键：
  本板六个按键是 **ADC1** 读的，任何"共享 ADC"的温度方案都会干扰按键。
- 读数**不能用 `%f`**：本工程 newlib 是 nano 配置，printf 不支持浮点 →
  在整数域四舍五入到 0.1 °C 再拼 `"%d.%d °C"`。
  `°`(U+00B0) **在 `ui_font_sc16` 里**（cmap 中 `range_start=164` 那张稀疏表的偏移 12 ✓），
  所以不用换成"度"字。

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


