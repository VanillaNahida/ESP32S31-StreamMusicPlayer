# QQ 音乐接入（在线音源 #2）

2026-11 新增。桌面上的「QQ音乐」图标与「网易云音乐」**并排**（`app_desc_t.order` 20 / 21），
打开的是**同一套页面代码**，只是音源不同 —— 三页骨架、分页/懒加载、浮层、播放器联动
只有一份，两家不会走成两个样子。

- 服务端契约：`NeteaseCloudMusicApi/ncm-api/docs/qq-provider.md`（同机 checkout）
- 能力清单：`NeteaseCloudMusicApi/ncm-api/docs/api-capabilities.md`
- 服务地址 / API Key：与网易云**共用**（`CONFIG_ONLINE_NETEASE_API_BASE` / `..._TOKEN`），
  只是路径挂在 `/qq` 下。

## 1. 结构

| 层 | 文件 | 职责 |
| --- | --- | --- |
| 凭证 + 请求 | `services/online_service/os_qq.cc` | NVS 存整份 credential JSON、base64 成 `X-QQMusic-Credential`、临期刷新、401 归类 |
| JSON 映射 | `os_qq_json.cc` | QQ 的 `Song`/`SongList` → `online_song_t`/`online_acct_playlist_t`（纯逻辑） |
| 业务 | `os_qq_data.cc` | 扫码登录、账号歌单、歌单曲目、我喜欢、搜索、取流、歌词、收藏 |
| 音源选择 | `apps/app_online/src/ao_provider.cc` | 当前音源 + 两家文案；两个桌面入口共用 `page_online()` |

播放链路按 `online_song_t.src` 分派（`ONLINE_SRC_NETEASE` / `ONLINE_SRC_QQ`）：

```
online_song_stream_info/url ──> os_stream_url.cc ──> online_qq_song_url()   (QQ)
                                                    └> online_netease_song_url()
online_song_cover_data      ──> os_media_cache.cc ─> 直接用抓歌时拼好的 y.gtimg.cn 地址
online_song_lyrics_data     ──> os_media_cache.cc ─> online_qq_lyric() / /lyric
app_music 的红心            ──> music_like.cc ─────> online_qq_like() / online_netease_like()
```

## 2. 登录（扫码）

1. `GET /qq/login/qrcode/qq` → `data.identifier` + `data.data`（base64 PNG）；
   固件解出 PNG 交给 `media_meta_image_mem_rgb565()` 画在浮层上。
2. 每 1.5 s 轮询 `/qq/login/qrcode/qq/status?identifier=…`：
   `event` 0 完成 / 1 待扫 / 2 待确认 / 3 过期 / 4 拒绝。
3. `event=0` 时响应里带 `data.credential`（QQMusicApi 的 Credential，snake_case），
   **整份 JSON 存 NVS 单条字符串**（键 `online/qqcred`），写完才提示成功。

固件把 QQ 的事件码统一映射成网易云那套（803/802/801/800），
`ao_result.cc` 的状态机因此不用改。

> ⚠ 二维码 identifier 实测 **128 字符**：请求/结果槽位都为它留了 192 字节，
> 别改回 64（截断后轮询永远对不上，界面会一直停在"等待扫码"）。

## 3. 凭证与刷新

- 请求头**成对**发：`X-QQMusic-Credential: base64(JSON)` +
  `X-QQMusic-Credential-Encoding: base64`（服务端只在这个头说 base64 时才解码）。
- 到期判断用凭证自带的 `musickey_create_time + key_expires_in`；
  **临期 10% 才刷**、距上次尝试至少 30 分钟、成功后先落盘再用
  （`POST /qq/login/refresh`，服务端同时做 L2 密文托管）。
- 业务请求 401/403 只置"凭证失效"标志 → 界面提示重新扫码，**不擅自删凭证**
  （一次网络抖动不该让用户重新扫）。
- 服务端没有"注销 QQ 登录"端点：`online_qq_logout()` = 删本地凭证。

## 4. 能力

| 功能 | 接口 | 说明 |
| --- | --- | --- |
| 我的歌单 | `/qq/user/{musicid}/created_songlists` + `/qq/user/{euin}/fav/songlists` | 自建 + 收藏合并；`self_created` 区分 |
| 我喜欢 | `/qq/user/{euin}/fav/songs`（dirid=201） | 作为**第一个歌单**出现在「我的歌单」里 |
| 歌单曲目 | `/qq/songlist/{id}/detail?page&num` | 与网易云同一套懒加载（游标必须是 100 的倍数） |
| 搜索 | `/qq/search/general_search` | 结果在 `data.song.items[]` |
| 播放地址 | `/qq/song/{mid}/play_url` | 服务端拼好 CDN，**一步给完整地址**；`url=null` = 无版权/非 VIP |
| 歌词 | `/qq/song/{mid}/lyric` | `data.lyric` 就是 LRC 文本 |
| 喜欢/取消 | `POST /qq/songlist/{add,del}_songs?dirid=201` | 播放器页红心 |
| 收藏到歌单 | `POST /qq/songlist/add_songs?dirid=<自建歌单 dirid>` | **长按歌曲行**弹出选择浮层 |

`dirid` 与歌单 ID 不是一回事：`created_songlists` 的每条都带 `dirid`（存在
`online_acct_playlist_t.dirid`），「我喜欢」固定 201。

### 4.1 取流的两档：整首 / 试听

上游对"需要会员"的歌，正常音质一律回 `result=104003`（无权限），**只有试听档有 purl**。
2026-11 线上实测（《发如雪》0027oMO61wWi55，匿名）：

| file_type | 音质 | 结果 |
| --- | --- | --- |
| 13 | MP3_128（默认） | 104003 |
| 12 / 11 / 15 | MP3_320 / OGG_96 / ACC_96 | 104003 |
| **30** | **TRY（RS02 试听）** | ✅ 有 purl（`.../RS02<mid><mid>.mp3?...`） |

> ⚠ 试听文件在 CDN 上叫 `RS02<media_mid>.mp3`（**不是** `RS02<mid><mid>.mp3`，后者 404）。
> 这一步由服务端 `play_url` 自动补 media mid，固件只管要 `file_type=30`。

所以 `online_qq_song_url()` 是**两档阶梯**：先要整首，`104003` 再要试听档；
试听时 `out_dur_ms` 故意留 0（元数据是整首长度，会把进度条/seek 带偏），
播放器改用引擎实测的片段长度。

### 4.2 音频流的 HTTPS 降级

真机 `-0x0087`（PSA_ERROR_INVALID_ARGUMENT）会在 **TLS 握手成功后的第一次读**偶发失败，
而 `audio_http` 这条传输没有 HTTP 层那套重试，一失败就"这首放不了"。
两家 CDN 都同时服务明文（实测 `aqqmusic.tc.qq.com` 的 http 直接 206，不重定向），
**2026-11 用户要求：音频流以明文 http 优先** —— 所以现在是 **http 起手**，
http 建连失败才回 https 兜底（同主机同路径，只换 scheme），不再等 8 s 超时后再降级。

### 4.3 两个真机踩坑

1. **二维码 base64 解码失败 `-0x002C`**：`online_qq_qr_create()` 先 `cJSON_Delete(root)`
   再拿 `valuestring` 去解码 —— 读的是已释放内存。现在先拷出来（并顺手去空白）再删。
2. **时长被"纠正"错了**：`tick_adopt_stream_duration()` 用引擎按平均码率估的 138 s
   覆盖了元数据的 192 s（M500 是 VBR）。现在 QQ 歌曲只要 `dur_ms > 0` 就不让它覆盖。

## 5. 界面

- 两个桌面图标：网易云（order 20）、QQ音乐（order 21），图标来自
  `assets/icons/qqmusic.png`（`tools/icons/gen_icon.py` 烘成 `qqmusic_logo` 96×96）。
- 页签仍是三页：`我的歌单` / `我添加的歌单` / `搜索`。
- 「我的歌单」顶部两条**推荐入口**：`每日30首`（服务端串行取 6 次"猜你喜欢"拼 30 首）
  和 `私人雷达`（服务端翻页拼 30 首）。点进去和网易云的"每日推荐"同一条路径：
  一次拿整份、不懒加载，队列 Key 都是 `DETAIL_KEY_QQ_DAILY` / `DETAIL_KEY_QQ_RADAR`。
  - ⚠ **每日30首在服务端是随机的**（每次请求 6 次"猜你喜欢"都是新歌），
    所以固件**按天缓存**：`/sdcard/online/pl_qq30_<yyyymmdd>.bin`（与歌单同一套二进制），
    同一天反复进出看到的是同一份，换天自然失效；登录/退出登录时清掉当日缓存。
    实现见 `ao_page_acct.cc` 的 `ao_daily_cache_take/store` 与
    `online_service` 的 `online_feed_cache_*`。
  - 它**仍然不等于 QQ 音乐 App 里的"每日30首"**：那是上游另一条接口（sidecar 的
    QQMusicApi 没开放），服务端只能拿"猜你喜欢"凑。要完全一致得先给服务端补上游接口。
- 登录昵称由**服务端**在扫码成功时一并返回（`data.nickname`，来自
  `/user/{euin}/homepage` 的 `base_info.name`），固件只负责存进 NVS 并显示。
  - QQ 的「我的歌单」里第一条是 **我喜欢的音乐**（心形行、红色强调），
    其余是自建 + 收藏的歌单；**没有**「每日推荐」入口（上游未开放）。
  - 「我添加的歌单」按 **QQ 歌单 ID** 抓取（`online_playlist_add_qq()`），
    二进制落盘格式与网易云一致（记录里的 rsv 区带 src/mid，见 `os_pl_format.h`）。
- 长按歌曲行 → 「收藏到歌单」浮层：**先选中歌单，再按「收藏」**（行点击只改选中项，
  不做任何写操作）；另有「删除」把这首歌从**当前**歌单移除。歌单多时最多列 16 个。
  - 防滑动误触：行点击带"按下→松手位移 > 14 px 就忽略"的判断；浮层也只在手指抬起后才建。
  - 目标歌单里已有这首歌时提示「这个歌单里已经有这首歌了」（服务端预检，见
    qq-provider.md §5.2；当前歌单的情况本地直接判定，不发请求）。
- 播放器页的红心对 QQ 歌曲同样可用（走 `add_songs/del_songs dirid=201`）。
- 播放器页标题上方的来源栏有两个药丸：左边「在线播放 / 本地播放」，右边紧挨
  **渠道标签**——网易云音乐（品牌红 #C20C0C + 白字）/ QQ音乐（品牌绿 #31C27C +
  深墨绿字，白字在这个绿上对比度不够）。本地曲目自动隐藏渠道标签。
  实现见 `music_view_player.cc` 的 `apply_provider_badge()`：按 `online_song_t.src` 判断，
  与"在线/本地"**分开**判 —— 切歌时可能"仍在线"但换了一家。

## 6. 已知限制

- QQ 匿名态只能拿到免费/试听音源；`play_url` 返回 `url=null` 时界面提示
  "这首没有播放权限（会员/版权）"。
- 收藏到歌单只开放**加歌**（没有删歌入口）；歌单的创建/删除/排序未做。
- 红心状态的"全量表"只翻前 5 页（500 首）—— 收藏超过 500 首时，第 500 首之后
  首次点红心会先判为"未喜欢"，点一下仍然会加进去（上游对重复收藏是幂等的）。
- `add_songs` / `del_songs` / `play_url` 是 2026-11 新加的服务端路由，
  **部署新服务端后**才可用（见 ncm-api 的 docs）。

## 7. 排障

| 现象 | 检查 |
| --- | --- |
| 扫码后一直"等待扫码" | identifier 是否被截断（应为 128 字符）；服务端 `/qq/healthy` 是否 200 |
| 登录成功但歌单是空的 | 凭证里有没有 `encryptUin`（收藏类接口必需）；看串口 `qq` 日志 |
| 点歌提示"没有播放权限" | 该曲需要 VIP，或匿名态下 `purl` 为空（上游行为） |
| 收藏失败 401 | 凭证过期 → 退出重新扫码 |
| 收藏失败 HTTP 422 | 2026-11 的老问题：Node 侧 axios 把数组参数序列化成 `song_id[]=1`，而 sidecar 的 `list[int]` 只认 `song_id=1`（`extra_forbidden`）→ 422。已修（`qq-sidecar.js` 的 `paramsSerializer: { indexes: null }` + 回归测试），**需要重新部署 ncm-api**。设备侧现在也会把服务端那句原因显示出来（`HTTP 422：…`） |
| 封面不显示 | QQ 封面按 `album.mid` 拼 `y.gtimg.cn`；无 mid 的歌没有封面 |
