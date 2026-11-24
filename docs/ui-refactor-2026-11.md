# UI 重构（2026-11）—— 设计稿落地说明

本文件记录 **assets/UIDesign/** 那四张图是怎么落到代码里的：改了哪些文件、坐标从哪来、
哪些地方因为硬件/字体限制做不到 1:1、以及以后改版要动哪里。

> 设计稿是 800x480 的 SVG；本板屏就是 800x480，所以下面的参考坐标在真机上等于像素。
> 代码里一律写设计稿的参考值再经 `UI_PX_W()/UI_PX_H()` 换算（AGENTS §6：不许出现 800/480）。
>
> ⚠ **2026-11-24 更新**：五个 UI 位图字体已搬到独立的 `fonts` 分区（mmap），
> app 槽从 12.31 MB 缩到 **9.56 MB**。本文里出现的 12.25/12.31 MB、"app 余量 ~80 KB"
> 都是当时的数字，现状见 `partitions.csv` 与 [font-partition.md](font-partition.md)。

---

## 1. 播放器主界面（播放器UI重构参考.svg）

| 设计稿元素 | 坐标 / 尺寸 | 代码位置 |
| --- | --- | --- |
| 封面卡片 | (64,76) 210x210，圆角 18，投影 dy=12 stdDev=14 | `build_cover_view()` |
| 封面圆角 | `clip_corner` 裁子对象（真封面与内置默认封面同一半径） | `build_cover_view()` |
| 默认封面 | 无专辑封面时贴内置黑胶（`assets/default_black_vinyl_record.png`） | `player_default_cover_dsc()` |
| 渠道胶囊 | 封面左下角 (12, 12) 内缩，很小、半透明、按平台上色 | `build_cover_view()` 的 `badge` |
| 歌名 / 歌手 | (64,306) / (64,336)；歌名过长横向跑马灯，不换行 | `build()` 的 `title`/`artist` |
| 歌词列 | x=336 起，宽 400，垂直中心 202 | `build_lyric_view()` |
| 进度条 | x=64..736，y=384，高 5，圆点 r=6（拖动跟手） | `build_seek()` |
| 时间 | (64,397) / 右端 736 | `build_seek()` 的 `times` |
| 控制排 | 图标中心 **y=422**（原 434，上移 12）；x = 141/235/325/400/472/561/641/688 | `build_controls()` |

要点：

- **背景图保留**（`assets/player_background.jpg`，`music_player_bg.cc` 贴）。
- 控制排不再有按钮底色，只有图标；中央播放键是 48 px 半透明白圆（#FFFFFF 16%）。
- 进度条/播放键是**白色**（设计稿），封面取色的"主题色"只留在频谱柱与各种转圈上 ——
  见 `music_view_player_accent_apply()` 的注释。
- **封面上不放任何文字**（2026-11 用户）：原来那条"歌名 + 来源"的底部信息条整条删掉，
  只在封面左下角留一个**很小的半透明圆角胶囊**显示渠道（本地播放 / QQ音乐 / 网易云），
  底色按平台品牌色（`MUSIC_SRC_COLOR_*`）。点胶囊可以隐藏它，状态记 NVS，
  在「播放器设置 → 音源标签」里可以重新打开。
- 歌词：左对齐 + 按距离淡出（100% / 45% / 30% / 18%），当前行换成大一号字体。

⚠ 一处"设计稿做不到 1:1"的地方，改之前先读：

**返回钮**：全局返回钮仍是那颗 64 px 蓝圆（用户要求"不变"），圆心 (56,72)、半径 32，
会压住封面左上角一小块。这是"按钮不动 + 封面照抄设计稿"的唯一交叠，属预期。

**字号**（2026-11 已解决）：当时中文只有 16 px 位图字体，22 px 一档只能给纯 ASCII 用。
现在项目自带了一份 **22 px 中文位图字体** `ui_font_sc22`（GB2312 第一级 + 符号 + ASCII，
4 bpp，实测 1.02 MB），`ui_metrics.h` 的 `UI_FONT_TITLE_22` 就是它：

- 第一级之外的生僻字**自动 fallback 到 16 px 主字体**（同一个字、小一号），不会缺字；
- 体积来自分区调整：删掉一直没人 mount 的 `usr` FAT 分区（384 KB）、dict 从 3.85 MB
  收到 3.80 MB，app 槽 11.81 MB → **12.25 MB**（见 `partitions.csv` 的沿革说明）；
- 改分区后必须 `idf.py fullclean` 重编并重烧分区表（dict 偏移没变，词库不用重烧）。

## 2. 歌单曲目详情页（歌单曲目详情页（带左侧封面卡片 + 右侧曲目清单）.svg）

- 版式模块：`ao_view.cc`（第 0/1 页共用，列表 ↔ 详情只换位置，清单对象不变）。
  - 左列 (44,4)：**竖排 flex**——封面 184x184 圆角 18 → 歌单名 → 副标题（页数）→
    播放全部 128x36。歌单名多高，副标题就从它下面接着排（margin 控制间距），
    两者不会再挤在一起。
  - **封面上不写字**：歌单名/页码只出现在封面下方。
  - 右侧清单 (264,0) 496 宽；行高 48（**等高**），序号/歌名/歌手/分割线；
    歌名过长时自己跑马灯，手指划清单时暂停、停 2.4 s 后恢复（`ao_style_marquee_*`）。
  - 歌手列右端内缩 12 px，给右侧滚动条让位（原来滚动条压在歌手名上）。
- **左封面 = 歌单第一首歌的封面**（「我喜欢的音乐」例外：用**最新收藏**那首，
  见 `online_last_liked_song_set`）：`ao_pl_cover.cc`
  - 工作线程 `online_song_cover_data()`（HTTP）+ `media_meta_image_mem_rgb565()` 解码到 PSRAM；
  - 单槽缓存（PSRAM，184x184 RGB565 ≈ 68 KB），换歌单时旧图留着直到新图到达；
  - **闲置 60 s 自动释放**（`AO_PL_COVER_IDLE_MS`）；详情页开着时 `ao_pl_cover_hold(true)`
    不计时，退出详情才开始计时；页面销毁时 `ao_pl_cover_release_all()` 立刻还内存。
- 返回键：详情是"同一页里换内容"，所以 **`ui_back_button_set_handler()`** 接管返回钮
  （`app_online.cc` 的 `online_back_handler`），按返回先回歌单列表，不退出 APP。
- 详情模式下页签收起、宿主上移到 y=88、标题变成"歌单"（`ao_pages_set_detail_mode()`）。

## 3. 在线音乐主界面（在线音乐主界面.svg）

- 背景：**`#132C3F` 单色**（2026-11 用户指定，`AO_C_BG`）+ 右上角径向光晕
  （`ao_style_screen_bg()` / `ao_style_glow()`）。原设计稿的三段斜向渐变已按需求撤掉。
  ⚠ 渐变描述符**必须有静态/堆存储期**（LVGL 样式里存的是指针），见 `ao_style.cc` 文件头。
- 顶栏：标题（UI_HEADER_TEXT_X）；**右上角「+」已按需求移除**（加歌单浮层不再有入口）。
- 页签：**四个**（我的歌单 / 我添加的歌单 / 搜索 / 账号），(54,76) 起，间距 26，
  当前页白 100%、其余白 50%，下面一条 3 px 强调色下划线（宽度 = min(文字宽, 44)）。
- 列表：白 6% 首行高亮块 + 青色圆点 + 7% 分割线（`ao_style_playlist_row()`）。

## 4. 账号页面（账号页面.svg，**新增**）

- 新文件 `ao_page_account.cc`，作为第 3 页。
- 账号信息卡 (54,16) 692x148：头像（渐变圆 + 白色人形）、昵称、状态胶囊、UID/歌单数。
- 操作区 y=188：已登录 = 「切换账号 / 重新登录」+「退出登录（珊瑚红）」；
  未登录 = 一个强调色「扫码登录」主按钮 + 提示行。
- 登录走 `ao_qr_open()`，退出走 `OREQ_LOGOUT`；结果回来由 `ao_result` 调
  `ao_account_rebuild()`（回调里不重建，避免删掉正在派发事件的按钮）。
- 第 0 页不再有"扫码登录"大块占位，只留一句提示 + 去账号页的入口。

---

## 5. 加载文案

统一走 `ao_style_loading_text()`：一组短句轮换（约 6 s 一句，throttle 内不换），
不再写死"载入中…"。文案表在 `ao_style.cc` 顶部。

## 6. 改动文件清单

```
components/ui_core/include/ui_core.h        + ui_back_button_set_handler()
components/ui_core/ui_core.cc               + 返回钮 handler 钩子
components/ui_core/include/ui_metrics.h     + UI_FONT_TITLE_22（22 px 中文大标题）
components/ui_font/ui_font_sc22.c           22 px 中文位图字体（生成物，GB2312 第一级）
components/ui_font/ui_font.{c,h}            ui_font_title22 包装 + fallback → ui_font_text
tools/font/gen_subset.py                    + DreamHanSansSC-W17-lv1.ttf（第一级子集）
tools/font/gen_lvgl_font.py                 SPECS 增加字号字段 + sc22 产物
partitions.csv                              删 usr / dict 3.80 MB / app 槽 12.25 MB
sdkconfig.defaults                          + CONFIG_LV_GRADIENT_MAX_STOPS=4（三段渐变）

components/apps/app_music/music_view_page.cc      整页版式重写（设计稿坐标）
components/apps/app_music/music_view_player.cc    图标键工厂 / 来源行 / 进度圆点 / 主题色
components/apps/app_music/music_view_lyric.cc     左对齐 + 按距离淡出
components/apps/app_music/music_view_output.cc    输出键改用图标键样式
components/apps/app_music/music_view_internal.h   控件表新增字段

components/apps/app_online/src/ao_style.{h,cc}      设计系统（背景/光晕/页签/行/卡片）
components/apps/app_online/src/ao_view.{h,cc}       歌单页两种版式（列表 / 详情）
components/apps/app_online/src/ao_pl_cover.{h,cc}   详情左封面（PSRAM + 闲置释放）
components/apps/app_online/src/ao_page_account.cc   账号页（新增）
components/apps/app_online/src/ao_page_pl.cc        第 1 页重写
components/apps/app_online/src/ao_page_acct.cc      第 0 页重写
components/apps/app_online/src/ao_pages.{h,cc}      四页骨架 / 下划线 / 详情模式
components/apps/app_online/src/app_online.cc        屏幕版式 + 返回键接管 + 第 3 页
components/apps/app_online/src/ao_result.cc         账号页刷新 + 封面 tick
```
---

## 7. 2026-11-24 用户逐条验收后的调整（第二轮）

这一轮都是一条条点名的「不美观」，逐条对应如下（按用户编号）：

| # | 要求 | 落点 |
| --- | --- | --- |
| 1 | 在线音乐蓝色背景改 #132C3F | `ao_style.h` 的 `AO_C_BG` + `ao_style_screen_bg()`（单色，光晕保留） |
| 2 | 歌单页封面不写字、文字在封面下、标题与页码自适应 | `ao_view.cc` 的 `build_left()`：左列改竖排 flex；封面上只剩图 |
| 2 | 歌单默认封面用 `assets/defalut_cover.png` | `assets/cover/` 烘成位图 + `pl_default_cover_dsc()` |
| 3 | 长歌名自动滚动、等高、划列表时停 | `ao_style_song_row()`（SCROLL_CIRCULAR）+ `ao_style_marquee_*` |
| 4 | 去掉右上角添加歌单按钮 | `app_online.cc`（`on_add_clicked` 一并删除） |
| 5 | 滚动条压住歌手名 | `ao_style_song_row()` 歌手列右端内缩 12 px |
| 6 | 播放器歌名不换行、过长滚动 | `music_view_page.cc` 的 `title`：`LV_LABEL_LONG_SCROLL_CIRCULAR` |
| 6 | 封面不写字，只留左下角小胶囊（按平台上色） | `music_view_page.cc` 的 `badge` + `music_view_player_apply_src_badge()` |
| 7 | 拖动进度条时圆点不跟手 | `on_seek_press()` 里补 `seek_knob_place()` |
| 8 | 输出设备按钮换成指定 SVG | `assets/icons/out_device.svg` → `out_device` 位图 + `music_view_make_icon_btn_image()` |
| 9 | 封面圆角；无封面用黑胶图，两者圆角一致 | `clip_corner` + `player_default_cover_dsc()`（同一父容器同半径） |
| 9b | 占位封面底色改**浅粉灰 #EDE9EB**、唱片不被裁剪、显示 188 | `C_COVER_BG` + `COVER_BMP_SIDE`/`COVER_DEF_SIDE`=188；素材改为**补透明边**凑方（不再居中裁） |
| 10 | 控制按键太靠屏幕下边界 | `CTL_Y` 434 → 422 |
| 11 | 我的歌单主界面文字与行卡片不居中 | `ao_style_playlist_row()` 改 `LEFT_MID`/`RIGHT_MID`，分割线下移到行底 |
| 12 | 我喜欢的音乐封面要用**最近收藏**那首 | `online_last_liked_song_set/get` + `ao_page_acct.cc` 的 `acct_fav_cover_song()` |
| 13 | 歌词滚动要有平滑过渡 | `music_view_lyric.cc` 的 `lyric_scroll_to()`（260 ms 缓出，动画载体是整列） |
| 14 | 渠道胶囊尽可能小、点击可隐藏、隐藏可记忆 | `badge` 尺寸/内边距 + `music_view_player_src_badge_*`（NVS `music_cfg/srcbadge`）+ 播放器设置里的开关 |

### 新增的资源管线（以后加内置图照这个来）

`assets/cover/photos.txt` 是**清单**，`tools/icons/gen_photo.py` 按它把 PNG
烘成**原始像素 .bin**（不生成巨大的 C 数组），各组件的 `CMakeLists.txt` 再用
`target_add_binary_data(..., BINARY, RENAME_TO ...)` 搬进 flash，运行期只是把
`lv_image_dsc_t` 的头指过去（零解码、零拷贝）：

```
assets/defalut_cover.png               → pl_default_cover_128x128.bin     32 KB  rgb565
assets/default_black_vinyl_record.png  → player_default_cover_188x188.bin 104 KB rgb565a8
```

⚠ **素材一律"补透明边凑方"，不要居中裁**（2026-11-24 用户报"唱片左右被切"）：
产品图常是紧贴着拍的（这张黑胶唱片本身就顶到了源图左/下边缘），按 `min(w,h)` 居中裁
会再切掉左右各 14~15 px。`gen_photo.py` 的 `fit_square()` 只补透明像素，可见内容的
位置与比例完全不变；代价是画布变大、同样的显示方框下内容略小 —— 所以显示尺寸从 176
提到 188，让我盘直径保持在 ~173 px（原来是 ~172 px）。

⚠ **位图边长 ≠ 显示边长**（2026-11-24 踩过，表现为"占位图斜切/错位"）：
`music_view_page.cc` 里是**两个**宏 —— `COVER_BMP_SIDE` 是烘进来的位图真尺寸
（决定 `header.w/h/stride` 与 alpha 平面偏移，**改它必须重烘 .bin**），
`COVER_DEF_SIDE` 只是显示方框（`LV_IMAGE_ALIGN_CONTAIN` 等比缩进去）。
以前合成一个宏时，把它改小 3 px 就会让 LVGL 按错的跨距逐行读 → 整张图越读越偏。
现在 `player_default_cover_dsc()` 会按 `w*h*3` 校验字节数，不符就打 ERROR。


⚠ `rgb565a8` 的布局是**平面分离**的：先 `w*h*2` 字节的 RGB565 平面，再 `w*h` 字节的
A8 平面（依据 `lv_draw_sw_img.c` 里 `mask_buf = src_buf + stride*h`、`mask_stride = stride/2`）。
**不能逐像素写 RGB,A,RGB,A…** —— 那样 RGB 与 alpha 双向错位，真机上就是整块花屏
（2026-11-24 修的就是这个：`tools/icons/gen_photo.py` 的 `to_rgb565a8()`）。
核对办法：按 LVGL 的平面约定把 .bin 反解回 RGBA 再看一眼（`--preview` 只是把源图缩了一遍，
看的是内存里的 RGBA，**看不出 .bin 本身的布局错**）。

⚠ **flash 很紧**：app 槽 12.31 MB，本轮之后只剩 ~80 KB。加图之前先看 `partitions.csv`
末尾的余量说明；真要腾空间，按文档里的顺序（coredump → dict → 字体）动。

### 分区表的两处修正（2026-11-24）

1. `dict` 原来写 0x3A0000，而 `0x22000+0x3A0000 = 0x3C2000` 会**压进 factory 8 KB**，
   构建期直接报 `Partitions overlap` —— 改成 `0x38E000`（词库实际 0x38AAD6 + 12 KB 余量）。
   起始偏移仍是 0x22000，**词库不需要重烧**。
2. `factory` 起点 0x3C0000 → **0x3B0000**（12.25 MB → 12.31 MB），给内置封面位图腾地方。

⚠ 改了分区表：烧录时要让 `idf.py flash` 重写分区表。另外 `sdkconfig` 的中文注释里
原来有一个损坏的 UTF-8 字节（`网易云账号` 的「号」掉了尾字节），`kconfgen` 会直接拒绝
解析、整个工程编不了 —— 本轮已修（只改那一个字节，配置项一个没动）。
