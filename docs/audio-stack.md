# 音频栈架构（2026-10 换用 libs/espaudiocore）

> 本文是**当前**音频链路的权威说明。AGENTS.md 第 11.4 节及之后关于
> `esp_audio_simple_player` / ESP-GMF 的记录都是**历史**（描述问题与排查过程仍有参考价值，
> 但代码已经不在了）。两者冲突时以本文为准。

---

## 1. 一句话架构

```
本地文件 ─┐
          ├─> libs/espaudiocore（解码，PCM 恒 16-bit）─> PCM ringbuf(32KB,PSRAM)
在线 HTTP ┘                                                   │
                                                              ▼
                                        audio_output.cc：esp_asrc → 48kHz/16bit/2ch
                                                              │
                              ┌───────────────────────────────┼──────────────────────┐
                              ▼                               ▼                      ▼
                     ES8389(TDM) → 功放             bt_service_write_pcm()      静音（只走时间轴）
```

- **解码**：`libs/espaudiocore`（vendored 组件，根 `CMakeLists.txt` 的
  `EXTRA_COMPONENT_DIRS` 注册）。7 种格式：WAV/MP3/AAC/FLAC/Vorbis/Opus/WavPack + M4A。
- **重采样/声道转换**：`espressif/esp_asrc`（**软件**路径，见 §4）。
- **在线传输**：自研（`audio_http.cc`，esp_http_client + Range + 重连），
  GMF 的 `esp_gmf_io_http` 已随 M2 一起退出构建。
- 组件列表里**不再有** `esp_audio_simple_player` / `gmf_audio` / `gmf_io` / `gmf_core`
  （只剩 `gmf_fft`，是 esp_asrc 的传递依赖）。

## 2. 文件与职责（`components/services/audio_service/`）

| 文件 | 职责 |
| --- | --- |
| `include/audio_service.h` | **唯一对外契约**（业务层只认它） |
| `audio_internal.h` | 组件内部契约：常量、模块接口、**三条线程纪律** |
| `audio_service.cc` | 门面：参数校验、会话锁、事件上报、起播编排、音量 NVS 唯一落盘点 |
| `audio_engine.cc` | espaudiocore 会话（起播/停/暂停/恢复）、事件映射、曲目信息探测 |
| `audio_output.cc` | 输出级：PCM ringbuf → esp_asrc → 三路输出；**时间轴**与播放健康度统计 |
| `audio_http.cc` | 在线传输：连接/嗅探/Range/续传/进度/速度 |
| `audio_seek.cc` | 拖动定位：本地库内 seek，在线 Range 重开会话 |

### 三条线程纪律（改代码前必读）

1. **会话锁**（`audio_core_lock()`）只保护"起播/停止/seek 重开"这类会动会话的流程。
   输出任务与事件回调**绝不取锁**。
2. **事件回调**跑在 espaudiocore 的解码任务上下文；`AUDIO_EVT_FINISHED` 跑在
   输出任务上下文；HTTP 出错上报跑在馈送任务上下文。三者都**禁止阻塞、禁止 LVGL**。
3. **写硬件的 PCM 恒为 48kHz/16bit/2ch**，所以时间轴只有一个口径：
   `ms = 送去硬件的字节数 ÷ 192`。别在别处按源采样率再算一套。

## 3. 为什么不用 espaudiocore 的 I2S 输出模式

本板 I2S 是 **TDM**（`components/gen_bmgr_codes/gen_board_periph_config.c:49`
是 `I2S_COMM_MODE_TDM`），而 espaudiocore 的 I2S sink 用 STD 的
`i2s_channel_reconfig_std_clock()/_slot()`；IDF 对非 STD 句柄直接返回
`ESP_ERR_INVALID_ARG`。若走那条路，采样率一变就配不下去 —— 44.1k 的音源会按
48k 时钟播（快 8.8%，明显变调）。

所以**一律走 ringbuf 输出**：解码后的 PCM 交给 `audio_output.cc`，
由它写 `esp_codec_dev_write()`（TDM 通道归板级管理器所有，我们只是消费者，
与 GMF 时代一致）。

## 4. 重采样为什么用 esp_asrc 的软件路径

S31 有硬件 ASRC，但硬件路径要经 GDMA 访问 PSRAM 缓冲，对 cache-line 对齐与一致性
有硬要求 —— `bt_service.cc` 已经踩过"先慢放、几秒后全是噪音"的坑并退回软件路径。
本工程沿用同样选择：`ESP_ASRC_PERF_TYPE_SW_SPEED` + complexity 3。
44.1k→48k 立体声每秒不到 200 KB，320 MHz 上余量充足。

> 想试硬件：把 `audio_output.cc::asrc_open()` 里的 `perf_type` 改成
> `ESP_ASRC_PERF_TYPE_AUTO`，并改用 `esp_asrc_get_buffer_alignment()` +
> `esp_asrc_align_alloc()` 分配缓冲。注意 **S31 的硬件 ASRC 只支持 16-bit 输入**
> （`esp_asrc/hw/caps/esp32s31/asrc_caps_check.c`），这也是我们把
> `cfg.output_bits` 定成 16 的原因之一。

## 5. 关键语义（与 GMF 时代不同，app 层依赖）

| 接口 | 现在的语义 |
| --- | --- |
| `audio_service_is_playing()` | **不含暂停态**（与旧版一致，由 engine 自己维护 `s_running`；不能用 `espaudiocore_is_running()`，它含暂停态） |
| `audio_service_output_active()` | 本会话是否已真的出过声；换曲与 seek 后清零（旧语义不变） |
| `audio_service_seek_supported()` | 本地文件**恒真**；在线仅当容器是 MP3/ADTS-AAC 且已知"总长+时长" |
| `audio_service_seek_landing_ms()` | **恒 false**（落点核对机制退休：本地由解码器保证帧对齐，在线的落点由 HTTP Range 保证） |
| `audio_service_play_uri()` | 对 m4a/mp4/ogg/opus 返回 `ESP_ERR_NOT_SUPPORTED`（这些容器要可 seek 的源，不能边下边解）→ app_music 改走"先下载到 SD 再当本地文件播" |
| `audio_service_set_duration_ms()` | 仍然**必须在 `play_uri()` 之后调**；在线平均码率（seek 的"时间→字节"）靠它 |
| 曲目信息 | 不再有 MUSIC_INFO 事件：本地起播时用 `media_meta_probe_audio()` 探测填 `audio_info_t`；在线用时长提示 + Content-Length 算码率 |

### 在线 seek 的两个硬边界（`audio_seek.cc`）

1. **只支持 MP3 / ADTS-AAC**：这两类解码器靠帧同步字能从流中间接上。
   FLAC/WAV/M4A 从中间开始时拿不到容器头（STREAMINFO / RIFF / moov），解不出来 ——
   对它们直接判"不可拖动"。
2. 落点是**最近的可解码帧**（MP3 一帧 26 ms），比 GMF 时代"标签算进音频长度导致早到几秒"准得多。

## 6. 我们对 vendored 库做过的补丁（`libs/espaudiocore`）

这些是**换到 ESP-IDF 6.1 + riscv32-esp-elf (GCC 15)** 后必须的构建/平台修正，
原作者的工具链上靠巧合通过。升级该库时注意保留：

| 位置 | 修改 | 原因 |
| --- | --- | --- |
| `CMakeLists.txt` | 从 include 路径里**去掉** `lib/ffbase` | 它自己的 `string.h` / `time.h` 会顶替 C 库同名头（`-I` 优先于系统目录），导致 `memcpy`/`va_start` 全"未声明" |
| `CMakeLists.txt` | 全组件定义 `FF_UNIX FF_LITTLE_ENDIAN FFBASE_OPT_SIZE` | ffbase 的平台自动探测不认识 newlib/RISC-V；而 `FFBASE_OPT_SIZE` 是"允许单独编译 sort.c/str-format.c"的开关（否则符号重复定义）。原来只有 `decoder_m4a.cpp` 单独定义过前两个 |
| `CMakeLists.txt` | `decoder_m4a.cpp` 加 `-fpermissive` | 它 include avpack 的 C 头，`void*` 隐式转换在 C++ 里非法（GCC 标了 `[-fpermissive]`） |
| `lib/avpack/base/mp4.h` | 新增 `MP4_BBOX_TBL` 宏（C 下是 `static const struct mp4_bbox`，C++ 下是 `extern const struct mp4_bbox`） | 那些 box 表用 C 的 tentative definition 分两步写，C++ 里是"大小未知的定义"，连 `-fpermissive` 都不降级 |

## 7. 排查用日志（真机验证时先看这几行）

| 日志 | 含义 |
| --- | --- |
| `audio_eng: probe: N bps, N ms, N Hz, Nch` | 本地文件起播时的参数探测结果（0 = 探不到） |
| `audio_out: source format: N Hz Nch` / `asrc ready (sw): ...` | 源格式与重采样器建立（**没有 asrc 那行 = 48k/2ch 直通**） |
| `audio_out: play stats: PCM N KB/s (…) audio N ms, codec N ms, gap max N ms, route N, src N Hz Nch` | 每 5 秒一行：`gap max` 大 = 被饿住；`codec` 大 = 写 codec 慢；`PCM` 应接近 192 KB/s |
| `audio_http: HTTP 206 from N, remaining N (total N)` | Range 续传/seek 生效 |
| `audio_http: server ignored Range (200), skipping N bytes locally` | 服务端不支持 Range（慢，但功能对） |
| `audio_http: read error, reconnecting #N from N` | CDN 掐长连接后自动续传（正常现象） |
| `audio_seek: local seek -> N ms` / `stream seek -> N ms = byte N` | 两条 seek 路径 |
| `audio_svc: 容器 fmt=N 不能边下边解` | 触发 app 层的"先下载再播"回退 |

## 8. 真机验证清单（换库后必须走一遍）

代码只做到"编译通过 + 逻辑自洽"，**音频链路必须真机听**。按下面顺序走，
每条都给了"该看到什么日志"，对不上就是那一环有问题：

| # | 操作 | 期望日志 / 现象 |
| --- | --- | --- |
| 1 | 插卡 → 本地音乐 → 放一首 **MP3** | `audio_eng: probe: N bps, N ms, N Hz, 2ch`；`audio_out: source format: 44100 Hz 2ch`（或 48000）；**有 `asrc ready (sw)` 说明在重采样** |
| 2 | 听 30 秒 | `audio_out: play stats: PCM ~190 KB/s`（48k/16/2 满速就是 192 KB/s；明显低于 150 就要查 `gap max`/`codec` 两列） |
| 3 | 拖进度条到中段 | `audio_seek: local seek -> NNN ms`，声音从新位置接上（**不再有"定位失败：码率未知"**） |
| 4 | 放一首 **FLAC 44.1k/24bit** | 应当出声且不变调（变调=重采样没生效）；`play stats` 里 `(asrc)` 标记在 |
| 5 | 放一首 **单声道 WAV** | 左右都有声（mono→stereo 走的是 esp_asrc 的声道转换） |
| 6 | 切到**蓝牙**输出 | `audio_out: output route -> 1`；听耳机出声；`play stats` 的 `PCM` 仍接近 192 KB/s |
| 7 | 在线歌单 → 点一首 | `audio_http: prepare: ...` → `sniffed N bytes -> format 1`（1=MP3）→ `streaming ... (fmt 1, total N)` |
| 8 | 缓冲期间看播放器页 | 转圈 + `缓冲中 xx KB/s · NN%`（百分比要有；KB/s 是 `audio_http_speed()` 的 Kbps ÷8） |
| 9 | 在线拖动 | `audio_seek: stream seek -> N ms = byte N` + 之后的 `HTTP 206 from N`；声音跳到目标附近（MP3 误差 <100 ms） |
| 10 | 在线听到一半拔网/断流 | `audio_http: read error, reconnecting #1 from N` → 恢复；或 `audio_eng: end of stream` + app 的"提前结束"重刷（`s_eof_retry`） |
| 11 | 放一首 **m4a/ogg** 的在线歌 | `audio_svc: 容器 fmt=N 不能边下边解` → 自动改走"先下载到 SD 再播"（会有下载等待） |
| 12 | 暂停/恢复/上一首/下一首/播完自动下一首 | 无爆音、无卡死；`audio_out: playback finished (gen N)` 之后 app 才切歌 |

回归重点（历史上出过问题的）：
- 本地 FLAC 24-bit 的**卡顿**（§AGENTS 11.5.7~11.5.11 那一串）：现在不再有 GMF 的
  `Not found frame` 与 `Run timeout`，若仍卡，看 `play stats` 的 `gap max`（被饿住）
  与 `codec`（写 codec 慢）两列区分。
- 在线 **403 过期地址**：`audio_http: HTTP status 403 (URL 可能已过期)` → app 弹
  "音乐链接已过期，正在刷新链接..." → 重刷后**从断点续播**（`stream seek -> N ms`）。
- 蓝牙放音时的**射频共存**（AGENTS §11.5.6）：仍与 WiFi 抢 2.4G，与本改动无关。

## 9. 已知限制 / 后续可做

1. **在线流"喂不上"会被当成流结束**：ringbuf 源在 ringbuf 空时等 300 ms，
   仍无数据就返回 0，libFLAC/Helix 视作 EOF。工程靠 4 MB 预读缓冲规避，
   真出现长停顿表现为"提前结束"，由 app 层 `s_eof_retry`（最多 3 次）重刷地址 + 断点续播兜住。
   要根治需要给 ringbuf 源加"生产者还活着"的信号（改库）。
2. **在线 seek 不支持 FLAC/WAV/M4A**（见 §5）。
3. **硬件 ASRC 未启用**（见 §4）。
4. **在线进度百分比**依赖服务端给 `Content-Length`（分块传输时不显示百分比，UI 已兼容）。
5. `audio_service_position_ms()` 是"送去硬件的字节 ÷192"，比耳朵听到的**超前**
   一个 PCM ringbuf（32 KB ≈ 170 ms）+ codec DMA 的量 —— 目前只用于日志，
   若将来要做"精确进度"需要减掉这段积压。
