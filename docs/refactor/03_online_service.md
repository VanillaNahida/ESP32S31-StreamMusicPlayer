# 03 `online_service.cc` 拆分重构蓝图

> 审计对象：`components/services/online_service/online_service.cc`
> 真实行数：**3227 行**（LF 计数；`read`/`grep` 工具的行号即此口径。用 pwsh 的
> `Get-Content | Measure-Object -Line` 会数成 3041 行，因为 .NET 还把若干 Unicode
> 换行符当行界 —— 本报告一律以工具行号为准）
> 头文件：`components/services/online_service/include/online_service.h`（295 行）
> 本报告为**只读审计产物**，未修改任何源文件。
>
> **2026-11 增补**：本报告描述的是"拆出 online_service 组件"那一次重构。之后新增了
> QQ 音乐音源（`os_qq.cc` 凭证/请求、`os_qq_json.cc` 纯逻辑映射、`os_qq_data.cc` 业务），
> 以及 `os_http_client` 的两个自定义请求头 + POST 支持；它们的契约见
> [docs/qq-music.md](../qq-music.md)，与本文的行号表无关。

---

## 1. 文件概览

### 1.1 include（文件头 23–49 行）

| 类别 | 内容 |
| --- | --- |
| 标准库 | `stdio.h` `string.h` `stdlib.h` `strings.h` `ctype.h` `time.h` `dirent.h` `sys/stat.h` `unistd.h` |
| ESP-IDF | `esp_log.h` `esp_heap_caps.h` `esp_timer.h` `esp_http_client.h` `esp_crt_bundle.h` `nvs.h` `nvs_flash.h` |
| 第三方 | `cJSON.h`、`mbedtls/base64.h`（二维码 PNG 的 base64 解码，48 行、2719 行） |
| 工程内 | `wifi_service.h`（只为 `wifi_service_has_ip()`，44 行 → 1798–1801 行） |

**没有** include 任何 LVGL / FreeRTOS / 音频头 —— grep 确认全文件无
`xSemaphore` / `xTaskCreate` / `vTaskDelay` / `portMUX` / `esp_lv_adapter` / `lv_*` / `std::`。
即：本文件是**纯同步阻塞的"网络 + 存储"服务**，不含 UI、不含线程原语、不含内部锁。

### 1.2 结构分区（按行号）

| 区间 | 分区 | 关键符号 |
| --- | --- | --- |
| 1–141 | 文件头契约 + 宏 + 歌单 NVS/定长记录类型 + 顶层静态状态 | `online_rec_t` `online_pl_hdr_t` `s_ids` `s_names` `s_last_err` |
| 143–230 | **PSRAM 封面/歌词 LRU 缓存容器** | `mem_cache_t` `mem_cache_free_slot/find/slot/put` |
| 232–289 | 小工具 | `str_copy` `psram_alloc` `id_from_url` `file_ok` `mkdir_if_needed` |
| 291–379 | **HTTP 接收缓冲与事件回调** | `http_sink_t` `sink_reserve` `http_evt` |
| 381–573 | **HTTP GET（文本/JSON/二进制，自管 302）** | `net_auth_hdr` `url_for_log` `http_get_text` |
| 575–728 | **HTTP 下载到文件（自管 302，边读边写 SD）** | `http_download` |
| 730–1040 | 歌单 NVS + **二进制落盘/读回** + 抓歌单 | `nvs_load_ids/save_ids` `pl_path/pl_save/pl_load` `parse_songs` `online_playlist_*` |
| 1042–1160 | 搜索（服务端 + 本地兜底） | `url_encode` `search_local` `online_search` |
| 1162–1307 | 音频缓存（SD）：嗅探扩展名、命中查找、LRU 清理、整曲下载 | `ext_from_sniff` `find_cached_audio` `audio_cache_trim` `online_song_prepare` |
| 1309–1495 | **封面/歌词（PSRAM 缓存，不落 SD）** | `cover_format` `online_song_cover_data` `online_song_lyrics_data` |
| 1497–1761 | **播放地址 302 解析 + 时长估算 + 流式准备** | `mp3_bitrate_bps` `online_song_stream_info` `online_song_stream_url` `online_song_prepare_stream` |
| 1763–1823 | 缓存清理 / 网络判定 / 目录清理 | `psram_cache_clear` `online_cache_clear` `online_has_network` `wipe_dir` |
| 1825–1906 | init + 歌单 ID 查询/重命名 | `online_service_init` `online_playlist_count/id/name/rename` |
| 1908–2027 | 搜索历史（NVS） | `s_hist` `nvs_load/save_history` `online_history_*` |
| 2029–2165 | 账号体系常量 + JSON 递归取值 + API base | `json_find_str/int` `online_netease_api_base(_set)` |
| 2167–2560 | **Cookie（内置 / SD / 压缩 / 存盘）+ 昵称 + 退出登录** | `cookie_pick` `net_cookie_builtin_load` `cookie_compact` `net_cookie_save` |
| 2561–2780 | **自建 API GET + 扫码登录三接口** | `net_api_get_ex` `online_netease_qr_key/create/poll` |
| 2782–3227 | **账号数据：账号信息 / 账号歌单分页 / 歌单曲目分页 / 每日推荐 / 直链** | `json_id_str` `pick_array` `song_from_json` `fill_songs` `online_netease_*` |

---

## 2. 全部函数清单（89 个顶层定义）

`行数 = 结束行 − 起始行 + 1`；结束行 = 函数体在列 0 的 `}`（共 90 个 `^}$`，其中 2188 行是
`extern "C" {` 的收尾，非函数）。**同名的 `net_cookie_builtin_load` 有两份实现（`#if/#else` 条件编译）**。

| 起始 | 结束 | 行数 | 函数名 | static | 分类 | 一句话职责 |
| --- | --- | --- | --- | --- | --- | --- |
| 161 | 170 | 10 | `mem_cache_free_slot` | ✔ | PSRAM 缓存 | 释放一个缓存槽的 `data` 并清零元数据 |
| 172 | 181 | 10 | `mem_cache_find` | ✔ | PSRAM 缓存 | 按歌曲 ID 查表并**更新 last_ms（LRU 触碰）** |
| 184 | 203 | 20 | `mem_cache_slot` | ✔ | PSRAM 缓存 | 取空槽；没有空槽则淘汰 `last_ms` 最小者并递减字节计数 |
| 206 | 230 | 25 | `mem_cache_put` | ✔ | PSRAM 缓存 | 插入条目（接管 `data` 所有权），再循环淘汰直到总量 ≤ 上限 |
| 234 | 252 | 19 | `str_copy` | ✔ | 工具 | UTF-8 安全截断拷贝（不切在多字节字符中间） |
| 254 | 258 | 5 | `psram_alloc` | ✔ | 工具 | 优先 `heap_caps_malloc(SPIRAM)`，失败退回内部 RAM `malloc` |
| 261 | 271 | 11 | `id_from_url` | ✔ | 工具/纯逻辑 | 从 `...&id=123` 里抠出歌曲 ID |
| 273 | 280 | 8 | `file_ok` | ✔ | 工具 | `stat` 判"是普通文件且 ≥ min_size" |
| 282 | 289 | 8 | `mkdir_if_needed` | ✔ | 工具 | 目录不存在才 `mkdir(0777)` |
| 320 | 354 | 35 | `sink_reserve` | ✔ | HTTP | 响应缓冲**从 16 KB 起翻倍**扩容（PSRAM 优先），超 limit 置 `overflow` |
| 356 | 379 | 24 | `http_evt` | ✔ | HTTP 回调 | 从 `HTTP_EVENT_ON_HEADER` 抓 `Location`；`ON_DATA` 追加进 sink |
| 406 | 414 | 9 | `net_auth_hdr` | ✔ | HTTP | 拼 `Bearer <CONFIG_ONLINE_NETEASE_API_TOKEN>`，**static 缓冲**懒初始化 |
| 426 | 440 | 15 | `url_for_log` | ✔ | HTTP 安全 | 砍掉查询串的 URL（防止 cookie 进日志），**static 缓冲** |
| 442 | 573 | **132** | `http_get_text` | ✔ | HTTP | **同步 GET 文本/二进制，自管最多 4 跳 302**；成功把 malloc 缓冲交给调用方 |
| 579 | 728 | **150** | `http_download` | ✔ | HTTP + SD | 同步下载到文件：先 302 探测、再 `fopen/fwrite` 流式落盘，回填 16 字节嗅探头 |
| 732 | 760 | 29 | `nvs_load_ids` | ✔ | NVS | 读 `n`+`id0..id7`+`nm0..nm7` 到 `s_ids/s_names` |
| 762 | 787 | 26 | `nvs_save_ids` | ✔ | NVS | 全量重写（多余键 `nvs_erase_key`）后 `commit` |
| 789 | 797 | 9 | `id_index` | ✔ | 工具 | 在已保存 ID 里线性查下标，找不到 −1 |
| 801 | 808 | 8 | `pl_path` | ✔ | 落盘 | 拼 `/sdcard/online/pl_<id>.bin` |
| 811 | 848 | 38 | `pl_save` | ✔ | 落盘 | **写定长头 + N 条定长记录**（非原子，失败即 unlink） |
| 851 | 899 | 49 | `pl_load` | ✔ | 落盘 | 校验 magic/version/count 后读回数组（跳过空标题记录，**不校验文件长度**） |
| 904 | 958 | 55 | `parse_songs` | ✔ | JSON→模型 | Meting 歌单 JSON 数组 → `online_song_t[]`（title/author/pic/url/lrc） |
| 960 | 1003 | 44 | `online_playlist_add` | ✘ | 歌单 | 抓歌单 → 解析 → 落盘 → 记 NVS（已存在只刷新） |
| 1005 | 1024 | 20 | `online_playlist_remove` | ✘ | 歌单 | 从 NVS 数组删除（后面元素前移）并 unlink 二进制 |
| 1026 | 1040 | 15 | `online_playlist_load` | ✘ | 歌单 | 先读本地二进制；`NOT_FOUND` 时抓一次再读 |
| 1046 | 1062 | 17 | `url_encode` | ✔ | 纯逻辑 | RFC3986 百分号编码（只放行 unreserved） |
| 1065 | 1110 | 46 | `search_local` | ✔ | 纯逻辑+SD | 无网兜底：扫所有已保存歌单文件做 `strcasestr` 子串匹配 |
| 1116 | 1160 | 45 | `online_search` | ✘ | 搜索 | 先服务端 `type=search`，失败/无网退本地 |
| 1164 | 1183 | 20 | `ext_from_sniff` | ✔ | 纯逻辑 | 16 字节魔数 → 扩展名（flac/ogg/wav/m4a/mp3/aac） |
| 1186 | 1211 | 26 | `find_cached_audio` | ✔ | SD 缓存 | 按 `<id>.` 前缀在 audio 目录里找已下载文件（≥32 KB） |
| 1214 | 1257 | 44 | `audio_cache_trim` | ✔ | SD 缓存 | 文件数 > 48 时反复扫描删 mtime 最旧的（O(n²)） |
| 1259 | 1307 | 49 | `online_song_prepare` | ✘ | 兜底下载 | 命中缓存即返回；否则下到 `.part` → 嗅探 → rename → 清缓存 → 预热封面/歌词 |
| 1325 | 1346 | 22 | `cover_format` | ✔ | 纯逻辑 | **只按魔数**识别 JPEG/PNG，其余给出中文原因（WebP/GIF/HTML…） |
| 1348 | 1440 | **93** | `online_song_cover_data` | ✘ | 封面 | 命中 PSRAM 缓存→拷贝；否则带 `?param=300y300` 下载→校验格式→入缓存→拷贝 |
| 1446 | 1495 | 50 | `online_song_lyrics_data` | ✘ | 歌词 | 同上，纯文本 LRC；返回 NUL 结尾拷贝 |
| 1504 | 1524 | 21 | `mp3_bitrate_bps` | ✔ | 纯逻辑 | 扫 MP3 帧头（MPEG1/2 Layer III）取码率 bps |
| 1532 | 1654 | **123** | `online_song_stream_info` | ✘ | **302 解析** | 解析 302 拿最终直链 + `Content-Length` + 读 4 KB 估总时长 |
| 1656 | 1741 | 86 | `online_song_stream_url` | ✘ | **302 解析** | 同上但**不读 body、不算时长**（只回最终 URL） |
| 1747 | 1761 | 15 | `online_song_prepare_stream` | ✘ | 流式准备 | 只预热封面+歌词（拿完立即 free），不碰音频 |
| 1764 | 1774 | 11 | `psram_cache_clear` | ✔ | 缓存清理 | 逐槽释放封面/歌词缓存并清零字节计数 |
| 1776 | 1796 | 21 | `online_cache_clear` | ✘ | 缓存清理 | 删 audio 目录全部文件 + 清 PSRAM 缓存（保留歌单） |
| 1798 | 1801 | 4 | `online_has_network` | ✘ | 工具 | 转发 `wifi_service_has_ip()` |
| 1806 | 1823 | 18 | `wipe_dir` | ✔ | 工具 | 删目录内全部文件再尝试 `rmdir`（清理 cover/、lrc/ 遗留） |
| 1834 | 1870 | 37 | `online_service_init` | ✘ | init | 幂等：建目录、清历史遗留、载 NVS 歌单/历史/昵称/登录态 |
| 1872 | 1875 | 4 | `online_playlist_count` | ✘ | 查询 | 已保存歌单数 |
| 1877 | 1880 | 4 | `online_last_error` | ✘ | 查询 | 最近一次失败的中文原因（空则 NULL） |
| 1882 | 1885 | 4 | `online_playlist_id` | ✘ | 查询 | 第 idx 个歌单 ID（越界 NULL） |
| 1887 | 1894 | 8 | `online_playlist_name` | ✘ | 查询 | 第 idx 个显示名（没起名返回**空串**） |
| 1896 | 1906 | 11 | `online_playlist_rename` | ✘ | 歌单 | 改显示名并落 NVS（NULL/空串 = 清除） |
| 1913 | 1935 | 23 | `nvs_load_history` | ✔ | NVS | 读 `shn`+`sh0..sh9` 到 `s_hist` |
| 1937 | 1955 | 19 | `nvs_save_history` | ✔ | NVS | 全量重写搜索历史 |
| 1958 | 1969 | 12 | `hist_move_to_front` | ✔ | 纯逻辑 | 把第 idx 条提到最前（整体后移） |
| 1971 | 1974 | 4 | `online_history_count` | ✘ | 查询 | 历史条数 |
| 1976 | 1979 | 4 | `online_history_at` | ✘ | 查询 | 第 idx 条关键词 |
| 1981 | 2002 | 22 | `online_history_add` | ✘ | 历史 | 去重（已存在则提前）+ 头插 + 截断到 10 + 落盘 |
| 2004 | 2016 | 13 | `online_history_remove` | ✘ | 历史 | 删单条并前移 + 落盘 |
| 2018 | 2027 | 10 | `online_history_clear` | ✘ | 历史 | 清空并落盘 |
| 2088 | 2104 | 17 | `json_find_str` | ✔ | 纯逻辑 | 递归找字符串字段，**先直接子节点再下钻**（防内层同名字段顶掉外层） |
| 2106 | 2122 | 17 | `json_find_int` | ✔ | 纯逻辑 | 同上，取数值字段 |
| 2124 | 2141 | 18 | `online_netease_api_base` | ✘ | 账号 | 懒加载 API base（NVS `online/nb` → Kconfig 默认），**static 缓存** |
| 2143 | 2165 | 23 | `online_netease_api_base_set` | ✘ | 账号 | 写/清 NVS 覆盖 base 并同步内存 |
| 2202 | 2239 | 38 | `cookie_pick` | ✔ | 纯逻辑 | 按**名字**从原始 cookie 串精确提取 `NAME=VALUE` |
| 2246 | 2266 | 21 | `net_cookie_builtin_load` | ✔ | 登录（`#if`） | 用编进固件的 cookie 填 `s_net_cookie`（只取 MUSIC_U/__csrf） |
| 2274 | 2277 | 4 | `net_cookie_builtin_load` | ✔ | 登录（`#else`） | 未嵌入内置 cookie 时的恒 false 桩 |
| 2281 | 2319 | 39 | `net_cookie_load` | ✔ | 登录 | 从 SD 读 cookie（**latch `s_net_cookie_loaded`**），含旧版截断体检与内置回退 |
| 2332 | 2432 | **101** | `cookie_compact` | ✔ | 纯逻辑 | Set-Cookie 大串 → `NAME=VALUE; …`：去属性、同名取最后、MUSIC_U/__csrf 优先 |
| 2434 | 2456 | 23 | `net_cookie_save` | ✔ | 登录 | 压缩后写内存 + 落 SD（`netease_cookie.txt`），日志打 raw→compact 字节数 |
| 2458 | 2462 | 5 | `online_netease_logged_in` | ✘ | 登录 | 触发懒加载后返回登录态 |
| 2472 | 2481 | 10 | `online_netease_cookie_reload` | ✘ | 登录 | 未登录时才重读 SD（解决"SD 晚挂导致永久未登录"） |
| 2485 | 2501 | 17 | `nvs_load_nick` | ✔ | NVS | 懒加载昵称（latch `s_net_nick_loaded`） |
| 2503 | 2507 | 5 | `online_netease_nickname` | ✘ | 账号 | 返回昵称（空串 = 未知） |
| 2509 | 2541 | 33 | `online_netease_nickname_set` | ✘ | 账号 | 值未变则跳过写 flash；写/清 NVS |
| 2543 | 2547 | 5 | `online_netease_cookie` | ✘ | 登录 | 返回 cookie 串（空串 = 未登录） |
| 2549 | 2559 | 11 | `online_netease_logout` | ✘ | 登录 | 清内存 + 删 cookie 文件 + 清昵称 + `online_cache_clear()` |
| 2571 | 2620 | 50 | `net_api_get_ex` | ✔ | 账号 HTTP | base+path（可选 `?cookie=<urlencoded>`）走 `http_get_text`；**static url[4096]** |
| 2622 | 2625 | 4 | `net_api_get` | ✔ | 账号 HTTP | 不带 cookie 的薄封装 |
| 2627 | 2663 | 37 | `online_netease_qr_key` | ✘ | 扫码登录 | `GET /login/qr/key?timestamp=&ua=` → `unikey` |
| 2665 | 2732 | 68 | `online_netease_qr_create` | ✘ | 扫码登录 | `GET /login/qr/create?...&qrimg=true` → `qrurl` + base64 PNG（mbedtls 解码，PSRAM） |
| 2734 | 2780 | 47 | `online_netease_qr_poll` | ✘ | 扫码登录 | `GET /login/qr/check?key=&timestamp=&ua=` → code 800/801/802/803；803 时存 cookie |
| 2790 | 2801 | 12 | `json_id_str` | ✔ | 纯逻辑 | 数字/字符串 ID → 字符串（走 `valuedouble`，防 32 位溢出） |
| 2804 | 2822 | 19 | `pick_array` | ✔ | 纯逻辑 | 在 root 或 `data/playlist/result/body` 包装里找目标数组 |
| 2825 | 2885 | 61 | `song_from_json` | ✔ | 纯逻辑 | 网易云 song 对象 → `online_song_t`（ar/artists、al/album 双格式；**补 lrc_url**） |
| 2887 | 2903 | 17 | `fill_songs` | ✔ | 纯逻辑 | 数组批量转换，返回实际条数（0 → NOT_FOUND） |
| 2905 | 2944 | 40 | `online_netease_account` | ✘ | 账号数据 | `GET /user/account`（带 cookie）→ uid/nickname，成功时顺带存昵称 |
| 2946 | 3035 | **90** | `online_netease_account_playlists` | ✘ | 账号数据 | `GET /user/playlist` **按 limit=100 翻页**（最多 10 页，`more` 判续） |
| 3041 | 3063 | 23 | `playlist_detail_fallback` | ✔ | 账号数据 | 第一页就失败时退 `GET /playlist/detail`（老接口，仅小歌单可行） |
| 3065 | 3130 | 66 | `online_netease_playlist_page` | ✘ | 账号数据 | `GET /playlist/track/all` 每页 100 首循环到 max/末尾；首跳失败走兜底 |
| 3132 | 3166 | 35 | `online_netease_daily_songs` | ✘ | 账号数据 | `GET /recommend/songs` → `dailySongs`；先体检 MUSIC_U 再请求 |
| 3168 | 3227 | 60 | `online_netease_song_url` | ✘ | 账号数据 | `GET /song/url/v1?id=&level=standard` → 直链 + size/br（时长 = size×8/br） |

**总量**：89 个函数 / 3227 行；最大的 5 个：`http_download`(150)、`http_get_text`(132)、
`online_song_stream_info`(123)、`cookie_compact`(101)、`online_song_cover_data`(93)、
`online_netease_account_playlists`(90)。

---

## 3. 全部全局 / 静态变量清单

### 3.1 顶层静态对象

| 行号 | 名字 | 类型 | 用途 / 所有权 | 并发 |
| --- | --- | --- | --- | --- |
| 46 | `TAG` | `const char *` | 日志标签 `"online"` | 只读 |
| 135 | `s_ready` | `bool` | init 幂等标志 | 读多写一，无锁 |
| 136 | `s_ids` | `char[8][24]` | 歌单 ID 表（NVS 镜像） | 无锁，读写混合 |
| 138 | `s_names` | `char[8][40]` | 歌单显示名（与 `s_ids` 下标一一对应） | 同上 |
| 139 | `s_id_count` | `size_t` | 有效歌单数（`s_ids/s_names` 的长度） | 同上 |
| 141 | `s_last_err` | `char[64]` | 最近一次失败的中文原因（`online_last_error()` 直接返回其指针） | **跨任务覆盖**，无锁 |
| 156 | `s_cover_cache` | `mem_cache_t[8]` | 封面字节 LRU 表（本模块所有，非拷贝外发） | 无锁 |
| 157 | `s_cover_cache_bytes` | `size_t` | 封面缓存当前字节数（与上限 4 MB 比较） | 无锁 |
| 158 | `s_lrc_cache` | `mem_cache_t[8]` | 歌词文本 LRU 表 | 无锁 |
| 159 | `s_lrc_cache_bytes` | `size_t` | 歌词缓存字节数（上限 256 KB） | 无锁 |
| 1910 | `s_hist` | `char[10][64]` | 搜索历史（新→旧） | 无锁 |
| 1911 | `s_hist_n` | `size_t` | 历史条数 | 无锁 |
| 2071 | `s_net_base` | `char[96]` | 账号 API base 缓存（懒加载，`online_netease_api_base()` 返回其指针） | 无锁 |
| 2072 | `s_net_cookie` | `char[4096]` | 压缩后的登录 cookie（含 MUSIC_U） | 无锁 |
| 2073 | `s_net_cookie_loaded` | `bool` | "已尝试读过 cookie"的 latch（**失败也会置位**） | 无锁 |
| 2074 | `s_net_logged_in` | `bool` | 登录态 | 无锁 |
| 2077 | `s_net_nick` | `char[64]` | 账号昵称（NVS 镜像，`online_netease_nickname()` 返回其指针） | 无锁 |
| 2078 | `s_net_nick_loaded` | `bool` | 昵称懒加载 latch | 无锁 |

### 3.2 函数级 `static` 缓冲（**拆分时最容易踩的"隐藏全局"**）

| 行号 | 所在函数 | 名字 / 大小 | 为什么必须知道 |
| --- | --- | --- | --- |
| 408 | `net_auth_hdr` | `static char hdr[192]` | 返回值指向它；多任务并发会互相覆盖 |
| 428 | `url_for_log` | `static char buf[192]` | 同上，注释自己写了"只保证取完立刻用" |
| 2252 | `net_cookie_builtin_load` | `static char music_u[1024]` | MUSIC_U 实测 ~700 字符，**故意不放栈**（工作线程栈小） |
| 2350 | `cookie_compact` | `static char pairs[12][1024]`（~12 KB） | 注释 2344–2349：曾因放栈上导致 `Stack protection fault` |
| 2439 | `net_cookie_save` | `static char compact[2048]` | 同上，避免 4 KB 栈溢出 |
| 2574 | `net_api_get_ex` | `static char url[4096]` | 账号接口 URL 可达 2–3 KB；也是 `buffer_size_tx` 要 ≥4 KB 的原因 |
| 1506–1507 | `mp3_bitrate_bps` | `static const int v1l3/v2l3[16]` | 纯常量表，无状态 |
| 2342 | `cookie_compact` | `static const char *const attrs[]` | cookie 属性黑名单 |
| 2406 | `cookie_compact` | `static const char *const prio[]` | `MUSIC_U=` / `__csrf=` 优先输出 |
| 2810 | `pick_array` | `static const char *const wraps[]` | `data/playlist/result/body` 包装层名 |
| 1048 | `url_encode` | `static const char *hex` | 十六进制表 |

### 3.3 关键宏（拆分后要跟着搬的常量）

`ONLINE_ROOT`/`ONLINE_AUDIO_DIR`/`ONLINE_COVER_DIR`/`ONLINE_LRC_DIR`/`ONLINE_PL_FMT`(51–56)、
`NVS_NS`/`NVS_KEY_*`(58–67)、`API_BASE`(69)、`API_SEARCH_BASE`(73)、`HTTP_TIMEOUT_MS 15000`(74)、
`HTTP_BUF 2048`(75)、`JSON_MAX_LEN 1 MB`(77)、`COVER_MAX_BYTES 4 MB`(82)、
`ONLINE_COVER_PARAM 300`(101)、`AUDIO_MAX_BYTES 250 MB`(104)、`AUDIO_CACHE_MAX 48`(106)、
`COVER_CACHE_MAX/BYTES_MAX`(109–110)、`LRC_CACHE_MAX/BYTES_MAX`(111–112)、
`ONLINE_PL_MAGIC/VERSION`(114–115)、`HTTP_SINK_MIN 16 KB`(305)、`NET_AUTH_TOKEN`(398–402)、
`NET_NVS_NS/KEY_BASE`(2036–2037)、`NET_BASE_MAX 96`(2038)、`NET_COOKIE_MAX/SEND`(2050–2051)、
`NET_JSON_MAX 512 KB`(2059)、`NET_COOKIE_FILE`(2060)、`NET_BASE_DEFAULT`(2065–2069)、
`NET_URL_MAX 4096`(2569)。

---

## 4. 外部依赖清单

### 4.1 依赖的外部 API（按模块）

| 模块 | 用到的符号（行号示例） | 用途 |
| --- | --- | --- |
| `esp_http_client` | `esp_http_client_init`(486/618/1595/1707)、`config_t`(461/593/1570/1682)、`esp_http_client_set_header`(494)、`esp_http_client_perform`(496)、`esp_http_client_open`(622/1599/1711)、`fetch_headers`(629/1606/1718)、`esp_http_client_read`(687/1630)、`get_status_code`(497/630/1607/1719)、`get_content_length`(668)、`cleanup` | 所有网络 IO（文本/JSON/封面/歌词/音频下载/302 探测） |
| `esp_crt_bundle` | `esp_crt_bundle_attach`(466/596/1573/1685) | https 证书包（CDN 音频、部分封面） |
| `cJSON` | Parse/Delete/GetObjectItem/GetArrayItem/GetArraySize/Is*/ArrayForEach | 所有 JSON 解析 |
| `mbedtls` | `mbedtls_base64_decode`(2719) | 二维码 PNG base64 → 字节 |
| `nvs` / `nvs_flash` | `nvs_open/get_u8/get_str/set_u8/set_str/erase_key/commit/close` | 歌单 ID/名字、搜索历史、昵称、API base |
| `esp_heap_caps` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)`(256/337) | 大缓冲（响应体、封面、数组）优先 PSRAM |
| `esp_timer` | `esp_timer_get_time`(176/221/2643/2680/2747) | LRU 时间戳 + 二维码请求缓存破坏参数 |
| FATFS / newlib | `fopen/fread/fwrite/fclose/unlink/rename/stat/mkdir/rmdir/opendir/readdir/rewinddir/closedir` | 歌单二进制、音频缓存、cookie 文件、目录清理 |
| `wifi_service` | `wifi_service_has_ip()`(1800) | 在线功能前置条件 |
| `esp_log` | `ESP_LOGx` | 日志 |
| 链接期 | `_binary_ncm_ck_builtin_start`(2186/2248) | 由 `CMakeLists.txt` 的 `EMBED(TEXT)` 注入的内置 cookie（条件编译） |

**未使用**：LVGL、FreeRTOS 原语、audio_service、media_meta、bt_service、bsp、esp_lvgl_adapter。
本模块与 UI 完全解耦，只通过 `online_service.h` 被 app 层调用。

### 4.2 本模块对外导出的 API（`include/online_service.h`）

**结构体 / 宏**

- `online_song_t`（h:52–59）：`id / title[128] / artist[96] / cover_url[320] / audio_url[320] / lrc_url[320]`
- `online_account_t`（h:232–235）：`uid[24] / nickname[64]`
- `online_acct_playlist_t`（h:240–246）：`id[24] / name[40] / cover_url[320] / track_count / self_created`
- 宏：`ONLINE_MAX_PLAYLISTS 8`、`ONLINE_ID_MAX 24`、`ONLINE_NAME_MAX 40`、`ONLINE_TITLE_MAX 128`、
  `ONLINE_ARTIST_MAX 96`、`ONLINE_URL_MAX 320`、`ONLINE_HISTORY_MAX 10`、`ONLINE_KW_MAX 64`、
  `NET_QR_WAIT/SCAN/OK/EXPIRE = 801/802/803/800`、`ONLINE_PL_PAGE_SONGS 100`

**函数（30 个）**

| 分组 | 函数 |
| --- | --- |
| 生命周期 | `online_service_init`(h:63)、`online_cache_clear`(h:291) |
| 网络 | `online_has_network`(h:66) |
| 歌单 | `online_playlist_count/id/name/rename`(h:70–79)、`online_playlist_add`(h:106)、`online_playlist_remove`(h:109)、`online_playlist_load`(h:115) |
| 搜索 | `online_search`(h:124) |
| 历史 | `online_history_count/at/add/remove/clear`(h:89–94) |
| 错误 | `online_last_error`(h:100) |
| 播放准备 | `online_song_stream_url`(h:134)、`online_song_stream_info`(h:140)、`online_song_prepare_stream`(h:142)、`online_song_prepare`(h:148) |
| 封面/歌词 | `online_song_cover_data`(h:156)、`online_song_lyrics_data`(h:288) |
| 账号/登录 | `online_netease_api_base(_set)`(h:180–181)、`online_netease_qr_key/create/poll`(h:184–189)、`online_netease_logged_in`(h:199)、`online_netease_cookie`(h:200)、`online_netease_cookie_reload`(h:209)、`online_netease_nickname(_set)`(h:219–221)、`online_netease_logout`(h:223) |
| 账号数据 | `online_netease_account`(h:238)、`online_netease_account_playlists`(h:249)、`online_netease_playlist_page`(h:267)、`online_netease_daily_songs`(h:271)、`online_netease_song_url`(h:279) |

**外部调用方（拆分时必须保持契约）**

- `components/apps/app_online/app_online.cc`：113 `online_playlist_load`（自检）、514/521（歌单/搜索
  工作项）、558/565/571（账号）、592（歌单分页）、603（每日推荐）、1408 `online_netease_cookie_reload`
- `components/apps/app_music/app_music.cc`：2019/2039 `online_song_prepare_stream`、
  2029 `online_song_stream_info`、2046 `online_song_cover_data`、2047 `online_song_lyrics_data`

---

## 5. 线程 / 回调模型

| 上下文 | 本文件里发生什么 | 有锁吗 |
| --- | --- | --- |
| **app_online 的工作线程**（`online_work_task`，`app_online.cc:486`） | 调用 `online_playlist_load/search/netease_*` 等全部阻塞 API | ❌ 无锁（该线程是这些 API 的**主要**调用者） |
| **app_music 的在线准备路径**（`app_music.cc:2019–2047`） | `online_song_stream_info` / `online_song_prepare_stream` / `cover_data` / `lyrics_data` | ❌ 无锁 —— **与 app_online 工作线程可能并发** |
| `esp_http_client` 事件回调（`http_evt`，356–379） | 在**调用 `perform/open/read` 的那个任务**里被同步调用（IDF 事件回调不是独立任务） | 只改本栈上的 `http_sink_t`，天然安全；`Location` 抓取依赖它 |
| 其它 | 无 `esp_timer`/`lv_timer` 回调、无自建任务、无 ISR | — |

**结论**：头文件 h:30–31 明写"所有 API 都是同步阻塞的，必须放在工作线程里调用"，
但**代码里没有任何互斥**。全局可变状态（`s_ids/s_names/s_hist/s_cover_cache/s_lrc_cache/
s_last_err/s_net_cookie/s_net_base/s_net_nick` + 3.2 的函数级 static 缓冲）在两个调用者
并发时会互相破坏（详见风险 R1–R5）。

**阻塞/超时事实**：`HTTP_TIMEOUT_MS = 15000`（74 行）被写进每个 `cfg.timeout_ms`
（465/595/1572/1684）；没有设置 `cfg.keep_alive_enable`（显式 `false`，避免长连接占用）、
没有 `cfg.disable_auto_redirect=false` 的自动跳转（**全部手动处理 302**）、没有单独设
`cfg.addr_type`/`cfg.port`（URL 全量给），`cfg.buffer_size = 4096` 与
`cfg.buffer_size_tx = 4096`（476/484 等，注释 469–483 解释：默认 512 装不下 cookie 查询串）。

---

## A. 附加必答 1：HTTP 请求是怎么发的

### A.1 三个发送原语

| 原语 | 行号 | 阻塞点 | 用途 | 超时 |
| --- | --- | --- | --- | --- |
| `http_get_text(url, &out, &len, max_len)` | 442–573 | `esp_http_client_perform()`(496) **一次性阻塞到响应结束** | JSON（歌单/搜索/账号/二维码）、LRC 文本、封面字节 | `timeout_ms = 15000`(465) |
| `http_download(url, dest, min_size, sniff)` | 579–728 | `esp_http_client_open/fetch_headers/read` 循环(622/629/687) | 整曲音频落 SD（兜底路径） | `timeout_ms = 15000`(595)；每块 `HTTP_BUF=2048`(75/682) |
| 流地址探测（内联在两处） | 1568–1654 / 1680–1741 | `open`(1599/1711) + `fetch_headers`(1606/1718)；`stream_info` 额外 `read` 4 KB(1630) | 解析 302 拿最终 URL（+时长） | `timeout_ms = 15000`(1572/1684) |

**共性配置**（每处都手写一遍，重复 4 份）：

```c
cfg.url = cur;  cfg.timeout_ms = 15000;  cfg.crt_bundle_attach = esp_crt_bundle_attach;
cfg.disable_auto_redirect = true;        /* ★ 自己读 Location 再发一跳 */
cfg.keep_alive_enable = false;
cfg.buffer_size = 4096; cfg.buffer_size_tx = 4096;
cfg.event_handler = http_evt; cfg.user_data = &sink;
```

**为什么关自动跳转**（注释 386–391）：该实例把 `http://<api-host>/api?...` 302 到
https 同路径，IDF 的自动跳转实测会**丢查询串** → 服务器回 400。

### A.2 响应接收：`http_sink_t` + `sink_reserve` + `http_evt`

- `http_sink_t`（293–302）：`buf/len/cap/limit/overflow` + `location[512]`。
  注释 299–301 明确：`esp_http_client_get_header()` 取的是**请求**头，Location 只能从
  事件回调里拿。
- `sink_reserve`（320–354）：从 `HTTP_SINK_MIN = 16 KB`(305) **翻倍**扩容，`heap_caps_malloc`
  走 PSRAM、失败退内部 RAM；`need > limit` 或分配失败 → `overflow = true` 并把错误传上去。
  注释 307–318 记录历史坑：早先 `malloc(max_len)`（封面 4 MB）实测 `ESP_ERR_NO_MEM`。
- `http_evt`（356–379）：`HTTP_EVENT_ON_HEADER` 里 `strcasecmp("Location")` → 存 `s->location`；
  `HTTP_EVENT_ON_DATA` 里扩容 + 追加 + 补 `\0`；扩容失败返回 `ESP_FAIL` 中止传输。

### A.3 鉴权 / 日志安全 / Cookie 传输

- `net_auth_hdr()`（406–414）：`Authorization: Bearer <CONFIG_ONLINE_NETEASE_API_TOKEN>`
  （398–402）。**只对 `online_netease_api_base()` 前缀的 URL 加**（493 行判断），
  绝不发给第三方 meting 代理（注释 404–405）。
- `url_for_log()`（426–440）：日志里**砍掉查询串**，因为账号接口把 cookie 塞在
  `?cookie=MUSIC_U=…`（注释 416–424）。所有失败日志都走它（543/548/555/624/662）。
- cookie 传输：`net_api_get_ex`（2571–2620）把 cookie 做**百分号编码**后拼进查询串
  （2595–2609），并在发送前 `online_netease_cookie_reload()`（2586）兜一次"SD 晚挂"。
- 401/403 特判：498–501 打出"请在 sdkconfig 填 CONFIG_ONLINE_NETEASE_API_TOKEN"。

### A.4 重定向处理（自己实现，最多 4 跳）

| 位置 | 相对 Location 处理 | 失败语义 |
| --- | --- | --- |
| `http_get_text` 511–537 | ✔ 517–534（用 `scheme://host` 前缀拼回） | `s_last_err = "重定向缺少 Location"` / 循环满 4 跳 → `"重定向次数过多"` |
| `http_download` 632–659 | ✔ 640–656（同上） | 同上 |
| `online_song_stream_info` 1609–1619 | ✖ 直接用 Location 原文（1617） | 同上 |
| `online_song_stream_url` 1722–1730 | ✖ 同上（1727） | 同上 |

---

## B. 附加必答 2：Meting API 的调用点

| 接口 | 代码位置 | URL 拼装 | 备注 |
| --- | --- | --- | --- |
| 歌单 | `online_playlist_add` **970** | `API_BASE(69) + "?server=netease&type=playlist&id=%s"` | 抓完 `parse_songs` → `pl_save` |
| 搜索 | `online_search` **1131–1133** | `url_encode(keyword)` → `API_SEARCH_BASE(73) + "?server=netease&type=search&id=%s"` | 关键词放 `id=`；注释 70–72：`keywords=` 在该实例上会被忽略 |
| 歌词 URL（**间接**） | `song_from_json` **2880–2883** | `API_BASE + "?server=netease&type=lrc&id=%u"` | 账号歌没有 meting 的 `lrc` 字段，这里合成 URL，再由 `online_song_lyrics_data`(1476) 取纯文本 |
| 播放地址（**间接**） | `song->audio_url`，来自 `parse_songs` **943**（JSON 的 `url` 字段） | 即 meting `type=url&id=<歌曲ID>` | 真正"发出去"的是 302 探测（1565/1677 起） |
| 封面（**间接**） | `song->cover_url`，来自 `parse_songs` **942**（JSON 的 `pic` 字段） | `p*.music.126.net` 直链 + `?param=300y300`（1383–1389） | 注释 84–100：必须给两维尺寸，否则返回 7.6 MB 原图 |
| 自建账号 API base | `online_netease_api_base`(2124–2141)、`_set`(2143–2165) | NVS `online/nb` → `CONFIG_ONLINE_NETEASE_API_BASE` → `"http://localhost:3000"`(2065–2069) | 所有 `/user/*`、`/login/*`、`/playlist/*`、`/song/url/v1`、`/recommend/songs` 都拼在它后面（2575） |

账号类路径拼装点：`/user/account`(2912)、`/user/playlist`(2966)、`/playlist/detail`(3045)、
`/playlist/track/all`(3087–3089)、`/recommend/songs`(3148)、`/song/url/v1`(3183)、
`/login/qr/key`(2642)、`/login/qr/create`(2678–2680)、`/login/qr/check`(2746–2747)。

---

## C. 附加必答 3：歌单二进制落盘格式

文件路径：`/sdcard/online/pl_<歌单ID>.bin`（`ONLINE_PL_FMT` 56 行，`pl_path` 801–808）。
小端、无字节序转换、**无 CRC/校验和**、无文件长度字段。

### C.1 头（`online_pl_hdr_t`，118–133 定义，写于 821–829）

| 偏移 | 字节 | 字段 | 值 / 说明 |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `ONLINE_PL_MAGIC = 0x4F504C31`（`'OPL1'`，114） |
| 4 | 4 | `version` | `ONLINE_PL_VERSION = 1`（115） |
| 8 | 4 | `count` | 记录条数（`pl_load` 校验 `1 ≤ count ≤ 5000`，862） |
| 12 | 4 | `rsv` | 恒 0 未用 |

### C.2 记录（`online_rec_t`，118–126 定义，写于 830–843；**定长 1252 字节**）

| 偏移 | 字节 | 字段 | 来源 |
| --- | --- | --- | --- |
| 0 | 4 | `id` (uint32) | `online_song_t.id` |
| 4 | 128 | `title[128]` | `ONLINE_TITLE_MAX`，`str_copy` 截断（UTF-8 安全） |
| 132 | 96 | `artist[96]` | `ONLINE_ARTIST_MAX` |
| 228 | 320 | `cover_url[320]` | `ONLINE_URL_MAX` |
| 548 | 320 | `audio_url[320]` | **账号歌为空串**（直链靠 cookie 现取） |
| 868 | 320 | `lrc_url[320]` | Meting 的 `lrc` 或合成 URL |
| 1188 | 64 | `rsv[64]` | 恒 0（预留） |

> ⚠ **文档漂移**：文件头注释第 6 行写"定长 896 字节"，与实际的
> `4+128+96+320+320+320+64 = 1252` 不符（896 应是早期 `ONLINE_URL_MAX` 更小时的残留）。
> 拆分时应把"格式契约"集中到一处并加静态断言（`_Static_assert(sizeof(online_rec_t)==1252)`）。

### C.3 读写行为

- `pl_save`（811–848）：`fopen("wb")` → 写头 → 逐条 `str_copy` 组装后 `fwrite`；
  任一步失败 → `fclose + unlink` 返回 `ESP_FAIL`。**非原子**（无 `.tmp` + rename）。
- `pl_load`（851–899）：读头校验（magic/version/count 范围）→ `psram_alloc(count*sizeof(online_song_t))`
  → 逐条 `fread`，**`fread` 失败就 break**（截断文件不报错），标题为空串的记录被**跳过并压缩**
  （887–889）；`got == 0` 才返回 `ESP_ERR_INVALID_RESPONSE`。
- `search_local`（1065–1110）：**自己另写了一份读循环**（1076–1101，只校验 magic），
  与 `pl_load` 逻辑重复 —— 拆分时应合并到一个 `pl_read_records()`。
- `online_playlist_remove`（1005–1024）：NVS 数组前移 + `unlink` 文件。
- 其它落盘产物：`/sdcard/online/audio/<歌曲ID><ext>`（`ext` 由 `ext_from_sniff` 决定，
  1259–1290；先写 `<id>.part` 再 rename）、`/sdcard/online/netease_cookie.txt`（`NET_COOKIE_FILE` 2060）。
  历史遗留的 `/sdcard/online/cover/`、`/sdcard/online/lrc/`、`audio/*.lrc` 在 init 里清掉
  （1841–1862 调 `wipe_dir` 1843/1844）。

  > **2026-11 更新**：init 不再无条件创建 `/sdcard/online/audio`、也不再清扫 `audio/*.lrc`
  > （正常播放是纯流式，不落盘）；该目录改成"兜底下载真要写文件时"在
  > `online_song_prepare()` 里按需 `os_mkdir_if_needed()`。
  > `cover/`、`lrc/` 的 wipe 保留 —— 那是给从老固件升上来的 SD 卡擦屁股用的。

---

## D. 附加必答 4：封面 / 歌词 PSRAM 缓存的所有权与生命周期

### D.1 容器

`mem_cache_t`（149–154）：`{uint32_t id; uint8_t *data; size_t len; uint32_t last_ms;}`；
两张定长表：`s_cover_cache[8]` / `s_lrc_cache[8]`（156–159），上限
`COVER_CACHE_BYTES_MAX = 4 MB`(110) / `LRC_CACHE_BYTES_MAX = 256 KB`(112)。

| 操作 | 行号 | 行为 |
| --- | --- | --- |
| 命中 | `mem_cache_find` 172–181 | 更新 `last_ms`（**写操作**，无锁） |
| 取槽 | `mem_cache_slot` 184–203 | 先找空槽；否则选 `last_ms` 最小者 → 递减 `*bytes` → `mem_cache_free_slot` |
| 插入 | `mem_cache_put` 206–230 | 接管 `data` 所有权；插入后 `while (*bytes > cap_bytes)` 继续淘汰 |
| 清空 | `psram_cache_clear` 1764–1774 | 逐槽 free + 计数归零（被 `online_cache_clear` 1794 与 `logout` 2556 调用） |

### D.2 对外 API 的所有权契约（**拆分时不能改的语义**）

| API | 行号 | 命中缓存 | 未命中 | 返回内存归属 | 释放方式 |
| --- | --- | --- | --- | --- | --- |
| `online_song_cover_data` | 1348–1440 | `psram_alloc(hit->len)` + memcpy（1360–1364） | `http_get_text` 下载 → `cover_format` 校验（1412）→ `mem_cache_put`（1427，把**下载缓冲交给缓存**）→ 再 `psram_alloc(len)` 拷贝给调用方（1429–1433） | **调用方所有**（h:152–153 契约） | `free()`（调用方；`app_music.cc:2046` 之后自行释放） |
| `online_song_lyrics_data` | 1446–1495 | `psram_alloc(hit->len+1)` + memcpy + 补 `\0`（1458–1463） | 下载（1476）→ 入缓存（1481）→ 拷贝（1483–1488） | **调用方所有** | `free()` |
| `online_song_prepare_stream` | 1747–1761 | — | 取封面+歌词后**立即 free**（1756–1757） | 无外发内存 | — |
| `online_song_prepare` | 1259–1307 | — | 命中/下载音频文件 + 预热封面歌词后 free（1300/1305） | `audio_path` 由调用方给 | — |

**要点**：
1. 缓存里的内存**永远不直接外发**（内部注释 146–147、18–20 明确"返回调用方 malloc 拷贝"），
   所以 LRU 淘汰不会影响正在播的封面。
2. 失败路径：下载成功但 `psram_alloc` 拷贝失败 → 返回 `ESP_ERR_NO_MEM`，**缓存里仍留着数据**
   （不泄漏也未释放，属可接受）；`http_get_text` 失败/长度 < 64 时 `free(raw)`（1405）不泄漏。
3. 封面**格式白名单**在 `cover_format`（1325–1346）按魔数判断；不支持时返回
   `ESP_ERR_NOT_SUPPORTED` 并把原因写进 `s_last_err`（1420）。
4. 与 `media_meta` 的耦合：`COVER_MAX_BYTES(4 MB)` 必须 ≤ `MEDIA_META_COVER_FILE_MAX`
   （注释 78–82），拆分后这两个常量应放进同一处或加 `_Static_assert`。
5. **无锁**：三张表 + 计数 + `s_last_err` 全部裸访问；`mem_cache_find` 还会写 `last_ms`
   —— 只要两个任务同时请求封面（app_music + app_online）就有数据竞争。
6. **`mem_cache_put` 224–229 存在死循环隐患**（见风险 R1）。

---

## E. 附加必答 5："播放地址 302 解析"的完整调用链

```
app_music.cc（播放准备工作项）
 └─ online_song_prepare_stream(song)            online_service.cc:1747   /* 只预热封面+歌词 */
 └─ online_song_stream_info(song, buf, cap, &bytes, &dur)   :1532        /* 主路径 */
      ├─ if (song->audio_url[0] == '\0')                                  /* 账号歌 */
      │     └─ online_netease_song_url(id, ...)  :3168
      │           └─ net_api_get_ex("/song/url/v1?id=<id>&level=standard", with_cookie=true) :2571
      │                 └─ http_get_text(...)    :442 → esp_http_client_perform :496
      │           ← JSON data[0].url（**已是直链，不再 302**）+ size/br
      │           ← out_dur_ms = size*8*1000/br（准确时长，:3210–3214）
      └─ else（Meting 歌）
            for (hop = 0; hop < 4; hop++)     :1568
              esp_http_client_init(cfg, disable_auto_redirect=true)  :1570–1595
              esp_http_client_open(cli, 0)                            :1599   /* 只发头，不读 body */
              total = esp_http_client_fetch_headers(cli)              :1606   /* = Content-Length */
              status = esp_http_client_get_status_code(cli)           :1607
              ├─ 301/302/303/307/308 → loc = probe.location（http_evt:363–368 抓的）
              │     cleanup → cur = loc → continue                    :1609–1619
              │     ⚠ 这里**不处理相对 Location**（对比 :517/:640）
              ├─ != 200 → s_last_err "服务器返回 HTTP %d（版权受限？）"  :1620–1624
              └─ 200 → malloc(4096) → esp_http_client_read(cli,…)      :1627–1630
                    └─ mp3_bitrate_bps(head, got)                      :1504–1524
                    └─ *out_dur_ms = total * 8 * 1000 / bitrate        :1634–1640
                    *out_bytes = total; out = cur（最终 https 直链）    :1646–1649

app_music.cc
 └─ audio_service_play_uri(最终地址)  /* GMF 边下边播；403 过期时 app_music 重刷 */
```

三者的区别与关系：

| 函数 | 行号 | 是否读 body | 是否返回时长/大小 | 用途 |
| --- | --- | --- | --- | --- |
| `online_song_stream_url` | 1656–1741 | ✖（只 `fetch_headers`） | ✖ | 只要地址（如自检、快速校验） |
| `online_song_stream_info` | 1532–1654 | ✔ 读 4 KB 前缀 | ✔ `out_bytes`/`out_dur_ms` | **播放主路径**（进度条/总时长） |
| `online_song_netease_song_url` | 3168–3227 | ✖（JSON 里直接给 size/br） | ✔ | 账号歌分支（无 meting URL） |

失败/边界行为：4 跳用尽 → `"重定向次数过多"`（1652）；302 无 Location →
`"重定向缺少 Location"`（1614）；非 200 → `"服务器返回 HTTP %d（版权受限？）"`（1622）；
无网 → `online_has_network()` 提前 `ESP_ERR_INVALID_STATE`（1560–1562）；
`mp3_bitrate_bps` 解析不出（FLAC/OGG/M4A 流）→ `*out_dur_ms` 保持 0（不强塞错误）。

**与播放器侧配合的既有约定**（`app_music.cc`，见 AGENTS.md §11.4）：
直链带时间戳签名会 403 → `online_url_refresh()` 复用这条链路重刷并断点续播；
因此**这条链路必须保持"幂等、可重复调用、每次都能拿到新签名地址"**的语义，重构时不能加缓存。

---

## F. 附加必答 6：为"自研 HTTP 传输替换"准备的耦合点清单

| # | 耦合点 | 位置 | 替换时的等价要求 |
| --- | --- | --- | --- |
| 1 | `esp_http_client_config_t` 初始化（4 份重复代码） | 461–484、593–616、1570–1593、1682–1705 | 必须收敨成一个 `os_http_req_t`，否则自研传输要改 4 处 |
| 2 | **关闭自动重定向 + 从事件回调抓 `Location`** | 467/597/1574/1686；`http_evt` 363–368 | 自研传输必须暴露"响应头回调/取头"能力，或直接返回最终 URL |
| 3 | 相对 Location 拼回主机 | 517–534、640–656 | 统一的 URL 解析工具（绝对/相对/跨协议） |
| 4 | 分块/流式接收回调 | `HTTP_EVENT_ON_DATA` 370–377 + `sink_reserve` | 自研传输需支持"边收边回调 + 可中止（返回失败即断开）" |
| 5 | 缓冲上限语义（`limit` → `overflow`） | 325–328、541–545 | 需要"超限可诊断"的错误通道，不能只回一个错误码 |
| 6 | 上传/请求头拼接大小（`buffer_size` / `buffer_size_tx`） | 476/484 等 + 注释 469–483 | 自研传输不能有 512 B 请求行硬限制（否则 1 KB cookie 发不出去） |
| 7 | TLS 证书包 + 系统时间依赖 | 466/596/1573/1685 + 文件头注释 9–12 | https（音频 CDN）需要 SNTP 已完成；自研传输要么复用 mbedtls 校验证书，要么明确接受明文回退策略 |
| 8 | `Authorization` 头按 base 前缀下发 | 492–495 | 需要"按目标主机决定是否带凭证"的策略钩子（防 token 泄漏给第三方） |
| 9 | 查询串里带 cookie（URL 编码） | 2589–2610 | 自研传输若能自定义请求头，应改为 `Cookie:` 头（更安全、更短），但要与服务端契约同步 |
| 10 | `Content-Length` / `fetch_headers` 返回值 | 668、1606 | 流式播放的"总时长/seek 映射"依赖它（AGENTS.md §11.4 GMF 那个坑） |
| 11 | 逐块读取接口（`esp_http_client_read` 循环） | 686–709、1630 | 下载路径需要"读 N 字节返回实际数、0=EOF、<0=错误"的等价语义 |
| 12 | 超时语义（`timeout_ms = 15000` 覆盖连接+读） | 74 + 4 处 | 自研传输要区分 connect/read/total 超时，并保持"15 s 量级"不破坏 UI 预期 |
| 13 | 4 跳重定向上限与错误文案 | 456/591/1568/1680；514/571/1652/1739 | 文案进 `s_last_err` 给 Toast，重构后要保持中文可读 |
| 14 | `keep_alive_enable = false` | 468/598/1575/1687 | 自研传输若默认复用长连接，需评估与 GMF 流式（4 MB 环形缓冲）抢连接的影响 |

**建议抽象接口**（放 `os_http.h`，实现见 §6）：

```c
/* 返回值全部用 esp_err_t，语义与现有一致；回调返回 false 表示中止传输 */
typedef bool (*os_http_data_cb)(const uint8_t *data, size_t len, void *user);

typedef struct {
    const char *url;              /* 允许 302：内部按 hop 循环 */
    int         timeout_ms;       /* 默认 15000 */
    size_t      max_bytes;        /* 响应上限（超限报 ESP_ERR_INVALID_SIZE） */
    const char *auth_bearer;      /* 仅当 url 前缀匹配受信 base 时才发 */
    os_http_data_cb on_data;      /* NULL = 收到内存缓冲（返回 malloc 缓冲） */
    void       *user;
} os_http_req_t;

esp_err_t os_http_get(const os_http_req_t *req, uint8_t **out_buf, size_t *out_len);
esp_err_t os_http_get_headers(const char *url, int *out_status, int64_t *out_content_len,
                              char *out_location, size_t loc_cap);
esp_err_t os_http_download(const char *url, const char *dest, size_t max_bytes,
                           uint8_t sniff[16]);
esp_err_t os_http_set_transport(const os_http_transport_t *impl);   /* 自研传输注入点 */
```

---

## 6. 拆分方案

### 6.1 目标目录树（保持 `online_service` 组件不变，只拆内部文件）

```
components/services/online_service/
├── CMakeLists.txt                     # 只改 SRCS 列表（+ 可选 host 测试目标）
├── include/
│   ├── online_service.h               # ★ 对外契约：一行不改（唯一 public 头）
│   └── internal/                      # 新增：仅组件内部使用
│       ├── os_http.h                  #   HTTP 抽象层（§F 的接口）
│       ├── os_json.h                  #   JSON → online_song_t 映射
│       ├── os_pl_format.h             #   歌单二进制格式（magic/记录布局/编解码）
│       ├── os_store.h                 #   NVS + 文件 + 目录 的薄封装
│       ├── os_cache.h                 #   通用 LRU 字节缓存容器（可 host 单测）
│       ├── os_net.h                   #   网络判定 / s_last_err / TAG
│       └── os_util.h                  #   str_copy / psram_alloc / url_encode 等
└── src/
    ├── online_service.cc              # 仅保留 init + 查询 + 编排（~180 行）
    ├── os_http_client.cc              # esp_http_client 实现（~360 行）
    ├── os_util.cc                     # 工具（~120 行）
    ├── os_json.cc                     # 纯逻辑 JSON 映射（~230 行）
    ├── os_pl_format.cc                # 纯逻辑二进制编解码（~140 行）
    ├── os_playlist.cc                 # 歌单 NVS + 抓取 + 落盘编排（~230 行）
    ├── os_search.cc                   # 搜索（~170 行）
    ├── os_media_cache.cc              # LRU 容器 + 封面/歌词 API（~290 行）
    ├── os_audio_cache.cc              # SD 音频缓存 + 兜底下载（~180 行）
    ├── os_stream_url.cc               # 302 解析 + 时长（~280 行）
    ├── os_history.cc                  # 搜索历史（~120 行）
    ├── os_login.cc                    # cookie + 扫码登录（~330 行）
    └── os_account.cc                  # 账号/歌单分页/每日推荐/直链（~300 行）
```

（如需更细，可把 `os_login.cc` 再拆 `os_cookie.cc`(cookie 纯逻辑 + 存盘) 与
`os_qr_login.cc`(三个 qr 接口 + `net_api_get*`)；把 `os_account.cc` 拆
`os_account.cc`(账号信息) + `os_pl_page.cc`(分页取歌) + `os_song_url.cc`(直链)。）

### 6.2 每个文件的职责边界与接口

| 文件 | 从原文件搬走的内容（行号） | 对外接口（内部头） | 预估行数 |
| --- | --- | --- | --- |
| `src/online_service.cc` | 1834–1906、1776–1801、（1806–1823 移 `os_store`） | 原 `online_service.h` 的全部 public 函数（转发或实现） | ~180 |
| `src/os_http_client.cc` | 291–728 | `os_http_get/get_headers/download/set_transport`（§F 原型） | ~360 |
| `src/os_util.cc` | 232–289、1806–1823 | `os_str_copy` `os_psram_alloc` `os_id_from_url` `os_mkdir_if_needed` `os_wipe_dir` `os_file_ok` | ~120 |
| `src/os_json.cc` | 2088–2122、2790–2822、2825–2903、904–958 | `os_json_find_str/int` `os_json_id_str` `os_pick_array` `os_song_from_json` `os_parse_songs_json`（**只依赖 cJSON + os_util**） | ~230 |
| `src/os_pl_format.cc` | 118–133 的类型 + 811–899 的编解码 | `os_pl_pack(const online_song_t*, size_t, uint8_t**, size_t*)` / `os_pl_unpack(const uint8_t*, size_t, online_song_t**, size_t*)` / `os_pl_header_t`（纯内存，**不碰文件系统**） | ~140 |
| `src/os_playlist.cc` | 732–808、960–1040、1164–1183 | `os_pl_nvs_load/save` `os_pl_path` `online_playlist_*` 实现 | ~230 |
| `src/os_search.cc` | 1042–1160 | `os_url_encode`（纯）`os_search_local` `online_search` | ~170 |
| `src/os_media_cache.cc` | 143–230、1325–1495 | `os_cache_find/slot/put/clear`（对泛型槽）+ `online_song_cover_data/lyrics_data` | ~290 |
| `src/os_audio_cache.cc` | 1186–1257、1259–1307 | `os_ext_from_sniff` `os_find_cached_audio` `os_audio_cache_trim` `online_song_prepare` | ~180 |
| `src/os_stream_url.cc` | 1497–1761 | `os_mp3_bitrate_bps`（纯）`online_song_stream_url/info` `online_song_prepare_stream` | ~280 |
| `src/os_history.cc` | 1908–2027 | `os_hist_load/save` + `online_history_*` | ~120 |
| `src/os_login.cc` | 2167–2560、2561–2780 | `os_cookie_pick/compact`（纯）`os_cookie_load/save/reload` `online_netease_qr_*` `online_netease_logout/logged_in/cookie` | ~330 |
| `src/os_account.cc` | 2029–2165、2782–3227 | `online_netease_api_base(_set)` `online_netease_account(_playlists)` `online_netease_playlist_page/daily_songs/song_url` | ~300 |

### 6.3 "纯逻辑 / 可 host 单测" 的分界（**重构的核心收益**）

| 归入纯逻辑（无 ESP-IDF、无 LVGL、无文件系统） | 现状行号 | 单测要点 |
| --- | --- | --- |
| `os_str_copy`（UTF-8 安全截断） | 234–252 | 多字节字符边界、cap=0/1 |
| `os_id_from_url` | 261–271 | 无 `id=`、负数、超 32 位 |
| `os_url_encode` | 1046–1062 | 中文、空格、`%`、`&`、缓冲边界 |
| `os_ext_from_sniff` | 1164–1183 | 6 种魔数 + 未知 + 小于 4 字节的嗅探头 |
| `os_cover_format` | 1325–1346 | JPEG/PNG/WebP/GIF/XML/HTML/垃圾字节 |
| `os_mp3_bitrate_bps` | 1504–1524 | MPEG1/2 各码率索引、Layer I/II 应跳过、无同步字 |
| `os_cookie_pick` / `os_cookie_compact` | 2202–2239 / 2332–2432 | 属性过滤、同名取最后、MUSIC_U 优先、超长截断、`pairs` 溢出边界 |
| `os_json_*`（cJSON 依赖，可在 host 用系统 cJSON 编） | 2088–2122、2790–2903 | 内外层同名 `code`、`ar[]`/`artists[]` 双格式、10 位 ID 溢出 |
| `os_pl_pack/unpack` | 811–899 | 定长布局、截断文件、count 越界、空标题压缩语义 |
| `os_cache`（LRU 容器，泛型化） | 143–230 | **淘汰顺序、字节计数、超限收敛（现有一段死循环，见 R1）** |
| `hist_move_to_front` | 1958–1969 | 头插/去重/满表 |

### 6.4 迁移顺序（每步可编译、行为不变）

1. **建骨架 + 抽 util**：`os_util.{h,cc}` + `os_net.h`（`TAG`、`s_last_err` 改为
   `os_net_set_last_error()` 集中写入）；`online_service.cc` include 新头，函数体不动。
2. **抽 HTTP 抽象**：先写 `os_http.h` + `os_http_client.cc`（把 442/579 两个函数原样搬入，
   参数换成 `os_http_req_t`），`http_get_text`/`http_download` 变薄包装；4 处内联的
   302 探测改成调 `os_http_get_headers()`。
3. **抽纯逻辑**：`os_json.cc`、`os_pl_format.cc`（先只做搬运 + 静态断言），
   原函数改成调用新实现；此时就能给 host 单测加 CI（同一份 .cc 编两遍）。
4. **拆歌单域**：`os_playlist.cc`（NVS + 落盘 + 抓取编排）、`os_history.cc`。
5. **拆媒体域**：`os_cache`（LRU）+ `os_media_cache.cc`（封面/歌词）+ `os_audio_cache.cc`。
6. **拆流地址域**：`os_stream_url.cc`（含 `mp3_bitrate_bps`）。
7. **拆账号域**：`os_login.cc`（cookie + 扫码）、`os_account.cc`（账号/分页/直链）。
8. **收尾**：`online_service.cc` 只剩 init/查询/转发（~180 行）；加并发保护（见 R1/R4）：
   建议在所有 public 入口加一把 `xSemaphoreCreateRecursiveMutex`（或明确"只允许一个
   网络工作线程 + app_music 走同一线程"的纪律，并在头文件写清）。
9. **验证**：`idf.py build` + 真机跑 AGENTS.md §11.4 的日志清单
   （`redirect ->`、`stream duration est:`、`cover … cached in PSRAM`、`playlist … saved`）。

---

## 7. 风险清单（拆分时最容易回归的点）

| # | 风险 | 触发场景 | 防护 / 验证手段 |
| --- | --- | --- | --- |
| **R1** | **`mem_cache_put` 淘汰循环死锁**（224–229）：`while (*bytes > cap_bytes)` 里调 `mem_cache_slot`，而它**优先返回空槽**（189–191）→ `old != e` → `*bytes` 不变 → **无限循环** | 累计字节超上限、但表里还有空槽：例如 7 张 ~600 KB 的封面（4.2 MB > 4 MB，槽位 8 个） | 改成"按 `last_ms` 淘汰且必须释放了内存才继续"，或用 `do { before = *bytes; ... } while (*bytes > cap && *bytes < before);`；host 单测直接构造该场景 |
| **R2** | 302 相对 Location 处理不一致：`http_get_text`(517)、`http_download`(640) 会拼回主机，两处 stream 探测**(1617/1727) 不会** | 服务端改用相对跳转 | 统一走 URL 工具函数；单测覆盖 `Location: /x/y` |
| **R3** | 把"每次现取直链"优化成"缓存直链" | 网易云签名 URL 有时效，缓存后 403 | 保留"无缓存"语义（AGENTS.md §11.4 的 403 重刷依赖它）；代码注释写死该约束 |
| **R4** | 全局状态无锁：`s_ids/s_names/s_hist/s_cover_cache/s_lrc_cache/s_last_err/s_net_cookie` + 5 个函数级 static 缓冲（408/428/2252/2350/2439/2574） | app_online 工作线程与 app_music 在线准备并发 | 加互斥或收敛到单一网络任务；`url_for_log/net_auth_hdr` 的 static 返回值改成"调用方给缓冲" |
| **R5** | `s_last_err` 被后一次失败覆盖，UI Toast 可能显示别的操作的原因 | 并发/连续失败 | 改成"每次 public 调用开始时清空 + 由调用方带回错误串"，或按操作分槽 |
| **R6** | 二进制格式漂移：头部注释写 896 字节、实际 1252；改 `ONLINE_URL_MAX` 会**静默**让旧文件读不出（只校验 magic/version，862） | 改结构体字段/长度 | 加 `_Static_assert` + 版本号提升 + 尺寸字段；`pl_load` 增加"文件长度 ≥ 头+count*rec"校验（现在截断只 break，876–878） |
| **R7** | `pl_save` 非原子（816–844）：写一半掉电/拔卡 → 文件损坏且**旧数据也没了** | 抓歌单时断电 | 写 `<id>.bin.tmp` → `rename`；`pl_load` 失败时保留旧文件 |
| **R8** | `search_local` 复制了 `pl_load` 的读盘逻辑（1076–1101），只校验 magic、不校验 count 上限 | 损坏文件 | 合并到同一 `os_pl_read_records()`（顺带获得 count 校验） |
| **R9** | `audio_cache_trim` O(n²)（1233–1255：每次删除都 `rewinddir` + 全量 `stat`）在 SD 上很慢 | 缓存文件 > 48 | 改成"收集 (mtime,path) → 排序 → 删前 N"；或在后台/低优先级任务里做 |
| **R10** | `psram_alloc` 内部 RAM 兜底（254–258）会把 4 MB 缓冲需求打到内部堆 | PSRAM 未就绪 | 大缓冲（>64 KB）失败即返回错误，不做内部 RAM 兜底；`sink_reserve` 同步改 |
| **R11** | cookie 走查询串（2589–2610）：URL 长度逼近 4096、可能被代理/服务端日志记录 | 自研 HTTP 替换时若顺手改头传递，服务端可能不认 | 先保持查询串 + `url_for_log` 脱敏；改头需服务端同改（列为已知技术债） |
| **R12** | `net_cookie_load` 的 latch（2283–2286）在 SD 未挂时也置位 → 需要 `online_netease_cookie_reload()`（2472）补救 | 开机立刻进在线页 | 拆分时保留 latch + reload 的配对语义，并给 reload 加注释；考虑改成"失败不 latch" |
| **R13** | `net_api_get_ex` 的 `static url[4096]`（2574）+ `cookie_compact` 的 `static pairs[12][1024]`（2350）+ `compact[2048]`（2439）共 ~18 KB `.bss`，且**不可重入** | 两个任务同时调账号接口 | 移入"网络会话上下文"结构体（栈/堆），或加锁；`.bss` 瘦身也顺带完成 |
| **R14** | 登录态语义依赖"服务端没 MUSIC_U 就按游客返回"（3144–3146 日志）——失败**不报错** | cookie 失效 | 保留 `online_netease_account()` 返回 `ESP_ERR_INVALID_STATE` 的判据，并保留日志；UI 依此提示重新登录 |
| **R15** | 时长估算只认 MP3（1634–1640）：FLAC/OGG/M4A 流 `out_dur_ms` 恒 0 | 账号歌/非 MP3 源 | 明确契约"0 = 未知"，不要让 UI 拿 0 当时长；后续可加 FLAC/ADTS 头解析（media_meta 已有 `probe_*` 可复用） |
| **R16** | 封面/歌词"只进 PSRAM"的假设依赖 `COVER_MAX_BYTES ≤ MEDIA_META_COVER_FILE_MAX`（注释 78–82） | 任一侧改常量 | 两常量集中定义 + `_Static_assert`；在 `media_meta` 侧加注释交叉引用 |
| **R17** | `online_cache_clear()`（1776）会删 audio 目录里**正在播放**的文件 | 播放在线缓存曲目时清缓存 | 保留"UI 只在停止播放后调用"的约定，或在服务侧记录 in-use 文件 |
| **R18** | init 期清理历史遗留（1841–1862）用 `unlink` 遍历目录，若与播放/下载并发会删掉半成品 | 开机时 app 已在起播 | init 必须在其它在线操作之前完成；拆分后把"cleanup"独立成显式步骤并加 `s_ready` 门禁 |

---

## 8. 拆分收益小结

- 3227 行 → 13 个文件（最大 `os_http_client.cc` ~360 行，最小 `os_history.cc` ~120 行），
  全部落在 300–600 行目标区间（`os_util.cc`/`os_history.cc` 略小可接受）。
- **纯逻辑约 1100 行可脱离 ESP-IDF 编译**：LRU 容器、歌单二进制编解码、URL/百分号编码、
  魔数与帧头解析、cookie 压缩/提取、JSON→模型映射、历史栈操作 —— 直接支撑 host 侧单测。
- **HTTP 传输彻底隔离在 `os_http.h` 之后**：自研传输只需提供一个实现并
  `os_http_set_transport()`，其余 2500 行业务代码一行不改（§F 的 14 个耦合点是验收清单）。
- 最大遗留风险未消除前不要动缓存与并发：**R1（死循环）、R4（无锁）** 优先级最高。
