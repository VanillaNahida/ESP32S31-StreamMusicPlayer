# 04 `app_online.cc` 拆分重构蓝图

> 审计对象：`components/apps/app_online/app_online.cc`
> 真实行数：**3157 行**（LF 计数，与 `read`/`grep` 行号一致；用户口头说的"约 2711 行"不准，
> pwsh 的 `Get-Content | Measure-Object -Line` 也会少算）
> 头文件：`components/apps/app_online/include/app_online.h`（仅 `app_online_register_apps()`）
> 本报告为只读审计产物，未修改任何源文件。

---

## 1. 文件概览

### 1.1 include（18–42 行）

| 类别 | 内容 |
| --- | --- |
| 标准库 | `stdio.h` `string.h` `stdlib.h` |
| ESP-IDF | `esp_log.h` `esp_heap_caps.h` `freertos/FreeRTOS.h` `freertos/task.h` `freertos/queue.h` |
| 工程 UI 层 | `ui_core.h` `ui_metrics.h` `ui_ba.h`（碧蓝档案风格对话框/二次确认工具箱）`ui_app_registry.h` `ui_icons.h`（`netease_logo` 位图） |
| 服务层 | `app_music.h`（`app_music_play_online` / `app_music_online_detach` / `app_music_open_player`）、`audio_service.h`（自检用）、`online_service.h`、`media_meta.h`（二维码 PNG 解码）、`wifi_service.h` |
| 第三方 | `lvgl_pinyin_ime/lv_pinyin_ime.h` |

**没有** include `esp_lvgl_adapter` —— 本文件**完全不用锁**（见 §5）。LVGL 调用全部发生在
LVGL 任务里的控件构建/事件回调；网络全部在自建 `online_work` 任务里。

### 1.2 结构分区

| 区间 | 分区 |
| --- | --- |
| 1–100 | 文件头契约（需求/线程模型）+ 宏（`WORK_TASK_STACK 16 KB`、`POLL_MS 200`、`SEARCH_MAX 200`、分页宏、`IME_FIT_HEIGHT`、`ONLINE_SELFTEST 0`） |
| 102–155 | `#if ONLINE_SELFTEST` 在线链路自检任务（默认关） |
| 157–206 | 工作线程：`oreq_kind_t`（12 种请求）、`oreq_t`、请求队列、**结果槽位** |
| 208–255 | 账号页状态 + 二维码浮层状态 |
| 266–459 | 二维码登录浮层（`qr_*`）+ 账号状态行 |
| 461–635 | `work_publish` / `online_work_task` / `work_submit(_ex)` |
| 637–815 | 页面骨架（三页 tab、两页滑动动画）、详情身份 Key（`detail_*`）、分页数学（`page_*`/`acct_page_count`） |
| 845–1002 | 队列所有权（`queue_replace`/`online_install`/`online_drop_stale`）、账号歌单懒加载（`acct_lazy_*`）、点行起播 |
| 1004–1218 | 行构造与列表工具（`make_row`/`fill_song_rows_range`/`page_rows`/`make_back_label`/`loading_rows`/`detail_wait_rows`）+ 手势切页 |
| 1220–1602 | 第 0 页「我的歌单」：账号页（登录态、每日推荐、账号歌单 + 详情懒加载、退避重试） |
| 1604–1949 | 第 1 页「我添加的歌单」：详情、长按菜单（重命名/更新/删除）、重建 |
| 1951–2217 | 第 2 页「搜索」：历史、结果分页、输入法交互 |
| 2219–2287 | 三页滑动动画与 tab |
| 2289–2514 | 浮层（添加歌单 / 重命名 / 搜索）+ 拼音键盘/自带键盘 |
| 2516–2826 | `on_result()`（**271 行的结果分发大函数**）+ `poll_cb` |
| 2828–3157 | 页面销毁清理 / `page_online()` 控件树 / 注册与任务创建 |

---

## 2. 全部函数清单（96 个顶层定义）

（263/265/267/268、832–843、1647、1690/1691 是**前向声明**，不计入。）

| 起始 | 结束 | 行数 | 函数名 | static | 分类 | 一句话职责 |
| --- | --- | --- | --- | --- | --- | --- |
| 103 | 154 | 52 | `online_selftest_task` | ✔ | 自检（`#if 0`） | 开机 12 s 后跑"抓歌单→落盘→读回→下载→封面→起播"全链路自检 |
| 270 | 275 | 6 | `qr_status_set` | ✔ | 二维码 UI | 改二维码浮层状态行文字 |
| 277 | 283 | 7 | `qr_stop_poll` | ✔ | 二维码 UI | 删轮询定时器 |
| 286 | 325 | 40 | `qr_show_png` | ✔ | 二维码 UI | PNG→RGB565（`media_meta_image_mem_rgb565`）→建 `lv_image_dsc_t` 贴上（**接管 png 并 free**） |
| 327 | 333 | 7 | `qr_poll_cb` | ✔ | lv_timer 回调 | 每 1.5 s 投 `OREQ_QR_POLL` |
| 335 | 352 | 18 | `qr_close` | ✔ | 二维码 UI | 停轮询、`ui_ba_close`、释放 `s_qr_dsc`/`s_qr_px`、清指针 |
| 354 | 358 | 5 | `qr_cancel_clicked` | ✔ | 事件回调 | 取消 → `qr_close()` |
| 360 | 367 | 8 | `qr_refresh_clicked` | ✔ | 事件回调 | 清 key、提示"正在获取"、重新下单 `OREQ_QR_KEY` |
| 369 | 401 | 33 | `qr_open` | ✔ | 二维码 UI | 建二维码浮层（防重入）、挂关闭回调、建图/状态/按钮、下单取码 |
| 403 | 407 | 5 | `login_clicked` | ✔ | 事件回调 | 「扫码登录」→ `qr_open()` |
| 410 | 414 | 5 | `logout_confirmed` | ✔ | 二次确认回调 | 确认后投 `OREQ_LOGOUT` |
| 416 | 421 | 6 | `logout_clicked` | ✔ | 事件回调 | 弹二次确认「退出登录」 |
| 424 | 459 | 36 | `acct_row_build` | ✔ | LVGL 构建 | 画账号状态行（昵称 / 扫码登录 / 退出登录） |
| 461 | 484 | 24 | `work_publish` | ✔ | **线程协议** | 发布结果到静态槽位；页面已死则就地释放（**kind 最后写，作发布标志**） |
| 486 | 612 | **127** | `online_work_task` | ✔ | **工作线程** | 自建任务主循环：等上一份结果被搬走 → 按 `kind` 调 online_service → `work_publish` |
| 614 | 630 | 17 | `work_submit_ex` | ✔ | 线程协议 | 组 `oreq_t` 入队（队列满 → Toast「上一个操作还没做完」） |
| 632 | 635 | 4 | `work_submit` | ✔ | 线程协议 | `work_submit_ex(kind, arg, 0, 0)` 薄包装 |
| 715 | 730 | 16 | `detail_want_key` | ✔ | **纯逻辑** | 当前详情要的 Key（歌单 ID / 账号歌单 ID / `DETAIL_KEY_DAILY` / 空串） |
| 733 | 737 | 5 | `detail_key_matches` | ✔ | **纯逻辑** | `s_cur_pl_id == want` 且缓冲非空 |
| 740 | 743 | 4 | `detail_owns_songs` | ✔ | **纯逻辑** | Key 匹配且已有歌可画 |
| 761 | 764 | 4 | `page_first` | ✔ | **纯逻辑** | 第 page 页第一首的绝对下标 |
| 766 | 772 | 7 | `page_count` | ✔ | **纯逻辑** | 总首数 → 页数（空列表也算 1 页） |
| 774 | 781 | 8 | `page_clamp` | ✔ | **纯逻辑** | 页码夹取到 `[0, pages-1]` |
| 791 | 801 | 11 | `acct_page_count` | ✔ | **纯逻辑** | 账号歌单页数；总数未知且末页填满时**多给一页**（防"只能看到前 100 首"） |
| 812 | 815 | 4 | `detail_mark_request` | ✔ | 状态 | 记下详情请求时刻（20 s 超时兜底起点） |
| 853 | 859 | 7 | `online_drop_stale` | ✔ | **队列所有权** | 把被替换下来的旧队列交 `app_music_online_detach()`（**绝不自己 free**） |
| 870 | 894 | 25 | `queue_replace` | ✔ | **队列所有权** | 换 `s_songs`（旧的挪进 `s_stale`）、复位懒加载/分页状态、按契约拷 Key |
| 897 | 900 | 4 | `online_install` | ✔ | 队列 | 整份安装（meting 歌单 / 搜索 / 每日推荐） |
| 912 | 932 | 21 | `acct_lazy_begin` | ✔ | **懒加载** | 按 `trackCount` 一次性 `calloc` 缓冲（**指针从此不变**），置 `s_acct_lazy` |
| 935 | 947 | 13 | `acct_lazy_append` | ✔ | **懒加载** | 把一页追加到已载入末尾（非连续则丢弃并告警） |
| 954 | 988 | 35 | `acct_detail_fetch_if_needed` | ✔ | **懒加载** | 唯一入口：当前页缺歌时按 `ACCT_FETCH_MAX` 补一段（`s_fetch_busy` 防重入） |
| 992 | 1002 | 11 | `online_row_clicked` | ✔ | 播放 | 点歌行 → `app_music_play_online(s_songs, n, idx)` + `app_music_open_player()` |
| 1014 | 1023 | 10 | `pager_gesture` | ✔ | 手势 | 左右横滑 → `page_show(±1)` |
| 1026 | 1031 | 6 | `pager_attach` | ✔ | 手势 | 给控件挂 `LV_EVENT_GESTURE` |
| 1033 | 1079 | 47 | `make_row` | ✔ | LVGL 构建 | 通用列表行（图标块 + 标题 + 右侧副标题），返回行对象 |
| 1082 | 1090 | 9 | `fill_song_rows_range` | ✔ | LVGL 构建 | 画 `s_songs[first, first+count)` 段，行上挂下标 |
| 1095 | 1100 | 6 | `list_scroll_top` | ✔ | LVGL 工具 | 列表重建后回到顶部 |
| 1103 | 1123 | 21 | `page_row_clicked` | ✔ | 事件回调 | 上一页/下一页（`user_data = (which<<1)|dir`）→ 改页号 + 重建 + 归顶 |
| 1126 | 1138 | 13 | `page_rows` | ✔ | LVGL 构建 | 画"上一页/下一页"两行 |
| 1152 | 1163 | 12 | `make_back_label` | ✔ | LVGL 构建 | 详情页"‹ 返回"行（**挂在页容器上，不放进可滚动列表**） |
| 1170 | 1194 | 25 | `loading_rows` | ✔ | LVGL 构建 | 统一"转圈 + 一行字"占位 |
| 1202 | 1218 | 17 | `detail_wait_rows` | ✔ | LVGL 构建 | 详情占位：转圈；超 20 s 换成"重试"入口 |
| 1225 | 1233 | 9 | `acct_back_clicked` | ✔ | 事件回调 | 账号详情返回列表 |
| 1235 | 1246 | 12 | `acct_daily_clicked` | ✔ | 事件回调 | 进"每日推荐"详情 + 下单 `OREQ_DAILY` |
| 1248 | 1274 | 27 | `acct_pl_clicked` | ✔ | 事件回调 | 进账号歌单详情：`acct_lazy_begin` + 补第一页 |
| 1276 | 1280 | 5 | `acct_login_clicked` | ✔ | 事件回调 | 账号页登录入口 → `qr_open()` |
| 1286 | 1295 | 10 | `login_name_sync` | ✔ | 状态 | NVS 昵称 → `s_login_name` 补齐（不覆盖界面上的新值） |
| 1300 | 1312 | 13 | `acct_backoff_update` | ✔ | 状态机 | 失败翻倍退避（上限 60 s），成功复位 5 s |
| 1314 | 1341 | 28 | `acct_kick_if_needed` | ✔ | 状态机 | 首次拉 / 退避到点重拉账号歌单（`s_acct_pls_*`） |
| 1344 | 1372 | 29 | `acct_reload_clicked` | ✔ | 事件回调 | 详情"重试"：区分"缓冲是别人的"（重建）与"这页没取到"（只补页） |
| 1375 | 1390 | 16 | `acct_nick_kick_if_needed` | ✔ | 状态机 | 无昵称时拉 `OREQ_ACCOUNT`，失败按退避重试 |
| 1392 | 1602 | **211** | `online_rebuild_page_acct` | ✔ | LVGL 重建 | 账号页总重建：未登录/登录态、每日推荐入口、歌单列表、详情分页与懒加载占位 |
| 1606 | 1613 | 8 | `pl_back_clicked` | ✔ | 事件回调 | 歌单详情返回列表 |
| 1616 | 1627 | 12 | `pl_reload_clicked` | ✔ | 事件回调 | 详情"重新载入这个歌单" → `OREQ_LOAD` |
| 1649 | 1671 | 23 | `pl_row_clicked` | ✔ | 事件回调 | 进歌单详情（挂 SHORT_CLICKED，见 1629–1644 注释）→ `OREQ_LOAD` |
| 1693 | 1702 | 10 | `pl_menu_close` | ✔ | 菜单 | 渐出关菜单并清 `s_menu_idx`/`s_menu_pending` |
| 1704 | 1709 | 6 | `pl_menu_deleted` | ✔ | 菜单 | 菜单 DELETE 回调：清指针 |
| 1712 | 1726 | 15 | `pl_menu_del_confirmed` | ✔ | 菜单 | 删除确认后投 `OREQ_REMOVE`（用自己存的 `s_del_id`） |
| 1728 | 1762 | 35 | `pl_menu_btn` | ✔ | 菜单 | 菜单三项动作分派：0 重命名 / 1 更新（`OREQ_ADD`）/ 2 删除（二次确认） |
| 1777 | 1786 | 10 | `pl_row_long_pressed` | ✔ | 菜单 | 长按只记录目标 + `lv_async_call`（按压期间不能建遮罩） |
| 1789 | 1805 | 17 | `pl_menu_deferred` | ✔ | 菜单 | 等手指抬起再 `pl_menu_build()`（否则松手的 CLICKED 会关掉菜单） |
| 1808 | 1815 | 8 | `pl_menu_scrim_clicked` | ✔ | 菜单 | 点空白关菜单（`MENU_GRACE_MS` 宽限内忽略） |
| 1818 | 1821 | 4 | `pl_menu_panel_clicked` | ✔ | 菜单 | 吞掉面板内点击（不穿透关闭） |
| 1824 | 1851 | 28 | `pl_menu_build` | ✔ | 菜单 | 建菜单卡片（重命名/更新/删除三行） |
| 1853 | 1949 | 97 | `online_rebuild_page_pl` | ✔ | LVGL 重建 | 第 1 页重建：歌单列表（长按菜单入口）或详情（分页/占位/重新载入） |
| 1953 | 1958 | 6 | `ime_open` | ✔ | 输入法 | 查**键盘对象**（不是容器）的隐藏位判断键盘是否打开 |
| 1960 | 1984 | 25 | `srch_row_clicked` | ✔ | 事件回调 | 结果行=播放；历史行=填回输入框并搜（**先拷关键词**，见 1972–1977） |
| 1990 | 2002 | 13 | `srch_hist_del_confirmed` | ✔ | 二次确认回调 | 删单条历史 + 重建 |
| 2004 | 2018 | 15 | `srch_row_long_pressed` | ✔ | 事件回调 | 历史行长按 → 删单条二次确认 |
| 2021 | 2027 | 7 | `srch_clear_confirmed` | ✔ | 二次确认回调 | 清空全部历史 + 重建 |
| 2029 | 2039 | 11 | `srch_clear_clicked` | ✔ | 事件回调 | 「清除」→ 二次确认（空历史提示） |
| 2047 | 2137 | 91 | `online_rebuild_page_srch` | ✔ | LVGL 重建 | 搜索页重建：历史 / 正在搜索 / 结果分页 / 各种空态文案 |
| 2139 | 2165 | 27 | `srch_do_search` | ✔ | 编排 | 拷关键词 → 判网 → 记历史 → 置 busy → 收键盘 → 重建 → `OREQ_SEARCH` |
| 2179 | 2190 | 12 | `srch_ta_event` | ✔ | 事件回调 | FOCUSED 弹键盘；VALUE_CHANGED 清结果态（**绝不在 DEFOCUSED 收键盘**） |
| 2197 | 2204 | 8 | `srch_hide_ime_clicked` | ✔ | 事件回调 | 点列表/空白收键盘并重建（历史回来） |
| 2207 | 2217 | 11 | `srch_ime_ready` | ✔ | 事件回调 | 键盘对勾：有词就搜，空则收键盘 |
| 2221 | 2232 | 12 | `slide_exec_cb` | ✔ | 动画 | 三页按 `s_page_off` 同步平铺（宽度取宿主，宽 0 时退屏幕宽） |
| 2234 | 2237 | 4 | `tab_style` | ✔ | LVGL 工具 | 页签选中/未选中配色 |
| 2239 | 2281 | 43 | `page_show` | ✔ | 页面切换 | 夹页号、改页签样式、离开搜索页收键盘、`lv_anim` 220 ms 缓出平移 |
| 2285 | 2285 | 1 | `tab_acct_clicked` | ✔ | 事件回调 | `page_show(0, true)` |
| 2286 | 2286 | 1 | `tab_pl_clicked` | ✔ | 事件回调 | `page_show(1, true)` |
| 2287 | 2287 | 1 | `tab_srch_clicked` | ✔ | 事件回调 | `page_show(2, true)` |
| 2294 | 2311 | 18 | `modal_close` | ✔ | 浮层 | `ui_ba_close` + 清 `s_modal*` |
| 2313 | 2344 | 32 | `modal_submit` | ✔ | 浮层 | 取文本：重命名 → `online_playlist_rename`；否则判网后 `work_submit(kind)`；关浮层 |
| 2350 | 2361 | 12 | `modal_on_delete` | ✔ | 浮层 | 浮层销毁：清指针 + 隐藏两个键盘（键盘不归它所有） |
| 2369 | 2376 | 8 | `ime_ready_cb` | ✔ | 输入法 | 对勾语义分派：重命名浮层 → 提交；否则 → 搜索 |
| 2379 | 2387 | 9 | `ime_hide` | ✔ | 输入法 | 收拼音键盘 + 自带键盘 |
| 2390 | 2395 | 6 | `ime_show_for` | ✔ | 输入法 | 给输入框挂/显示拼音键盘 |
| 2403 | 2473 | 71 | `modal_build_keyboards` | ✔ | 输入法 | 懒建键盘（**必须先 `lv_obj_update_layout` 再 `create`**）、attach/show/提到最前 |
| 2476 | 2514 | 39 | `modal_open` | ✔ | 浮层 | 建 BA 卡片（标题/输入框/提示/按钮），重命名预填当前名，再建键盘 |
| 2518 | 2522 | 5 | `on_add_clicked` | ✔ | 事件回调 | 「+」→ `modal_open(OREQ_ADD)` |
| 2526 | 2796 | **271** | `on_result` | ✔ | **结果分发** | 把槽位结果搬到本地 → 清发布标志 → 按 12 种 `kind` 分派（含二维码状态机、账号、懒加载分页） |
| 2798 | 2826 | 29 | `poll_cb` | ✔ | lv_timer 回调 | 200 ms：网络态变化重建三页、`on_result()`、登录态下昵称/歌单退避补拉 |
| 2830 | 2904 | 75 | `online_on_delete` | ✔ | 页面生命周期 | 停轮询、关浮层/菜单/确认框、**清所有控件指针**、清理未消费结果、复位重试与分页状态 |
| 2906 | 3131 | **226** | `page_online` | ✔ | LVGL 构建 | 建整页控件树（标题/tab/三页/列表）+ 首次 layout + 建 200 ms 定时器 + 挂 DELETE |
| 3135 | 3157 | 23 | `app_online_register_apps` | ✘ | 注册 | 注册桌面 App（`netease_logo`）+ 建请求队列 + 建 `online_work` 任务（栈 16 KB，优先级 3） |

**最大 5 个**：`on_result`(271)、`page_online`(226)、`online_rebuild_page_acct`(211)、`online_work_task`(127)、`online_rebuild_page_pl`(97)。

---

## 3. 全部全局 / 静态变量清单

### 3.1 线程/结果协议（跨任务，最关键）

| 行号 | 名字 | 类型 | 用途 | 访问上下文 |
| --- | --- | --- | --- | --- |
| 181 | `s_req_q` | `QueueHandle_t` | 请求队列（深度 **4**，3154–3146 创建） | LVGL 任务写、工作线程读（唯一的同步原语） |
| 182 | `s_work_task` | `TaskHandle_t` | 工作线程句柄（只判空用） | — |
| 184 | `s_page_alive` | `volatile bool` | 页面是否活着（工作线程据此决定要不要发布/就地释放） | 两边都读写 |
| 187 | `s_res_kind` | `volatile int` | **发布标志**（最后写、最先清） | 工作线程写、LVGL 读 |
| 188 | `s_res_err` | `int` | 结果错误码 | 同上 |
| 189 | `s_res_songs` | `online_song_t *` | 结果歌曲数组（所有权转给 LVGL 侧） | 同上 |
| 190 | `s_res_count` | `size_t` | 结果条数 | 同上 |
| 191 | `s_res_msg[64]` | `char` | 结果附加文本（歌单 ID / 昵称 / unikey） | 同上 |
| 194/195 | `s_res_off` / `s_res_want` | `int` | 懒加载分页附加信息（**必须在 `work_publish` 前写**） | 同上 |
| 202/203 | `s_res_img` / `s_res_img_len` | `uint8_t*` / `size_t` | 二维码 PNG（调用方 free） | 同上 |
| 204 | `s_res_qr` | `volatile int` | 扫码状态码 800/801/802/803 | 同上 |
| 205/206 | `s_res_pls` / `s_res_pls_n` | `online_acct_playlist_t*` / `size_t` | 账号歌单列表 | 同上 |

### 3.2 账号页 / 二维码

| 行号 | 名字 | 类型 | 用途 |
| --- | --- | --- | --- |
| 215–219 | `s_page_acct` `s_acct_back` `s_tab_acct` `s_acct_hint` `s_acct_list` | `lv_obj_t *` | 账号页容器/返回行/页签/说明行/行容器（页面销毁时必须置空，2859–2863） |
| 220–225 | `s_acct_detail` `s_acct_daily` `s_acct_sel` `s_acct_pls` `s_acct_pls_n` `s_acct_pls_asked` | bool/int/指针/size_t | 账号详情状态机（`s_acct_pls` 堆内存，LVGL 侧持有，2664–2668/2713–2718 释放换新） |
| 234–236 | `s_acct_pls_busy` `s_acct_pls_try_tick` `s_acct_nick_try_tick` | bool/uint32 | 在途标志 + 退避节流时间戳 |
| 241 | `s_acct_backoff_ms` | `uint32_t` | 退避间隔（初始 5 s，上限 60 s） |
| 246–253 | `s_qr_scrim` `s_qr_dlg` `s_qr_img` `s_qr_dsc` `s_qr_px` `s_qr_status` `s_qr_timer` `s_qr_key[64]` | 混合 | 二维码浮层状态；`s_qr_dsc`/`s_qr_px` 是堆内存（335–352 释放） |
| 254/255 | `s_login_name[64]` / `s_acct_asked` | char[] / bool | 昵称显示 + "问过账号信息了吗" |

### 3.3 页面骨架 / 三页状态

| 行号 | 名字 | 类型 | 用途 |
| --- | --- | --- | --- |
| 648–656 | `s_root` `s_tab_pl` `s_tab_srch` `s_host` `s_page_pl` `s_page_srch` `s_page` `s_page_off` `s_poll` | 混合 | 页面根/tab/宿主/两页容器/当前页/平移量/**200 ms 轮询定时器** |
| 661–665 | `s_pl_list` `s_pl_hint` `s_pl_back` `s_pl_detail` `s_sel_pl` | 混合 | 第 1 页列表与详情状态 |
| 668–680 | `s_srch_ta` `s_srch_hint` `s_srch_clear` `s_srch_list` `s_srch_has_res` `s_srch_busy` | 混合 | 搜索页控件与状态 |
| 712 | `s_cur_pl_id[24]` | `char` | **详情身份 Key**（当前 `s_songs` 属于哪个歌单） |
| 751–758 | `s_acct_page` `s_pl_page` `s_srch_page` `s_songs_cap` `s_songs_total` `s_acct_lazy` `s_fetch_busy` `s_fetch_fail` | 混合 | 分页与懒加载状态 |
| 810 | `s_detail_tick` | `uint32_t` | 详情请求时刻（超时兜底） |
| 820–826 | `s_modal` `s_modal_dlg` `s_modal_ta` `s_ime` `s_ime_box` `s_kb` `s_modal_kind` | 混合 | 浮层与两个键盘（`s_ime`/`s_kb` 挂 `lv_layer_top`，**全局复用不随页销毁**） |
| 1646–1688 | `s_menu` `s_menu_idx` `s_rename_idx` `s_menu_pending` `s_menu_tick` `s_menu_dlg` `s_del_idx` `s_del_id[24]` | 混合 | 长按菜单与删除确认状态 |
| 1987 | `s_hist_del_idx` | `int` | 待删历史下标（确认框期间列表可能已重建） |

### 3.4 队列所有权（**最敏感**）

| 行号 | 名字 | 类型 | 语义 |
| --- | --- | --- | --- |
| 683 | `s_songs` | `online_song_t *` | 当前队列（**裸指针，播放器也持有**，852–868 注释详述为何不能 free） |
| 684 | `s_song_n` | `size_t` | 已载入条数 |
| 686 | `s_stale` | `online_song_t *` | 被替换下来的旧队列，等播放器切走后由 `app_music_online_detach()` 释放 |
| 690 | `s_owner` | `int` | `OWNER_PL/OWNER_SRCH/OWNER_ACCT`（谁的结果） |

---

## 4. 外部依赖清单

### 4.1 依赖的外部 API

| 模块 | 具体调用（行号） | 用途 |
| --- | --- | --- |
| `online_service` | `online_playlist_load`(113/514)、`online_search`(521)、`online_playlist_add`(108/507)、`online_playlist_remove`(151/526)、`online_playlist_count/id/name/rename`(718/1657/2500/2313 附近)、`online_history_*`(1968/1998/2024/2033/2067/2122/2131/2158)、`online_netease_account`(558/565)、`online_netease_account_playlists`(571)、`online_netease_playlist_page`(592)、`online_netease_daily_songs`(603)、`online_netease_qr_key/create/poll`(535/537/546)、`online_netease_logout`(552)、`online_netease_logged_in`(426/2817)、`online_netease_cookie_reload`(1408)、`online_netease_nickname(_set)`(1291/2637)、`online_last_error`(110/2560/2575/2760/2783)、`online_has_network`(1316/1377/2154/2337/2803/1934/2116)、`online_song_cover_data`(124)、`online_song_lyrics_data`(129)、`online_song_prepare`(117)、`online_song_stream_url`(135) | 全部业务数据（自检段 103–154 也用它） |
| `app_music` | `app_music_play_online`(1000)、`app_music_open_player`(1001)、`app_music_online_detach`(856) | 交队列给播放器 / 进播放器页 / **旧队列所有权移交** |
| `media_meta` | `media_meta_image_mem_rgb565`(291)、`media_meta_cover_free`(300/309/349) | 二维码 PNG→RGB565 + 释放 |
| `audio_service` | `audio_service_play_uri`(140)、`audio_service_get_info`(143)、`audio_service_is_playing`(144)、`audio_service_stop`(147) | 仅自检段（`#if 0`） |
| `ui_core` / `ui_metrics` / `ui_icons` | `UI_FONT_*` `UI_COLOR_*` `UI_PX_W/H` `UI_PCT_X` `UI_PAGE_PAD` `UI_STATUSBAR_H` `UI_LIST_ROW_H`、`ui_toast`、`netease_logo` | 统一视觉、Toast、桌面图标 |
| `ui_ba` | `ui_ba_open/close/on_close/scrim_dismissible/hint/input/btn/opt_row/confirm_ex/confirm_close`、`BA_DANGER`/`BA_TEXT_BODY` | 二维码浮层、添加/重命名浮层、长按菜单、二次确认 |
| `ui_app_registry` | `ui_app_register`、`app_desc_t` | 注册桌面 App（3135–3142） |
| `lvgl_pinyin_ime` | `lv_pinyin_ime_create/attach/show/hide/set_fit_height/event_ready` | 搜索与重命名的中文输入 |
| LVGL 9 | `lv_obj_*` `lv_label_*` `lv_button_create` `lv_textarea_*` `lv_keyboard_*` `lv_spinner_create` `lv_image_*` `lv_timer_*` `lv_anim_*` `lv_async_call` `lv_indev_*` `lv_layer_top` `lv_obj_update_layout` | 全部 UI |
| FreeRTOS | `xQueueCreate`(3145)/`xQueueSend`(627)/`xQueueReceive`(491)、`xTaskCreate`(3148/3155)、`vTaskDelay`(502/141/106)、`pdMS_TO_TICKS` | 请求队列 + 工作线程 |
| ESP-IDF | `esp_log`、`calloc/free/snprintf/memcpy/strcmp` | 内存与日志 |

### 4.2 对外导出 API

`include/app_online.h` 只有一项：`void app_online_register_apps(void);`（由 `main` / app 注册流程调用）。
本 App **不对外暴露任何数据接口**，全部跨模块交互都是单向调用（见 4.1）。

---

## 5. 线程 / 回调模型

| 上下文 | 干什么 | 同步 |
| --- | --- | --- |
| **LVGL 任务**（`ui_router` 调度） | `page_online()` 建树、所有 `*_clicked` / LVGL 事件回调、`online_rebuild_page_*`、`on_result()`（由 `poll_cb` 调）、`page_show` 动画、浮层与键盘 | **不加锁**（全部在 LVGL 任务内串行） |
| **`online_work` 任务**（3135–3153，栈 16 KB，优先级 3） | `online_work_task` 循环：等请求 → 调 online_service 阻塞 API → `work_publish` 写槽位 | 与 LVGL 之间只有：① 请求队列 ② `s_res_*` 槽位 + `volatile s_res_kind` 发布标志 ③ `volatile s_page_alive` |
| **`poll_cb`（lv_timer，200 ms，3128 创建）** | 网络态变化重建三页、`on_result()`、退避补拉 | 在 LVGL 任务上下文，无锁安全 |
| **`qr_poll_cb`（lv_timer，1500 ms，2619 创建）** | 投 `OREQ_QR_POLL` | 同上 |
| **`pl_menu_deferred`（`lv_async_call`）** | 手指抬起后再建长按菜单 | 在 LVGL 任务下一轮执行 |
| **`online_selftest_task`（`#if ONLINE_SELFTEST`）** | 全链路自检（直接调 online_service 和 audio_service） | 独立任务，默认不编译 |

**交接协议（拆分时必须原样保留）**：

1. **请求**：`work_submit_ex()` 组 `oreq_t{kind, arg[64], off, cnt}` 入队（深度 4，非阻塞 `xQueueSend(...,0)`），
   队列满就 Toast「上一个操作还没做完」（627–629）。
2. **发布**：工作线程把 `s_res_songs/s_res_err/s_res_msg/s_res_off/s_res_want/s_res_img/s_res_pls`
   **先写好**，最后写 `s_res_kind`（483 注释：kind 是发布标志）→ LVGL 侧看到 kind 才读。
3. **消费**：`on_result()`（2526）先把 kind/err/songs/count/off/want/msg 拷到**本地**，
   再把槽位置空（2542–2545）—— 保证工作线程可以立刻开始下一轮。
4. **背压**：`online_work_task` 开头的 300×10 ms 等待（501–503）—— 让 LVGL 先把上一份搬走，
   避免"后一份覆盖前一份"（被覆盖的数组无人 free）。
5. **页面死亡**：`work_publish` 若发现 `!s_page_alive` 就地释放（468–478）；`online_on_delete`
   也会清一次槽位（2881–2890）。
6. **UI 操作纪律**：**没有任何 `esp_lv_adapter_lock`** —— 本文件所有 LVGL 调用都在 LVGL 任务里；
   工作线程**只碰静态槽位与 online_service**，绝不碰 `lv_obj_t*`。

---

## 6. 拆分方案

### 6.1 目标目录树

```
components/apps/app_online/
├── CMakeLists.txt                 # 只改 SRCS
├── include/app_online.h           # 对外契约不变
└── src/
    ├── app_online.cc              # 注册 + init + 页面生命周期（~200 行）
    ├── ao_work.cc                 # 工作线程 + 请求/结果协议（~230 行）
    ├── ao_model.cc                # ★纯逻辑：队列所有权/懒加载/分页数学/详情 Key（~260 行）
    ├── ao_pages.cc                # 三页切换 + 动画 + tab（~120 行）
    ├── ao_page_acct.cc            # 第 0 页 我的歌单（~330 行）
    ├── ao_page_pl.cc              # 第 1 页 我添加的歌单 + 长按菜单（~330 行）
    ├── ao_page_srch.cc            # 第 2 页 搜索（~230 行）
    ├── ao_qr_login.cc             # 二维码浮层 + 账号状态行（~190 行）
    ├── ao_modal.cc                # 浮层与键盘（添加/重命名/搜索）（~250 行）
    ├── ao_widgets.cc              # 行/占位/转圈等通用控件工厂（~180 行）
    └── ao_aocollect.inc？          # 不需要：用 ao_model.h 暴露状态访问器
```

### 6.2 文件职责与接口

| 文件 | 搬走的行号 | 需要暴露的头文件接口 | 预估行数 |
| --- | --- | --- | --- |
| `src/app_online.cc` | 2906–3157（`page_online` 拆到 `ao_pages`）、2830–2904 | `app_online_register_apps()`；`ao_ui_reset_all()`（页面销毁统一清指针） | ~200 |
| `src/ao_work.cc` | 157–206、461–635 | `ao_work_init()`、`ao_work_submit(kind,arg)`、`ao_work_submit_ex(kind,arg,off,cnt)`、`ao_work_take(ao_result_t *out)`（**把"槽位读+清"封成一个函数**，替代现在散在 `on_result` 里的搬运） | ~230 |
| `src/ao_model.cc` | 683–758、845–988、715–815 | `ao_queue_replace(buf,cap,count,key,owner)`、`ao_queue_install(...)`、`ao_queue_drop_stale()`、`ao_queue_current(&n)`、`ao_lazy_begin/append/fetch_if_needed()`、`ao_page_first/page_count/page_clamp/acct_page_count`、`ao_detail_want_key/matches/owns`（**不 include 任何 lv_*.h**） | ~260 |
| `src/ao_pages.cc` | 648–656、2219–2287、2906 骨架部分 | `ao_pages_build(lv_obj_t *scr)`、`ao_page_show(int,bool)`、`ao_tab_*_clicked`、`ao_pager_attach()`、`ao_page_get(int)` | ~120 |
| `src/ao_page_acct.cc` | 1220–1602 | `ao_acct_build(lv_obj_t *parent)`、`ao_acct_rebuild()`、`ao_acct_tick()`（退避补拉） | ~330 |
| `src/ao_page_pl.cc` | 1604–1949 | `ao_pl_build(parent)`、`ao_pl_rebuild()`、`ao_pl_menu_close()` | ~330 |
| `src/ao_page_srch.cc` | 1951–2217 | `ao_srch_build(parent)`、`ao_srch_rebuild()`、`ao_srch_do_search(kw)`、`ao_srch_ime_hide()` | ~230 |
| `src/ao_qr_login.cc` | 243–255、266–459 | `ao_qr_open()/close()/status_set()`、`ao_acct_row_build(col)`、`ao_login_name_sync()` | ~190 |
| `src/ao_modal.cc` | 817–843、2289–2514 | `ao_modal_open(kind)`、`ao_modal_close()`、`ao_ime_show_for(ta)`、`ao_ime_hide()`、`ao_ime_open()` | ~250 |
| `src/ao_widgets.cc` | 1004–1218 | `ao_make_row()`、`ao_fill_song_rows_range()`、`ao_page_rows()`、`ao_make_back_label()`、`ao_loading_rows()`、`ao_detail_wait_rows()`、`ao_list_scroll_top()` | ~180 |

### 6.3 "纯逻辑（可 host 单测，不依赖 LVGL）" 的分界

| 归入纯逻辑（`ao_model.cc` + 纯函数） | 行号 | 单测要点 |
| --- | --- | --- |
| 分页数学 | 761–781 | 0 首/整页边界/负页码 |
| 账号页数特例 | 791–801 | 总数未知 + 末页填满 → 多一页；总数已知 → 精确 |
| 详情 Key 匹配 | 715–743 | 三种 Key、空 Key、脏 Key（超长截断，892–893） |
| 队列所有权状态机 | 853–900 | `s_stale` 链、`buf == s_songs` 的自赋值保护（874） |
| 懒加载追加 | 912–988 | 非连续 off 丢弃（940–944）、cap 截断（977–979）、want 计算 |
| 退避节流 | 1300–1312、1314–1341、1375–1390 | 翻倍/上限/复位/时间未到 |
| 请求/结果状态机 | 159–206、461–635 | kind 顺序、页面死亡释放、`s_res_off/want` 时序 |
| 搜索页显示决策 | 2054–2056 | `busy/has_res/empty/ime_open` 四要素组合（可用伪 UI 状态做表驱动测试） |

（把 `lv_obj_t*` 相关全部隔离在 `ao_page_*.cc`/`ao_widgets.cc`/`ao_modal.cc`，
`ao_model.cc` 只操作 POD 数组 + `char key[24]`，即可用 `-DHOST_TEST` 直接编译单测。）

### 6.4 迁移顺序（每步可编译、UI 行为不变）

1. 建 `ao_model.h` + 把 715–1002/1300–1390 的状态与纯函数搬进 `ao_model.cc`（**只搬不改**），
   原文件 include 新头并删除旧定义。
2. 抽 `ao_work.cc`（157–206 + 461–635），`on_result` 改成 `ao_work_take()` 取一帧结构体。
3. 抽 `ao_widgets.cc`（1004–1218）。
4. 抽 `ao_qr_login.cc`（266–459），抽 `ao_modal.cc`（2289–2514）。
5. 抽三个页面文件（`ao_page_acct/pl/srch.cc`），`ao_pages.cc` 管骨架与切页。
6. `app_online.cc` 只剩注册 + `page_online()` 组装 + `online_on_delete`（改为调各页的 `*_reset()`）。
7. 加 host 单测（分页/Key/懒加载/退避），跑 `idf.py build` + 真机回归三页与扫码。

---

## 7. 风险清单

| # | 风险 | 触发场景 | 防护 / 验证 |
| --- | --- | --- | --- |
| **R1** | **`s_res_kind` 发布协议被破坏**（461–484/2526）：漏写"先填负载后写 kind"，或 `on_result` 忘了拷本地就清槽位 | 拆 `ao_work.cc` 时把赋值顺序改了 | 把发布/消费封成 `ao_work_publish(take)` / `ao_work_take(ao_result_t*)` 两个函数，禁止外部直接碰 `s_res_*`；`ao_result_t` 一次性拷全（含 off/want/img/pls） |
| **R2** | **队列所有权**：`s_songs` 被 free（播放器还持有）或 `s_stale` 永久泄漏 | 在 `online_install`/`OREQ_REMOVE`（2598–2601 注释）路径上"顺手 free" | 保留"只在 `queue_replace` 里挪进 `s_stale`、只由 `online_drop_stale`→`app_music_online_detach` 释放"的不变式；单测模拟播放器持有指针 |
| **R3** | 页面销毁后残留悬垂 `lv_obj_t*`（`online_on_delete` 的 24 个置空，2851–2873） | 拆文件后漏置空某个新加的指针，或某个指针搬到了别的文件 | 统一 `ao_ui_reset_all()`，用一张"页面拥有的指针表"集中清空；UI 回归：进出页面 20 次 + 切页 |
| **R4** | **键盘/浮层挂在 `lv_layer_top` 不随页销毁**（2409–2410、2461）：漏收 → 键盘盖在别的页上；误删 → 下次重建慢 1 s | 页面销毁路径改动 | 保留 `online_on_delete` 里 `modal_close/pl_menu_close/qr_close/ui_ba_confirm_close/ime_hide` 的顺序；回归：搜索页开键盘直接返回桌面再进 |
| **R5** | 长按菜单的"等手指抬起"机制（1777–1805）被简化成立即建菜单 | 拆 `ao_page_pl.cc` 时把 `lv_async_call` 去掉 | 保留 `s_menu_pending` + PRESSED 轮询 + `MENU_GRACE_MS`；注释里已写明根因（松手 CLICKED 会关掉菜单） |
| **R6** | `pl_row_clicked` 挂错事件（1629–1644：必须是 `SHORT_CLICKED`，不能 `CLICKED`） | 重构行构造时统一改成 CLICKED | 在 `ao_widgets`/`ao_page_pl` 里加注释 + 回归"长按不弹详情" |
| **R7** | 懒加载缓冲指针必须**永不 realloc**（72–74、905–911）：一旦改成 `realloc` 增长，播放器持有的指针失效 | 拆 `ao_model.cc` 时"优化"成动态增长 | 单测 + 代码注释锁死"cap 一次分配、超 `ONLINE_SONGS_MAX(1000)` 就截断" |
| **R8** | `s_cur_pl_id` 身份 Key 与 `s_songs` 不同步 → 列表显示"上一个歌单的歌"（用户曾报） | 换队列时漏调 `queue_replace` | 保持"只有 `queue_replace` 能改这三者"；回归：连续点开两个不同歌单 |
| **R9** | 详情超时兜底计时 `s_detail_tick`（809–815）漏重置 → 刚进详情就显示"载入超时" | 拆页面文件时忘了 `detail_mark_request()`（1242/1258/1623/1667/2909） | 把"进详情"封成一个 `ao_detail_enter(...)`，内部统一计时 + 重建 + 下单 |
| **R10** | 搜索关键词指向 `s_hist[]` 内部（1972–1979、2144–2153）被重排后错位（"点历史搜出另一条"） | 简化拷贝 | 保留"进入 `srch_do_search` 前必须 `snprintf` 拷贝"的纪律，并单测 |
| **R11** | 二维码/账号的内存释放路径：`s_qr_dsc`(303–306)、`s_qr_px`(299–302/348–351)、`s_res_img`(470/2884)、`s_acct_pls`(2664–2668/2713–2718) | 拆文件后出现"谁 free"分歧 | 每个资源在一个文件里集中释放；回归：开→关二维码 10 次、登录→退出→登录 |
| **R12** | `work_submit` 队列深度仅 4（3145）且非阻塞：连续点按会 Toast"上一个操作还没做完" | 拆分后误把队列改小/阻塞发送 | 保留 `xQueueSend(...,0)` + Toast；若改阻塞会卡 LVGL 任务 |
| **R13** | `poll_cb` 里的函数级 `static bool s_net_last`（2802）是隐藏全局 | 拆文件后丢失或重复定义 | 搬到页面状态结构体（`ao_page_state_t`），随页面复位 |
| **R14** | 页面创建/销毁与在途请求的竞态：`s_page_alive` 置位时机（2908）+ 结果清理（2881–2890）+ 工作线程 3 s 等待（501–503） | 拆 `ao_work.cc` 时改了顺序 | 单测模拟"发布后立刻销毁页面"，确认无泄漏（`s_res_*` 全 NULL） |
| **R15** | 拼音键盘首次创建必须先 `lv_obj_update_layout(s_ime_box)`（2413–2425），否则键位退化成 320 px 基准（"键盘太小"） | 抽 `ao_modal.cc` 时删掉那行 | 注释 + 真机截图对比键位大小 |
| **R16** | 三页页序（0 账号 / 1 我添加的歌单 / 2 搜索，2283–2287）与 `PG_*` 常量（82–84）、动画下标（2229–2231）必须一致 | 拆 `ao_pages.cc` 时加页/换序 | 保留常量集中定义 + `_Static_assert` 数量；回归：三个 tab 各点一次 |
| **R17** | 工作线程栈 16 KB（54）与 online_service 的 static 缓冲是"配套"的（46–52 注释：曾因 8 KB 栈崩在 snprintf） | 拆分后把某个大缓冲搬回栈上 | 保留栈大小注释；若新增栈上大缓冲，先算账 |

---

## 8. 附加必答

### A. 三条用户路径的端到端调用链

**1) 扫码登录**
```
点「扫码登录」(login_clicked:403 / acct_login_clicked:1276)
 → qr_open():369 建浮层(ui_ba_open) + work_submit(OREQ_QR_KEY):400
 → online_work_task:530 OREQ_QR_KEY: online_netease_qr_key(535) → online_netease_qr_create(537)
 → work_publish:541（先写 s_res_img/s_res_img_len:539）
 → poll_cb:2798 → on_result:2526 → case OREQ_QR_KEY:2605
    → qr_show_png(png,len):286（media_meta_image_mem_rgb565:291 → lv_image_set_src:322）
    → qr_status_set("请用网易云音乐 App 扫码"):2617 → 建 qr_poll_cb 定时器:2619
 → 每 1.5 s（QR_POLL_MS:244）：qr_poll_cb:327 → OREQ_QR_POLL → work:544 online_netease_qr_poll
 → on_result case OREQ_QR_POLL:2623：803 → 清 s_acct_asked/pls_asked、清昵称(2637)、qr_close(2638)、
    重建两页、work_submit(OREQ_ACCOUNT):2641；802/801/800 → 改状态行（2642–2649）
```

**2) 歌单增删改查（长按菜单）**
```
长按歌单行 pl_row_long_pressed:1777 → lv_async_call(pl_menu_deferred:1789)
 → 手指抬起 → pl_menu_build:1824（重命名/更新/删除三行）
 → pl_menu_btn:1728 → 0: modal_open(MODAL_RENAME):1744 → modal_submit:2313 → online_playlist_rename
               → 1: work_submit(OREQ_ADD, id_copy):1753（= 重新抓取刷新）
               → 2: ui_ba_confirm_ex → pl_menu_del_confirmed:1712 → work_submit(OREQ_REMOVE)
点行进详情 pl_row_clicked:1649 → work_submit(OREQ_LOAD, id):1670 → on_result case OREQ_LOAD:2567
 → online_install:897（→ queue_replace:870）→ online_rebuild_page_pl:1853
```

**3) 搜索（含历史）**
```
输入框聚焦 srch_ta_event:2179 → ime_show_for:2390 → modal_build_keyboards(…,true):2403
 → 键盘对勾 → ime_ready_cb:2369 → srch_ime_ready:2207 → srch_do_search:2139
    → online_history_add:2158 → s_srch_busy=true:2159 → ime_hide:2162 → rebuild:2163
    → work_submit(OREQ_SEARCH):2164 → work:518 online_search:521 → on_result case OREQ_SEARCH:2584
    → online_install(...,OWNER_SRCH):2587 → online_rebuild_page_srch:2047（结果分页 100/页）
点历史行 srch_row_clicked:1960（先拷关键词:1978–1979）→ srch_do_search
长按历史 srch_row_long_pressed:2004 → ui_ba_confirm_ex → srch_hist_del_confirmed:1990
「清除」 srch_clear_clicked:2029 → srch_clear_confirmed:2021 → online_history_clear
```

### B. 页面与对象生命周期要点

- `page_online()`(2906) 建树顺序：`s_page_alive=true` + 复位分页/退避 → 标题行（「+」按钮）→
  三个 tab（**账号页签必须最先建**，2963 注释）→ `s_host`（裁剪 + 手势）→ 三个页面容器
  （账号页 / 我添加的歌单 / 搜索，搜索页初始 `x = LV_PCT(100)` 在屏外，3064）→
  `lv_obj_update_layout(scr)`（**必须在整屏 layout 之后才取宿主宽度**，3116–3121）→ `page_show(0,false)` →
  两次 rebuild → 挂 DELETE 回调 → 建 `s_poll`(3128)。
- `online_on_delete()`(2830)：`s_page_alive=false` → 删 `s_poll` → 关浮层/菜单/二维码/二次确认 →
  `ime_hide()`（**只收不删**，2844–2849）→ 置空 24 个控件指针 → 清未消费结果槽位 →
  `online_drop_stale()` → 复位 `s_acct_pls_asked/s_acct_asked/页码/在途标志`。
- **不在 `online_on_delete` 里 free `s_songs`**（播放器可能还在放，2897 注释）—— 这是刻意的。

### C. 分页与懒加载数据流（账号歌单）

```
acct_pl_clicked:1248
 ├─ acct_lazy_begin(id, trackCount):912   ← calloc(cap)（cap=total 或 1000，夹在 [100,1000]）
 │     └─ queue_replace(buf, cap, 0, id, OWNER_ACCT) + s_acct_lazy=true
 ├─ online_rebuild_page_acct:1392（画"载入中"）
 └─ acct_detail_fetch_if_needed:954
       need = page_first(page)+100（有 total 则不超过 total）；want = min(need-loaded, ACCT_FETCH_MAX=200, cap-loaded)
       → work_submit_ex(OREQ_ACCT_SONGS, id, loaded, want):987
on_result case OREQ_ACCT_SONGS:2727
 ├─ 成功且 key 匹配 → acct_lazy_append(off, page, n):935（非连续则丢弃）
 │     └─ count < page_want ⇒ 到末尾 ⇒ s_songs_total = s_song_n（2740–2742）
 ├─ 空页且 off>0 ⇒ 也当作到末尾（2747–2754）
 └─ 失败 ⇒ s_fetch_fail=true（2758）→ 渲染时 `detail_wait_rows` 给"重试"（1344 的 reload 分支）
翻页 page_row_clicked:1103 → 改 s_acct_page → rebuild → 渲染末尾再调 fetch_if_needed（自然"按需"）
```

### D. 自检任务（`ONLINE_SELFTEST`，103–154）

用途：无 UI 交互验证 `online_playlist_add → load → online_song_prepare → cover/lyrics → stream_url →
audio_service_play_uri → audio_service_get_info` 整条链；结束时 `online_playlist_remove` 清理。
拆分时它应整段搬到独立文件（例如 `ao_selftest.cc`）并保持 `#if` 门控，**不要**把它混进业务文件。

---

## 9. 拆分收益小结

- 3157 行 → 11 个文件，最小 `ao_pages.cc` ~120 行、最大 `ao_page_acct.cc`/`ao_page_pl.cc` ~330 行，
  全部 ≤600 行；`on_result`(271)/`page_online`(226)/`rebuild_acct`(211) 三个"巨函数"被拆散。
- **~600 行纯逻辑可 host 单测**（分页数学、详情 Key、懒加载追加、退避、队列所有权、搜索显示决策）。
- **跨任务边界从 6 个裸全局收敛成 2 个函数**（`ao_work_submit*` / `ao_work_take`），
  这是本文件最容易回归的地方（R1/R14），也是拆分后**唯一**允许跨任务访问的通道。
