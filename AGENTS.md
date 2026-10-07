# AGENTS.md —— ESP32-S31-Korvo-1 流媒体音乐播放器

本文件给在本仓库工作的 AI Agent / 开发者提供**必须遵守的事实与命令**。与
`方屏流媒体音乐播放器 开发指南.md` 冲突时，以官方文档与本文件记录的实测事实为准。

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
| 按键 | `adc_button_group`，**GPIO42 / ADC1 ch0**，4 键（电压档 SET=380mV, MODE=820mV, VOLUME_DOWN=1340mV, VOLUME_UP=1870mV）→ **实际是 SET / MODE / VOL- / VOL+，不是指南写的 PLAY/SET/VOL-/VOL+** |
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
- **UI 实际用的是编译进去的位图字体 `ui_font_sc16`**（`components/ui_font/`）：
  16 px 梦源黑体、**GB2312 全集 + ASCII + LVGL 图标（FontAwesome 私有区码点）**。
  生成物 `ui_font_sc16.c` 不要手改，重新生成：
  `python tools/font/gen_lvgl_font.py`（先在 `tools/font/` 用 gen_subset.py 产
  `DreamHanSansSC-W17-subset.ttf`；需要 `npm i -g lv_font_conv`）。
  实测 **952 KB** 常量数据（明文 4bpp，压缩只省 16% 不值得开解压路径）。
- 不要用 LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk` 显示中文：它只收
  "1338 most common CJK radicals"，GB2312 其余汉字会渲染成**缺字方框**。
  `lv_font_montserrat_*` 完全没有汉字，禁止用于含中文的标签。

## 9. Python 依赖

**一律用组件自带的 `tools/venv`，禁止全局 `pip install`。**

## 10. 验证方式

- 改完必须 `idf.py build` 通过（贴出结果摘要，不只说"已完成"）
- 涉及 UI 的改动要在模拟器 `simulator/index.html` 或真机截图确认
- 纯逻辑（LRC / ID3 / JSON / 路径处理）抽成不依赖硬件的模块，便于 host 侧单测

## 11. 在线音乐 / 无线开关 / 拼音输入法（2026-10 新增）

### 11.1 分区表变了（app 槽 6.875 MB）

拼音输入法的两套字体是**编进 app** 的（`CONFIG_LV_PINYIN_IME_FONT_SRC_EMBEDDED=y`，
16 px 0.97 MB + 20 px 1.41 MB），app 从 ~5.3 MB 涨到 **~6.70 MB**，
原来的 6 MB app 槽装不下，于是：

| 分区 | 变化 | 原因 |
| --- | --- | --- |
| `ota_0` / `ota_1` | 6 MB → **6.875 MB**（0x6E0000） | 装得下 6.70 MB 的 app（余量 ~3%） |
| `assets` | 1.25 MB → **128 KB** | 全工程没有引用它（grep 过），缩掉腾地方 |
| `usr` | 1 MB → **384 KB** | 拼音用户词库（FAT，autosave）用不到 1 MB |

约束仍然满足：app 起点 `0x230000` 是 64 KB 对齐，两槽合计正好落在 16 MB 末尾。
**改了分区表必须 `idf.py fullclean` 重编**，并且要重烧分区表（`idf.py flash` 会写）。

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
- 歌词缓存：`/sdcard/online/audio/<歌曲ID>.lrc` —— 刻意与音频**同目录同主名**，
  这样 `media_meta_lyrics_load(音频路径)` 的第一档"同名 .lrc"直接命中，播放端零改动
- 封面缓存：`/sdcard/online/cover/<歌曲ID>.jpg`，用新增的
  `media_meta_jpeg_file_rgb565()` 解码（在线封面是独立 jpg，不在音频里）
- 歌单 ID 存 NVS `online`（`n` + `id0..id7`，最多 8 个）

**设计选择：先下载再播放**。好处是播放通路与本地文件完全一致（封面/歌词/进度/拖动定位
全部复用），代价是首播要等几秒（命中缓存后就是本地速度）。

### 11.5 Wi-Fi / 蓝牙开关与开机自动连

- 开关状态存各自 NVS：`wifi_cfg/on`、`bt_cfg/on`（默认开）；UI 在设置页顶部（`lv_switch`）
- 关掉会**真正释放资源**：WiFi `esp_wifi_stop+deinit` + 销毁 netif + 停 SNTP；
  蓝牙拆掉 PCM 通路 + `esp_bt_audio_deinit()` + 控制器 `disable+deinit`
  （**不要**调 `esp_bt_mem_release()`，那会让它再也起不来）
- 开机自动连：WiFi 连最近保存的 AP（原有逻辑）；蓝牙连"上次连过的设备"
  （`bt_cfg/last`，在 A2DP 连上时写入，init 后延时 3 秒发起）
- **SNTP**：WiFi 拿到 IP 后在事件回调里 `esp_netif_sntp_init("ntp.aliyun.com")`，
  顺便 setenv TZ=CST-8 —— 既让首页时钟不再"等待校时"，也让 https 证书校验有正确时间

### 11.6 拼音搜索

搜索浮层用 `lvgl_pinyin_ime`（`lv_pinyin_ime_create()` + `attach(textarea)`，
组件初始化是**同步**的，第一次点搜索会卡约 1 秒加载词库）。
`app_online` 里的 IME 实例只建一次，关浮层时 `hide` 不销毁 —— 重复建会反复加载词库与字体。
