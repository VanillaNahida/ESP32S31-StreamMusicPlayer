# docs/refactor —— 音频栈迁移与超大文件拆分

这个目录记录 2026-10 的两件大事：**把解码库换成 `libs/espaudiocore`**、
**把几个 3000 行级的 `.cc` 拆成职责单一的多文件模块**。

## 索引

| 文件 | 内容 |
| --- | --- |
| [`../audio-stack.md`](../audio-stack.md) | ★ **当前音频链路的权威说明**（架构 / 线程纪律 / 语义 / 库补丁 / 排查日志）。改音频代码前必读 |
| [`01_audio_service.md`](01_audio_service.md) | 旧 `audio_service.cc`（1269 行，GMF 版）的逐行审计：函数/变量/42 项 GMF API 清单、三路输出、seek 五套机制、拆分方案、**espaudiocore 适配点 27 条** |
| [`02_app_music.md`](02_app_music.md) | `app_music.cc` 拆分蓝图（函数分类、静态变量、回调/定时器、风险） |
| [`03_online_service.md`](03_online_service.md) | `online_service.cc` 拆分蓝图（含 HTTP 细节、歌单二进制格式、PSRAM 缓存所有权、302 解析链） |
| [`04_app_online.md`](04_app_online.md) | `app_online.cc` 拆分蓝图 |
| [`05_media_meta.md`](05_media_meta.md) | `media_meta.cc` 拆分蓝图（含内存所有权表、host 单测用例） |
| [`06_app_settings.md`](06_app_settings.md) | `app_settings.cc` 拆分蓝图 |
| [`07_bt_service.md`](07_bt_service.md) | `bt_service.cc` 拆分蓝图 |

> 02–07 是**重构前的只读审计报告**：里面的行号对应拆分前的文件，
> 拆分完成后它们就是"当初为什么这么拆"的历史依据，不要再按行号找代码。

## 一、解码库替换（已完成）

**从**：`espressif/esp_audio_simple_player`（ESP-GMF 之上的播放门面，内部管线
`io_file/io_http → aud_dec → aud_rate_cvt → aud_ch_cvt → aud_bit_cvt → out 回调`）

**到**：`libs/espaudiocore`（vendored，7 种格式 + M4A），配 `espressif/esp_asrc`
做采样率/声道转换、自研 HTTP 做在线传输、自研输出级做三路分流。

结果：`esp_audio_simple_player` / `gmf_audio` / `gmf_io` / `gmf_core` 全部退出构建
（只剩 `gmf_fft`，是 esp_asrc 的传递依赖）；app 二进制从 **8,246,144 B（8.25 MB）**
降到 **8,061,472 B（8.06 MB）**，省出约 180 KB。

**为什么值得换**（对照 `01_audio_service.md` 的审计）：

| 维度 | GMF 版 | espaudiocore 版 |
| --- | --- | --- |
| 本地 seek | "平均码率/实测码率 → 时间换字节 → 字节 seek"，码率未知就**拖不动** | 库内 `seek_ms()`，**帧对齐由解码器保证**，恒可用 |
| 重采样 | GMF `aud_rate_cvt`（实测 ~20% 实时） | `esp_asrc`（同源算法，但只在这一处） |
| 位宽 | `aud_bit_cvt` 固定 24→16 | `cfg.output_bits=16`（S31 硬件 ASRC 只吃 16-bit，链路本来就该是 16-bit） |
| 在线传输 | `esp_gmf_io_http`（4 MB 环 / Range / 重连，但句柄身份有坑） | 自研 `audio_http.cc`（同样的能力，代码归我们，句柄身份不再是坑） |
| 元数据/封面 | 自己解析 | `on_meta` 零拷贝（当前仍以 `media_meta` 为主） |
| 失败模式 | `Run timeout` 掐死整首、`定位失败：码率未知` | `esp_err_t` + `dec_err` 两层；本地 seek 不再有码率前提 |

**净损失 / 已知限制**：见 `../audio-stack.md` §8（在线 seek 只支持 MP3/AAC、
长停顿会被当成流结束（有 app 层重试兜底）、硬件 ASRC 未启用、M4A/OGG/Opus 不能边下边解
→ 走"先下载到 SD 再播"）。

## 二、超大文件拆分（进行中）

拆分原则（每个文件都要遵守）：

1. **对外头文件不动**（`audio_service.h` / `app_music.h` / `online_service.h` …），
   所有调用点零改动 —— 这样每一步都能单独编译、单独回归。
2. **纯逻辑与 UI / 硬件分离**：时间格式化、容器判定、选曲决策、歌词解析、
   二进制格式打包这类纯函数抽成不依赖 LVGL/硬件的模块（便于 host 侧单测）。
3. **注释跟着函数搬**：这些注释是踩坑记录（"为什么不能用 `++` 改 volatile"、
   "为什么复位码率锚点"），删了坑会复发。
4. **共享状态收敛**：拆出来的模块之间只经访问函数读写状态，不共享裸 static；
   多写者变量必须明确唯一 owner。
5. 每个组件拆完全量 `& .\tools\idf.ps1 build` 通过。

| 组件 | 拆分前 | 拆分后（`.cc` 个数 / 最长文件） | 状态 |
| --- | --- | --- | --- |
| `services/audio_service` | 1269 行 × 1 | 5 个 `.cc` + `audio_internal.h`，最长 `audio_output.cc` 610 | ✅ |
| `apps/app_music` | 3619 行 × 1 | 13 个 `.cc` + 13 个内部头，最长 `music_view_player.cc` 637 | ✅ |
| `services/online_service` | 3227 行 × 1 | 17 个 `.cc` + 16 个内部头，最长 `os_login.cc` 610 | ✅ |
| `apps/app_online` | 3157 行 × 1 | 11 个 `.cc` + 8 个内部头（都在 `src/`），最长 `ao_overlay.cc` 504 | ✅ |
| `services/media_meta` | 2104 行 × 1 | 8 个 `.cc` + 7 个内部头，最长 `mm_probe.cc` 565 | ✅ |
| `apps/app_settings` | 2036 行 × 1 | 11 个 `.cc` + 5 个私有头，最长 `as_page_about.cc` 407 | ✅ |
| `services/bt_service` | 1395 行 × 1 | 8 个 `.cc` + 6 个内部头，最长 `bts_feed.cc` 299 | ✅ |

**还剩下这几个 600~900 行的文件**（都不在最初点名的那批里，可另开一轮）：

| 文件 | 行数 |
| --- | --- |
| `services/wifi_service/wifi_service.cc` | 897 |
| `ui_core/ui_ba.cc` | 760 |
| `apps/app_momotalk/app_momotalk.cc` | 720 |
| `ui_core/ui_control_center.cc` | 693 |
| `services/storage_service/storage_service.cc` | 674 |

**验收方式**（每个组件都做了，且是"全新目录从零构建"而不是增量）：

```powershell
Remove-Item -Recurse -Force build
& .\tools\idf.ps1 build
```

最后一次全量结果（全部重构落地之后）：

```
esp32s31_stream_music_player.bin binary size 0x77e510 bytes.
Smallest app partition is 0xdf0000 bytes. 0x671af0 bytes (46%) free.
Project build complete.
```

> ⚠ **别只看增量构建**：本轮就出现过"增量绿、从零红"的情况 ——
> 新拆出来的 `.cc` 没进组件 `CMakeLists.txt` 的 `SRCS` 时，旧的 `.obj` 还在缓存里，
> 增量构建照样过，但从零构建链接期会报一大片 `undefined reference`。
> 另外还要留意 **C/C++ 链接名不匹配**：头里 `extern "C"` 声明、而定义它的 `.cc`
> 没有 include 那个头 → 定义拿到 C++ 修饰名（`nm` 里是 `T _Z31xxx` 对 `U xxx`）。
> app_music 就踩过一次（`music_app.cc` 少 include 自己的 `music_app.h`）。

**这一轮并行重构踩到的另外两件事**（供下次参考）：

1. **一个组件坏会挡住所有人**：`audio_service` 依赖 `bt_service`，所以 bt_service 在途
   的半成品会让其它 agent 的整包构建全红。多人并行时要么每个 agent 用独立 `build_*`
   目录（本轮就是这么做的），要么约好"任何时刻任何组件都能编过"。
2. **注释里写文件名/函数名的规范**：拆分后注释里的旧名字（`make_page_shell()` 之类）
   要跟着改名，否则下一个人按注释去找会找不到。


> 01–07 报告里的行数是**物理行数**（read/grep 的 LF 行号口径）。
> ⚠ 别用 `Get-Content | Measure-Object -Line` 数行：它按非 UTF-8 解码中文注释，
> 会把行数少算几十到两百行（曾把 3619 行报成 2942 行）。

## 三、本机构建入口

AGENTS.md 里那条 `. export.ps1` 在本机会被 PowerShell 执行策略拦住（临时激活脚本
没有签名），`idf.py build` 的 asyncio 管道也可能被沙箱拒。统一用：

```powershell
& .\tools\idf.ps1 build        # = 走 EIM profile + 代理 + bmgr 扩展
& .\tools\idf.ps1 cmakebuild   # 退路：直接 cmake --build build
```

（`tools/idf.ps1` 里同时打开了 profile 里的 ccache，增量构建约 30~60 秒。）
