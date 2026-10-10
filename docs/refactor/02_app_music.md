# app_music.cc 拆分重构蓝图（只读审计报告）

> 审计对象：`components/apps/app_music/app_music.cc`（+ `include/app_music.h`、`CMakeLists.txt`）
> 审计方式：`read` 全文分段读取（1–3604 行，无跳读），`grep` 统计外部调用点；**未修改任何现有源文件**（本次只新建本报告）。
> 行号基线：文件为 **LF 换行、3604 物理行**（非空行 3337，空行 267）。可直接用编辑器跳转核对。
> ⚠ 计数陷阱：用 `Get-Content | Measure-Object -Line` 之类的**非 UTF-8** 读取这个文件会因中文注释被错解码而**合并部分行**，得到 2942 这种偏小的数字（任务描述里的"约 2942 行"应即此来源）；本报告所有行号以 `read` 工具（UTF-8）的实际行号为准。

---

## 0. 事实基线

| 项 | 值 |
| --- | --- |
| 文件 | `components/apps/app_music/app_music.cc`，3604 行 / 121166 字符 / 157636 字节（UTF-8） |
| 头文件 | `include/app_music.h`（56 行），对外 5 个符号：`app_music_register_apps` / `app_music_play_online` / `app_music_online_detach` / `app_music_open_player` / `app_music_on_media_ctrl` |
| 构建 | `CMakeLists.txt` 只登记一个 SRCS：`"app_music.cc"`；REQUIRES：`lvgl ui_core bsp audio_service storage_service media_meta play_state bt_service online_service app_settings`；`-Wno-missing-field-initializers` |
| 外部调用方 | `components/apps/app_online/app_online.cc:856`（`app_music_online_detach`）、`:1000`（`app_music_play_online`）、`:1001`（`app_music_open_player`）；`main/main.cc:75`（`app_music_on_media_ctrl`）、`:254`（`app_music_register_apps`） |
| 函数 | **79 个函数定义**（74 个 `static` + 5 个对外符号） |
| 变量 | **100 个文件作用域可变静态变量** + `TAG`(45) + 1 个函数内 `static`（`bt_desc`，956） |
| LVGL 调用 | 约 **335 处**（`^\s*lv_[a-z_0-9]+\(` 计 335 行命中） |
| 工作线程 | 3 个（`cover` / `on_fetch` / `loc_scan`，均 prio 2，栈 8K/12K/8K） |
| 定时器 | 3 个 LVGL 定时器（500 ms / 100 ms / 500 ms），其中 2 个**常驻不销毁** |

本文件同时承担了 4 件本该分开的事：**播放状态机**、**在线媒体准备**、**本地文件浏览**、**LVGL 页面绘制**。
`tick_cb`（1317–1790，**474 行**）是全部状态迁移的中心轮询点，也是拆分的第一难点。

---

## 1. 全部函数清单

### 1.1 分类统计

| 分类 | 函数数 | 函数体行数（约） | 占文件 | 说明 |
| --- | --- | --- | --- | --- |
| LVGL 界面构建 | 22 | 723 | 20% | 含"输出设备弹窗"子组 10 个；`page_player` 单函数 224 行 |
| 播放控制状态机 | 13 | 759 | 21% | 含 `tick_cb` 474 行（占本类 62%） |
| 列表与队列管理 | 13 | 481 | 13% | 本地浏览页 + 异步扫描线程 + SD 热插拔 |
| 在线播放准备与续播 | 9 | 369 | 10% | 队列所有权、302 解析、地址重刷、断点续播 |
| 歌词 LRC 渲染与同步 | 6 | 124 | 3% | 纯文本渲染 + 高亮/居中不变式 |
| 进度条与 seek 交互 | 5 | 170 | 5% | 容器白名单、千分比换算、松手才定位 |
| 封面加载 | 4 | 127 | 4% | 工作线程解码 + 结果产权 |
| 小工具函数（含注册/路由） | 7 | 75 | 2% | `fmt_time`、`label_set_if_changed`、注册/入口 |
| **合计** | **79** | **≈2828** | **78%** | 其余 776 行 = 注释块、宏、前向声明、变量定义、空行 |

`static` 分布：**74 static / 5 非 static**（非 static 全部是 `app_music.h` 里的对外 API）。

### 1.2 LVGL 界面构建（22 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 368 | 19 | `apply_src_badge` | ✔ | 画"在线播放/本地播放"来源药丸（内部比较 `s_src_badge_state`，未变即返回） |
| 420 | 14 | `apply_play_mode_ui` | ✔ | 把循环模式画到按钮（LOOP/SHUFFLE + "1" 角标显隐） |
| 701 | 5 | `on_volume` | ✔ | 音量键 → `ui_volume_show()` 滑出音量条 |
| 937 | 8 | `out_icon_of` | ✔ | 输出类型 → LVGL 图标字符（BT / MUTE / VOLUME_MAX） |
| 946 | 29 | `out_name_of` | ✔ | 输出类型 → 中文名；蓝牙分支用 `bt_service_peer_name()`+`is_connected()` 拼"蓝牙音频设备 X 已连接"（内含函数级 `static char bt_desc`） |
| 976 | 8 | `out_picker_close` | ✔ | 关输出弹窗（`ui_ba_close` 异步退场 + 置空 `s_out_overlay`） |
| 985 | 15 | `on_out_pick` | ✔ | 点某一行输出 → `audio_service_set_output()` + Toast + 关窗 |
| 1001 | 5 | `on_out_scrim` | ✔ | 点弹窗空白处 = 关闭 |
| 1008 | 56 | `out_picker_row` | ✔ | 画一行输出选项（图标块 + 名字 + 状态 + 当前"使用中"高亮） |
| 1066 | 22 | `on_output_click` | ✔ | 开/关输出弹窗（`ui_ba_open` + 三行 + 长按提示） |
| 1101 | 16 | `open_bt_deferred` | ✔ | `lv_async_call` 延迟跳"设置→蓝牙"（等手指抬起，防松手误触列表行） |
| 1118 | 7 | `on_output_long` | ✔ | 长按输出键：先关弹窗，再置 `s_open_bt_req` + 排 async |
| 1127 | 11 | `out_btn_sync` | ✔ | 按实际 `audio_service_get_output()` 刷新按钮图标（变了才写） |
| 1792 | 44 | `player_on_delete` | ✔ | 播放器页 `LV_EVENT_DELETE`：关弹窗、落盘进度、**清 17 个控件指针**、丢未消费 ready（刻意不删定时器、不释放封面） |
| 1844 | 21 | `make_round_btn_ex` | ✔ | 造圆形按钮（可指定事件码，长按按钮要 `SHORT_CLICKED`） |
| 1866 | 5 | `make_round_btn` | ✔ | `make_round_btn_ex` 的 CLICKED 快捷版 |
| 2414 | 28 | `build_cover_view` | ✔ | 左栏封面容器：占位图标 + 真封面 `lv_image`（初始隐藏） |
| 2485 | 39 | `buf_hint_text` | ✔ | 刷"缓冲中 20 KB/s · 45%"（Kbps÷8；并保证"有文字就有转圈"） |
| 2534 | 9 | `set_buffering_hint` | ✔ | 进入缓冲态：置 `s_buf_show`、显示提示行、调 `buf_hint_text` |
| 2576 | 61 | `build_lyric_view` | ✔ | 右栏歌词区：[转圈+文字] flex 提示行 + 歌词列，末尾 `lyric_rebuild`+`lyric_apply_mode` |
| 2638 | 224 | `page_player` | ✔ | **播放器页整体构建**：分栏、来源药丸、标题/歌手、歌词区、进度条抓取区、控制排、事件注册、常驻定时器创建、封面回贴 |
| 3418 | 77 | `page_local` | ✔ | **本地列表页整体构建**：标题、提示行、列表列（flex+滚动条）、SD 轮询定时器、首次扫描 |

> 子组说明：**输出设备弹窗**共 10 个函数（937–1137），全部只服务于播放器页的一个按钮，建议整组抽成 `music_view_output.cc`。

### 1.3 播放控制状态机（13 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 199 | 10 | `save_progress` | ✔ | 进度记忆的唯一出口；在线流式不记（签名 URL 存了没用） |
| 410 | 8 | `play_mode_name` | ✔ | 枚举 → "列表循环/单曲循环/随机循环" |
| 440 | 25 | `on_mode_click` | ✔ | 循环模式切换；**在线时禁止随机**（拦下并 Toast） |
| 504 | 91 | `start_track_at` | ✔ | 本地起播（可带 `resume_ms`）：清在线残留、装队列/歌词/封面、摆 UI、落盘进度、`audio_service_play_path` |
| 596 | 4 | `start_track` | ✔ | `start_track_at(idx, 0)` 的换曲快捷版 |
| 601 | 62 | `play_next` | ✔ | 换曲总入口（dir=+1/-1/0）：在线走"重新解析地址"链路，本地走随机/顺序 |
| 664 | 32 | `on_play_pause` | ✔ | 播放/暂停键；管线 STOPPED 时从记忆进度重起播 |
| 697 | 1 | `on_prev` | ✔ | `play_next(-1)` |
| 698 | 1 | `on_next` | ✔ | `play_next(+1)` |
| 1142 | 16 | `on_audio_evt` | ✔ | **播放事件入口**（GMF 任务上下文）：只置 `s_advance_req` / `s_err_req` |
| 1261 | 50 | `media_cmd_tick` | ✔ | 100 ms 定时器消费蓝牙媒体键（PLAY/PAUSE/STOP/NEXT/PREV）+ 落盘 |
| 1312 | 4 | `app_music_on_media_ctrl` | ✘ | 对外 API：任意任务上下文只置 `s_media_cmd` |
| 1317 | 474 | `tick_cb` | ✔ | **中心轮询/状态机主干**（500 ms 常驻）：ready/失败、落点核对、缓冲提示、错误与播完换曲、卡死看门狗、墙钟、双路续播、进度落盘、时长推导、控件刷新、封面搬运 |
| 3520 | 43 | `restore_last_session` | ✔ | 开机恢复上次曲目与进度（只摆状态，不出声） |

### 1.4 列表与队列管理（13 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 2912 | 26 | `local_reserve_entries` | ✔ | 目录条目缓冲扩容（优先 PSRAM，64→翻倍→8192 上限） |
| 2940 | 58 | `local_add_row` | ✔ | 画一行（图标按 目录/音乐/其它 上色 + 名字 + 大小），`sel` 编码动作 |
| 3000 | 49 | `local_render` | ✔ | 只重画"当前页"（120 行/页）+ 提示行文案，翻页不重读 SD |
| 3062 | 32 | `local_show_loading` | ✔ | 列表换成"转圈 + 载入中…"（进目录立刻有反馈） |
| 3137 | 51 | `local_scan_task` | ✔ | **工作线程**：列目录（按容量重试）→ 建播放队列 → 按序发布结果；绝不碰 LVGL |
| 3190 | 33 | `local_scan_start` | ✔ | 起扫描；忙则把请求排进 `s_scan_pend_dir`；失败也发布失败结果 |
| 3225 | 24 | `local_apply_scan` | ✔ | 消费扫描结果（校验 `n ≤ cap`）并渲染 |
| 3254 | 8 | `local_rebuild` | ✔ | 铺加载态 + 起异步扫描，立刻返回 |
| 3263 | 63 | `on_local_entry` | ✔ | 行点击：上一级 / 翻页 / 进目录 / 播放并压入播放器页 |
| 3328 | 13 | `local_page_watch` | ✔ | 列表页 `LV_EVENT_DELETE`：清指针、作废在飞扫描、**删**SD 定时器 |
| 3354 | 31 | `local_sd_apply` | ✔ | SD 拔/插的界面与状态收尾（拔卡停播 + 回根目录） |
| 3386 | 31 | `local_sd_tick` | ✔ | 500 ms：取扫描结果（校验 seq）→ 起排队请求 → 比对 SD 代次 |
| 3418 | 77 | `page_local` | ✔ | 本地列表页构建（分类见 1.2，此处按"页面 = 列表页"计入列表管理口径） |

> `page_local` 同时属于"界面构建"和"列表管理"；1.2 的分类统计把它计入 LVGL 界面构建，本表保留以便阅读完整性（统计不重复计数）。

### 1.5 在线播放准备与续播（9 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 271 | 15 | `online_discard_ready` | ✔ | 丢弃"已下载好、还没起播"的结果（含封面/歌词缓冲释放）；只在 LVGL 任务调 |
| 297 | 7 | `online_owned_drop` | ✔ | 释放"托管中且已不是当前队列"的旧在线数组 |
| 305 | 14 | `app_music_online_detach` | ✘ | 对外 API：在线队列所有权交接（播放器在用就接管，否则立刻 free） |
| 1982 | 28 | `online_prepare_kick` | ✔ | 起在线准备任务（LVGL 任务里调）；拷贝歌曲结构、带请求序号 |
| 2011 | 76 | `online_prepare_task` | ✔ | **工作线程**：解析 302 流地址（失败回退下载）→ 抓封面/歌词进 PSRAM → 发布 ready（带序号校验） |
| 2105 | 45 | `online_url_refresh` | ✔ | 地址过期/断流：重刷一次地址并**记住断点**（`s_on_resume_ms`），复用点歌链路 |
| 2158 | 94 | `start_track_online_at` | ✔ | 在线起播：装 UI/歌词/封面、挂缓冲提示、`play_uri`、`set_duration_ms` |
| 2265 | 40 | `online_switch_ui_to` | ✔ | 在线切歌"界面先行"：先铺目标歌的标题/歌手/占位，避免"点了没反应" |
| 2310 | 50 | `app_music_play_online` | ✘ | 对外 API：接收在线歌单并从 idx 起播（停旧歌 → 切界面 → 缓冲提示 → 起准备） |

### 1.6 歌词 LRC 渲染与同步（6 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 908 | 10 | `on_toggle_view` | ✔ | 轻点封面展开/收起歌词（`s_lyric_mode` 现恒为 true，历史分支保留） |
| 1190 | 41 | `lyric_apply_cur` | ✔ | **高亮不变式**：复位上一行 + 点亮当前行 + 整列 `translate_y` 居中（只碰 2 行，避免整列重排） |
| 1233 | 12 | `update_lyric_highlight` | ✔ | 定时器用：行号变了才动 |
| 1252 | 7 | `lyric_resync` | ✔ | 定位后强制重贴（**不许**先清 `s_lyric_cur`） |
| 2446 | 28 | `lyric_rebuild` | ✔ | 按当前 `s_lrc` 重建最多 240 行标签（WRAP 而非 DOT） |
| 2549 | 26 | `lyric_apply_mode` | ✔ | 说明行/歌词列显隐 + 顺手藏缓冲转圈（"出声/换曲时调用"的约定） |

### 1.7 进度条与 seek 交互（5 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 720 | 23 | `track_seek_unsupported` | ✔ | 容器白名单（m4a/mp4/ogg/oga/opus 不给拖）；认扩展名前先切掉 `?vuutv=…` |
| 745 | 20 | `seek_pointer_permille` | ✔ | 触点 x 在抓取区里的千分比（0–1000） |
| 777 | 20 | `on_seek_press` | ✔ | 按下/拖动：只跟手改进度与"已播"，时长未知也允许进入拖动 |
| 799 | 5 | `on_seek_lost` | ✔ | 手指滑出抓取区：清 `s_seeking`（否则进度条永久不动） |
| 804 | 102 | `on_seek_release` | ✔ | 松手才真定位：两道关（容器/码率）→ 缓冲提示 → `seek_ms` → 落点核对计数 → 复位看门狗 → 对齐时间轴与歌词 |

### 1.8 封面加载（4 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 1877 | 10 | `cover_reap` | ✔ | 丢掉没人认领的解码结果 |
| 1890 | 48 | `cover_kick` | ✔ | 有请求且空闲就起解码任务；在线把 `s_on_cover_data` **所有权转移**进请求 |
| 1943 | 26 | `cover_task` | ✔ | **工作线程**：`media_meta_image_mem_rgb565` / `media_meta_cover_rgb565` 解码 → 写标志；绝不碰 LVGL |
| 2368 | 43 | `cover_show` | ✔ | （LVGL 侧）把像素贴到 `lv_image`；每张新分配描述符；先摘 src 再释放旧像素 |

### 1.9 小工具函数（含注册/路由，7 个）

| 起始行 | 行数 | 函数 | static | 职责 |
| --- | --- | --- | --- | --- |
| 480 | 9 | `fmt_time` | ✔ | ms → `M:SS`，0/超 24 h → `--:--` |
| 490 | 7 | `current_status_text` | ✔ | "未在播放/正在播放/已暂停"（日志用） |
| 1170 | 11 | `label_set_if_changed` | ✔ | 文本没变不写 label（避免 500 ms 抖动 + 堆碎片） |
| 3499 | 1 | `on_music_enter` | ✔ | 空实现（路由进入钩子） |
| 3500 | 1 | `on_music_exit` | ✔ | 空实现 |
| 3506 | 5 | `app_music_open_player` | ✘ | 对外 API：压栈打开播放器页 |
| 3564 | 41 | `app_music_register_apps` | ✘ | 对外 API：注册 music/local 两个 APP、订阅播放事件、**创建两个常驻定时器**、恢复上次会话 |

### 1.10 前向声明（非函数，拆分时注意成对迁移）

| 行 | 内容 |
| --- | --- |
| 321–332 | `lyric_rebuild` / `lyric_apply_mode` / `update_lyric_highlight` / `lyric_resync` / `cover_show` / `cover_reap` / `cover_kick` / `online_prepare_kick` / `set_buffering_hint` / `buf_hint_text` |
| 346–350 | `start_track_online_at` / `online_switch_ui_to` / `online_url_refresh` |
| 1874 | `cover_task` |
| 1979 | `online_prepare_task` |
| 2902–2903 | `local_rebuild` / `on_local_entry` |

---

## 2. 全局/静态变量清单

**总计 102 项**：100 个文件作用域可变静态变量 + `TAG`(45) + 1 个函数内 `static`(956)。
这 100 个变量就是"拆分后需要哪些跨模块访问器"的完整答案 —— 下面按**归属模块**分组（分组即建议的新家）。

### 2.1 播放核心状态（11 个变量 + `TAG` → 新 `music_state` / `music_player`）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 68 | `s_index` | `int` | 当前曲目下标（-1 = 无） | 写：`start_track_at`(527)、`play_next`(617,648–660)、`tick_cb`(1437)、`start_track_online_at`(2166)、`app_music_play_online`(2321)、`restore_last_session`(3552)；读：`tick_cb`、`on_play_pause`、`on_seek_*`、`page_player`、`cover_kick`、`online_*`、`media_cmd_tick` |
| 69 | `s_elapsed_ms` | `uint32_t` | 已播毫秒（**只在真出声时累加**，1616–1631） | 写：`start_track_at`(530)、`on_seek_release`(885)、`tick_cb`(1379,1629,1650,1662,1683,1693,1709,1713,1718)、`start_track_online_at`(2168)、`online_switch_ui_to`(2288)、`restore_last_session`(3553)；读：极广（歌词、进度、落盘、断点） |
| 70 | `s_dur_ms` | `uint32_t` | 总时长（0 = 未知） | 写：`start_track_at`(531)、`tick_cb`(1741,1745)、`start_track_online_at`(2169)、`online_switch_ui_to`(2289)、`restore_last_session`(3554)；读：`on_seek_*`、`tick_cb`(1474,1754)、`page_player` |
| 71 | `s_playing` | `bool` | 界面侧"在播"缓存 | 写：`start_track_at`(590)、`play_next`(631)、`on_play_pause`(685)、`media_cmd_tick`(1280,1285,1289)、`tick_cb`(1565,1632)、`start_track_online_at`(2244)、`app_music_play_online`(2351)；读：`tick_cb`(1445,1730)、`page_player`(2810) |
| 72 | `s_lrc` | `lrc_t` | 当前歌词（最多 240 行 + 时间轴） | 写：`start_track_at`(544)、`start_track_online_at`(2183,2190,2193)、`online_switch_ui_to`(2302)、`restore_last_session`(3560)；读：`lyric_rebuild`(2468)、`update_lyric_highlight`(1239)、`lyric_resync`(1257) |
| 73 | `s_lyric_cur` | `int` | 当前高亮行（**不变式：换行时必须先读旧值**） | 写：`lyric_apply_cur`(1193)、`lyric_rebuild`(2453)、`start_track_at`(533)、`start_track_online_at`(2171)、`on_toggle_view`(914)；读：`lyric_apply_cur`(1192)、`update_lyric_highlight`(1240) |
| 79 | `s_resume_ms` | `uint32_t` | 本地续播目标（等码率到手才定位） | 写：`start_track_at`(528)、`tick_cb`(1682,1692,1696)、`start_track_online_at`(2167)、`restore_last_session`(3556)；读：`tick_cb`(1628,1678) |
| 80 | `s_save_ticks` | `int` | 进度落盘节流计数（10 tick = 5 s） | 写：`start_track_at`(534)、`tick_cb`(1728,1729)、`start_track_online_at`(2172)、`restore_last_session`(3557)；读：`tick_cb` |
| 81 | `s_resume_wait` | `int` | 等码率的 tick 计数（12 tick 超时） | 写：`start_track_at`(535)、`tick_cb`(1688)、`start_track_online_at`(2173)、`restore_last_session`(3558)；读：`tick_cb`(1688,1693) |
| 99 | `s_track_path[600]` | `char[]` | **真正在播的路径**（喂 `play_uri`，也是 NVS 进度键） | 写：`start_track_at`(536)、`start_track_online_at`(2177)、`restore_last_session`(3559)；读：`save_progress`(204)、`on_play_pause`(691)、`on_seek_release`(831,888)、`media_cmd_tick`(1301)、`player_on_delete`(1801)、`tick_cb`(1678,1680,1730) |
| 394 | `s_play_mode` | `play_mode_t` | 列表/单曲/随机 | 写：`on_mode_click`(443–459)、`play_next`(651)、`app_music_play_online`(2332,2333)；读：`tick_cb`(1462)、`apply_play_mode_ui`、`play_mode_name` |
| 45 | `TAG` | `const char *` | 日志标签 `"app_music"` | 全文件 `ESP_LOGI/W/E/D` |

### 2.2 播放器页控件（23 → 新 `music_view_player`，**不出模块**，仅供视图内部使用）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 102 | `s_p_title` | `lv_obj_t *` | 歌名（整宽滚动 label） | `page_player`(2697)、`start_track_at`(548)、`start_track_online_at`(2195)、`online_switch_ui_to`(2272)、`player_on_delete`(1809) |
| 103 | `s_p_artist` | `lv_obj_t *` | 歌手 | 同上四处（2710 / 551 / 2198 / 2275 / 1809） |
| 104 | `s_p_seek` | `lv_obj_t *` | 进度条本体（`lv_bar`，千分比） | `page_player`(2750)、`start_track_at`(573)、`on_seek_press`(780,787,790)、`on_seek_release`(816,891)、`tick_cb`(1752,1757)、`start_track_online_at`(2219)、`online_switch_ui_to`(2285)、`player_on_delete`(1810) |
| 105 | `s_p_seek_hit` | `lv_obj_t *` | 30 px 透明抓取区 | `page_player`(2733–2747)、`seek_pointer_permille`(748,755)、`player_on_delete`(1810) |
| 106 | `s_p_cur` | `lv_obj_t *` | 已播时间 | `page_player`(2774)、`start_track_at`(576)、`on_seek_press`(791)、`on_seek_release`(894)、`tick_cb`(1759,1762)、`start_track_online_at`(2222)、`online_switch_ui_to`(2282)、`player_on_delete`(1809) |
| 107 | `s_p_tot` | `lv_obj_t *` | 总时长 | `page_player`(2784)、`tick_cb`(1764,1767)、`online_switch_ui_to`(2279)、`player_on_delete`(1809) |
| 108 | `s_p_btn` | `lv_obj_t *` | 播放/暂停按钮 | `page_player`(2810)、`start_track_at`(591)、`play_next`(632)、`on_play_pause`(686)、`media_cmd_tick`(1305)、`tick_cb`(1566,1769)、`start_track_online_at`(2245)、`app_music_play_online`(2352)、`player_on_delete`(1810) |
| 109 | `s_p_cover_box` | `lv_obj_t *` | 封面容器 | `build_cover_view`(2424)、`player_on_delete`(1811,1833) |
| 110 | `s_p_cover_img` | `lv_obj_t *` | 真封面 `lv_image` | `build_cover_view`(2434)、`cover_show`(2370,2382,2386,2399,2400,2407,2408)、`page_player`(2852–2854)、`player_on_delete`(1811,1834) |
| 111 | `s_p_lyric_box` | `lv_obj_t *` | 歌词区容器（居中基准） | `build_lyric_view`(2585)、`lyric_apply_cur`(1224)、`player_on_delete`(1812) |
| 112 | `s_p_lyric_hint` | `lv_obj_t *` | 说明行（缓冲中/暂无歌词） | `build_lyric_view`(2618)、`tick_cb`(1405)、`buf_hint_text`(2487,2519,2521)、`set_buffering_hint`(2536,2540)、`lyric_apply_mode`(2559–2564)、`player_on_delete`(1812) |
| 113 | `s_p_hint_row` | `lv_obj_t *` | [转圈][文字] flex 行 | `build_lyric_view`(2604)、`player_on_delete`(1813) |
| 114 | `s_p_buf_spin` | `lv_obj_t *` | 缓冲转圈（`lv_spinner`） | `build_lyric_view`(2608)、`buf_hint_text`(2497,2498)、`lyric_apply_mode`(2555,2556)、`player_on_delete`(1813) |
| 115 | `s_p_lyric_col` | `lv_obj_t *` | 歌词列（整列 translate_y） | `build_lyric_view`(2624)、`lyric_rebuild`(2448,2449,2456,2461)、`lyric_apply_cur`(1228)、`update_lyric_highlight`(1236)、`lyric_resync`(1254)、`lyric_apply_mode`(2567–2571)、`player_on_delete`(1812) |
| 116 | `s_p_lyric_labels[240]` | `lv_obj_t *[]` | 歌词行指针数组（~1 KB） | `lyric_rebuild`(2454,2469)、`lyric_apply_cur`(1207,1211,1213)、`player_on_delete`(1820) |
| 117 | `s_p_lyric_n` | `size_t` | 实际建了多少行 | `lyric_rebuild`(2452,2471)、`lyric_apply_cur`(1207,1222)、`lyric_apply_mode`(2551)、`player_on_delete`(1819) |
| 118 | `s_lyric_mode` | `bool` | 歌词展开态（现恒 true） | `lyric_apply_mode`(2552 置真)、`on_toggle_view`(911,913)、`start_track_at`(557)、`start_track_online_at`(2202)、`tick_cb`(1775)、`player_on_delete`(1817) |
| 119 | `s_seeking` | `bool` | 手指正在拖（期间 tick 不覆盖） | `start_track_at`(532)、`on_seek_press`(783)、`on_seek_lost`(802)、`on_seek_release`(810)、`tick_cb`(1752,1759)、`start_track_online_at`(2170)、`online_switch_ui_to`(2292)、`player_on_delete`(1818)、`restore_last_session`(3555) |
| 120 | `s_tick` | `lv_timer_t *` | 500 ms 常驻心跳 | `page_player`(2834,2835)、`app_music_register_apps`(3594,3595)；**任何地方都不删** |
| 365 | `s_p_src_badge` | `lv_obj_t *` | 来源药丸 | `page_player`(2686)、`apply_src_badge`(370,378–384)、`player_on_delete`(1815) |
| 366 | `s_src_badge_state` | `int` | 上次画的状态（-1 强制重画） | `apply_src_badge`(373,376)、`page_player`(2693)、`player_on_delete`(1816) |
| 395 | `s_p_mode_btn` | `lv_obj_t *` | 模式按钮里的图标 label | `page_player`(2800)、`apply_play_mode_ui`(422,423)、`player_on_delete`(1814) |
| 396 | `s_p_mode_badge` | `lv_obj_t *` | 单曲循环 "1" 角标 | `page_player`(2803)、`apply_play_mode_ui`(426–430)、`player_on_delete`(1814) |

### 2.3 封面（9 → 新 `music_cover`，跨线程，注意 volatile）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 136 | `s_cover_dsc` | `lv_image_dsc_t *` | 堆上描述符（每张新分配） | `cover_show`(2373,2374,2397)、`page_player`(2852,2853) |
| 137 | `s_cover_pixels` | `uint8_t *` | 当前显示的 RGB565 像素（PSRAM） | `cover_show`(2377,2378,2389,2396)、`page_player`(2852) |
| 138 | `s_cover_want` | `volatile int` | 待解码曲目（-1 无） | 写：`start_track_at`(566)、`start_track_online_at`(2216)、`online_switch_ui_to`(2297)、`page_player`(2856)；读/清：`cover_kick`(1892,1896) |
| 139 | `s_cover_busy` | `volatile bool` | 解码线程占用中 | `cover_kick`(1892,1931,1933)、`cover_task`(1965)、`page_player`(2855) |
| 140 | `s_cover_ready` | `volatile bool` | 工作线程已产出结果 | `cover_task`(1964)、`cover_reap`(1879,1880)、`tick_cb`(1780,1781) |
| 141 | `s_cover_out` | `uint8_t *` | 产出像素（等 LVGL 任务认领） | `cover_task`(1960)、`cover_reap`(1881,1882)、`tick_cb`(1784,1785,1787) |
| 142 | `s_cover_out_w` | `int` | 产出宽 | `cover_task`(1961)、`tick_cb`(1783) |
| 143 | `s_cover_out_h` | `int` | 产出高 | `cover_task`(1962)、`tick_cb`(1783) |
| 144 | `s_cover_out_idx` | `volatile int` | 产出对应的曲目下标（对不上就丢） | `cover_task`(1963)、`tick_cb`(1782) |

### 2.4 在线队列与准备（19 → 新 `music_online`）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 159 | `s_on_songs` | `const online_song_t *` | 当前在线队列（所有权见 287–318） | `online_owned_drop`(299)、`app_music_online_detach`(310)、`play_next`(604)、`start_track_at`(522)、`online_prepare_kick`(1984,1997)、`start_track_online_at`(2160,2163)、`online_switch_ui_to`(2267,2270)、`online_url_refresh`(2107)、`app_music_play_online`(2315)、`page_player`(2702,2703,2715,2716) |
| 160 | `s_on_count` | `size_t` | 队列长度 | `play_next`(604,615)、`online_prepare_kick`(1989)、`start_track_online_at`(2160)、`online_url_refresh`(2107)、`app_music_play_online`(2316)、`start_track_at`(523)、`page_player`(2702,2715) |
| 161 | `s_on_mode` | `bool` | 当前是否在线队列（**决定看门狗阈值/是否记进度**） | `save_progress`(201)、`start_track_at`(515)、`play_next`(604)、`on_play_pause`(678)、`media_cmd_tick`(1274)、`tick_cb`(1528,1547,1645,1666)、`cover_kick`(1903)、`online_url_refresh`(2107)、`start_track_online_at`(2165)、`app_music_play_online`(2317)、`local_sd_apply`(3361)、`apply_src_badge`(373,376,377)、`page_player`(2702,2715) |
| 173 | `s_on_stream[560]` | `char[]` | 待播地址（https 流或下载缓存路径） | 写：`online_prepare_task`(2071)；读：`on_play_pause`(678)、`media_cmd_tick`(1274)、`tick_cb`(1331) |
| 174 | `s_on_stream_local` | `bool` | true = 下载兜底（当本地文件播） | `online_prepare_task`(2072)、`save_progress`(201)、`on_seek_release`(846)、`start_track_online_at`(2189,2227,2250) |
| 175 | `s_on_dur_ms` | `uint32_t` | 在线总时长（估） | `online_prepare_task`(2028,2030)、`start_track_online_at`(2169) |
| 179 | `s_on_seq` | `uint32_t` | 请求序号（只在 LVGL 任务自增） | `play_next`(618)、`online_prepare_kick`(2000)、`online_prepare_task`(2051,2066,2080)、`tick_cb`(1330)、`online_url_refresh`(2143)、`app_music_play_online`(2322) |
| 189 | `s_on_cover_data` | `uint8_t *` | 已抓封面字节（JPEG/PNG，PSRAM） | `online_discard_ready`(275,276,277)、`cover_kick`(1907,1912,1914)、`online_prepare_task`(2075) |
| 190 | `s_on_cover_len` | `size_t` | 封面字节数 | 同上（278 / 1913,1915 / 2076） |
| 191 | `s_on_lrc_data` | `char *` | 已抓歌词文本 | `online_discard_ready`(280–283)、`start_track_online_at`(2182,2184,2185)、`online_prepare_task`(2077) |
| 192 | `s_on_lrc_len` | `size_t` | 歌词长度 | 同上（284 / 2186 / 2078） |
| 209 | `s_on_req` | `volatile int` | 待准备曲目（-1 无） | 写：`play_next`(624)、`online_url_refresh`(2146)、`app_music_play_online`(2340)、`start_track_at`(524)；读/清：`tick_cb`(1337,1338)、`online_prepare_kick`(1984,1987,1988) |
| 210 | `s_on_busy` | `volatile bool` | 准备线程占用中 | `play_next`(637)、`online_prepare_kick`(1984,2002,2004)、`online_prepare_task`(2021,2055,2083)、`online_url_refresh`(2110) |
| 211 | `s_on_ready` | `bool` | 准备完成待起播（**最后置位**） | `online_discard_ready`(273)、`online_prepare_task`(2081)、`tick_cb`(1326,1328) |
| 212 | `s_on_ready_idx` | `int` | ready 对应曲目 | `online_discard_ready`(274)、`online_prepare_task`(2079)、`tick_cb`(1327,1329) |
| 215 | `s_on_ready_seq` | `uint32_t` | ready 对应序号（过期就丢） | `online_prepare_task`(2080)、`tick_cb`(1330) |
| 216 | `s_on_failed` | `bool` | 准备失败待提示 | `play_next`(623)、`online_prepare_kick`(1994,2005)、`online_prepare_task`(2065)、`tick_cb`(1339,1340)、`online_url_refresh`(2145)、`app_music_play_online`(2339) |
| 223 | `s_on_prefetch` | `bool` | 预取标记（**当前无入口置真**，代码留着） | `online_prepare_kick`(1999,2001) |
| 295 | `s_on_owned` | `const online_song_t *` | 托管中待释放的旧队列 | `online_owned_drop`(299,300,301)、`app_music_online_detach`(311,312)、`start_track_at`(525)、`app_music_play_online`(2320) |

### 2.5 看门狗 / 重刷 / 续播（7 → 新 `music_player` 的逻辑状态）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 169 | `s_stall_ticks` | `int` | 无 PCM 连续 tick 数 | `on_seek_release`(884)、`tick_cb`(1545,1546,1613,1653)、`start_track_online_at`(2175) |
| 170 | `s_stall_kicks` | `int` | 本首踢管线次数（上限 3） | `on_seek_release`(883)、`tick_cb`(1573,1574,1604,1652)、`start_track_online_at`(2176) |
| 235 | `s_url_retry` | `int` | 本首地址重刷次数（上限 2） | `start_track_at`(516)、`play_next`(619)、`tick_cb`(1447)、`online_url_refresh`(2119,2123,2137)、`app_music_play_online`(2323) |
| 249 | `s_on_resume_ms` | `uint32_t` | 在线断点续播目标 | `start_track_at`(517)、`play_next`(620)、`tick_cb`(1645,1648,1661)、`online_url_refresh`(2132,2135,2138)、`app_music_play_online`(2324) |
| 250 | `s_on_resume_wait` | `int` | 等"总长+时长"的 tick 计数 | `online_url_refresh`(2133)、`tick_cb`(1659,1667) |
| 262 | `s_eof_retry` | `int` | 提前结束续播次数（上限 3，**只在真换曲时清零**） | `start_track_at`(518)、`play_next`(621)、`tick_cb`(1475,1476,1485)、`app_music_play_online`(2325) |
| 476 | `s_fail_streak` | `int` | 连续失败次数（3 次停下） | `tick_cb`(1433,1434,1446,1503,1504,1558,1561,1562) |

### 2.6 事件中转 / seek 时序标志（9 → 新 `music_state` 的"事件区"）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 471 | `s_advance_req` | `volatile bool` | 播完/出错 → 换曲请求 | `on_audio_evt`(1147,1152)、`tick_cb`(1418,1420) |
| 472 | `s_err_req` | `volatile bool` | 出错请求 | `on_audio_evt`(1151)、`tick_cb`(1417,1419,1608) |
| 474 | `s_media_cmd` | `volatile int` | 蓝牙媒体键（1–5） | `app_music_on_media_ctrl`(1314)、`media_cmd_tick`(1264,1268) |
| 475 | `s_media_timer` | `lv_timer_t *` | 100 ms 媒体键定时器 | `page_player`(2837,2839)、`app_music_register_apps`(3599,3600) |
| 334 | `s_buf_show` | `bool` | 正在显示"缓冲中" | `start_track_at`(560)、`tick_cb`(1344,1387,1389)、`set_buffering_hint`(2539)、`online_switch_ui_to`(2290) |
| 336 | `s_seek_buf_pending` | `bool` | 本次缓冲是 seek 引起（出声后打落点日志） | `start_track_at`(561)、`on_seek_release`(849,854)、`tick_cb`(1345,1395,1396,1651)、`online_switch_ui_to`(2291) |
| 342 | `s_seek_landing_wait` | `int` | 落点核对剩余 tick（10 次） | `on_seek_release`(864)、`tick_cb`(1371,1374,1383) |
| 344 | `s_seek_in_flight` | `bool` | seek 进行中（看门狗别误判） | `on_seek_release`(873)、`tick_cb`(1535,1537,1541,1544) |
| 345 | `s_seek_inflight_n` | `int` | seek 无 PCM 计数（上限 20 s/500 ms） | `on_seek_release`(874)、`tick_cb`(1538) |

### 2.7 输出设备弹窗（5 → 新 `music_view_output`）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 932 | `s_out_btn_icon` | `lv_obj_t *` | 控制排输出按钮图标 | `page_player`(2829)、`out_btn_sync`(1129,1135)、`player_on_delete`(1798) |
| 933 | `s_out_overlay` | `lv_obj_t *` | 弹窗遮罩（挂 `lv_layer_top`） | `out_picker_close`(978,981)、`on_output_click`(1069,1077) |
| 934 | `s_out_dlg` | `ui_ba_dialog_t` | BA 对话框句柄（与 overlay 同生共死） | `out_picker_close`(980)、`on_output_click`(1076,1080,1082) |
| 935 | `s_out_shown` | `audio_out_t` | 图标当前画的是哪个输出 | `page_player`(2830)、`out_btn_sync`(1133,1134)、`player_on_delete`(1799) |
| 1099 | `s_open_bt_req` | `bool` | 长按跳设置的待处理请求 | `on_output_long`(1122)、`open_bt_deferred`(1104,1112) |
| 956 | `bt_desc` | `static char[]`（**函数内**） | 蓝牙设备名文案缓冲（`out_name_of` 专用） | 仅 `out_name_of`(966) |

### 2.8 本地浏览页（9 → 新 `music_view_local` / `music_source_local`）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 2873 | `s_list_col` | `lv_obj_t *` | 列表列（flex + 滚动条） | `page_local`(3443)、`local_page_watch`(3331)、`local_add_row`(2942)、`local_render`(3002,3005)、`local_show_loading`(3064)、`local_apply_scan`(3227,3232)、`local_rebuild`(3256)、`local_sd_apply`(3356,3372) |
| 2874 | `s_list_hint` | `lv_obj_t *` | 路径/页数提示行 | `page_local`(3436,3481)、`local_page_watch`(3332)、`local_render`(3021–3026)、`local_show_loading`(3090,3091)、`local_sd_apply`(3369,3370,3380,3381) |
| 2875 | `s_cur_dir[512]` | `char[]` | 当前目录 | `local_render`(3024,3026,3030)、`on_local_entry`(3270,3274,3295,3302)、`local_sd_apply`(3368,3379)、`page_local`(3490,3492) |
| 2876 | `s_entries` | `fs_entry_t *` | 目录条目缓冲（PSRAM） | `local_reserve_entries`(2924,2927,2933)、`local_scan_task`(3148,3151)、`on_local_entry`(3292) |
| 2877 | `s_entries_cap` | `size_t` | 缓冲容量（条） | `local_reserve_entries`(2914,2919,2934,2936)、`local_scan_task`(3148,3155,3158,3162)、`local_apply_scan`(3242,3244,3245) |
| 2878 | `s_entry_n` | `int` | 当前目录条目数 | `local_render`(3007,3017,3018,3023,3026,3043)、`local_apply_scan`(3231,3241,3242,3245)、`on_local_entry`(3289)、`local_sd_apply`(3365)、`page_local`(3478) |
| 2879 | `s_page` | `int` | 当前页（120 行/页） | `local_render`(3009–3015)、`on_local_entry`(3276,3282–3285)、`local_sd_apply`(3366,3378)、`page_local`(3489) |
| 2880 | `s_local_sd_timer` | `lv_timer_t *` | 500 ms SD 热插拔轮询（**随页面删**） | `page_local`(3465,3466)、`local_page_watch`(3336,3337,3338) |
| 2881 | `s_local_sd_gen` | `uint32_t` | 上次见到的 SD 代次 | `page_local`(3464)、`local_sd_tick`(3411,3414) |

### 2.9 本地扫描线程（8 → 新 `music_source_local`，跨线程 volatile）

| 行 | 名字 | 类型 | 用途 | 主要读写者 |
| --- | --- | --- | --- | --- |
| 3127 | `s_scan_busy` | `volatile bool` | 扫描线程占用中 | `local_scan_task`(3181)、`local_scan_start`(3196,3211,3214)、`local_sd_tick`(3404) |
| 3128 | `s_scan_done` | `volatile bool` | 有新结果待取 | `local_scan_task`(3180)、`local_scan_start`(3212,3218)、`local_sd_tick`(3394,3395)、`local_page_watch`(3334)、`page_local`(3476) |
| 3129 | `s_scan_fail` | `volatile bool` | 本次扫描失败 | `local_scan_task`(3178)、`local_scan_start`(3204,3216)、`local_apply_scan`(3230) |
| 3130 | `s_scan_seq` | `volatile uint32_t` | UI 侧最新请求号 | `local_scan_start`(3195,3205,3210,3217)、`local_sd_tick`(3396,3400)、`local_page_watch`(3333)、`local_sd_apply`(3367)、`page_local`(3475) |
| 3131 | `s_scan_run_seq` | `volatile uint32_t` | 已完成结果的请求号 | `local_scan_task`(3179)、`local_scan_start`(3205,3217)、`local_sd_tick`(3396,3400) |
| 3132 | `s_scan_n` | `volatile int` | 扫到的条目数（<0 失败） | `local_scan_task`(3177)、`local_scan_start`(3203,3215)、`local_apply_scan`(3241) |
| 3133 | `s_scan_pend` | `volatile bool` | 扫描期间的排队请求 | `local_scan_start`(3198)、`local_sd_tick`(3404,3405)、`local_page_watch`(3335)、`page_local`(3477) |
| 3134 | `s_scan_pend_dir[512]` | `char[]` | 排队中的目录 | `local_scan_start`(3197)、`local_sd_tick`(3406) |

### 2.10 局部类型定义（拆分时需要跟着走）

| 行 | 名字 | 说明 |
| --- | --- | --- |
| 128–134 | `cover_req_t` | 封面请求（idx / from_mem / data / len / path[512]） |
| 388–392 | `play_mode_t` | 列表/单曲/随机 |
| 1972–1977 | `online_req_t` | 在线请求（song 拷贝 / idx / prefetch / seq） |
| 3122–3125 | `local_scan_req_t` | 扫描请求（dir / seq） |

---

## 3. 跨模块调用清单

统计口径：`grep` 命中的**真实调用点**（注释里提到的 API 不计入行号，但会在"用来干什么"里说明）。

### 3.1 `audio_service_*`（业务为主，共约 45 个调用点）

| API | 行号 | 干什么 | 性质 |
| --- | --- | --- | --- |
| `audio_service_set_event_cb` | 3579 | 注册 `on_audio_evt`（播完/出错） | 业务·注册 |
| `audio_service_play_path` | 586 | 本地文件起播 | 业务 |
| `audio_service_play_uri` | 2232 | 在线流/下载兜底起播 | 业务 |
| `audio_service_pause` | 1284, 1579 | 媒体键暂停；看门狗踢管线 | 业务 |
| `audio_service_resume` | 1273, 1580, 1591 | 媒体键恢复；踢管线后确认在跑 | 业务 |
| `audio_service_toggle` | 676 | 播放/暂停键 | 业务 |
| `audio_service_stop` | 630, 1288, 1564, 2350 | 换曲/停止/连续失败/点歌先停旧 | 业务 |
| `audio_service_is_playing` | 495, 685, 847, 1544, 1589, 1625, 1632 | 状态判定（UI 文案、看门狗、墙钟、按钮图标） | 业务判定（被绘制复用） |
| `audio_service_output_active` | 1388, 1536, 1544, 1625, 1651 | "真的出声了"：撤缓冲提示、seek 结束、墙钟累加 | 业务判定 |
| `audio_service_get_info` | 1687, 1739 | 取码率/时长（续播定位 + 总时长推导） | 业务 |
| `audio_service_seek_supported` | 835, 1646 | 两道关之一 / 在线续播就绪判定 | 业务 |
| `audio_service_seek_ms` | 852, 1581, 1649, 1712 | 定位（拖动、踢管线兜底、在线续播、本地续播） | 业务 |
| `audio_service_seek_landing_ms` | 1373 | 落点核对 | 业务 |
| `audio_service_set_duration_ms` | 2243 | 告诉服务在线时长（**必须在 `play_uri` 之后**） | 业务 |
| `audio_service_stream_progress` | 1398, 1576, 2503 | "缓冲中 xx%" 百分比 | 业务取数 → 供绘制 |
| `audio_service_stream_speed` | 2502 | 下载速度（**Kbps**，UI 要 ÷8） | 业务取数 → 供绘制 |
| `audio_service_set_output` | 988 | 切换输出设备 | 业务 |
| `audio_service_get_output` | 1010, 1132, 2825, 2830 | 弹窗高亮 / 按钮图标同步 | 业务取数 → 供绘制 |

未使用（本文件无需）：`audio_service_init` / `set_volume` / `get_volume` / `set_mute`（音量面板在 `ui_volume_panel.cc`）。

### 3.2 `storage_service_*` / `bsp_*`（业务，约 20 个调用点）

| API | 行号 | 干什么 |
| --- | --- | --- |
| `storage_service_track` | 506, 1738, 1920, 2705, 2706, 2719 | 取曲目结构（路径/标题/歌手/大小） |
| `storage_service_count` | 644, 1463, 3184, 3542 | 队列长度（换曲/播完判定/日志） |
| `storage_service_load_dir` | 3169, 3542 | 把"当前目录这一层"设为播放队列 |
| `storage_service_browse` | 3151 | 列目录（返回真实条数） |
| `storage_service_index_of_path` | 3314, 3546 | 路径 → 队列下标 |
| `storage_service_sd_is_mounted` | 3360, 3480 | 热插拔/建页判定 |
| `storage_service_sd_generation` | 3410, 3464 | SD 代次（换卡识别） |
| `bsp_sdcard_mount_point` | 3379, 3490 | 挂载点（避免写死 `/sdcard`） |
| `bsp_sdcard_is_mounted` | 3527 | 开机恢复前的检查 |

### 3.3 `media_meta_*`（业务；其中 LRC 索引与封面解码是两类）

| API | 行号 | 干什么 |
| --- | --- | --- |
| `media_meta_lyrics_load` | 544, 2190, 3560 | 三级查找歌词（边车 .lrc / lrc 目录 / 内嵌 USLT） |
| `media_meta_lrc_parse` | 2183 | 解析内存里的在线 LRC 文本 |
| `media_meta_lrc_reset` | 2193, 2302 | 清空歌词（无歌词 / 切歌占位） |
| `media_meta_lrc_index_at` | 1239, 1257 | 时间 → 行号（高亮与同步的**唯一依据**） |
| `media_meta_cover_rgb565` | 1952 | 本地文件内嵌封面解码（工作线程） |
| `media_meta_image_mem_rgb565` | 1950 | 在线封面（内存字节）解码（工作线程） |
| `media_meta_cover_free` | 1785, 1882, 2378, 2385, 2405 | 释放像素（丢结果 / 换封面 / 页面不在） |

### 3.4 `play_state_*`（业务·进度记忆）

| API | 行号 | 干什么 |
| --- | --- | --- |
| `play_state_track_set` | 207（`save_progress` 内） | 唯一的进度写入口（在线流式被 `save_progress` 挡掉） |
| `play_state_track_get` | 3524 | 开机恢复上次曲目+位置 |
| `play_state_commit` | 584, 693, 1303, 1803, 2229 | 换曲/暂停/停止/退页/在线兜底起播时立刻落盘 |

### 3.5 `online_song_*` / `online_service`（业务，全部在工作线程或准备链路）

| API | 行号 | 干什么 |
| --- | --- | --- |
| `online_song_prepare_stream` | 2019, 2039 | 只抓封面/歌词进 PSRAM（预取分支 / 流式成功分支） |
| `online_song_stream_info` | 2029 | 解析 302 拿最终 https 地址 + 时长 |
| `online_song_prepare` | 2035 | 流地址拿不到时的下载兜底 |
| `online_song_cover_data` | 2046 | 取封面字节拷贝（调用方所有） |
| `online_song_lyrics_data` | 2047 | 取 LRC 文本拷贝（调用方所有） |

### 3.6 `bt_service_*`（业务取数 → 绘制）

| API | 行号 | 干什么 |
| --- | --- | --- |
| `bt_service_peer_name` | 957 | 弹窗里显示对端设备名 |
| `bt_service_is_connected` | 964, 1048 | **必须卡这道**：只配对未连接时不显示"已连接" |
| `bt_service_is_streaming` | 1049 | "已连接 · 推流中/未推流" |

### 3.7 UI 层（`ui_core` / `ui_ba` / `ui_app_registry` / 音量）

| API | 行号 | 干什么 |
| --- | --- | --- |
| `ui_toast` | 451, 462, 508, 587, 638, 668, 815, 832, 836, 853, 990, 994, 997, 1114, 1341, 1435, 1439, 1563, 1570, 2139, 2233, 2356, 3297, 3310, 3316, 3363 | 全部用户提示（**26 处**：业务模块也会调，拆分时经 view 端口转发） |
| `ui_volume_show` / `ui_volume_hide` | 704 / 1073 | 音量条（打开输出弹窗前先收起） |
| `ui_ba_open` / `ui_ba_on_close` / `ui_ba_hint` / `ui_ba_close` | 1076 / 1080 / 1086 / 980 | BA 风格弹窗（**`ui_ba_on_close` 传 lambda**，唯一一处 C++ lambda 回调） |
| `ui_router_push` | 3324, 3509 | 压栈播放器页（本地列表点歌后 / 对外 API） |
| `ui_app_register` | 3575, 3576 | 注册 "music" 与 "local" 两个 APP |

### 3.8 LVGL（约 335 处调用）

`lv_obj_*` / `lv_label_*` / `lv_bar_*` / `lv_image_*` / `lv_timer_*` / `lv_indev_*` / `lv_async_call` / `lv_spinner_create` 等；
生命周期相关的高风险 API：`lv_obj_clean`(2449, 3005, 3065, 3232, 3372)、`lv_timer_delete`(3337)、`lv_obj_delete`(经 `ui_ba_close` 间接)、`lv_async_call`(1109, 1123)。

### 3.9 ⚠ "业务逻辑"与"LVGL 绘制"当前的混用点（拆分的核心清单）

下列**业务函数内部直接写 LVGL**，是拆分时必须改造成"调用视图端口"的位置：

| 函数 | 行号区间 | 里面写 LVGL 的部分 |
| --- | --- | --- |
| `apply_src_badge` | 378–384 | 5 处 label/style |
| `apply_play_mode_ui` | 423–430 | 图标文本 + 角标显隐 |
| `start_track_at` | 548–593 | 标题/歌手、进度归零、已播文本、按钮图标 |
| `play_next` | 632–634 | 按钮图标 |
| `on_play_pause` | 686–688 | 按钮图标 |
| `on_seek_press` | 787–794 | 进度条 + 已播文本 |
| `on_seek_release` | 816–817, 891–897 | 进度条 + 已播文本 |
| `media_cmd_tick` | 1305–1307 | 按钮图标 |
| `tick_cb` | 1566, 1752–1777, 1783 | 进度条/时间/按钮/歌词/封面搬运 |
| `out_btn_sync` | 1135 | 按钮图标 |
| `lyric_apply_cur` | 1211–1228 | 高亮样式 + 整列 translate_y |
| `cover_show` | 2370–2409 | 描述符 + `lv_image_set_src` |
| `online_switch_ui_to` | 2272–2303 | 标题/歌手/时间/进度/封面占位/歌词重建 |
| `start_track_online_at` | 2195–2246 | 同上一整套 + 缓冲提示 |
| `app_music_play_online` | 2352–2353 | 按钮图标（很小） |

反向混用（绘制函数里做业务/设备动作）较少：`on_out_pick`(988 调 `set_output`)、`on_local_entry`(3319 调 `start_track` + 3324 `ui_router_push`)、`on_play_pause`/`on_seek_*`/`on_mode_click`（事件即业务入口，属于正常"回调层"）。
**结论**：拆分的关键不是"把函数按名字分堆"，而是在这 15 处把 `lv_*` 调用替换成 `music_view_*()` 端口调用。

---

## 4. 消息 / 回调 / 定时器清单

### 4.1 事件来源总表

| 来源 | 入口（行号） | 运行上下文 | 走向 |
| --- | --- | --- | --- |
| GMF 播放管线完成/出错 | `on_audio_evt`(1142–1157)，注册于 3579 | **GMF 播放任务** | 只置 `s_advance_req` / `s_err_req` → `tick_cb`(1417–1507) |
| 蓝牙 AVRCP 媒体键 | `main.cc:75` → `app_music_on_media_ctrl`(1312–1315) | 蓝牙栈任务 | 置 `s_media_cmd` → `media_cmd_tick`(1261–1310) |
| 触摸/按键（LVGL） | 各 `lv_obj_add_event_cb`（见 4.3） | LVGL 任务 | 直接改状态或调 `play_next` 等 |
| 页面销毁 | `LV_EVENT_DELETE`：2832 / 3461 | LVGL 任务 | `player_on_delete`(1792) / `local_page_watch`(3328) |
| 工作线程产出 | `s_cover_ready`(140) / `s_on_ready`(211) / `s_scan_done`(3128) | 三个 worker | `tick_cb`(1326, 1780) / `local_sd_tick`(3394) |
| SD 热插拔 | `storage_service_sd_generation()`(3410) | LVGL 任务轮询 | `local_sd_apply`(3354) |
| 其它 APP | `app_music_play_online`(2310) / `app_music_online_detach`(305) / `app_music_open_player`(3506) | LVGL 任务（约定） | 直接执行 |
| 开机 | `main.cc:254` → `app_music_register_apps`(3564) | main 任务 | 注册 APP、订阅事件、建 2 个常驻定时器、`restore_last_session` |

### 4.2 定时器

| 定时器 | 周期 | 创建处 | 销毁 | 说明 |
| --- | --- | --- | --- | --- |
| `s_tick` → `tick_cb` | **500 ms**(`TICK_MS` 54) | `page_player`(2835) 与 `app_music_register_apps`(3595) 双保险 | **永不销毁**（1805–1807 明确要求） | 状态机主干；页面不在时也必须跑（"后台播完不续播"就是删它导致的） |
| `s_media_timer` → `media_cmd_tick` | **100 ms** | `page_player`(2839) / `register_apps`(3600) | **永不销毁**（3597–3601） | 蓝牙按键"跟手"；必须常驻 |
| `s_local_sd_timer` → `local_sd_tick` | **500 ms**(`LOCAL_SD_POLL_MS` 3352) | `page_local`(3466) | `local_page_watch`(3337) **随页面删** | 与上面两个约定**相反**，拆分时最易搞错 |
| `lv_spinner` 内部动画 | 自带 | `build_lyric_view`(2608) / `local_show_loading`(3077) | 随父对象 | 不要自己起 timer |

### 4.3 LVGL 事件回调注册表

| 目标对象 | 事件 | 回调 | 行 |
| --- | --- | --- | --- |
| `left`（封面整块） | `CLICKED` | `on_toggle_view` | 2666 |
| `s_p_seek_hit` | `PRESSED` / `PRESSING` | `on_seek_press` | 2741 / 2742 |
| `s_p_seek_hit` | `RELEASED` | `on_seek_release` | 2743 |
| `s_p_seek_hit` | `PRESS_LOST` | `on_seek_lost` | 2747 |
| 模式按钮 | `CLICKED`（经 `make_round_btn`） | `on_mode_click` | 2798 |
| 上一首/播放/下一首/音量 | `CLICKED` | `on_prev` / `on_play_pause` / `on_next` / `on_volume` | 2809–2813 |
| 输出按钮 | **`SHORT_CLICKED`** | `on_output_click` | 2826（必须用 SHORT_CLICKED，见 2820–2823） |
| 输出按钮 | `LONG_PRESSED` | `on_output_long` | 2828 |
| 输出弹窗遮罩 | `CLICKED` | `on_out_scrim` | 1079 |
| 弹窗行 | `CLICKED`（user_data = 输出枚举） | `on_out_pick` | 1062 |
| `ui_ba_on_close` | 关闭回调 | **C++ lambda**(1080) | 1080 |
| `scr`（播放器页） | `DELETE` | `player_on_delete` | 2832 |
| `scr`（本地页） | `DELETE` | `local_page_watch` | 3461 |
| 列表行 | `CLICKED`（user_data = `sel`） | `on_local_entry` | 2996 |
| — | `lv_async_call` | `open_bt_deferred`（还按着会自重排） | 1123 / 1109 |

### 4.4 工作线程

| 线程名 | 创建处 | 栈 / 优先级 | 输入 | 输出（发布顺序敏感） |
| --- | --- | --- | --- | --- |
| `cover` | 1932 `xTaskCreate(cover_task, …)` | 8 KB / 2 | `cover_req_t*`（**独占**，在线封面字节随请求转移） | `s_cover_out*` → `s_cover_ready` → `s_cover_busy=false`；`vTaskDelete(NULL)` |
| `on_fetch` | 2003 | 12 KB / 2（TLS 需要） | `online_req_t*`（歌曲结构拷贝 + seq） | `s_on_cover_data`/`s_on_lrc_data`/`s_on_stream` → `s_on_ready_seq` → **`s_on_ready` 最后置位** |
| `loc_scan` | 3213 | 8 KB / 2 | `local_scan_req_t*`（dir + seq） | `s_scan_n`/`s_scan_fail`/`s_scan_run_seq` → `s_scan_done` → **`s_scan_busy=false` 最后**（3172–3181 的顺序不能改） |

三个线程的共同纪律：**绝不碰 LVGL**；结果交给 LVGL 任务的定时器消费；同一时刻只允许一个实例（`s_cover_busy` / `s_on_busy` / `s_scan_busy`）。

### 4.5 `tick_cb`（1317–1790，474 行）逐段详解

| # | 行号 | 段落 | 干了什么 | 依赖状态 |
| --- | --- | --- | --- | --- |
| 1 | 1323 | 来源徽标兜底 | `apply_src_badge()`（内部去抖） | `s_on_mode`,`s_src_badge_state` |
| 2 | 1326–1347 | 在线 ready / 失败 | `s_on_ready` 且 seq 相符 → `start_track_online_at`；不符 → 丢弃释放。否则 `s_on_req>=0` → 再 kick；`s_on_failed` → Toast + `lyric_apply_mode` | `s_on_ready/_idx/_seq/s_on_seq/s_on_req/s_on_failed` |
| 3 | 1371–1385 | seek 落点核对 | 最多 10 tick 等 `seek_landing_ms`，偏差 >3 s 就对齐 `s_elapsed_ms` + `lyric_resync` | `s_seek_landing_wait` |
| 4 | 1387–1408 | 缓冲提示 | `output_active` → 撤提示（并可能打落点日志）+ `lyric_apply_mode`；否则 `buf_hint_text()` 刷速度/百分比 | `s_buf_show`,`s_seek_buf_pending` |
| 5 | 1417–1448 | 事件消费 | 取走 `s_err_req`/`s_advance_req`；出错先 `online_url_refresh()`（成功则不 advance），否则 `s_fail_streak++`，≥3 停下并清 `s_index`；播满 3 s 清 `s_fail_streak`/`s_url_retry` | `s_fail_streak`,`s_url_retry` |
| 6 | 1460–1507 | 播完换曲 | 先判"提前结束"（`dur>30 s && elapsed+15 s<dur`）→ 重刷+续播（`s_eof_retry` ≤3）；再按 单曲/列表 与 在线/本地 分流到 `play_next` / `start_track` | `s_eof_retry`,`s_play_mode`,`s_on_mode` |
| 7 | 1528–1614 | **卡死看门狗** | 阈值 在线 20 s / 本地 8 s；seek 进行中先豁免（1535–1543，超 20 s 才放手）；本地超时 → Toast + `play_next(+1)`（连续 3 次停下）；在线前 3 次 pause/resume（并确认真的在跑），第 4 次升级为"重刷地址+续播"，刷新额度用完才置 `s_err_req` | `s_stall_ticks/_kicks`,`s_seek_in_flight/_inflight_n`,`s_fail_streak` |
| 8 | 1625–1632 | 墙钟 | **只在 `is_playing && output_active` 时** `s_elapsed_ms += 500`（`s_resume_ms>0` 期间不计）；同步 `s_playing` | `s_resume_ms` |
| 9 | 1645–1668 | 在线断点续播 | `s_on_resume_ms>0 && s_on_mode && playing`：等 `seek_supported` → `seek_ms` 回去（成功则对齐时间、挂落点核对、复位看门狗、`lyric_resync`）；等超 12 tick → 放弃从头放 | `s_on_resume_ms/_wait` |
| 10 | 1678–1722 | 本地续播 | 容器不支持 → 从头；等码率 ≤12 tick（超时改用墙钟）；**到期保护**（`target+5 s ≥ duration` → 从头）；成功 → `seek_ms` + `lyric_resync` | `s_resume_ms/_wait` |
| 11 | 1728–1733 | 进度落盘节流 | 每 10 tick（5 s）调一次 `save_progress`（内部再经 play_state 8 s 防抖） | `s_save_ticks` |
| 12 | 1736–1749 | 总时长推导 | 优先 `info.duration_ms`，否则 `size_bytes*8*1000/bitrate`，<1 s 视为未知 | `s_dur_ms` |
| 13 | 1752–1777 | 控件刷新 | 进度条（千分比，`s_seeking` 时跳过）、已播、总时长、按钮图标、输出图标、歌词高亮 | `s_seeking`,`s_lyric_mode` |
| 14 | 1780–1789 | 封面搬运 | `s_cover_ready` → 下标相符才上屏，否则释放；最后 `cover_kick()` | `s_cover_out*` |

> 这段是**唯一的跨页面状态机**（页面不在时也在跑），因此拆分目标是把 #5–#10 变成**纯逻辑**（可 host 单测），#1/#4/#13/#14 变成**端口调用**。

### 4.6 另两个定时器

**`media_cmd_tick`(1261–1310，50 行)**：取 `s_media_cmd` → 1=PLAY（`resume` 失败则按在线/本地重起播）、2=PAUSE、3=STOP、4=NEXT、5=PREV；2/3 时立刻 `save_progress`+`play_state_commit`；末尾统一刷按钮图标 + 日志。

**`local_sd_tick`(3386–3416，31 行)**：① `s_scan_done` 且 `run_seq==seq` → `local_apply_scan`（否则丢结果并打日志）；② `s_scan_pend && !busy` → 起排队扫描并 return；③ 比对 `storage_service_sd_generation()`，变了才 `local_sd_apply`。

---

## 5. 拆分方案

### 5.1 目标分层（单向依赖）

```
         ┌────────────────────────── music_app.cc（注册/对外 API/定时器/页面描述符）
         │                                   │
         │  安装端口                          │ 注册页面
         ▼                                   ▼
 music_view_port.h ◄──── 逻辑层 ────► 服务层（audio/online/storage/media_meta/play_state/bt）
   （纯函数指针）        music_player.cc
                        music_online.cc         视图层（唯一允许 include lvgl.h）
                        music_source_local.cc   music_view_player.cc / music_view_local.cc / music_view_output.cc
                        music_logic.cc / music_state.cc
```

规则一句话：**逻辑不认识 LVGL，视图不认识 audio/network**，两者只通过 `music_state.h`（数据）与 `music_view_port.h`（动作）握手。

### 5.2 文件划分表

| 新文件 | 预估行数 | 职责边界 | 从现有行迁移 |
| --- | --- | --- | --- |
| `include/music_state.h` | ~190 | **只装"播放核心状态"**：曲目/时间轴/播放模式/进度记忆/在线队列句柄/看门狗/seek 标志/事件中转 + 枚举 + 常量 + **粗粒度状态迁移函数**声明。**零依赖**（只 `<stdint.h>/<stdbool.h>/<stddef.h>`；`online_song_t` 用不透明指针，取值由 `music_online` 提供） | 68–81、99、159–161、169–262、295、334–345、365–366、394、471–476；`play_mode_t`(388) |
| （状态**不**进 `music_state.h` 的部分） | — | 各自留在归属模块：封面 136–144 → `music_cover.h`（`cover_req_t` 128）；播放器控件 102–120、365–366、395–396 → `music_view_player.cc` 内部；输出弹窗 932–935、1099 → `music_view_output.cc`；本地浏览 2873–2881 → `music_view_local.cc`；扫描线程 3127–3134 → `music_source_local.h`（`local_scan_req_t` 3122）；`online_req_t`(1972) → `music_online.h` | 128–134、102–120、365–366、395–396、932–935、1099、2873–2881、3127–3134、1972–1977 |
| `music_state.cc` | ~200 | 迁移函数实现（起播/切歌/seek 完成/看门狗复位/进度节流），派生量（千分比、当前状态文案 490–496） | 199–208、410–417、490–496 |
| `include/music_logic.h` + `music_logic.cc` | ~120 + ~380 | **纯函数**（无 LVGL、无 ESP 头，host 可测）：`fmt_time`、`track_seek_unsupported`、`next_index(mode,cur,n,dir,rand)`、`progress_permille`、`seek_target_ms`、`is_early_eof`、`watchdog_action`、`resume_decision`、`buffer_hint_text`（Kbps→KB/s，含 0.1 精度） | 480–488、720–742、745–764 的算术部分、615/651–660 的选曲算术、1474 的早停判据、1528–1610 的决策分支、1688–1711 的续播判据、2485–2523 的文本部分、1755 |
| `music_player.cc` | ~600 | 播放控制状态机：起播/换曲/播放暂停/事件消费/看门狗/墙钟/双路续播/进度记忆/开机恢复；`tick_cb` 拆成 ≤13 个小函数（见 5.6） | 504–698、1142–1157、1317–1790、3520–3562 |
| `music_online.cc` | ~450 | 在线队列所有权（287–318）、请求序号与准备线程（1979–2086）、地址重刷与断点（2105–2149）、在线起播（2158–2251）、点歌入口（2310–2359） | 271–318、1970–2359 |
| `music_source_local.cc` | ~420 | 本地数据源：目录扫描线程（3137–3222）、结果消费与分页模型（3225–3261、3000–3048 的数据部分）、条目缓冲扩容（2912–2937）、SD 热插拔代次（3386–3416 的数据部分）、目录推导（3533–3541） | 2896–2937、3095–3261、3342–3416 |
| `music_cover.cc` | ~230 | 封面工作线程与像素产权：`cover_kick`/`cover_task`/`cover_reap`、描述符分配与释放策略（**唯一**改动像素所有权的地方） | 122–144、1874–1968、2361–2410 的分配/释放部分 |
| `music_view_player.cc` | ~560 | 播放器页：`page_player` 拆分（分栏/标题/歌词区/进度条/控制排）、歌词渲染与同步（1190–1258、2446–2574）、封面贴图、来源徽标、模式图标、缓冲提示、seek 事件、`player_on_delete` | 908–917、1182–1258、1792–1864、2412–2574、2638–2861 |
| `music_view_local.cc` | ~380 | 本地列表页：`page_local` 拆分、行渲染（2940–2997）、分页渲染（3000–3048）、加载态（3062–3093）、行点击（3263–3325）、页面销毁（3328–3340）、`local_sd_tick` 的 UI 分支 | 2873–2881（控件部分）、2939–3093、3254–3340、3418–3494 |
| `music_view_output.cc` | ~240 | 输出设备弹窗整组 + 音量键 + 按钮图标同步 | 700–705、919–1137、2825–2830 |
| `music_app.cc`（或保留名 `app_music.cc`） | ~200 | 对外 5 个 API、`app_music_register_apps`(3564–3604)、两个常驻定时器的创建、`page_player`/`page_local` 的描述符、端口安装 | 305–318、1312–1315、2310–2359（转发）、3497–3604 |
| `music_view_null.cc`（**仅 host 单测**） | ~80 | 视图端口空实现（每个函数只记录调用，供断言） | 新增 |
| **合计** | **≈4040** | 比现在多约 12%：多出来的是头文件、端口层与跨模块边界检查 —— 这是分离逻辑与绘制的固定成本 | — |

### 5.3 共享状态如何组织：`music_state.h`

**不要**给 100 个变量各配一对 getter/setter（那等于把全局变量换个写法，仍然无法约束时序）。建议：**一个 POD 状态结构 + 少量"状态迁移"函数**。

```c
/* include/music_state.h —— 零依赖（不 include lvgl.h / ui_metrics.h / esp_err.h） */
#define MUSIC_TRACK_PATH_MAX 600      /* 沿用 98 行的 TRACK_PATH_MAX */
#define MUSIC_STREAM_MAX     560      /* 沿用 173 行 */

typedef enum { PLAY_MODE_LIST = 0, PLAY_MODE_ONE, PLAY_MODE_SHUFFLE } play_mode_t;
typedef enum { MUSIC_SRC_LOCAL = 0, MUSIC_SRC_ONLINE_STREAM, MUSIC_SRC_ONLINE_CACHED } music_src_t;

typedef struct {
    /* 曲目与时间轴 */
    int      index;                       /* s_index */
    char     track_path[MUSIC_TRACK_PATH_MAX];   /* s_track_path */
    uint32_t elapsed_ms, dur_ms;          /* s_elapsed_ms / s_dur_ms */
    bool     playing;                     /* s_playing */
    play_mode_t play_mode;                /* s_play_mode */
    /* 进度记忆 */
    uint32_t resume_ms; int save_ticks, resume_wait;
    /* 歌词（lrc_t 由 music_lyric 适配，避免把 esp_err.h 拉进来） */
    music_lyric_t lrc; int lyric_cur; bool lyric_mode;
    /* 来源 */
    music_src_t src;                      /* 由 (on_mode, on_stream_local) 派生 */
    /* 在线队列句柄（数组本体归 music_online 所有）。
     * ⚠ online_song_t 是**匿名结构的 typedef**（online_service.h:52），无法前置声明：
     *   要么在这里存 `const void *` 由 music_online 解引用（推荐，保持零依赖），
     *   要么让 music_state.h include online_service.h —— 后者只依赖 stdbool/stdint/esp_err，
     *   **不含 lvgl**，可接受，但 host 单测要给 esp_err.h 一个最小 stub。 */
    const void *on_songs; size_t on_count; bool on_mode;
    char     on_stream[MUSIC_STREAM_MAX]; bool on_stream_local; uint32_t on_dur_ms;
    /* 看门狗与重试 */
    int stall_ticks, stall_kicks, url_retry, eof_retry, fail_streak;
    uint32_t on_resume_ms; int on_resume_wait;
    /* seek 时序 */
    bool seeking, buf_show, seek_buf_pending, seek_in_flight; int seek_landing_wait, seek_inflight_n;
    /* 事件中转（跨任务，保持 volatile） */
    volatile bool advance_req, err_req; volatile int media_cmd;
} music_state_t;

music_state_t *music_state(void);                 /* 固件唯一实例 */
void music_state_begin_local(music_state_t *, int idx, const char *path, uint32_t resume_ms);
void music_state_begin_online(music_state_t *, int idx, const char *uri, uint32_t dur_ms);
void music_state_clear_online(music_state_t *);   /* 切回本地（等价 515–525） */
void music_state_note_seek_started(music_state_t *, uint32_t target);
void music_state_note_seek_landed(music_state_t *, uint32_t ms);
void music_state_note_output_active(music_state_t *);   /* 撤缓冲提示 + 清 seek 标志 */
void music_state_reset_watchdog(music_state_t *);
uint32_t   music_state_progress_permille(const music_state_t *);
```

要点：
- **`music_state_t` 只放 POD**，`lrc_t` 通过 `music_lyric_t`（自带 `count/timed/行数组`）适配，`music_lyric.cc` 负责调用 `media_meta_lrc_*`。这样 `music_logic.cc` + `music_state.cc` 能**脱离 ESP-IDF 编译**（host 单测的前提）。
- 需要 volatile 语义的字段（`advance_req`/`err_req`/`media_cmd`，以及 `music_cover` / `music_online` / `music_source_local` 里的 8+8+9 个跨线程标志）**保持 volatile 原样**，不要为了"干净"改成普通 `bool`，也不要在访问器里加锁（现有设计是无锁 + 序号校验）。
- `s_tick` / `s_media_timer` / `s_local_sd_timer` / `s_out_*` / 控件指针**不进状态结构**：它们属于视图层，只在视图模块内部可见 —— 这直接消灭了"业务模块误删定时器""业务模块误用已销毁对象"的可能。

### 5.4 业务 → 绘制的唯一通道：`music_view_port.h`

```c
/* include/music_view_port.h —— 只依赖 music_state.h */
typedef struct {
    void (*now_playing)(const char *title, const char *artist);
    void (*progress)(int32_t permille, uint32_t elapsed_ms, uint32_t dur_ms);
    void (*play_icon)(bool playing);
    void (*src_badge)(music_src_t src);
    void (*play_mode)(play_mode_t mode);
    void (*buffering)(bool show, const char *text);   /* text 可为 NULL */
    void (*lyric_rebuild)(void);
    void (*lyric_sync)(uint32_t elapsed_ms);
    void (*cover_show)(uint8_t *px, int w, int h);    /* 所有权转移：NULL = 回占位 */
    void (*cover_reap)(void);
    void (*toast)(const char *text, uint32_t ms);
} music_view_port_t;

void music_view_install(const music_view_port_t *port);   /* 由 music_app.cc 调用一次 */
const music_view_port_t *music_view(void);               /* 逻辑层调用；未安装时用空实现 */
```

约定：
1. 端口在 `app_music_register_apps()` 里**安装一次并且永不卸载**（因为 `tick_cb` 常驻）；
2. 每个端口函数内部对"页面不存在"做空操作 —— 这正是现有 `if (s_p_xxx)` 保护的等价形式（`player_on_delete` 已把 15+ 个指针置空）；
3. 端口**不返回错误码**，也不阻塞；`ui_toast` 直接作为端口暴露（26 处调用点全部改走它）；
4. `tick_cb` 里 `audio_service_*` 取数（如 `is_playing` / `output_active` / `stream_speed`）属于**业务**，留在逻辑层取数后按值传给端口 —— 这样逻辑层的测试可以用 fake 服务替换。

### 5.5 头文件依赖与防循环规则

| 层 | 允许 include | 禁止 |
| --- | --- | --- |
| `music_state.h` | `<stdint.h> <stdbool.h> <stddef.h>` | lvgl.h、esp_err.h、ui_metrics.h、任何服务头（在线队列句柄用 `const void *`，因为 `online_song_t` 是**匿名结构 typedef**，不能前置声明） |
| `music_logic.h` | `music_state.h` | 其它一切 |
| `music_view_port.h` | `music_state.h` | lvgl.h（**这是"业务不依赖 LVGL"的关键闸门**） |
| 逻辑 `.cc`（player/online/source_local/logic/state） | `music_state.h` `music_logic.h` `music_view_port.h` + 服务头（audio/online/storage/media_meta/play_state/bt/esp_log） | **任何 `music_view_*.h`** |
| 视图 `.cc`（view_player/local/output） | `music_view_port.h` `music_state.h` `music_logic.h`（只读纯函数）+ `lvgl.h` `ui_metrics.h` `ui_core.h` `ui_ba.h` `ui_app_registry.h` | audio/online/storage 服务头（**视图不认识播放器**） |
| `music_app.cc` | 以上全部 + `app_music.h` | — |

**循环包含的根治办法**：依赖方向固定为 `view → port ← logic`，两侧都只依赖 `port`，因此结构上不可能出现回边；`music_app.cc` 是唯一同时看见两侧的翻译单元（它只做"安装 + 注册"，不含业务与绘制）。

### 5.6 迁移顺序（每步都能编译 + 真机验证）

| 步骤 | 动作 | 验证点 |
| --- | --- | --- |
| S1 | 新建 `music_logic.cc`，先搬 **3 个纯函数**：`fmt_time`(480)、`track_seek_unsupported`(720)、`buf_hint_text` 的文本部分(2485)；同时建 host 单测 | 单测：`fmt_time` 边界（0/超 24 h/59 s 进位）、`.mp3?vuutv=…` 判定、Kbps→KB/s（320 kbps 必须显示 40.0 KB/s 而非 320） |
| S2 | 建 `music_state.h/.cc`，把 100 个静态变量**机械搬迁**（可先用 `#define s_index MS->index` 过渡，确认行为不变后再删宏）；`tick_cb` 暂不动 | 编译通过 + 真机跑一遍 7 条主路径（本地播放/在线播放/拖动定位/循环模式/蓝牙按键/退页再进/拔插卡） |
| S3 | 拆封面：`music_cover.cc`（线程 + 产权）+ 视图侧贴图 | 快速连续切 5 首不泄漏（`heap_caps_get_free_size` 前后对比）、退页再进封面还在 |
| S4 | 拆在线：`music_online.cc`（队列/准备/重刷/续播） | 点歌即切、403 重刷续播、断流提前结束续播、播放地址刷新次数上限 |
| S5 | 拆本地：`music_source_local.cc` + `music_view_local.cc` | 大目录秒进、翻页瞬发、换目录不串页、拔卡/插卡刷新 |
| S6 | 拆播放器视图：`music_view_player.cc` / `music_view_output.cc` / `music_app.cc`；把 `tick_cb` 切成 **13 个小函数**：#1 `tick_src_badge`、#2 `tick_online_ready`、#3 `tick_seek_landing`、#4 `tick_buffer_hint`、#5 `tick_consume_events`、#6 `tick_on_finished`、#7 `tick_watchdog`、#8 `tick_advance_clock`、#9 `tick_resume_online`、#10 `tick_resume_local`、#11 `tick_save_progress`、#12 `tick_derive_duration`（并入 #13 刷新）、#13 `tick_refresh_widgets` + `tick_pump_cover` | 每个新函数 ≤60 行；S2 的 7 条路径全部重跑；`tick_cb` 本体降到 ≤50 行的调度器 |

S6 完成后 `tick_cb` 的预期形态（示意，行号按新文件）：

```c
static void tick_cb(lv_timer_t *t) {          /* 只做调度，不再含业务分支 */
    (void)t;
    tick_src_badge();          tick_online_ready();
    tick_seek_landing();       tick_buffer_hint();
    tick_consume_events();     tick_on_finished();
    tick_watchdog();           tick_advance_clock();
    tick_resume_online();      tick_resume_local();
    tick_save_progress();      tick_refresh_widgets();
    tick_pump_cover();
}
```

### 5.7 构建与 host 单测

```cmake
# components/apps/app_music/CMakeLists.txt（拆分后）
idf_component_register(
    SRCS
        "music_app.cc"            # 对外 API + 注册 + 端口安装
        "music_state.cc"  "music_logic.cc"
        "music_player.cc" "music_online.cc" "music_source_local.cc" "music_cover.cc"
        "music_view_player.cc" "music_view_local.cc" "music_view_output.cc"
    INCLUDE_DIRS "include"
    REQUIRES lvgl ui_core bsp audio_service storage_service media_meta
             play_state bt_service online_service app_settings
)
```

host 侧单测（**新增，不影响固件构建**）：单独一个 `tools/host_test/music_logic/` CMake 工程，只编译
`music_logic.cc` + `music_state.cc` + `music_view_null.cc` + fake 服务（把 `audio_service_*` / `media_meta_*` / `play_state_*` 用同名 stub 实现），覆盖：
① `fmt_time`/千分比/`seek_target_ms` 边界；② 容器白名单；③ `next_index`（列表/单曲/随机，n=1、n=0）；④ 提前结束判据（1474 行的 `>30 s` / `+15 s` 阈值）；⑤ 看门狗决策（8 s/20 s、seek 豁免、3 次踢后升级）；⑥ 续播到期保护（`RESUME_END_GUARD_MS`，1707–1711）；⑦ 缓冲提示文本（含"无速度时不显示 0 KB/s"）。

> 注意 `ui_metrics.h` **依赖 lvgl**（第 65 行起用 `lv_color_hex` 定义颜色宏）—— 所以逻辑模块连 `ui_metrics.h` 都不能 include，尺寸/颜色只出现在视图层。

---

## 6. 风险清单

按"拆分时最容易引入回归"排序，★ = 关键（必须写进 review checklist）。

### ★ R1 `tick_cb` 是跨页面存活的唯一状态机，页面销毁的置空不变式极易被拆散
`tick_cb` 由 `app_music_register_apps()`(3594) 常驻创建，页面只是"显示"（1805–1807 明确注释：删掉它 = 后台播完不续播、耳机按键没人理）。
`player_on_delete`(1809–1834) 一次清掉 **17 个控件指针**（1809–1815）+ `s_src_badge_state=-1`(1816) + `s_lyric_mode/s_seeking/s_p_lyric_n`(1817–1819) + `memset(s_p_lyric_labels)`(1820)，这是"定时器往已销毁对象写"的**唯一防线**。
拆分后若把"清指针"分散到各视图文件而漏掉一个（尤其 `s_p_src_badge` / `s_p_buf_spin` / `s_p_mode_badge` 这三个只写一两次的），表现是**概率性 hard fault**（LVGL 事件/定时器访问已释放对象），且只在"退页→再进"时复现。
→ 对策：视图层用**一个结构体**装全部控件指针，销毁时 `memset` 整个结构体；逻辑层通过端口访问，端口内部统一判空。

### ★ R2 LVGL 对象与"非 LVGL 资源"的生命周期是**故意错位**的
- 封面像素与描述符（`s_cover_pixels` 137 / `s_cover_dsc` 136）**页面销毁时刻意不释放**（1825–1834 的说明），下次进页 `lv_image_set_src` 直接贴回（2852–2854）；释放只发生在换曲时的 `cover_show(NULL,0,0)`。
- `cover_show` 的顺序不可改：**先 `lv_image_set_src(NULL)` 摘旧 src，再 free 描述符，最后 free 像素**（2370–2380）；描述符**每张新分配**而不是复用（2364–2366：LVGL 图片缓存按指针做键）。
- 输出弹窗挂在 `lv_layer_top`（不随页面销毁），必须由 `player_on_delete`(1797) 显式关；关闭一律 `lv_obj_delete_async`（926–927）—— 回调是弹窗自己的子对象发出的，同步删会在事件派发中抹掉子树（历史 hard fault F4）。
- `local_page_watch`(3337) 反而**必须删** `s_local_sd_timer`，与播放器页"永不删定时器"完全相反。
→ 对策：把 `music_cover.cc` 定为**像素所有权的唯一 owner**，视图层只通过 `cover_show()`/`cover_reap()` 交接；在头文件上写死"谁 free 谁"的注释（现状靠 122–144 的长注释，拆开后注释会被拉远）。

### ★ R3 静态变量被多处写（100 个变量里有一批是"多写者"）
按"赋值点个数"实测（`grep` 全文核对，已排除声明行与注释）：
`s_elapsed_ms` **13 处**、`s_playing` **10 处**、`s_seeking` **8 处**、`s_seek_buf_pending` **7 处**、`s_dur_ms` **7 处**、`s_on_resume_ms` **7 处**、`s_index` **6 处**、`s_resume_ms` **6 处**、`s_on_mode`/`s_stall_ticks`/`s_stall_kicks`/`s_url_retry`/`s_eof_retry`/`s_buf_show`/`s_lyric_cur`/`s_fail_streak` 各 **3–5 处**。
其中三处有**隐式时序约定**，拆成 setter 后极易破坏：
① `s_on_seq`（179）只在 LVGL 任务自增（618、2143、2322 三处 `++`），工作线程只读一次比较（2051、2066、1330）；
② `s_lyric_cur`（73）必须在赋值**前**读出旧值才能复位上一行（1182–1189、1192–1193、1250）；
③ `s_seek_*` 五个标志 + `s_stall_*` 必须成套复位（873–885、1649–1653）。
→ 对策：**只提供粗粒度迁移函数**（5.3 的 `music_state_note_*`），并对上述三处写不变量注释；`s_scan_seq = s_scan_seq + 1` 的写法必须保留（volatile 不能用 `++`，C++20 起 deprecated + `-Werror`，3195/3333/3367/3475 四处）。

### ★ R4 工作线程的"发布顺序"和"内存所有权转移"经不起搬运
- 三个 worker 都是"先写数据 → 最后置 done/busy"（3172–3181 注释明确：反过来会出现两个线程同写 `s_entries`）。
- `online_prepare_task` 发布的是**所有权转移**：`s_on_cover_data/s_on_lrc_data` 交给 LVGL 任务，只有它在消费/丢弃时 free（2073–2081、`online_discard_ready` 271–285）；`cover_kick` 又把封面字节**转移**进解码请求（1911–1915），由线程内释放。
- `s_on_songs` 由 `app_online` 交接（287–318）：`app_music_online_detach` 里"先 `online_owned_drop()` 再接管"、"先让 `s_on_songs` 指向新数组再 free 旧的"（2318–2320）顺序不能变。
→ 对策：把队列所有权集中到 `music_online.cc` 一个 owner；`music_cover.cc` 独立持有像素产权；禁止在别的模块 free 这两类指针（用头文件注释 + review checklist 兜）。

### R5 `on_audio_evt` 运行在 GMF 播放任务上下文
只能置标志（1141、1413–1415 的注释：在回调里重入播放器 = 崩溃）。拆分后若有人把 `music_view_*` 或 `audio_service_*` 调用写进事件回调就废了。同理 `app_music_on_media_ctrl`(1312) 可在任意任务上下文调用。

### R6 `tick` 里"墙钟只在真出声时累加"是歌词同步的唯一依据
1625–1631：`is_playing && output_active` 才 `+= 500`，且 `s_resume_ms > 0` 期间不累加。任何"顺手每分钟 tick 都加一下"的改动都会让歌词整体提前（缓冲越久偏得越多），而且**只在慢网络下可见**。

### R7 看门狗阈值与 seek 标志的耦合
在线 20 s / 本地 8 s（166–167）、`SEEK_STALL_MAX_TICKS`(163) 20 s、踢 3 次升级（1573）、`s_fail_streak` 3 次停下（1434/1561）；本地超时必须用 `play_next(+1)` 而**不能**置 `s_advance_req`（1555–1556：单曲循环下会把同一个坏文件再放一遍 = 死循环）。这些分支被拆到不同文件后，"为什么不用 advance_req"的注释很容易留在原地而代码搬走。

### R8 长按/短按事件语义与延迟跳页
输出按钮短按必须挂 `LV_EVENT_SHORT_CLICKED` 而非 `CLICKED`（2820–2823：长按后 `CLICKED` 无条件补发 → 既跳设置页又开弹窗）；长按跳页必须等手指抬起（1093–1100 的 `lv_async_call` 自重排）。视图拆分时若把 `out_btn` 的创建与事件注册分到两个文件，极易改回 `CLICKED`。

### R9 字符串与缓冲边界
`s_track_path[600]`(99) 必须装得下 `s_on_stream[560]`(173)（90–98 的注释：`-Werror=format-truncation` 抓过 512<560）；`s_on_stream[560]` 与 `audio_service` 的 `uri[600]` 对齐；`s_cur_dir`(2875) / `s_scan_pend_dir`(3134) / `cover_req_t.path`(133) 都是 `STORAGE_PATH_MAX`。拆模块后 `snprintf` 的尺寸参数容易出现 `sizeof(指针)` 的经典错误（尤其把数组当参数传时）。

### R10 `esp_lv_adapter.h` 是**疑似无用包含**（34 行）
全文件没有任何 `esp_lv_adapter_*` 调用（已 grep 确认）。拆分时可以顺手清掉，但**必须编译验证**（LVGL 可能经 `ui_core.h` 间接进来）。属于低风险 cleanup，不要和结构性改动混在一次提交里。

### R11 其它易漏点（清单式）
- `label_set_if_changed`(1170) 的"读 label 现值比较"策略：拆分后若改成缓存文本，会与"切歌/暂停直接写 label"的路径不同步。
- `lyric_apply_mode`(2549) 兼任"藏缓冲转圈"，被 `tick_cb`/`start_track_online_at`/`lyric_rebuild` 依赖（"出声/换曲"语义）—— 名字与职责不符，改名时别丢掉这个副作用。
- `online_prepare_kick` 的 `s_on_prefetch`(223) 是**死代码**（无入口置真），拆分时要么删除要么补注释，别顺手接到 tick 上（会重新引入"占住唯一准备槽位 1–3 秒"的问题，1348–1349）。
- `app_music_online_detach` 只被 `app_online.cc:856` 调用；`app_music_open_player` 被 `app_online.cc:1001` 调用 —— 改文件名时**对外 5 个符号名必须保持不变**，否则要同时改 `app_online.cc`、`main.cc`。

---

## 7. 拆分后的验证清单

| 验证 | 方法 |
| --- | --- |
| 编译 | `idf.py build` 通过（贴结果摘要，不只看"已完成"） |
| 配平自查 | `tools/font/venv/Scripts/python.exe tools/check_balance.py <新文件...>`（不是编译验证，抓漏括号） |
| 主机单测 | `tools/host_test/music_logic` 全绿（5.7 的 7 类用例） |
| 真机 7 条主路径 | ① 本地播放 → 拖动定位 → 退出重进（封面/进度还在）② 在线点歌 → 缓冲提示带速度/百分比 → 出声后提示消失 ③ 在线 403/断流 → 日志 `online url stale/expired … refreshing` + `online resume at N ms` ④ 在线单曲循环/列表循环 ⑤ 蓝牙耳机 PLAY/PAUSE/NEXT/PREV 在**非播放器页**生效 ⑥ 本地音乐进大目录（秒进 + 转圈真的转 + 翻页瞬发）⑦ 拔卡/插卡自动刷新 |
| 泄漏回归 | 连续切 10 首 + 退页/进页 5 次，比 `esp_get_free_heap_size()` / `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` 差值（封面 240×240 RGB565 ≈ 115 KB 是重点） |
| 无 UI 时状态机 | 只进首页（不进播放器页）让在线歌播完，确认自动续播（R1 的回归哨兵） |

---

*报告基于 app_music.cc 3604 行全文逐段阅读 + grep 统计生成；行号可直接在编辑器中跳转核对。本次审计未修改任何现有源文件。*
