目标设备的 USB 配置描述符，结果如下：

**UAC 版本：1.0**
- AudioControl 描述符里的 `bcdADC = 0x0100`，也就是 UAC 1.0
- Windows 实际绑定的是 `usbaudio.sys`（USB Audio 1.0 类驱动，`wdma_usb.inf`），不是 UAC2 的 `usbaudio2.sys`

**设备基本信息**
| 项 | 值 |
|---|---|
| VID/PID | `0C76:1710`（Solid State System） |
| 产品名 | USB PnP Audio Device |
| USB 版本 | 1.10（Full-Speed，12 Mbps） |
| 配置 | 接口0=AudioControl，接口1/2=AudioStreaming，接口3=HID |
| 位置 | Port_#0004，Hub_#0005 |

**采样率与格式**

| 方向 | 接口 | Alt | 声道 | 位深 | 采样率 | 端点 |
|---|---|---|---|---|---|---|
| 播放（出） | IF1 | 1 | 2 | 16 bit | **48000 Hz** | EP 0x01，192 B/ms |
| 播放（出） | IF1 | 2 | 2 | 24 bit | **48000 Hz** | EP 0x01，288 B/ms |
| 录音（入） | IF2 | 1 | 2 | 16 bit | **48000 Hz** | EP 0x82，192 B/ms |
| 录音（入） | IF2 | 2 | 2 | 24 bit | **48000 Hz** | EP 0x82，288 B/ms |

关键结论：
- 该设备只支持 **48 kHz 单一声率**，描述符里 `bSamFreqType = 1`，没有 44.1 kHz、96 kHz
- 位深可选 16 bit 或 24 bit，声道固定 2
- 播放端点 `bmAttributes = 0x0D`，属于同步（synchronous）等时端点，且没有反馈端点（implicit feedback）；传输间隔为 1 ms，包长 192/288 B
- 此外还带一个 HID 接口（MI_03，consumer control），接在 ESP32-S31 上可以忽略

**对 ESP32-S31 的含义**
- UAC 1.0 正好在 `usb_host_uac` 的支持范围内，协议层没有障碍。
- 播放必须走 48 kHz；源是 44.1 kHz 时得重采样，工程里已有 `espressif/esp_asrc` 可用。
- `uac_host_device_start()` 里按 `bit_resolution` 选择 alt：16 bit 或 24 bit 二选一，建议 16 bit（省带宽、省 CPU）。
- 它是同步等时端点、没有反馈端点，`usb_host_uac` 的隐式反馈路径需要在实机上验证一次，因为这是唯一有不确定性的点。
- 电流远低于 500 mA，Type-A 口供电没问题；但注意 UAC 独占 Type-A 口，该口就不能再插 U 盘了。

---

## 本工程的实现（2026-10-21）

上面那些结论已经落到代码里：

| 项 | 落点 |
| --- | --- |
| 新组件 | `components/services/usb_audio_service/`（USB Host + UAC 1.0 类驱动 + 推流） |
| 输出通道 | `audio_service.h` 新增 `AUDIO_OUT_USB`；`audio_output.cc` 的 `write_out()` 分流 |
| UI | 播放器页输出弹窗（`music_view_output.cc`）**只在 `usb_audio_is_ready()` 时**多出一行「USB 声卡」 |
| 拔插处理 | USB 设备回调里**立刻**通知 → `audio_service` 立即切回板载 |
| 启动点 | `main.cc` 首帧之后 `usb_audio_init()`（不占开机关键路径） |

### 为什么不需要额外的重采样

本工程输出级已经把 PCM 统一成 **48 kHz / 16-bit / 2ch**（`audio_output.cc` 的 esp_asrc 那一层，
时间轴 `ms = 字节 ÷ 192` 就是按它定的）。目标设备恰好只支持 48 kHz、16/24 bit、2ch ——
所以**同一块 PCM 原样写进 UAC 端点**即可，连一次 memcpy 都省。
选 16 bit 而不是 24 bit：省带宽（192 B/ms vs 288 B/ms）、省 CPU，听感没区别。

### 拔出为什么能"立即"切回内置输出（三道保险）

1. **USB/UAC 设备回调**（`uac_device_cb`，在 UAC 后台任务里）：一收到 `DISCONNECTED` /
   `TRANSFER_ERROR` 就把 `s_ready` 清 0 并回调上层 —— **纯内存操作**，微秒级。
   这里绝不去 `close/stop`（对已拔掉的设备做控制传输会一直超时，那就不是"立即"了）。
2. **audio_service 的状态回调**（`on_usb_audio_state`）：若当前路由是 `AUDIO_OUT_USB`，
   立刻 `audio_output_set_route(AUDIO_OUT_SPEAKER)` —— 解静音 + 开功放，纯标志与 GPIO 时序。
3. **写路径兜底**（`write_out`）：万一前两道还没跑到（拔出瞬间的竞态），这一块 PCM
   写 USB 失败时就地改走板载并顺手把路由切回去 —— 用户听不到断档。

收尾（`uac_host_device_stop/close`）交给 `usb_audio_task` 的事件队列，晚几十毫秒无妨；
`usb_audio_write()` 与它之间有句柄互斥锁，避免"写到正在被 close 的句柄上"。

### 日志怎么读

```
I usb_audio: UAC host installed, waiting for a USB sound card      ← 检测已就绪
I usb_audio: found UAC device 0C76:1710 "USB PnP Audio Device" (iface 1, 2 alt)
I usb_audio:   alt 1: 2ch 16bit, discrete rates                    ← 每档 alt 都打出来
I usb_audio:   alt 2: 2ch 24bit, discrete rates
I usb_audio: USB sound card ready: "USB PnP Audio Device" alt 1 @ 48000 Hz/2ch/16bit (50 ms buffer)
...
W usb_audio: USB audio disconnected -> not ready                    ← 拔出瞬间（第 1 道）
W audio_svc: USB audio gone -> switch back to speaker immediately    ← 路由已切（第 2 道）
```

下列情况会**只打日志、不出 UI 选项**（不假装能用）：

- 设备没有 `48 kHz / 2ch / 16 bit` 的 alt（`this device has no 48 kHz/2ch/16bit alt`）；
- `uac_host_device_start()` 失败（`uac_host_device_start failed: ...`）；
- 根本不是 UAC 设备（`uac_host_install` 之后没有任何 `found UAC device` 行）。

### 验证清单

1. 上电 → 日志出现 `UAC host installed`；
2. 插声卡 → `found UAC device ...` + `USB sound card ready ...`；
3. 播放器页点输出按钮 → 弹窗多出「USB 声卡 <设备名>（已连接 · 48 kHz）」→ 选中，声音从声卡出，
   板载扬声器静音；
4. 音量键照旧（走 UAC 的 volume control，由声卡自己衰减）；
5. **播放中直接拔掉声卡** → 1 秒内声音回到板载扬声器 + 状态栏提示「USB 声卡已断开，已切回扬声器」；
   再插回去，输出弹窗里那行会重新出现。

### 依赖

`espressif/usb`（1.5.0）+ `espressif/usb_host_uac`（1.5.0），两者 targets **都包含 `esp32s31`**。
它们是 esp_video(UVC) 带进 `managed_components/` 的，本工程已有版本即可，**不需要联网再拉**。
⚠ IDF 6.x 的 **IDF 树里已经没有 `usb` 组件**了（`components/` 下找不到 `usb_host.h`），
USB Host 库一律来自上面那个托管件，别去 IDF 里找。
