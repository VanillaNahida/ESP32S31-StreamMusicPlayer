# 图库 App（SD 卡图片浏览）与播放器页背景图（2026-10）

## 1. 需求（用户原话）

> 1. `assets/player_background.jpg` 作为播放器背景图
> 2. 添加图片浏览功能，主界面添加"图库"APP，点开后可浏览存储卡的常见图片格式
>    （jpg,jpeg,png,gif），支持双指放大缩小双击放大/还原，支持左右滑动切换图片，
>    支持右上角信息图标查看图片详细信息（分辨率，大小，格式，修改日期）
>    图库的主界面图标为 `assets/icons/gallery.png`

## 2. 文件与职责

| 文件 | 职责 |
| --- | --- |
| `components/apps/app_gallery/gallery_app.cc` | 页面骨架（标题 / ⓘ / 查看器区域）、当前索引、50 ms 轮询把解码结果贴上去、注册到桌面 |
| `components/apps/app_gallery/gallery_scan.cc` | 递归扫 `/sdcard`（深度 3、最多 600 张），带 `stat` 的大小与修改时间；**文件头解析**原始分辨率 |
| `components/apps/app_gallery/gallery_decode.cc` | 解码工作线程（core 0）：静图 → RGB565（PSRAM），GIF → 原始字节 |
| `components/apps/app_gallery/gallery_view.cc` | 查看器：缩放/平移/双击/捏合/左右滑/ⓘ 弹窗 |
| `components/apps/app_music/music_player_bg.cc` | 播放器页背景图（与首页壁纸同一套做法） |
| `components/services/media_meta/mm_image.cc` | `mm_photo_file_rgb565()`：大图解码 —— JPEG 走**硬件**、PNG 走**逐行流式**（见 §5.1 / §5.2） |
| `components/services/storage_service` | `fs_entry_t.is_image` + `storage_service_is_image()`：文件管理页据此给图片行画图库小图标 |
| `components/apps/app_music/music_view_local.cc` | 文件管理里图片行用 `gallery_file` 小图标，点击走 `gallery_open_path()`（见 §11） |

## 2.5 扫描为什么快（2026-10 优化：从几秒降到几十毫秒）

**根因：对每个目录项都调了一次 `stat()`。**

FATFS 的 `stat`/`f_stat` 是"从目录头开始线性查名字"（ff.c 的 `dir_find`），
所以它对**一次** stat 就要把整个目录再扫一遍 —— 对 N 个条目各 stat 一次就是
**O(N²)** 次目录扇区读。300 个文件的目录 = 300 × (300/16) ≈ 5600 次扇区读，
SD 上轻轻松松 3~6 秒，再往下递归两层就是十几秒 —— 这就是"进入图库后扫描时间过长"。

现在（`gallery_scan.cc`）：

| 做法 | 说明 |
| --- | --- |
| 类型用 `readdir` 的 **`d_type`** | VFS 的 `vfs_fat_readdir_r` 从 `f_readdir` 的 FILINFO 里填（`AM_DIR`），**不额外读卡**；只有 `DT_UNKNOWN` 才退回一次 stat 兜底 |
| 大小/修改时间**不在扫描阶段取** | 交给解码工作线程：它本来就要打开那个文件读文件头，顺手 `stat` 一次（O(目录长度)），随解码结果一起交回 UI（`gallery_view_note_file_stat()`） |
| 非图片文件连路径都不拼 | 省 snprintf |
| 条目数组显式放 PSRAM | 600 × ≈780 B ≈ 468 KB |

整趟扫描于是变成"每个目录顺序读一遍"= **O(N)**。
日志里能量出来：`gallery: scan /sdcard: N image(s) in M ms`。

另外 UI 侧把等待做成**有进度**的：扫描期间提示行显示
`正在扫描存储卡… 已找到 N 张`（`gallery_scan_progress()`，只在数字变化时改标签）——
原来是一动不动的一行字，扫大卡时体感像卡死。

> ⚠ 同样的 O(N²) 模式在 `storage_service_browse()` 里也有（它靠 stat 拿文件大小，
> 文件管理页要显示大小，所以不能直接删）。要优化它得换个思路：目录用 `d_type`
> 免 stat（目录不需要大小），文件仍 stat —— 能把"目录多"的场景砍一半。

## 3. 线程与数据流

```
LVGL 任务                          工作线程（core 0）
  gallery_page_build()
   └─ scan_start() ──────────────► scan_task: gallery_scan(/sdcard)
  poll_cb（50 ms）                   └─ s_items / s_count，置 s_scan_done
   ├─ 扫完 → show_index(0)
   │    └─ gallery_decode_request(i) ──► decode_task:
   │                                     ├─ 文件头 → 原始分辨率
   │                                     └─ 解码（静图 RGB565 / GIF 字节）
   └─ gallery_decode_take(i) ◄──────── s_done_*（只保留**最新**一次请求）
        └─ gallery_view_set_still()/set_gif()
```

纪律（与全工程一致）：
- **SD 扫描与解码都不在 LVGL 任务里**。600 张图的目录扫一遍要几百毫秒，一张
  4000x3000 的 JPEG 软解也要几百毫秒，放 LVGL 里就是整屏卡住。
- 只保留最新一次解码请求：快速翻页时旧的那张解完直接丢掉（`take()` 里比对 seq）。
- 像素产权明确：`take()` 之后归 view，换图时由 view 释放（`media_meta_cover_free`）。

## 4. 手势

| 手势 | 实现 |
| --- | --- |
| 双指缩放 | `LV_EVENT_GESTURE` + `lv_event_get_gesture_type() == LV_INDEV_GESTURE_PINCH`，缩放 = `pinch_base × lv_event_get_pinch_scale()` |
| 双击放大/还原 | LVGL 没有内置双击：自己判两次 `PRESSED` 间隔 < 350 ms 且落点相距 < 40 px，在"适应屏幕"与 2× 之间切换 |
| 单指拖动平移 | 放大后（`zoom > fit`）用 `lv_indev_get_vect()` 移动，位移按图片与容器大小差夹紧 |
| 左右滑翻页 | 未放大时才生效（`lv_indev_get_gesture_dir()`）——放大后单指是平移，否则拖过头就换图 |

缩放用 `lv_image_set_scale(obj, 256)`（256 = 100%），**不用 transform_scale**：
后者是绘制期变换、要额外一层 ARGB8888，2000 px 的图每帧重算会卡死；
`lv_image` 自己的 scale 是绘制时按比例取样（GIF 也一样，它是 `lv_image` 子类）。

### 4.1 为此打开的三个 LVGL 选项（sdkconfig + sdkconfig.defaults）

```
CONFIG_LV_USE_GESTURE_RECOGNITION=y   # PINCH 识别器
CONFIG_LV_USE_FLOAT=y                 # 上面的 depends on；S31 有硬件 FPU（SOC_CPU_HAS_FPU=1）
CONFIG_LV_USE_GIF=y                   # lv_gif，按**内存源**播放（不走 LVGL 的 FS 驱动）
```

⚠ **`LV_USE_FLOAT=y` 的副作用**：`lv_value_precise_t` 从 `int32_t` 变成 **`float`**，
于是 C++ 里用 int 花括号初始化 `lv_point_precise_t` 会命中 `-Werror=narrowing`。
本次只影响 `components/ui_core/ui_ba.cc` 的 6 处（已显式转 `lv_value_precise_t`）；
以后新写这类初始化要记得加 cast。

## 5. 大图解码：为什么不能整解

`media_meta_image_file_rgb565()` 是"封面"语义：**先按原始尺寸整解**进临时缓冲再盒式
降采样。4000x3000 的照片整解要 **24 MB** RGB565，直接 OOM —— 图库打开手机照片会失败。

所以新增 `media_meta_photo_file_rgb565()`（`mm_image.cc` 的 `mm_photo_file_rgb565`）：

- **JPEG** 用 esp_new_jpeg 的**解码期 scale**（按 1/2^n 直接解到目标尺寸），峰值内存≈输出；
- **PNG** 仍走原路径（PNG 没有解码期缩放；实际使用里 PNG 多是截图，尺寸不会离谱）；
- 文件上限 12 MB（整份读进 PSRAM 才能解）。

图库传 `max_dim = UI_W(800)`：`pick_div()` 会缩到"长边 ≤ 2×max_dim"，
所以 4000x3000 → 1000x750（1.5 MB），1920x1080 → 960x540。放大到 4 倍仍够看。

## 5.1 JPEG：S31 硬件解码（2026-10 换）

S31 有硬件 JPEG 编解码器（`soc_caps.h`: `SOC_JPEG_CODEC_SUPPORTED=1`），驱动是
`esp_driver_jpeg`（`driver/jpeg_decode.h`）。`mm_photo_file_rgb565()` 里先试硬解
（`hw_jpeg_to_rgb565()`），不行再软解（`jpeg_to_rgb565()`，esp_new_jpeg，能在解码期缩放）。

**四条硬约束**（都在代码注释里，改之前先看）：

| 约束 | 处理 |
| --- | --- |
| 输出缓冲必须**缓存行对齐、长度也对齐**，否则 `jpeg_decoder_process()` 直接 `ESP_ERR_INVALID_ARG` | 只能用 `jpeg_alloc_decoder_mem()`（按 MAX(PSRAM 缓存行,16) 对齐并回填实际长度）。它是 `heap_caps_aligned_calloc`，**可以**用 `media_meta_cover_free()` 释放 |
| 4:2:0/4:2:2 的图按 **16 像素**对齐渲染，行跨距可能是 `align16(w)*2` | 解码后按行 `memmove` 压紧（`pw != w` 时） |
| **没有解码期缩放**，只能整张解 | 只用于输出 ≤ **5 MB**（≈2.4 MP，刚好装下 1088x1920 截图）的图；更大的走软解 |
| 缓存同步 | 驱动内部已做（输入 C2M 写回、输出 M2C 失效），我们不用管 |

- **软解那条路上的一个真 bug（2026-10 修）**：`pick_div()` 算出 1/1 时原来把
  `scale.width` 填成**原始宽高**，而 esp_new_jpeg 要求设了的 scale 必须是 8 的倍数 ——
  700 宽的图直接报 `E JPEG_DEC: After scale and clipper, width(700) isn't multiply of 8.`
  整张解不出来。现在 `div == 1` 时填 **0**（= 不缩放）。
- **渐进式 / 算术编码 / CMYK 的 JPEG**：esp_new_jpeg 与 S31 硬解都不支持，真机日志是
  `E JPEG_DEC: Not supported JPEG standard.`。现在先解析 SOF：只要不是 SOF0/SOF1、
  或分量数不是 1/3，就直接 `ESP_ERR_NOT_SUPPORTED`（UI 显示
  "不支持的格式（WebP / HEIF / 渐进式 JPEG）"），并打一行
  `jpeg unsupported: SOF02 3ch 1920x1080 (path)`。
- 引擎句柄 `s_hw_jpeg` **懒创建、一直复用**（只被解码工作线程访问，不需要锁）。
- **红蓝互换**的话把 `MM_HW_JPEG_RGB_ORDER` 从 `BGR` 改成 `RGB`（默认值取自 IDF 官方测试用例）。
- 判定日志：`I media_meta: jpeg 1088x1920 -> hw` / （软解只在 LOGD 打 `-> sw`）。

## 5.2 PNG：逐行解 + 边解边缩（2026-10 换）

原来的 `png_image` 简化 API 必须先把**整张**图按原始尺寸解成 RGB888 再降采样：

| 图 | 峰值内存 | 结果 |
| --- | --- | --- |
| 1080x1920 截图 | 6.2 MB + 4.1 MB ≈ 10 MB | 勉强 |
| 2000x3000 | 18 MB + 12 MB = 30 MB | **ESP_ERR_NO_MEM** |

真机日志里那两条 `decode failed (ESP_ERR_NO_MEM)` 就是这个。现在换成 libpng 的
**逐行**接口（`png_read_row()`，`png_stream_to_rgb565()`）：常驻只有"一行 RGB"+ 输出，
每读满 `div` 行按 `div×div` 块做盒式平均写出一行 RGB565 → **峰值 ≈ 最终输出（约 1 MB）**。
`div` 与 JPEG 的 `pick_div()` 同一口径（长边落到 `[max_dim, 2*max_dim)`）。

- 隔行 PNG：`png_set_interlace_handling()` 返回 passes，只在**最后一遍**写输出。
- 归一化：调色板→RGB、低位灰度→8、tRNS→alpha、16bit→8、灰→RGB、**丢 alpha**。
- 失败（含 libpng `longjmp`）会回落老的 `mm_image_to_rgb565()`；封面/歌词那条路**没动**。
- 已知小瑕疵：最后一行/列不满 `div` 个采样点时用固定除数，最右/最下一列略暗一格（看不出）。

## 5.4 错误提示分原因（2026-10）

真机日志里同一句"这张打不开"背后是四个完全不同的原因，现在分开（`gal_err_t` +
`gallery_decode_last_error()`，GIF 在自己那边直接给具体提示）：

| 现象 | 归因 | 提示 |
| --- | --- | --- |
| `decode failed (ESP_ERR_INVALID_SIZE)` | 文件 > `MM_PHOTO_FILE_MAX`(12 MB)，整份读不进 PSRAM | 文件太大（超过 12 MB） |
| `decode failed (ESP_ERR_NOT_SUPPORTED)` | **扩展名骗人**：`.png` 其实是 **WebP**（抖音分享图）、或 HEIF | 不支持的格式（WebP / HEIF 暂不支持） |
| `decode failed (ESP_ERR_NO_MEM)` | 真的没内存 | 内存不足，这张解不了 |
| `decode failed (ESP_FAIL)` | 数据损坏 / 解码器不认 | 这张打不开（数据可能已损坏） |

⚠ 之前 `.png` 实际是 WebP 时会走到 JPEG 解析器，日志是一句莫名其妙的
`E JPEG_DEC: JPEG segment format error.`。现在 `mm_photo_file_rgb565()` **先嗅
`RIFF....WEBP` / `....ftyp`** 并直接返回 `ESP_ERR_NOT_SUPPORTED`。

## 5.3 GIF：LVGL 的 `MAX_WIDTH` 打了补丁（480 → 1280）

GIF 打不开的根因在 LVGL 自带的 AnimatedGIF：`gif.c` 的 `GIFInit()` 里

```c
if (pGIF->iCanvasWidth > MAX_WIDTH) { pGIF->iError = GIF_TOO_WIDE; return 0; }   // gif.c:249
```

而 `AnimatedGIF.h` 里 `#define MAX_WIDTH 480`（`ucLineBuf[MAX_WIDTH]` 是"一行像素索引"的缓冲，
必须 ≥ 画布宽）。所以**任何宽于 480 的 GIF 都会被判为 too big，直接 load failed** ——
真机上那几张 1.2~1.7 MB 的 GIF 全中。

补丁（`managed_components/lvgl__lvgl/src/libs/gif/AnimatedGIF.h`）：

```c
- #define MAX_WIDTH 480
+ #define MAX_WIDTH 1280
```

⚠⚠ **托管组件更新 / `rm -rf managed_components` 后这一行会被还原成 480**，
届时宽 GIF 又会"load failed"。还原后的表现很好认：日志里
`W gallery: gif too wide: canvas 800x800 > 1280` 不会出现，而是走到
`W gallery: gif load failed (canvas 800x800, ...)` —— 看到这句就该检查这个补丁。

图库侧另外做了**预检**（`gallery_view_set_gif()`）：先自己读逻辑屏幕描述符（第 6..9 字节）
判三件事，给出具体原因而不是 LVGL 那句笼统的 "Couldn't load the source"：

| 情况 | 日志 / 提示 |
| --- | --- |
| 头不对 | `gif: bad header (N bytes)` → "GIF 文件损坏" |
| 画布宽 > 1280 | `gif too wide: canvas WxH > 1280 (AnimatedGIF MAX_WIDTH)` → "GIF 太宽（宽度上限 1280）" |
| 画布像素 > `UI_W*UI_H*6`（≈2.3 MP，draw_buf 4.6 MB） | `gif too large: canvas WxH (N KB RGB565)` → "GIF 太大" |
| lv_gif 仍失败 | `gif load failed (canvas WxH, N bytes)` |

## 5.5 全屏查看与翻页交互（2026-10 用户改版）

### 单击 = 切换全屏

用户原话："图库界面应单击隐藏状态栏等其他组件全屏查看，再次单击恢复"。

- 单击收起：**状态栏 + 页面标题 + ⓘ + 底部文件名条 + 全局返回钮**，查看器从
  "标题栏以下"铺满整块屏；再单击全部还原。
- 左右箭头**保留**（那是唯一的翻页入口）。
- 铺满的做法是**临时把 screen 的 padding 清零**（`pad_all/pad_top = 0`）+ body 改成
  `UI_W x UI_H`、`ALIGN_TOP_LEFT`；**不是**给 body 算绝对坐标 —— body 平时是
  "100% 宽 + BOTTOM_MID"，它的 x 其实等于 `pad_left`、宽等于 `UI_W-16`，
  硬算只会错一个 padding，而且 align 锚点还在，父容器一改尺寸就又飞回去。
- ⚠⚠ **谁隐藏谁负责恢复**：状态栏与全局返回钮是**全系统共用**的，
  `on_scr_deleted` 里必须无条件 `ui_status_bar_set_visible(true)` +
  `ui_back_button_suppress(false)`，否则离开图库后整个系统就少一条状态栏、少一个返回键。
- **单击判定为什么要延迟 400 ms**：LVGL 的 `SHORT_CLICKED` 与"双击放大"共用同一串
  触摸事件。做法是 SHORT_CLICKED 只**记时间戳**，由 app 的 50 ms 轮询
  （`gallery_view_take_pending_tap()`）在**超过双击窗口（350 ms）**后才真正切全屏；
  双击在 `on_pressed` 里把时间戳清掉。所以 `GAL_TAP_DELAY_MS` 必须 > 350，
  否则双击放大的第一下会先把全屏切了。
- 新增的 ui_core 接口：`ui_status_bar_set_visible(bool)`、`ui_back_button_suppress(bool)`
  （后者只是置抑制位，返回钮的可见性仍由 `ui_back_button_update()` 统一算）。

### 左右滑 → 左右箭头

用户原话："左右滑动手势会和双指缩放冲突，建议改为界面最左边和最右边的箭头翻页"。

- `on_gesture` 里的 `lv_indev_get_gesture_dir()` 翻页**整段删掉**；双指捏合、放大后
  拖动、双击放大/还原全部保留。
- 箭头是 `gallery_view_create()` 里建的两个半透明圆钮（44 px，`LV_SYMBOL_LEFT/RIGHT`），
  拴在 stage 上、`LEFT_MID`/`RIGHT_MID`；只有 1 张图时整体隐藏
  （`gallery_view_set_pager_visible()`）。
- 点击走 `gal_page_cb_t`（±1），app 侧 `on_page()` 负责首尾循环。

## 6. ⓘ 详细信息里的"分辨率"是**原始**的

解码会折半，拿解码后的尺寸当分辨率是错的。`gallery_scan.cc` 的
`gallery_image_size()` 只读文件头拿原始尺寸：

| 容器 | 位置 |
| --- | --- |
| JPEG | 找 `SOF0/1/2` 段（跳过 DHT/JPG/DAC），height@+5、width@+7，大端 |
| PNG | 固定第 16 字节：width(4) height(4)，大端 |
| GIF | 逻辑屏幕描述符：第 6..9 字节，小端 |

解码结果与原始不同时，弹窗显示 `4000 x 3000（解码 1000 x 750）`。

## 7. 修改日期

来自 `stat(path)` 的 `st_mtime`（FATFS 的 `vfs_fat_stat` 会把目录项里的 fdate/ftime
换算成 unix 秒）。
⚠ 必须用 **stat(路径)**，不能用 `fstat(fd)` —— `vfs_fat_fstat` 把 `st_mtime` 恒写 0。
⚠ 卡上文件的修改时间是**写卡那台机器**给的，本地没校时也不影响读取（只是显示的时间本身可能不准）。
mtime 为 0 时弹窗显示"未知（FAT 未记录）"。

## 8. 播放器页背景图

`assets/player_background.jpg` 用 `target_add_binary_data` 嵌进 `app_music`
（符号 `_binary_player_bg_jpg_start/_end`），开机时后台任务（core 0，prio 2）
解成 RGB565（约 770 KB PSRAM）并压在 `screen` 的 `bg_image` 上，
`recolor` 黑色 + `opa 120` 压暗一层（否则右侧歌词/标题看不清）。

播放器页的 screen 是**每次进页面新建**的 → 页面建好后再 `music_player_bg_attach(scr)` 贴一次；
screen 的 `LV_EVENT_DELETE` 里把指针清空，避免解码任务往已释放的 screen 上写。

## 9. 踩过的坑（都已在代码里注释）

1. **页面 DELETE 里不能删子对象**：LVGL 删对象是"先删子、再给父发 DELETE"，
   所以在 screen 的 DELETE 回调里 `lv_obj_delete(s_img)` 是 use-after-free。
   `gallery_view_destroy()` 只释放自己 malloc 的像素/GIF/描述符，指针清空。
2. **载入中也要能左右滑**：手势处理器一开始要求"已经有图"，结果第一张解失败时
   用户被卡死在那一页（连翻页都不响应）。现在 PINCH 分支才要求有图。
3. **`-Werror=narrowing` / `-Werror=comment` / `-Werror=format-truncation`**：
   注释里写 `*px/*w` 会被当成嵌套注释；`GAL_NAME_MAX` 必须放到 FATFS 上限 256
   （否则 `snprintf("%s", d_name)` 触发截断告警）。

## 11. 从「文件管理」打开图片（2026-10）

需求：**支持在主界面的文件管理里打开图片文件查看**，图片文件的小图标是
`assets/icons/gallery.svg`。

### 11.1 列表

`storage_service_browse()` 本来就**列出目录里的全部条目**（不过滤类型，只给每条打
`is_dir` / `is_audio` 标记 —— ⚠ 以后别在这里加"只留音频"的过滤，否则图片就看不见了）。
本次给它补了一个标记：

```c
bool storage_service_is_image(const char *name);   // jpg/jpeg/png/gif
bool is_image;                                     // fs_entry_t 新增字段
```

`music_view_local.cc` 的行工厂多了 `is_image` 参数：

- 图标底色：目录=琥珀 / 音乐=蓝 / **图片=紫（0x6C5CE7，与图库 App 图标同色）** / 其它=灰；
- 图标本体：其它行是 16 px 字形（`LV_SYMBOL_*`），**图片行换成烘好的位图
  `gallery_file`（24×24，来自 `assets/icons/gallery.svg`）** —— LVGL 内置 symbol 里
  没有"图片"这个字形，用 `LV_SYMBOL_FILE` 顶会让图片和普通文件长一样；
- 点击：图片行 → `gallery_open_path(全路径)`（压栈打开图库并**定位到这张**），
  不再是"不是可播放的音频文件"的 Toast。

### 11.2 图库侧：定位到某张

`gallery_open_path(path)` 只把路径存进 `s_pending_open` 再 `ui_router_push()` ——
因为扫卡是**异步**的，打开那一刻列表可能还不存在。列表就绪后
`resolve_pending_open()` 把路径换成下标；找不到（被删 / 在扫描深度之外）退回第 0 张。
返回是压栈返回 → 回到原目录、页码不变。

### 11.3 顺带修掉的一个真 bug：第二次进图库永远卡在"正在扫描…"

`gallery_page_build()` 原来无条件 `s_list_ready = false`，而 `scan_start(false)` 看到
`s_items` 非空就提前返回 —— 于是"扫完"那个状态永远等不到，poll 里
`if (!s_list_ready …) return;` 一路 return。现在列表已就绪时直接
`s_list_ready = true;` + `show_index()`。

## 10. 已知限制 / 后续

- **换卡不会自动重扫**：同一页面反复进出复用上次的列表。要支持热插拔重扫，照
  `storage_service_sd_generation()` 的做法加"代次比对"即可（播放器那边的曲库就是这么做的）。
- **GIF 整份读进 PSRAM**（上限 6 MB），且逐帧解码跑在 LVGL 任务里 —— 超大 GIF 会掉帧。
- 单次最多列 600 张（超出按文件名序截断）。
- **没有"预取下一张"**：左右滑到下一张时仍要等一次解码（大图 100~300 ms，期间显示"载入中…"）。
  要更顺可以加一级预取（在解码线程空闲时提前解 next/prev）。

- 没有缩略图网格：进去就是大图浏览，左右滑翻页。
