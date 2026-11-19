# 串口诊断命令（AI 分析与性能优化用）

实现在 [`main/console_cmds.cc`](../main/console_cmds.cc)，由 `main.cc` 末尾的
`console_cmds_init()` 启动（IDF 6.1 的 `esp_console_new_repl_stdio()`）。
与日志**共用 console UART**（COMx，115200，见 AGENTS §3.1），所以提示符会和日志交错，
属正常现象。

为什么有它：排查"某个文件卡顿 / 速度不对 / 拖不动"时，光看播放日志看不出**文件本身**
长什么样（采样率、位深、容器、标签多大、音频数据从第几字节开始），而这些数正是判断
"该往哪优化"的依据。

## 命令

| 命令 | 说明 |
| --- | --- |
| `songinfo <绝对路径>` | 单个文件：容器 / 采样率 / 位深 / 声道 / 时长 / 码率 / 文件大小 / **音频数据偏移** / 标签（标题·艺术家·专辑·作曲家）+ **性能提示** |
| `songscan [目录]` | 递归（≤6 层）扫描目录，**一行一个文件（TSV）**，适合整库统计与离线分析；默认 `/sdcard` |
| `playstat` | 当前播放状态快照：是否在播 / 输出是否活跃 / 位置 / **源格式** / 平均码率 / seek 可用性 / 流进度与下载速度 |
| `help` | IDF 自带，列全部命令 |

### `songinfo` 输出示例与判读

```
path: /sdcard/洛雪下载无损音乐/xxx.flac
container_guess: FLAC
file_size: 38834495
probe: ok
sample_rate: 44100          ← 源采样率；≠48000 就要走重采样
channels: 2
bits: 24                    ← 源位深；>16 在链路上多一道位深转换
duration_ms: 194931
bitrate_bps: 1576232        ← 平均码率（FLAC/WAV 是按"音频字节 ÷ 时长"补的）
expected_bytes_by_bitrate: 38407065  (差值 +427430 = 非音频数据，多为封面/标签)
audio_data_offset: 427430   ← 音频数据起点；这块越大，起播要跳过的字节越多
tag_read: ok
tag_title/tag_artist/tag_album/tag_composer: …
perf_need_asrc: yes   (源 44100 Hz 2ch -> 输出恒 48000 Hz 2ch)
perf_samples_per_sec: 88200   (采样率 × 声道；越大解码越吃力)
perf_load_vs_44100: 1.00 x
perf_extra_bit_convert: yes (源 24 bit -> 输出 16 bit)   ← 只有 >16 bit 才出现
perf_audio_bytes_per_sec: 199199   (每秒要读多少字节)
```

关键判读：
- `perf_need_asrc: yes` + `perf_load_vs_44100 > 1`（例如 96 kHz = 2.18×）⇒ 解码与重采样
  压力都是成倍上去的，配合 `play stats` 里的 `产/墙`、`asrc`、`wait` 三列判断瓶颈在哪；
- `perf_extra_bit_convert: yes` ⇒ 24-bit 音源要多走一道位深转换；
- `expected_bytes_by_bitrate` 的差值是**非音频数据**（内嵌封面最占），也是"起播慢/读得久"
  的一个来源；与 `audio_data_offset` 互相印证；
- `probe: not supported/...` ⇒ 这个容器没有探测实现（M4A/OGG），时长与 seek 会退化 ——
  播放时由解码库兜底（日志 `duration from decoder: …`）。

### `songscan` 输出（TSV）

```
容器  采样率  位深  声道  时长ms  码率bps  大小B  路径
flac  44100   24    2     194931  1576232  38834495  /sdcard/.../a.flac
mp3   32000   16    2     210000  128000    3360000  /sdcard/.../b.mp3
# 共 N 个音频文件
```

可以直接复制到 PC 侧做统计（比如"哪些文件走重采样 / 哪些是高采样率"），
`#` 开头的是注释行，`probe_fail` 表示该文件探测失败。

## 注意

- 命令跑在 **REPL 任务**（不是 LVGL 任务），所以不碰任何 UI 对象 —— 只调
  `media_meta` / `audio_service` 的只读接口；
- `songinfo` / `songscan` 会**同步读 SD**，扫描大目录时命令会阻塞一会儿（这是有意的：
  诊断要一口气把结果打完）；
- REPL 栈 8 KB（`songscan` 递归 6 层 × 512 B 路径缓冲 + 探测/打印），
  **别随手放大递归层数或路径缓冲**；
- 加新命令：改 `console_cmds.cc` 的 `cmds[]` 数组即可（别忘了包含 `console_cmds.h` 自己，
  否则 `main.cc` 按 C 链接调用会 `undefined reference` —— 本工程踩过同类坑）。
