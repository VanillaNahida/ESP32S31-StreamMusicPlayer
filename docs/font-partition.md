# 字体分区（fonts）与 mmap 加载

> 2026-11 起，UI 的五个预渲染位图字体不再编进 app，改为放在独立的
> `fonts` 分区（`partitions.csv`），运行时用 `esp_partition_mmap()`
> 直接映射使用。本文说明为什么这么做、镜像长什么样、怎么改字体。

## 1. 为什么把字体搬出 app

字体是**只读、按字形随机访问**的数据，却占了 app 里一大块常量：

| 字体 | 字符集 | 字号/bpp | 位图 | 段大小 |
| --- | --- | --- | ---: | ---: |
| `sc16` | GB2312 + ASCII + LVGL 图标 | 16 px / 4 bpp | 901,026 B | 1,036,530 B |
| `sc22` | GB2312 第一级 + 符号 + ASCII | 22 px / 4 bpp | 963,848 B | 1,044,632 B |
| `jp1` | JIS 第 1 水準 + 日文记号 | 16 px / 4 bpp | 3,331 B | 4,243 B |
| `jp2` | JIS 第 2 水準 | 16 px / 2 bpp | 436 B | 628 B |
| `dict16` | 词典生僻字 + `extra_chars.txt` | 16 px / 2 bpp | 606,309 B | 779,925 B |
| **合计** | | | 2,474,950 B | **2,866,325 B** |

以前它们以 `const uint8_t glyph_bitmap[]` 的形式编进 app，app 槽 12.31 MB
被挤到只剩几十 KB。搬到 `fonts` 分区后：

- **app 不再含字形数据**：固件从 ~12.83 MB 降到 **9.31 MB**（9,759,488 B），
  app 槽 9.56 MB 仍留 ~261 KB（2.7%）；
- **不复制到 RAM**：`glyph_bitmap` / `glyph_dsc` 指针直接指向 flash
  映射区，只额外 malloc 约 3.5 KB 的 cmap 表；
- **换字体只重烧 fonts 分区**，app 不用重编。

> 为什么不用 LVGL 自带的 `lv_binfont_create_from_buffer()`？
> 那个加载器会把整个 .bin **解析并复制到堆**（`glyph_bitmap`、`glyph_dsc`、
> `cmaps` 全 malloc），花 ~2.7 MB PSRAM 和几百毫秒，与“mmap 直接用”相反。
> 本工程自己接 `lv_font_fmt_txt_dsc_t` 的指针，见第 5 节。

## 2. 分区布局

`partitions.csv`（16 MB flash）：

```
nvs        0x9000    0x6000
otadata    0xf000    0x2000
phy_init   0x11000   0x1000
coredump   0x12000   0x10000
dict       0x22000   0x38E000   拼音词库（3.72 MB）
fonts      0x3B0000  0x2C0000   UI 字体镜像（2.75 MB）
mainapp    0x670000  0x990000   app（9.56 MB）
```

- `fonts` 是 **data 类型、子类型 0x41** 的自定义分区，内容是裸 blob，
  没有文件系统，不需要挂载；
- app 起点 `0x670000` 是 64 KB 对齐；`0x3B0000 + 0x2C0000 = 0x670000`，
  `0x670000 + 0x990000 = 0x1000000`，无空洞；
- fonts 镜像 2,866,325 B，分区 2,883,584 B，余 **17,259 B**（只够再加
  ~200 个 2bpp 生僻字，扩字符集前先算账）；
- app 镜像 9,759,488 B，分区 10,027,008 B，余 **267,520 B**（2.7%）。

改 `partitions.csv` 后必须 `idf.py fullclean` 重编并重烧分区表。

## 3. 数据流

```
assets/fonts/*.ttf
   │  tools/font/gen_subset.py      切字符集
   ▼
DreamHanSansSC-W17-{subset,jp1,jp2,dict,lv1}.ttf
   │  tools/font/gen_lvgl_font.py   调 lv_font_conv --format lvgl
   ▼
components/ui_font/ui_font_{sc16,sc22,jp1_16,jp2_16,dict16}.c   ← 生成物，保留作打包输入
   │  tools/font/pack_fonts.py      抽数组 + 按 LVGL 结构重排
   ▼
build/fonts.bin                      ← 构建时自动生成（不签入仓库）
   │  esptool_py_flash_to_partition(flash "fonts" ...)
   ▼
fonts 分区（0x3B0000）
   │  esp_partition_mmap(ESP_PARTITION_MMAP_DATA)
   ▼
ui_font.c 接 lv_font_fmt_txt_dsc_t / lv_font_fmt_txt_cmap_t
   ▼
lv_font_text / lv_font_title22（fallback 链）
```

**生成**（改了字符集/字号/bpp 时）：

```powershell
tools\font\venv\Scripts\python.exe tools\font\gen_subset.py
python tools\font\gen_lvgl_font.py
python tools\font\pack_fonts.py --verify
```

日常改完 .c 直接 `idf.py build` 即可：CMake 自定义命令发现 .c/脚本/sdkconfig
有变化会重打 `build/fonts.bin`，并作为 `flash` 目标的一部分写到 `fonts` 分区。

> `pack_fonts.py` 只用标准库，不需要 `tools/font/venv`；`gen_subset.py`
> 需要 fontTools，必须用 venv。

## 4. 镜像格式（UIFP v1）

所有整数小端。偏移都相对各自段的起点，段起点 16 字节对齐，因此映射后
`lv_font_fmt_txt_glyph_dsc_t` 在 RISC-V 上不会非对齐访问。

### 4.1 头（20 B）

| 偏移 | 类型 | 字段 | 说明 |
| ---: | --- | --- | --- |
| 0 | `char[4]` | `magic` | `"UIFP"` |
| 4 | `u16` | `version` | 1 |
| 6 | `u16` | `font_count` | 5 |
| 8 | `u32` | `blob_size` | 整个镜像字节数 |
| 12 | `u32` | `payload_crc` | 从第 20 字节到 blob_size 的 CRC32（IEEE，同 `zlib.crc32`） |
| 16 | `u32` | `glyph_dsc_size` | 8（LARGE=n）或 16（LARGE=y） |

### 4.2 目录项（60 B × 5）

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `char[16]` | `name`（`"sc16"` 等，NUL 补齐） |
| 16 | `u32` | `offset`（相对 blob，16 对齐） |
| 20 | `u32` | `size` |
| 24 | `u32` | `glyph_count` |
| 28 | `u32` | `bitmap_size` |
| 32 | `u32` | `cmap_num` |
| 36 | `i32` | `line_height` |
| 40 | `i32` | `base_line` |
| 44 | `i32` | `underline_position` |
| 48 | `i32` | `underline_thickness` |
| 52 | `u32` | `bpp` |
| 56 | `u32` | `subpx` |

### 4.3 字体段

段头 16 B：`cmaps_off`、`glyph_dsc_off`、`bitmap_off`、保留。

- `cmaps_off` → `ui_font_cmap_pack_t[cmap_num]`（20 B/项）：
  `range_start`、`unicode_list_off`、`glyph_id_ofs_off`、`range_length`、
  `glyph_id_start`、`list_length`、`type`、pad；
- `glyph_dsc_off` → 原样的 `lv_font_fmt_txt_glyph_dsc_t[glyph_count]`
  （布局由 sdkconfig 的 `CONFIG_LV_FONT_FMT_TXT_LARGE` 决定）；
- `bitmap_off` → 原样的 `glyph_bitmap[]`（逐字形连续打包，无行对齐）；
- `unicode_list` / `glyph_id_ofs_list` 放在 cmap 表和 glyph_dsc 之后，
  2 字节对齐。

`glyph_dsc` 里的 `bitmap_index` 就是位图区内的字节偏移，**不需要重算**。
打包时会校验每个字形的跨度 `ceil(w*h*bpp/8)` 与下一个 `bitmap_index`
严格衔接 —— 解析错了会当场报错，不会等到真机花屏。

## 5. 运行期加载（`components/ui_font/ui_font.c`）

`ui_font_init()`（`ui_core_init()` 第一件事）依次做：

1. `esp_partition_find_first(DATA, ANY, "fonts")`；
2. `esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, ...)`
   映射整个分区（映射区永远不释放，字体要活到关机）；
3. 校验 magic/version/`glyph_dsc_size`/blob_size/CRC；
4. 对每个字体：
   - malloc 一个 `lv_font_fmt_txt_dsc_t` 和 `cmap_num` 个
     `lv_font_fmt_txt_cmap_t`（内部 RAM，共约 3.5 KB）；
   - `dsc->glyph_bitmap` / `dsc->glyph_dsc` 指向映射区；
   - cmap 的 `unicode_list` / `glyph_id_ofs_list` 指向映射区；
   - 复制 `line_height` / `base_line` / `bpp` / `subpx` 到 `lv_font_t`，
     函数指针用 LVGL 的 `lv_font_get_glyph_dsc_fmt_txt` /
     `lv_font_get_bitmap_fmt_txt`；
5. 串 fallback 链：

   ```
   sc16(常用中文,4bpp) → jp1(常用日文,4bpp) → jp2(生僻日文,2bpp)
       → dict16(生僻字+补充表,2bpp)

   sc22(22px 标题) → sc16 链
   ```

6. 自检一次 `lv_font_get_glyph_dsc("中")`，失败只告警。

任何一步失败都会 `ESP_LOGE` 原因并退回 `lv_font_get_default()`
（中文变方框但不会崩），串口提示重烧 fonts 分区。

### 为什么 `glyph_dsc` 必须和 `LV_FONT_FMT_TXT_LARGE` 一致

`lv_font_fmt_txt_glyph_dsc_t` 有两种布局：

- `LARGE=y`（本工程）：16 B，`bitmap_index`/`adv_w` 32 位，box/ofs 16 位；
- `LARGE=n`：8 B，20 位 `bitmap_index` + 12 位 `adv_w` + 8 位 box/ofs。

字体 `sc16` / `sc22` 的位图都超过 1 MB（20 位上限），所以本工程用 LARGE。
镜像头里记了 `glyph_dsc_size`，固件启动时和 `sizeof()` 比对，不一致直接
拒绝加载 —— 避免按错误跨度读成乱码。改这个开关后必须重跑
`pack_fonts.py`（它会读 sdkconfig 自动选布局）。

## 6. 烧录

`idf.py flash` 会自动写：

- bootloader / 分区表 / otadata；
- app（`mainapp`）；
- `fonts` 分区（`build/fonts.bin`，由 `esptool_py_flash_to_partition`
  注册）。

`dict` 分区**仍需单独烧**（词库 3.71 MB，见 `lvgl_pinyin_ime` 的说明）。

手动只烧字体：

```powershell
python tools\font\pack_fonts.py --verify
esptool.py --chip esp32s31 -p COMx -b 921600 write_flash 0x3B0000 build\fonts.bin
```

## 7. 常见故障

| 串口日志 | 原因 | 处理 |
| --- | --- | --- |
| `分区表里没有 fonts 分区` | 分区表是旧的 | `idf.py fullclean` + 重烧分区表 |
| `没有 UIFP 字体镜像` | fonts 分区没烧/烧的是旧数据 | `idf.py flash` |
| `CRC 校验失败` | 烧录不完整 / 镜像与分区不匹配 | 重打 `pack_fonts.py` 并重烧 |
| `glyph_dsc 布局不匹配` | blob 与 sdkconfig 的 LARGE 不一致 | 重跑 `pack_fonts.py`（自动读 sdkconfig） |
| 中文全是方框 | 加载失败退回默认字体 | 看上面的 `E ui_font` 日志 |

## 8. 体积与后续

- `fonts` 分区只剩 ~17 KB：再加字前先算
  `ceil(w*h*bpp/8) + 16 B`/字（16 px 4bpp 约 128 B，2bpp 约 64 B）。
- 22 px 字体目前 4 bpp（1.04 MB）；如果以后要腾 500 KB，可降到 2 bpp，
  代价是标题抗锯齿变粗（16 px 主字体不要动）。
- 未压缩明文位图是刻意选择：LVGL 的 `LV_FONT_FMT_TXT_PLAIN` 路径能把
  字形直接展开到绘制缓冲；开 `LV_USE_FONT_COMPRESSED` 会在每次渲染时解压，
  本工程不划算。
