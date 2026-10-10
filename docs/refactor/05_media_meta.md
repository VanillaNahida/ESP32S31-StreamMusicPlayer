# 05 `media_meta.cc` 拆分重构蓝图

> 审计对象：`components/services/media_meta/media_meta.cc`
> 真实行数：**2104 行**（LF 计数，与 `read`/`grep` 行号一致；用户口头说"约 1954 行"不准，
> pwsh 的 `Get-Content | Measure-Object -Line` 也会少算）
> 头文件：`components/services/media_meta/include/media_meta.h`（170 行）
> 同组件其它文件：`gbk_table.c`（136 KB 源，126×191×2 = 48 KB flash 只读表）、
> `CMakeLists.txt`（`REQUIRES esp_new_jpeg libpng`）、`lodepng/`（**只有 LICENSE.txt，代码未使用**）
> 本报告为只读审计产物，未修改任何源文件。

---

## 1. 文件概览

### 1.1 include（18–31 行）

| 类别 | 内容 |
| --- | --- |
| 标准库 | `stdio.h` `stdlib.h` `string.h` `ctype.h` `strings.h` `sys/stat.h` |
| ESP-IDF | `esp_log.h`、`esp_heap_caps.h`（`heap_caps_aligned_alloc` 给 PNG 全尺寸缓冲） |
| 解码器 | `esp_jpeg_dec.h` + `esp_jpeg_common.h`（`esp_new_jpeg` 组件）、`png.h`（官方 **libpng** 组件的 `png_image` 简化 API） |

**不依赖 LVGL、不依赖 FreeRTOS、不依赖 NVS/网络** —— 全文件 grep 无 `lv_`、
无 `xSemaphore`/`xTaskCreate`、无 `esp_lv_adapter`。唯一的"外部状态"是
`gbk_to_uni[126][191]`（1964 行 `extern`，定义在 `gbk_table.c`）。

### 1.2 结构分区

| 区间 | 分区 |
| --- | --- |
| 1–54 | 文件头契约 + `TAG` + `LRC_MAX_LINES 400` + `media_meta_lrc_reset()` |
| 56–296 | **LRC 时间轴纯逻辑**：`ensure_cap` / `push_line` / `parse_tag` / `cmp_line` / `lrc_parse_utf8` / 边文件 / 二分索引 |
| 298–560 | **字节序工具 + ID3/FLAC 封面提取**：`be32/be24/synchsafe32/mime_is_png`、`audio_data_offset`、`copy_image`、`id3_cover`、`flac_cover`、`pick_div` |
| 562–778 | **解码到 RGB565**：`jpeg_to_rgb565`（单图）+ `media_meta_cover_rgb565/cover_free/image_file_rgb565/image_mem_rgb565` |
| 780–973 | **MP3 帧头探测**：码率/采样率表、`mp3_frame_t`、`mp3_parse_header`、`media_meta_probe_mp3`（ID3 跳过 + Xing/Info） |
| 975–1317 | **通用探测**：`probe_fill_bitrate`、`probe_flac`（块链 + STREAMINFO + 留证）、`probe_wav`、`probe_adts_aac`、`media_meta_probe_audio` |
| 1319–1576 | **ID3 文本/歌词**：`id3_text_to_utf8`（Latin1/UTF-16/UTF-8）、`skip_encoded_string`、`sylt_to_lrc`、`id3_lyrics`（USLT/SYLT） |
| 1578–1750 | 内嵌歌词对外入口 + **歌词查找/落盘**：`load_lrc_file`、`lyrics_dir_for`、`lyrics_cache_path`、`media_meta_lyrics_load`、`media_meta_lyrics_load_file` |
| 1751–1948 | **`image_to_rgb565`**（186 行）：PNG（libpng `png_image`）与 JPEG 两路 → 盒式降采样 → RGB565 |
| 1950–2104 | **编码转换**：`utf8_put`、`is_valid_utf8`、`lrc_bytes_to_utf8`（BOM/UTF-8/GBK）、`media_meta_lrc_parse` |

---

## 2. 全部函数清单（47 个顶层定义）

（34/42 行是前向声明，不计入；806/816 行是结构体收尾。）

| 起始 | 结束 | 行数 | 函数名 | static | 分类 | 一句话职责 |
| --- | --- | --- | --- | --- | --- | --- |
| 44 | 54 | 11 | `media_meta_lrc_reset` | ✘ | LRC | `free(lines)` 并清零 count/cap、`timed=true` |
| 56 | 75 | 20 | `ensure_cap` | ✔ | **纯逻辑** | 行数组按 2 倍扩容（上限 `LRC_MAX_LINES=400`），`realloc` |
| 77 | 90 | 14 | `push_line` | ✔ | **纯逻辑** | 追加一行（时间 + 文本），并去掉行尾空白 |
| 93 | 139 | 47 | `parse_tag` | ✔ | **纯逻辑** | 解析一个 `[mm:ss.xx]`；元信息标签（`[ti:` 等）返回 `0xFFFFFFFF` 标记 |
| 141 | 148 | 8 | `cmp_line` | ✔ | **纯逻辑** | `qsort` 比较器（按时间） |
| 150 | 254 | **105** | `lrc_parse_utf8` | ✔ | **纯逻辑** | 逐行解析：**一行多标签**各生成一行、乱序后排序、无标签则整段当纯文本（`timed=false`） |
| 256 | 278 | 23 | `media_meta_lrc_load_sidecar` | ✘ | IO | 把音频路径扩展名换成 `.lrc` 后 `load_lrc_file`（路径缓冲 600 B，防长中文名截断） |
| 280 | 296 | 17 | `media_meta_lrc_index_at` | ✘ | **纯逻辑** | 二分查"pos_ms 时刻应高亮的行"（无歌词 -1） |
| 313 | 316 | 4 | `be32` | ✔ | **纯逻辑** | 大端 32 位读 |
| 318 | 321 | 4 | `be24` | ✔ | **纯逻辑** | 大端 24 位读（FLAC 块长度） |
| 324 | 328 | 5 | `synchsafe32` | ✔ | **纯逻辑** | ID3v2 的 7-bit synchsafe 长度 |
| 330 | 344 | 15 | `mime_is_png` | ✔ | **纯逻辑** | 只看 `image/png` 前缀（大小写不敏感）——**仅用于日志/回退提示** |
| 346 | 362 | 17 | `media_meta_audio_data_offset` | ✘ | IO | MP3 的 ID3v2 总长（10 + synchsafe size），其它容器 0 |
| 365 | 377 | 13 | `copy_image` | ✔ | 内存 | `malloc + memcpy` 把图片字节拷出（所有权交调用方） |
| 381 | 478 | **98** | `id3_cover` | ✔ | 解析+IO | 遍历 ID3v2.2/2.3/2.4 帧找 `PIC`/`APIC`，拷出图片字节（unsync/extended header 直接放弃） |
| 482 | 549 | 68 | `flac_cover` | ✔ | 解析+IO | 走 FLAC 元数据块链找 `PICTURE(type 6)` 并拷出图片 |
| 553 | 560 | 8 | `pick_div` | ✔ | **纯逻辑** | 按长边选 1/2^n 降采样倍数 |
| 562 | 639 | 78 | `jpeg_to_rgb565` | ✔ | 解码 | JPEG 单图解码：先 `parse_header` 探尺寸 → 再开一次 `jpeg_dec_process` 出 RGB565 |
| 643 | 687 | 45 | `media_meta_cover_rgb565` | ✘ | **对外** | 音频内嵌封面：按扩展名选 `id3_cover`/`flac_cover` → `image_to_rgb565` |
| 689 | 694 | 6 | `media_meta_cover_free` | ✘ | **对外** | `jpeg_free_align(pixels)`（NULL 安全） |
| 715 | 758 | 44 | `media_meta_image_file_rgb565` | ✘ | **对外** | 独立封面文件（JPEG/PNG）→ RGB565；大小门限 `MEDIA_META_COVER_FILE_MAX`(713) |
| 765 | 778 | 14 | `media_meta_image_mem_rgb565` | ✘ | **对外** | 内存缓冲版（在线封面/二维码/首页背景走它），同一套解码器 |
| 818 | 878 | 61 | `mp3_parse_header` | ✔ | **纯逻辑** | 解析 MPEG 帧头 → 版本/层/采样率/码率/帧长/每帧样本/声道 |
| 880 | 973 | **94** | `media_meta_probe_mp3` | ✘ | **对外** | 跳 ID3v2 → 找合法帧头（下一帧复核）→ CBR 用帧码率；有 Xing/Info 用帧数算**准确时长**+平均码率 |
| 993 | 996 | 4 | `le16` | ✔ | **纯逻辑** | 小端 16 位 |
| 997 | 1000 | 4 | `le32` | ✔ | **纯逻辑** | 小端 32 位 |
| 1004 | 1009 | 6 | `probe_fill_bitrate` | ✔ | **纯逻辑** | 码率缺失时按 `音频字节×8÷时长` 补平均码率（seek 需要的正是这个值） |
| 1029 | 1178 | **150** | `probe_flac` | ✔ | 解析+IO | 允许前置 ID3v2 → 块链找 STREAMINFO（34 B）→ 总采样数÷采样率；块链留证 + 越界/回退 |
| 1184 | 1234 | 51 | `probe_wav` | ✔ | 解析+IO | `RIFF/WAVE` 块链取 `fmt `（字节率/声道/采样率）与 `data` 长度 → 时长/码率 |
| 1245 | 1274 | 30 | `probe_adts_aac` | ✔ | 解析+IO | ADTS 头 7 B：采样率索引 + 13bit 帧长 → 瞬时码率，时长按文件大小估 |
| 1276 | 1317 | 42 | `media_meta_probe_audio` | ✘ | **对外** | **按魔数分派**：`.mp3` 转 `probe_mp3`，否则依次试 FLAC/WAV/AAC |
| 1335 | 1409 | 75 | `id3_text_to_utf8` | ✔ | **纯逻辑** | ID3 文本帧编码转换：Latin1 / UTF-16(BOM/BE) / UTF-8 → UTF-8 |
| 1412 | 1424 | 13 | `skip_encoded_string` | ✔ | **纯逻辑** | 按编码跳过 ID3 帧里的一个字符串字段 |
| 1427 | 1486 | 60 | `sylt_to_lrc` | ✔ | **纯逻辑** | ID3 `SYLT`（同步歌词）→ LRC 文本 |
| 1489 | 1576 | 88 | `id3_lyrics` | ✔ | 解析+IO | 只读 ID3 里的 `USLT`（优先）或 `SYLT` 帧 body，不整块读标签 |
| 1578 | 1595 | 18 | `media_meta_embedded_lyrics` | ✘ | **对外** | 内嵌歌词原文（仅 .mp3），malloc 交给调用方 |
| 1597 | 1600 | 4 | `media_meta_lyrics_free` | ✘ | **对外** | `free(text)` |
| 1604 | 1629 | 26 | `load_lrc_file` | ✔ | IO | 读 `.lrc` 文件（≤256 KB）→ `media_meta_lrc_parse` |
| 1632 | 1643 | 12 | `lyrics_dir_for` | ✔ | **纯逻辑** | `/sdcard/xx/yy.mp3` → `/sdcard/lrc` |
| 1646 | 1658 | 13 | `lyrics_cache_path` | ✔ | **纯逻辑** | 音频路径 → `/sdcard/lrc/<同名>.lrc` |
| 1660 | 1712 | 53 | `media_meta_lyrics_load` | ✘ | **对外** | 三级查找：同目录 `.lrc` → `/lrc` 缓存 → 内嵌（取到后**自动落盘**到 `/lrc`） |
| 1719 | 1750 | 32 | `media_meta_lyrics_load_file` | ✘ | **对外** | 直接读一个独立 `.lrc` 文件（在线流式歌词，≤512 KB） |
| 1763 | 1948 | **186** | `image_to_rgb565` | ✔ | 解码 | 按魔数分派 PNG(libpng `png_image`, 有 png bomb 边长门限)/JPEG → 全尺寸临时缓冲（PSRAM 16 B 对齐）→ 盒式降采样 → `jpeg_calloc_align` 输出 RGB565 |
| 1966 | 1979 | 14 | `utf8_put` | ✔ | **纯逻辑** | 码点 → UTF-8（≤3 字节） |
| 1982 | 2022 | 41 | `is_valid_utf8` | ✔ | **纯逻辑** | 严格 UTF-8 校验（过长编码/代理区/超范围） |
| 2025 | 2092 | 68 | `lrc_bytes_to_utf8` | ✔ | **纯逻辑** | BOM(UTF-8/UTF-16LE/BE) → 合法 UTF-8 原样 → 否则按 GBK 查表转 UTF-8（认不出给 `?`） |
| 2095 | 2104 | 10 | `media_meta_lrc_parse` | ✘ | **对外** | 公开入口：先做编码归一化再交给 `lrc_parse_utf8` |

**最大 5 个**：`image_to_rgb565`(186)、`probe_flac`(150)、`lrc_parse_utf8`(105)、`id3_cover`(98)、
`media_meta_probe_mp3`(94)。

---

## 3. 全部全局 / 静态变量清单

| 行号 | 名字 | 类型 | 用途 / 所有权 | 拆分要点 |
| --- | --- | --- | --- | --- |
| 37 | `TAG` | `const char *` | 日志标签 `"media_meta"` | 只读 |
| 39 | `LRC_MAX_LINES` | 宏 = 400 | 歌词行数硬上限 | 与 `ensure_cap` 配套 |
| 713 | `MEDIA_META_COVER_FILE_MAX` | 宏 = 4 MB | 独立封面文件/内存缓冲上限 | **必须 ≥ 在线侧 `COVER_MAX_BYTES`(4 MB)** |
| 795–799 | `k_mp3_br_v1_l1/l2/l3`、`k_mp3_br_v2_l1/l23` | `static const uint16_t[16]` | MP3 码率表（kbps） | 纯常量，随 `mp3_parse_header` 搬 |
| 802–806 | `k_mp3_sr[3][4]` | `static const uint32_t` | 采样率表（MPEG1/2/2.5） | 同上 |
| **1026** | `s_flac_blk[16]` | `static struct {uint8_t type; long len; long pos;}` | **FLAC 块链留证**（诊断用） | ⚠ **唯一的可变全局状态**：`probe_flac` 写、失败时打印；两个任务并发探测会互相覆盖（只影响日志） |
| **1027** | `s_flac_blk_seen` | `static int` | 已记录块数 | 同上；拆分后建议改成 `probe_ctx_t` 局部结构 |
| 1242–1243 | `k_aac_rate_tab[13]` | `static const uint32_t` | ADTS 采样率表 | 纯常量 |
| 1964 | `gbk_to_uni[126][191]` | `extern const uint16_t`（定义在 `gbk_table.c`） | GBK→Unicode 表（48 KB flash） | 只在 `lrc_bytes_to_utf8` 用；host 单测可直接链这个 .c |

无其它全局：所有缓冲都是栈上/`malloc`/`realloc`，`lrc_t` 由调用方持有。

---

## 4. 外部依赖清单

### 4.1 依赖的外部 API

| 模块 | 具体调用（行号） | 用途 |
| --- | --- | --- |
| `esp_new_jpeg` | `DEFAULT_JPEG_DEC_CONFIG`(567/595/1846)、`jpeg_dec_open`(570/601/1852)、`jpeg_dec_parse_header`(577/609/1859)、`jpeg_dec_get_outbuf_len`(616/1864)、`jpeg_dec_process`(628/1877)、`jpeg_dec_close`(578/611/629/1860/1865/1878)、`jpeg_calloc_align`(621/1894/1909)、`jpeg_free_align`(631/692) | JPEG 解码 + 16 字节对齐分配/释放 |
| `libpng`（官方组件） | `png_image_begin_read_from_memory`(1787)、`png_image_free`(1789/1805/1814/1826)、`PNG_IMAGE_SIZE`(1810)、`png_image_finish_read`(1818)、`PNG_FORMAT_RGB`(1809) | PNG 解码（简化 API，一次调用出 RGB888） |
| `esp_heap_caps` | `heap_caps_aligned_alloc(16, need, SPIRAM\|8BIT)`(1811) | PNG 全尺寸临时缓冲（PSRAM） |
| newlib / FATFS | `fopen/fclose/fread/fseek/ftell/rewind/mkdir`（381–478、482–549、658–756、880–973、1029–1317、1588–1629、1660–1750） | 读音频/歌词文件、写 `/lrc` 缓存 |
| `esp_log` | `ESP_LOGI/W/D` | 诊断（含 FLAC 块链留证） |
| 本组件 | `gbk_table.c` 的 `gbk_to_uni`(2078) | GBK→UTF-8 |

**未使用**：LVGL、FreeRTOS 同步原语、NVS、网络、`audio_service`、`online_service`。
`lodepng/` 目录只有 LICENSE，**未编译、未引用**。

### 4.2 对外导出 API（`include/media_meta.h`）

| 分组 | 符号 |
| --- | --- |
| 类型/宏 | `lrc_line_t{time_ms, text[160]}`、`lrc_t{lines,count,cap,timed}`、`audio_probe_t{bitrate,duration_ms,sample_rate,channels}`、`LRC_TEXT_MAX 160` |
| LRC | `media_meta_lrc_reset`(h:38)、`media_meta_lrc_load_sidecar`(h:44)、`media_meta_lrc_parse`(h:48)、`media_meta_lyrics_load_file`(h:52)、`media_meta_lyrics_load`(h:62)、`media_meta_lrc_index_at`(h:69) |
| 歌词原文 | `media_meta_embedded_lyrics`(h:65)、`media_meta_lyrics_free`(h:66) |
| 封面 | `media_meta_cover_rgb565`(h:89)、`media_meta_cover_free`(h:93)、`media_meta_image_file_rgb565`(h:104)、`media_meta_image_mem_rgb565`(h:116) |
| 探测 | `media_meta_audio_data_offset`(h:125)、`media_meta_probe_mp3`(h:147)、`media_meta_probe_audio`(h:166) |

**外部调用方（拆分不得破坏的契约）**

| 调用方 | 位置 | 用到的接口 |
| --- | --- | --- |
| `audio_service.cc` | 392（MUSIC_INFO 处理里） | `media_meta_probe_audio`（本地文件码率/时长优先用探测值） |
| `audio_service.cc` | 563 | `media_meta_audio_data_offset`（seek 的"时间→字节"基准） |
| `app_music.cc` | 544 / 2190 / 3560 | `media_meta_lyrics_load` |
| `app_music.cc` | 2183 / 1239 / 1257 / 2302 / 2193 | `media_meta_lrc_parse` / `media_meta_lrc_index_at` / `media_meta_lrc_reset` |
| `app_music.cc` | 1950/1952 | `media_meta_image_mem_rgb565`（在线封面）/ `media_meta_cover_rgb565`（本地内嵌封面） |
| `app_music.cc` | 1785/1882/2378/2385/2405 | `media_meta_cover_free` |
| `app_online.cc` | 291 | `media_meta_image_mem_rgb565`（二维码 PNG）+ `media_meta_cover_free` |
| `ui_core/ui_home_bg.cc` | 81/96 | `media_meta_image_mem_rgb565`（内置首页背景 JPEG）+ free |

---

## 5. 线程 / 回调模型

- **本文件不含任何任务/定时器/锁**；它是"被调用方"。运行上下文完全取决于调用者：

| 调用点 | 上下文 | 说明 |
| --- | --- | --- |
| `audio_service.cc:392/563` | **播放器/GMF 事件处理上下文**（MUSIC_INFO 事件里） | 同步小 IO（最多读几十字节 + 少量 `fseek`），头文件 h:164 明确"别在 LVGL 任务里调" |
| `app_music.cc:1950/1952`（封面工作项） | app_music 自建的封面加载任务 | `media_meta_cover_rgb565` 是全尺寸软解，**几十~几百 ms**（h:86–87 明示同步阻塞） |
| `app_music.cc:544/2190/3560`（歌词） | app_music 的工作/页面构建路径 | `media_meta_lyrics_load` 会读文件、必要时解 ID3 并**写 `/lrc`** |
| `app_online.cc:291`（二维码） | **LVGL 任务**（`qr_show_png` 在事件/结果处理里直接调） | PNG 240×240 小图，实测可接受；但这是**唯一在 LVGL 任务里做解码**的地方，拆分时要标注 |
| `ui_home_bg.cc:81` | 启动期（LVGL 任务/初始化） | 内置背景图解码 |

- **可重入性**：除 `s_flac_blk[]/s_flac_blk_seen`（1026–1027，仅诊断用途）之外，
  所有函数只用参数与栈/堆，**天然可重入**；`lrc_t` 的所有权完全在调用方
  （`media_meta_lrc_reset` 释放 `lines`）。
- **内存所有权**（见 §A）由 h:80/h:92/h:99/h:110 的契约固定：**像素一律 `media_meta_cover_free`**，
  歌词文本一律 `media_meta_lyrics_free`，`lrc_t` 内部缓冲一律 `media_meta_lrc_reset`。

---

## 6. 拆分方案

### 6.1 目标目录树

```
components/services/media_meta/
├── CMakeLists.txt              # SRCS 增加新文件；保留 REQUIRES esp_new_jpeg libpng
├── include/
│   ├── media_meta.h            # 对外契约：一行不改
│   └── internal/
│       ├── mm_bits.h           #   be16/be24/be32/le16/le32/synchsafe32（纯）
│       ├── mm_lrc.h            #   LRC 解析与时间轴（纯）
│       ├── mm_id3.h            #   ID3v2 帧遍历/文本/歌词（纯解析 + 帧 IO）
│       ├── mm_container.h      #   MP3/FLAC/WAV/ADTS 的"魔数分派 + 参数探测"（纯解析 + IO）
│       ├── mm_image.h          #   解码到 RGB565（JPEG/PNG）
│       └── mm_text.h           #   UTF-8/UTF-16/GBK 编码转换（纯）
└── src/
    ├── media_meta.cc           # 只留对外入口的薄编排（~150 行）
    ├── mm_bits.cc              # 字节序工具（~60 行）
    ├── mm_lrc.cc               # LRC 解析/索引/编码归一化（~330 行）
    ├── mm_id3.cc               # ID3v2：封面帧、USLT/SYLT、文本编码（~380 行）
    ├── mm_cover.cc             # 封面提取编排 + 独立图文件/内存入口（~200 行）
    ├── mm_image.cc             # JPEG/PNG → RGB565 + 降采样（~300 行）
    ├── mm_probe.cc             # MP3 帧头 + FLAC/WAV/ADTS + probe_audio 分派（~420 行）
    └── mm_lyrics_file.cc       # 歌词三级查找 + /lrc 落盘（~180 行）
```

### 6.2 文件职责与接口

| 文件 | 搬走的行号 | 内部接口（建议） | 预估行数 |
| --- | --- | --- | --- |
| `src/media_meta.cc` | 对外入口转发 + `media_meta_cover_free/lyrics_free/lrc_reset` | 只 include 各 `mm_*.h` | ~150 |
| `src/mm_bits.cc` | 313–328、993–1000 | `mm_be24/be32/le16/le32/synchsafe32` | ~60 |
| `src/mm_lrc.cc` | 39、44–296、1950–2104 | `mm_lrc_parse_utf8(text, lrc_t*)`、`mm_lrc_index_at`、`mm_lrc_bytes_to_utf8(raw,n,&out)`、`mm_lrc_load_file(path,lrc_t*)`、`mm_lrc_cache_path(audio,out,cap)` | ~330 |
| `src/mm_id3.cc` | 330–344、1335–1576 | `mm_id3_cover(FILE*, mm_blob_t*)`、`mm_id3_lyrics(FILE*, char**)`、`mm_id3_text_to_utf8(...)`、`mm_id3_skip_frame(...)` | ~380 |
| `src/mm_cover.cc` | 298–312、365–377、482–549、643–778 | `mm_flac_cover(FILE*, mm_blob_t*)`、`mm_cover_from_audio(path,...)`、`mm_cover_from_file(path,...)`、`mm_cover_from_mem(data,len,...)` | ~200 |
| `src/mm_image.cc` | 553–639、1751–1948 | `mm_image_to_rgb565(data,len,max_dim,out,w,h)`、`mm_pick_div(long_side,max_dim)` | ~300 |
| `src/mm_probe.cc` | 780–1317 | `mm_mp3_parse_header`、`mm_probe_mp3(path,audio_probe_t*)`、`mm_probe_flac/wav/adts(FILE*,size,out)`、`mm_probe_audio(path,out)` | ~420 |
| `src/mm_lyrics_file.cc` | 1604–1750 | `mm_lyrics_load(path,lrc_t*)`、`mm_lyrics_load_file(lrc,out)`、`mm_lyrics_dir_for/cache_path` | ~180 |

### 6.3 纯逻辑 / 可 host 单测边界（**本文件的最大收益点**）

| 纯逻辑（零 ESP-IDF 依赖，只要把 `esp_err_t` 换成 `int` 就能 host 编译） | 行号 |
| --- | --- |
| LRC 解析（多标签/乱序/纯文本回退） | 93–254 |
| LRC 二分索引 | 280–296 |
| 编码转换（UTF-8 校验、UTF-16 BOM、GBK 查表） | 1966–2092 |
| 字节序 / synchsafe | 313–328、993–1000 |
| MP3 帧头解析（版本/层/码率/采样率/帧长） | 818–878 |
| FLAC STREAMINFO 取值、WAV 块链、ADTS 头 | 1057–1078、1199–1232、1248–1272 |
| ID3 文本编码转换 / SYLT→LRC | 1335–1486 |
| 封面"魔数 → 解码器"分派与 `pick_div` | 553–560、1769 |
| 长度/边界判定（`probe_fill_bitrate`） | 1004–1009 |

**建议的无 IO 接口改造**（把 FILE* 抽掉，纯靠内存缓冲，便于 host 单测）：

```c
/* 全部只读输入缓冲，不做任何 IO；返回值用 int（0 = OK）以便 host 编译 */
int mm_probe_flac_mem(const uint8_t *buf, size_t len, audio_probe_t *out);
int mm_probe_wav_mem (const uint8_t *buf, size_t len, audio_probe_t *out);
int mm_probe_adts_mem(const uint8_t *buf, size_t len, audio_probe_t *out);
int mm_mp3_probe_mem (const uint8_t *buf, size_t len, audio_probe_t *out);
int mm_id3_cover_mem(const uint8_t *buf, size_t len, mm_blob_t *out);
int mm_id3_lyrics_mem(const uint8_t *buf, size_t len, char **out);
```

（现有 `FILE*` 版本退化成"读进缓冲 + 调 `*_mem`"；`id3_cover/flac_cover/id3_lyrics`
原本就只做 `fseek` + 读帧，换成"缓冲 + 偏移"是机械改动。）

### 6.4 迁移顺序（每步可编译、行为不变）

1. `mm_bits.cc` + `mm_text.cc`（313–328/993–1000、1966–2092）——纯搬。
2. `mm_lrc.cc`（39–296 + `media_meta_lrc_parse`）；`media_meta.cc` 只留转发。
3. `mm_image.cc`（553–639、1751–1948）——**只搬不动**，保持 `jpeg_calloc_align` 输出契约。
4. `mm_id3.cc`（330–344、1335–1576）、`mm_cover.cc`（365–549、643–778）。
5. `mm_probe.cc`（780–1317）——顺手把 `s_flac_blk[]` 收进 `probe_flac` 的局部上下文。
6. `mm_lyrics_file.cc`（1604–1750）。
7. 加 host 单测（见 §C），跑 `idf.py build` + 真机回归：MP3 封面、FLAC 封面、
   在线封面（内存版）、二维码、LRC 高亮、FLAC 时长/拖动。

---

## 7. 风险清单

| # | 风险 | 触发场景 | 防护 / 验证 |
| --- | --- | --- | --- |
| **R1** | `probe_flac` 块链偏移**曾经**算飞（真机 94 MB/138 MB）：根因是"只 `data_off += nlen` 不 `fseek`"。**现已在 1100/1128 修好**（每步都真移文件位置），并在 1122 加了"块长 > 文件大小立刻停"+ 1123–1163 留证 | 拆分时"优化"掉某次 `fseek`（例如把两次合并） | 保留"改偏移 + 真移位置"成对出现；单测用真实 FLAC（427430 偏移那个样本）核对 `data_off`/码率 |
| **R2** | `image_to_rgb565` 输出必须由 `jpeg_calloc_align` 分配（1759/1894/1909），因为 `media_meta_cover_free` 用 `jpeg_free_align`(692) | 拆 `mm_image.cc` 时改成 `malloc`/`heap_caps_malloc` → free 时崩溃或堆损坏 | 在 `mm_image.cc` 里写死"输出只能 jpeg_calloc_align"；单测：所有出口都过 `media_meta_cover_free` |
| **R3** | PNG 全尺寸缓冲用 `heap_caps_aligned_alloc`(1811) 却用 `free(full)`(1821) 释放 | IDF 契约上应对应 `heap_caps_free`；换分配器/分区后可能不匹配 | 统一成 `heap_caps_free`（或统一 `jpeg_free_align`）；这是**现存的小隐患**，拆分时顺手修 |
| **R4** | 头文件与实现的**上限不一致**：h:101/h:113 写"超过 2 MB"，实际 `MEDIA_META_COVER_FILE_MAX = 4 MB`(713)、`image_file`(735)/`image_mem`(774) 都按 4 MB 判 | 调用方按文档准备 2~4 MB 缓冲 | 统一常量 + 更新注释；与在线侧 `COVER_MAX_BYTES`(4 MB) 用 `_Static_assert` 绑定 |
| **R5** | `s_flac_blk[16]/s_flac_blk_seen`(1026–1027) 是**唯一可变全局**，无锁 | 两个任务同时 `media_meta_probe_audio` 一个坏 FLAC → 日志串味 | 收进局部 `probe_ctx_t`；或加注释说明"仅诊断" |
| **R6** | PNG "png bomb" 只挡了边长（`side_limit = max_dim*8`，1800–1807），**没挡总像素内存**；JPEG 同样按原始尺寸解 | 精心构造的 8×max_dim 方图 | 已可接受（例：max_dim=240 → 1920×1920×3 ≈ 11 MB PSRAM，会 `ESP_ERR_NO_MEM` 而非崩溃）；拆分时保留门限顺序（**先判尺寸再申请**） |
| **R7** | `lrc_parse_utf8` 的固定 512 B 行缓冲（165 行附近）+ `LRC_TEXT_MAX=160` 截断 | 超长行（含大量标签） | 单测：一行 20 个标签、超长文本；确认 `push_line` 的 `snprintf` 截断不越界 |
| **R8** | `parse_tag` 用 `sscanf("%d:%d")` 且手写小数位处理（113–130） | `[1:2:3]`、`[00:60.999]`、`[offset:-500]` | 单测边界；`offset` 标签当前**只跳过不生效**（107），拆分时别误当成 bug 去"修" |
| **R9** | `media_meta_lyrics_load` 会**写 SD**（1685–1709：内嵌歌词自动落盘 `/lrc`） | 拆分后把它放进"只读"模块，或并发调用导致半截文件 | 保留"取到就落盘、失败只告警"的语义；落盘不是原子的（无 tmp+rename） |
| **R10** | 路径缓冲：`lyrics_cache_path`/`lyrics_dir_for` 用 600 B（1648/1674–1675）、`media_meta_lrc_load_sidecar` 用 600 B(264) | 超长中文路径（FATFS LFN 255/段，多级仍可能超） | 保留长度检查（265–269）；单测超长路径 |
| **R11** | `id3_cover` 对 unsynchronisation / extended header 直接放弃（396–399），`id3_lyrics` 只认 USLT/SYLT | 特殊 MP3 没有封面/歌词 | 保持"宁可不显示也不解析垃圾"的策略，注释保留 |
| **R12** | `media_meta_audio_data_offset` **只认 MP3**（358–361），其它容器返回 0 | FLAC/WAV 的 seek 基准偏移缺失（AGENTS.md §11.4 记的"流上 `s_data_offset=0` 早到几秒"） | 拆分时把"容器 → 数据偏移"扩展点留出来（FLAC 可用 STREAMINFO 后的偏移），但**不改行为** |
| **R13** | `probe_adts_aac` 的时长是"文件大小估"（1266–1272），且 `channels==0 → 2`(1263) 是猜的 | AAC 文件时长偏差 | 注释写明是估算；单测覆盖 sfi=13/帧长 ≤7 的拒绝分支 |
| **R14** | `probe_wav` 中 `data` 长度写 0 时按剩余文件算（1223–1228）；块按偶数字节对齐（1215） | 非常规 WAV | 单测：奇数字节块、`data` 在 `fmt ` 之前 |
| **R15** | GBK 转换里"认不出的字节 → `?`"（2086）不吞后续内容；`is_valid_utf8` 对 4 字节序列也放行 | 混合编码歌词 | 单测：UTF-16LE 无 BOM、GBK 半角日文、非法 UTF-8 序列 |
| **R16** | `ensure_cap` 上限 400 行（62–67）：超出后 `push_line` **静默丢弃**后续行 | 长歌词（>400 行） | 保持静默（不报错）但日志已打 `parsed %u lyric lines`(252)；单测 401 行输入 |

---

## 8. 附加必答

### A. 内存与所有权总表

| 函数 | 返回的内存 | 分配方式（行号） | 谁释放 |
| --- | --- | --- | --- |
| `media_meta_cover_rgb565` | `*pixels`（RGB565） | `image_to_rgb565` → `jpeg_calloc_align`(1909) | **调用方** `media_meta_cover_free()`(689) |
| `media_meta_image_file_rgb565` | `*pixels` | 同上；文件读取缓冲 `malloc`(739) 内部已 `free`(756) | `media_meta_cover_free()` |
| `media_meta_image_mem_rgb565` | `*pixels` | 同上（不复制输入） | `media_meta_cover_free()`；**输入 data 必须在调用期间有效**（h:112） |
| `image_to_rgb565`（内部） | `*out_pixels` | `jpeg_calloc_align`(1894/1909)；中间全尺寸缓冲 `heap_caps_aligned_alloc`(1811) / `jpeg_calloc_align`(1869) 在函数内释放 | 见上 |
| `id3_cover` / `flac_cover` / `copy_image` | `*out`（图片原始字节） | `malloc`(368) | `media_meta_cover_rgb565` 内部 `free(raw)`(680)——**不外发** |
| `media_meta_embedded_lyrics` | `*text`（UTF-8） | `sylt_to_lrc`/`id3_lyrics` 内部 `malloc` | **调用方** `media_meta_lyrics_free()`(1597) |
| `media_meta_lyrics_load` / `_load_file` / `lrc_parse` | 填 `lrc_t` | `ensure_cap` 的 `realloc`(68) | **调用方** `media_meta_lrc_reset()`(44) |
| `media_meta_audio_data_offset` / `probe_*` | 无（填结构体） | — | — |

**失败路径检查结论**：`id3_cover`/`flac_cover` 失败不分配；`copy_image` 失败返 `ESP_ERR_NO_MEM`；
`image_to_rgb565` 的 PNG 路径在 `finish_read` 失败时 `free(full)`(1821)；
`load_lrc_file`/`media_meta_lyrics_load_file` 的缓冲在所有分支都 `free`(1627/1748)。
**未发现泄漏或双重释放**（唯一可疑点是 R3 的 `heap_caps_aligned_alloc` + `free` 配对）。

### B. 解码器依赖与像素格式

- **分派原则**：`image_to_rgb565` 只看**魔数**（1769：`89 50 4E 47` → PNG，否则走 JPEG）；
  `mime_is_png`(330) 只用于 `id3_cover` 的日志/提示，**不作解码依据**（APIC 的 MIME 常常撒谎）。
- **JPEG 路径**：`jpeg_to_rgb565`(562–639) 是"开两次"的写法（注释 566：输出格式/尺寸要
  `parse_header` 之后才知道）；`image_to_rgb565` 内的 JPEG 分支（1846–1878）额外
  用 PSRAM 分配输出（1869），失败时回退到 `jpeg_to_rgb565`(1873)。
- **PNG 路径**：libpng 简化 API `png_image`，目标 `PNG_FORMAT_RGB`（**丢 alpha**，省 1/4 内存，
  1780–1782），随后与 JPEG 共用同一段盒式降采样（1760–1761）。
- **输出格式**：RGB565 **little-endian**、16 字节对齐（h:77/h:702），可直接给
  `lv_image_dsc_t{cf=LV_COLOR_FORMAT_RGB565, stride=w*2}`（app_online 313–319 就是这么用的）。
  **没有做 `LV_COLOR_16_SWAP`** —— 交换由 LVGL 显示配置负责（本文件不参与）。
- **`max_dim` 语义**：目标**长边不超过 max_dim**；`pick_div`(553–560) 选 1/2^n 缩放，
  然后 `image_to_rgb565` 做盒式平均。**不使用** `esp_new_jpeg` 自带的 scale
  （1753–1756 注释：它要求 8 的倍数且最大 1/8，小图会直接解码失败）。
- **可重入性**：JPEG 句柄全部局部（`jpeg_dec_open/close` 成对）；PNG 的 `png_image` 也是栈上结构体
  + 局部缓冲 → **两个路径都可重入**；libpng 的 `png_image` 简化 API 不共享全局状态（本文件未用
  逐行 API 的 read 回调版本）。

### C. host 单测可行性评估

**结论：本文件是全工程最适合 host 单测的模块之一**（占 2104 行里的约 1100 行可脱离硬件）。

| 需要抽掉的依赖 | 处理方式 |
| --- | --- |
| `esp_err_t` / `ESP_ERR_*` | 内部模块改用 `int` 返回码（对外仍映射成 `esp_err_t`） |
| `esp_log.h` | 提供 `#ifdef HOST_TEST` 的空宏 |
| `FILE*` / `fseek` | 改成"内存缓冲 + 偏移"的 `*_mem()` 变体（§6.3 原型） |
| `heap_caps_aligned_alloc` / `jpeg_*_align` | host 侧映射到 `aligned_alloc(16)` / `free` |
| `esp_jpeg_dec` / `libpng` | **不参与纯逻辑单测**；`image_to_rgb565` 只在集成测试里跑 |
| `gbk_table.c` | 直接一起编译（纯常量表） |

**建议的单测用例（≥10）**

1. LRC：一行多标签 `[00:01.00][00:02.50]词` → 两行；乱序输入 → 排序正确。
2. LRC：无任何时间标签 → 逐行收下且 `timed=false`；`[ti:][ar:]` 被忽略。
3. LRC：`[00:60.999]`、`[1:2:3]`、缺 `]`、空行 —— 不崩、不越界。
4. LRC：400 行上限（第 401 行被丢弃）、含 UTF-8 中文的 160 B 截断不切半字符。
5. `media_meta_lrc_index_at`：pos 早于首行 → −1；正好等于某行时间 → 该行；末行之后 → 末行。
6. UTF-8 校验：过长编码（`C0 80`）、代理区（`ED A0 80`）、超 `10FFFF` → `is_valid_utf8=false`。
7. GBK 转换：`0xC4 0xE3`（"你"）→ `E4 BD A0`；非法尾字节 → `?` 且不吞后续。
8. UTF-16LE/BE（带 BOM）→ UTF-8；UTF-8 BOM 被跳过。
9. MP3 帧头：MPEG1 Layer III 128 kbps/44.1 kHz、MPEG2 Layer III、Layer I（应被 `probe_mp3` 跳过）、
   `br_idx=0/15`、`sr_idx=3` —— 全部走 `mp3_parse_header` 真值表。
10. FLAC：构造 STREAMINFO（44.1 kHz/2ch/24bit/8,596,480 采样）→ `duration_ms=194932`；
    块链含 PICTURE(423169)+PADDING(4096) → 音频偏移 **427430**（与 PC 离线解剖一致，AGENTS.md §11.5.10）。
11. FLAC 异常：块长 = `0xFFFFFF` > 文件大小 → 立刻停走 + 走兜底码率（分支 1122–1163）。
12. WAV：`fmt ` 在 `data` 之后、`data` 长度写 0、奇数字节块对齐。
13. ADTS：sfi=13 拒绝；帧长 ≤7 拒绝；正常帧的码率/时长估算。
14. ID3：APIC（v2.3/v2.4/v2.2）偏移与长度字段 → 图片字节正确；unsync 标志 → `NOT_SUPPORTED`。
15. ID3：USLT（UTF-16）→ UTF-8 文本；只有 SYLT → 转成 LRC 文本并能被 LRC 解析器吃下。
16. `media_meta_audio_data_offset`：三个带不同 ID3v2 大小的 MP3 → 偏移精确。

---

## 9. 拆分收益小结

- 2104 行 → 8 个文件（60~420 行），把 `image_to_rgb565`(186)、`probe_flac`(150)、
  `lrc_parse_utf8`(105)、`id3_cover`(98)、`media_meta_probe_mp3`(94) 五个大函数各自独立成域。
- **~1100 行纯逻辑**（LRC/编码/字节序/帧头/ID3/块链）可脱离 ESP-IDF 与解码器编译，
  直接对接 host 单测 —— 这是本文件最划算的重构收益（解析类 bug 全靠真机日志定位的历史可以结束）。
- 解码（`mm_image.cc`）与容器解析（`mm_probe.cc`/`mm_id3.cc`）分离后，
  "解不出来"和"读不出来"能一眼区分，避免再出现 AGENTS.md §11.5.8 那种"码率 0 / 时长 0 /
  解不出帧"三类问题混在一起的排查困境。
