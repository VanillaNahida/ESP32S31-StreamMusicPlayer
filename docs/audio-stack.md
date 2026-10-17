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
                              audio_output.cc：esp_asrc（S31 硬件 ASRC/GDMA）→ 48kHz/16bit/2ch
                                                              │
                              ┌───────────────────────────────┼──────────────────────┐
                              ▼                               ▼                      ▼
                     ES8389(TDM) → 功放             bt_service_write_pcm()      静音（只走时间轴）
```

- **解码**：`libs/espaudiocore`（vendored 组件，根 `CMakeLists.txt` 的
  `EXTRA_COMPONENT_DIRS` 注册）。7 种格式：WAV/MP3/AAC/FLAC/Vorbis/Opus/WavPack + M4A。
- **重采样/声道转换**：`espressif/esp_asrc`（**S31 硬件 ASRC 外设 + GDMA**，见 §4）。
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

## 4. 重采样：**硬件 ASRC**（软件路径已被真机证伪，2026-10 换掉）

### 4.1 为什么必须换

软件路径（`ESP_ASRC_PERF_TYPE_SW_SPEED`）= esp_audio_effects 的 `esp_ae_rate_cvt`，
44.1k→48k 立体声在真机上**吃掉一整个核**，而且它跟输出任务同核，于是两边一起饿死：

```
audio_out: play stats: PCM 78 KB/s | 产 2125 ms / 墙 5018 ms (42.3%) | asrc 5047 ms (cpu 5047 ms)
task_wdt: Task watchdog got triggered: IDLE1 (CPU 1)
task_wdt: CPU 1: audio_out
--- 0x40188598: fa_resample_process at rsp_proc.c.obj:?      ← 就卡在软件重采样里
```

`asrc 5047 ms / 墙 5010 ms` = 100% 的墙钟都花在重采样上；生产端只剩 42%，
两个 IDLE 都喂不到狗 → WDT。板载/蓝牙都一样（板载 I2S 是硬件时钟，说明是**产不出**，
不是送不出）。

### 4.2 换成了什么

`espressif/esp_asrc` 的 **硬件路径**：S31 的 ASRC 外设（`SOC_ASRC_SUPPORTED`，
2 个 stream）+ 两路 GDMA，数据是内存到内存的**突发**转换，`esp_asrc_hw_process()`
阻塞在 RX 收帧事件上 —— CPU 占用≈0。

选哪条路由 menuconfig 的 **Audio Service → 输出级 ASRC 性能模式** 决定，默认 `AUTO`：

| 选项 | 行为 |
| --- | --- |
| `AUTO`（默认，推荐） | 硬件能干的都交给硬件；只做声道上混（48k mono→stereo）时落回软件的声道转换；硬件 open 失败自动退回软件 |
| `HW_ONLY` | 只走硬件；本工程仍会在 open 失败时退回软件（宁可慢，不能变调） |
| `SW_SPEED` | 旧行为，只走软件（对照用，44.1k→48k 吃一个整核） |

### 4.3 三条硬约束（代码里都照做了，别拆）

1. **缓冲必须放内部 RAM**（或按 cache line 对齐的 PSRAM）。硬件路径经 GDMA 读写内存，
   PSRAM 缓冲在每次 process 前后都要 `esp_cache_msync()`、地址与长度还要 cache-line
   对齐，否则返回 `ESP_ASRC_ERR_INVALID_ALIGNMENT`。`audio_output.cc::asrc_buf_alloc()`
   一律 `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`，于是两条 msync 都变成空操作，
   彻底绕开"先慢放、几秒后全是噪音"那类 cache 一致性问题（`bt_service` 当年踩过）。
2. **喂进去的整片要先 memcpy 到内部 RAM 的 `s_in_buf`**。PCM ringbuf 在 PSRAM 里，
   切出来的片不保证对齐 —— 拷一遍就把"地址/长度对齐 + 一致性"整类问题挡在门外
   （4 KB ≈ 50 µs，可忽略）。
3. **S31 的硬件 ASRC 只认 16-bit / 1~2ch / 8000|16000|24000|32000|44100|48000**
   （`esp_asrc/hw/caps/esp32s31/asrc_caps_check.c`）。ringbuf 恒为 16-bit
   （espaudiocore `cfg.output_bits=16`），所以 44.1k/32k/24k/16k/8k 全部走硬件；
   **22050 Hz 不在表里 → AUTO 自动落回软件**（日志 `asrc ready (sw)`），这是设计内行为。

### 4.4 ⚠ 硬件 ASRC 只有 2 个 stream

立体声一次就吃满 2 个，也就是**同时只能有一路立体声硬件重采样**。
`bt_service` 那条 48k → A2DP 协商率的重采样**刻意继续走软件**（`bts_audio.cc`），
避免跟这里抢；真抢了也只是 AUTO 自动退回软件，不会失败、不会没声。

> 历史：`bt_service` 曾因为"缓冲归属/同步没对齐"踩到 "先慢放、几秒后全是噪音" 并退回
> 软件路径 —— 那次的经验是**别把 PSRAM 缓冲交给 GDMA**，本次正是照这条做的。

**遗留（未做）**：蓝牙路的链路是 `44.1k 源 →(输出级)48k →(bt_service)协商率 44.1k`，
也就是**同一段音频被重采样两次**。输出级那次现在免费（硬件），`bt_service` 那次仍是
软件（`bts_audio.cc`，complexity 3）。如果换成蓝牙后**仍然**一卡一卡，下一步就是这里：
① complexity 3 → 2；或 ② 干脆让输出级按 A2DP 协商率输出（省掉一次无谓的重采样，
代价是 48k 那条"192 B/ms"的时间轴不变式要跟着改）。

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
| `src/decoder_mp3.cpp` | `audio_free(xing_);` 后补 `xing_ = nullptr;` | ★★ **真机崩溃根因**：`close()` 里还会 `if (xing_) audio_free(xing_)` → 同一块内存第二次 free（详见 §7.1）。只在**可 seek 的源（本地文件）**上触发 |
| `CMakeLists.txt` | 从 include 路径里**去掉** `lib/ffbase` | 它自己的 `string.h` / `time.h` 会顶替 C 库同名头（`-I` 优先于系统目录），导致 `memcpy`/`va_start` 全"未声明" |
| `CMakeLists.txt` | 全组件定义 `FF_UNIX FF_LITTLE_ENDIAN FFBASE_OPT_SIZE` | ffbase 的平台自动探测不认识 newlib/RISC-V；而 `FFBASE_OPT_SIZE` 是"允许单独编译 sort.c/str-format.c"的开关（否则符号重复定义）。原来只有 `decoder_m4a.cpp` 单独定义过前两个 |
| `CMakeLists.txt` | `decoder_m4a.cpp` 加 `-fpermissive` | 它 include avpack 的 C 头，`void*` 隐式转换在 C++ 里非法（GCC 标了 `[-fpermissive]`） |
| `lib/avpack/base/mp4.h` | 新增 `MP4_BBOX_TBL` 宏（C 下是 `static const struct mp4_bbox`，C++ 下是 `extern const struct mp4_bbox`） | 那些 box 表用 C 的 tentative definition 分两步写，C++ 里是"大小未知的定义"，连 `-fpermissive` 都不降级 |
| `src/audio_player.cpp` | `ctx_destroy()` 加逐段日志（`teardown: …`） | **仅诊断用**（panic 拿不到 backtrace），定位结束后可删 |

> ⚠ **`libs/` 在外层仓库的 `.gitignore` 里** —— 以上改动外层 git 不跟踪。
> 它们同时导出成了 [`patches/espaudiocore-local.patch`](patches/espaudiocore-local.patch)，
> 重新克隆该库后用 `git apply -p1 --directory=libs/espaudiocore <补丁>` 还原（见 [patches/README.md](patches/README.md)）。


## 7. 排查用日志（真机验证时先看这几行）

| 日志 | 含义 |
| --- | --- |
| `audio_eng: probe: N bps, N ms, N Hz, Nch` | 本地文件起播时的参数探测结果（0 = 探不到） |
| `audio_out: source format: N Hz Nch` / `asrc ready (hw\|sw): ...` | 源格式与重采样器建立（**没有 asrc 那行 = 48k/2ch 直通**）。`(hw)` = S31 硬件 ASRC 在跑（`asrc` 列应只有几百微秒）；`(sw)` = 硬件用不了（22050 Hz / stream 被占）才退回软件 |
| `audio_out: asrc: 硬件路径开不起来 (...)，退回软件重采样` | 期望之外的软件回退，看括号里的错误码 |
| `E ESP_HW_ASRC: HW ASRC process timeout` | 硬件路径一次 DMA 没收到帧（`ASRC_TIMEOUT_MS=200`）；连着出就去查 GDMA/缓冲 |
| `audio_out: play stats: PCM N KB/s (…) audio N ms, codec N ms, gap max N ms, route N, src N Hz Nch` | 每 5 秒一行：`gap max` 大 = 被饿住；`codec` 大 = 写 codec 慢；`PCM` 应接近 192 KB/s |
| `audio_http: HTTP 206 from N, remaining N (total N)` | Range 续传/seek 生效 |
| `audio_http: server ignored Range (200), skipping N bytes locally` | 服务端不支持 Range（慢，但功能对） |
| `audio_http: read error, reconnecting #N from N` | CDN 掐长连接后自动续传（正常现象） |
| `audio_seek: local seek -> N ms` / `stream seek -> N ms = byte N` | 两条 seek 路径 |
| `audio_svc: 容器 fmt=N 不能边下边解` | 触发 app 层的"先下载再播"回退 |

## 7.1 ✅ 换曲瞬间的 double-free：**根因是 MP3 解码器里漏了置空**（2026-10 定案）

**现场**（两次崩溃的日志特征一样，值得记住）：

```
I audio_eng: playback finished (gen 2)          ← 或 espaudiocore: decode loop stopped by request
assert failed: tlsf_free rom/heap/tlsf/tlsf.c:629 (!block_is_free(block) && "block already marked as free")
```

判读钥匙是**它只发生在"关闭 MP3 解码器"的那一次切歌上**：
第一次崩在"32 kHz MP3 → 停止"（同一条日志里 FLAC → MP3 关了 FLAC 解码器，没事），
第二次崩在"32 kHz MP3 自然放完 → 起来下一首"。两次都不是"有关 ASRC 就崩"。

**根因**（`libs/espaudiocore/src/decoder_mp3.cpp`）：

```cpp
/* open() 里解析 Xing/LAME 头取精确时长 */
int n = in_->read(xing_, 2880);
if (n > 4) {
    xi = parseXingHeader(xing_, (size_t)n);
    audio_free(xing_);          // ← 释放了，但**没有置空**
    ...
}
...
void close() {
    ...
    if (xing_) audio_free(xing_);   // ← 对同一块内存的**第二次 free**
```

为什么作者没踩到：这条分支要求 `in_->can_seek()`，**只有本地文件源**才成立；
ringbuf（网络流）源不可 seek，永远走不到这里 —— 以流式为主的验证路径自然看不出来。

**修法**：`audio_free(xing_);` 后面补 `xing_ = nullptr;`（另加注释说明）。

> 复核方式：把库里**所有** `audio_free(...)` 逐个看"下一行有没有置空" —— 全库只有这一处漏了
> （`id3_parser.cpp` 那处是局部变量，只 free 一次，正常）。

### 顺带修掉的两个**真竞态**（不是这次崩溃的原因，但确实是 bug）

排查过程中还发现"输出任务"与"会话拆除"之间有两处 use-after-free，已一并修掉：

1. `pump_format()` 读 `espaudiocore_get_format()` 与 `audio_engine_stop()` 释放库会话对象之间
   存在 TOCTOU 窗口；
2. `esp_asrc_process()`（输出任务）与 `esp_asrc_close()`（拆除）会并发操作同一份 ASRC 状态。

修法：**凡是"读库 / 用 ASRC"的动作都持会话锁，并在锁内重新确认会话还在**。
注意**写硬件不占锁**（`esp_codec_dev_write` / `bt_service_write_pcm` 要阻塞几十毫秒，
占锁会把 LVGL 一起卡住）—— 流程是"锁内取数 + 重采样（结果拷进本任务私有的 `s_stage`）
→ 锁外写硬件"。

诊断过程中的可复用经验：这类 panic 的 backtrace 是 **`frame did not save the PC`**，
拿不到调用栈。有效手段是**在拆除路径上加逐段日志**（`session stop: …` / `teardown: delete dec`
/ `asrc close: h=…`），让"崩溃前最后一行"把范围缩到某一次 free。

## 7.2 总时长（duration）从哪来：一张表说清，别再各写一套

界面上的"总时长"只有一个出口：**app 侧的 `music_state.dur_ms`**。引擎侧另有一份
`s_info.duration_ms` / `s_dur_hint_ms`，通过 `audio_service_get_info()` 暴露给 app。

| 场景 | 来源 | 谁填 | 备注 |
| --- | --- | --- | --- |
| 在线（CK 直链 / 中间件） | `/song/url/v1` 的 `size` × 8 ÷ `br` | `os_account.cc` → `online_song_stream_info()` → app → `set_duration_ms()` | **准确**（服务端自己报的字节数与码率） |
| 在线（meting 兜底） | Content-Length × 8 ÷ 首帧码率（读 4 KB 前缀估） | 同上 | VBR 会偏 → 靠下面的"实测纠偏"修 |
| 在线（纠偏，起播 5 s 后一次） | **解码器已吃掉的字节** ÷ 已播毫秒，再乘回总字节 | `audio_engine_bitrate()` 覆盖 `s_info.duration_ms` | app 侧 `tick_adopt_stream_duration()` 采纳（差 >5% 才改） |
| 本地 MP3/FLAC/WAV/ADTS | `media_meta_probe_audio()`（Xing / STREAMINFO / 字节率 / 帧长） | `engine_start_file()` 的探测 → app `tick_derive_duration()` | 探测值优先 |
| 本地 M4A/OGG/Opus | **`espaudiocore_get_duration_ms()`** | `audio_engine_poll_duration()`（输出任务、会话锁内、拿到就短路） | 容器探测没覆盖这些格式；顺带 `stat()` 文件大小补出平均码率，**seek 因此也能用了** |
| 本地（有码率没时长） | 文件字节 × 8 ÷ 码率 | app `tick_derive_duration()` | 原有兜底 |

⚠⚠ **在线"实测码率"必须是"解码器已吃掉的字节"**，不是"已下到的字节"：
下载前沿跑在播放前面（4 MB 缓冲经常整首装下），拿 `audio_http_downloaded()` 除以已播时长
得到的是**越来越小**的假码率 —— 乘回总字节算时长，界面上的总时长就会**一边播一边往上爬**
（2026-10 用户报"在线总时长不对"就是这个；它还会把 app 的"提前结束"判据一起带偏）。
正确算法：`consumed = audio_http_downloaded() − audio_http_ring_filled()`。

⚠ **纠偏只对在线流生效**（`s_is_stream`）、且**每会话只做一次**（`s_stream_dur_fixed`）：
本地绝不能走这段 —— 探测失败时（M4A/OGG）`s_probe_bps` 是 0，而 HTTP 层里可能还留着
**上一次在线播放**的 total/downloaded，拿它算"实测码率"会把本地时长改成垃圾值。

## 8. 真机验证清单（换库后必须走一遍）

代码只做到"编译通过 + 逻辑自洽"，**音频链路必须真机听**。按下面顺序走，
每条都给了"该看到什么日志"，对不上就是那一环有问题：

| # | 操作 | 期望日志 / 现象 |
| --- | --- | --- |
| 1 | 插卡 → 本地音乐 → 放一首 **44.1k MP3** | `audio_eng: probe: N bps, N ms, N Hz, 2ch`；`audio_out: source format: 44100 Hz 2ch`；**`asrc ready (hw): 44100 Hz 2ch -> 48000 Hz 2ch`** —— 这里必须是 `(hw)`，`(sw)` 说明硬件没用上 |
| 2 | 听 30 秒 | `audio_out: play stats: PCM ~190 KB/s`（48k/16/2 满速就是 192 KB/s；明显低于 150 就要查 `gap max`/`codec` 两列） |
| 3 | 拖进度条到中段 | `audio_seek: local seek -> NNN ms`，声音从新位置接上（**不再有"定位失败：码率未知"**） |
| 4 | 放一首 **FLAC 44.1k/24bit** | 应当出声且不变调（变调=重采样没生效）；`play stats` 末尾是 `(asrc-hw)`，且 `asrc` 列**只有几百微秒**、`产/墙` 应≈100% |
| 5 | 放一首 **单声道 48k WAV** | 左右都有声（mono→stereo 走 esp_asrc 的声道转换；48k 不变率，AUTO 会落在软件声道转换上，日志是 `(sw)` —— 正常） |
| 5b | 放一首 **22050 Hz 文件** | 出声且不变调；`asrc ready (sw)` + `asrc: 硬件路径开不起来` 是**设计内**的（22.05k 不在硬件表里） |
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
3. **硬件 ASRC 只覆盖 6 种采样率**（8/16/24/32/44.1/48k）且只有 2 个 stream，
   22050 Hz 之类的率只能走软件（见 §4.1、§4.4）。
4. **在线进度百分比**依赖服务端给 `Content-Length`（分块传输时不显示百分比，UI 已兼容）。
5. `audio_service_position_ms()` 是"送去硬件的字节 ÷192"，比耳朵听到的**超前**
   一个 PCM ringbuf（32 KB ≈ 170 ms）+ codec DMA 的量 —— 目前只用于日志，
   若将来要做"精确进度"需要减掉这段积压。

## 10. 蓝牙音量：AVRCP 绝对音量（2026-10 换掉软件缩放）

### 10.1 为什么换

旧做法是**每一块喂给蓝牙的 PCM 都乘一遍音量系数**（`audio_output.cc` 的
`pcm16_scale()`）。问题是：

- 音量衰减发生在**我们这边**，耳机自己的音量条/档位**不动**，用户按耳机上的音量键
  看到的是另一套数值；
- 每块 PCM 多一次全量 memcpy + 逐样本乘法（48k/2ch 满速 192 KB/s），白烧 CPU；
- 16-bit 域缩放**天然掉信噪比**：音量 20% 时等于把 16 bit 有效位砍到 13 bit。

A2DP 源的标准做法是 **AVRCP 绝对音量**（手机都是这么干的）：把 0~100 的期望音量用
`SetAbsoluteVolume` 发给对端，由耳机/音箱**自己**衰减 —— 对端音量条跟着动，我们零成本、
零音质损失。

### 10.2 怎么接的（全部走 `esp_bt_audio_vol.h`）

```
audio_service_set_volume(60)                       ← 用户按 VOL± / 拖滑条 / 控制中心
  → audio_output_set_volume(60)
     → [route=蓝牙] bt_service_set_volume(60)       bt_service/bts_volume.cc
        → esp_bt_audio_vol_set_absolute(60)         ← esp_bt_audio 的 vol_ops
           → BTC 投递 AVRCP SetAbsoluteVolume（非阻塞）
              → 对端回 SET_ABSOLUTE_VOLUME_RSP + VOLUME_CHANGE
                 → 组件派发 ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE
                    → bts_volume_on_peer() → audio_service 同步 s_volume + NVS（**不回发**）
```

| 点 | 事实 |
| --- | --- |
| 角色 | `cfg.classic.roles` 必须带 **`ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT`**（A2DP 源 + AVRCP Controller）。不写它，`esp_bt_audio_vol_set_absolute()` 恒返回 `ESP_ERR_INVALID_STATE` 并在组件里刷 `Operation failed: vol_ops.set_absolute is not registered` —— 因为 vol_ops 只在 CT 连上时才注册 |
| 取值域 | **0~100**（组件内部再转 0~127），不是 0~127；别自己换算 |
| 何时下发 | ① 用户改音量（`bt_service_set_volume`）② 切到蓝牙输出（`audio_output_set_route`）③ A2DP 流启动（`bts_events.cc` 的 `STREAM_STARTED`，补发缓存的音量） |
| 为什么要在流启动时补发 | AVRCP Controller 是在 ACL 建链**之后**才连上的，「连接成功」那一刻它多半还没就绪。不补发的话：用户在扬声器下把音量调到 30% 再切蓝牙，耳机会按**它自己的**音量放 = 一耳朵巨响 |
| 兜底 | AVRCP 不可用（CT 没连上 / 对端不支持）时，`write_out()` 退回 `pcm16_scale()` 软件缩放。判据是 `bt_service_volume_is_avrcp()`（= 上一次 `set_absolute` 的返回值） |
| ⚠ 绝不能两条同时生效 | 那就是**平方衰减**（滑到 50% 听感像 25%） |
| ⚠ 防乒乓 | 对端收到 `SetAbsoluteVolume` 会回一条 `VOLUME_CHANGE` 通知；`bts_volume_on_peer()` 只在**值真的变了**时才通知上层，且上层（`audio_output_note_peer_volume`）**绝不回发**，否则两个设备互相刷 |

> 还有一个**限流**：`bts_volume_set()` 里有 60 ms / 5% 的节流（`BTS_VOL_MIN_INTERVAL_MS`）——
> 音量滑条每来一个 `PRESSING` 就调一次（实测 60~100 Hz），不限流就是每次拖滑条上千条
> AVRCP 命令 + 组件自己那行 `CT: Set volume to N` 的 ESP_LOGI，115200 的 console 直接被刷爆。
> 被丢掉的那几次只更新缓存，最新值由下一次调用或 `bts_volume_reapply()` 补发。

### 10.3 判读日志

| 日志 | 含义 |
| --- | --- |
| `bt_vol: AVRCP 绝对音量 可用：音量交给对端（vol=60%）` | CT 已连上，`set_absolute` 成功 —— 之后音量由耳机执行 |
| `bt_vol: AVRCP 绝对音量 不可用：退回软件缩放（vol=60%）` | CT 没连上或对端不支持 → `write_out()` 开始做软件缩放（能听出来，但音质/CPU 都吃亏） |
| `bt_vol: 下发音量 60% 给对端（AVRCP ok）` | 流启动 / 切路由时的补发 |
| `bt_vol: 对端 AVRCP 把音量改成 40%` | 耳机/手机自己拧了音量，本地状态与 NVS 已同步 |
| `E BT_AUD_OPS: Operation failed: vol_ops.set_absolute is not registered` | **AVRCP CT 没开**（roles 少写了 AVRC_CT）或还没连上 |

### 10.4 组件版本

`esp_bt_audio` 用的是本机已有的 **1.1.0~2**（`managed_components/espressif__esp_bt_audio`），
它**已经带 `esp_bt_audio_vol.h`**，API 与官方 1.2.0 文档一致（`esp_bt_audio_vol_set_absolute(uint32_t vol)`，0~100）。
工程里写的是 `espressif/esp_bt_audio: "^1.1.0"`（语义上允许 1.2.0）；联网后想显式升到 1.2.0：

```powershell
idf.py add-dependency "espressif/esp_bt_audio^1.2.0"
idf.py update-dependencies
```

> 本机当时没有外网（组件仓库 `components-file.espressif.com` 不可达、代理未运行），
> 硬改 yml 会让解析失败、整个配置过程报「Cannot establish a connection to the component registry」。
