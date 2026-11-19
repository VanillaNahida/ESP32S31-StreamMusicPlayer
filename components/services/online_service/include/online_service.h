/*
 * online_service.h —— 在线音乐（网易云歌单，走自建网易云 API）
 *
 * ⚠ 2026-11 服务端换代：**meting 代理已被删除**（/meting/api* 一律 404），
 *   现在全部走 @neteasecloudmusicapienhanced/api 的**原生接口**
 *   （参考工程 @neteasecloudmusicapienhanced/api）：
 *
 *   鉴权   Authorization: Bearer <API Key>        ← CONFIG_ONLINE_NETEASE_API_TOKEN
 *          （也认 X-Api-Key；401 = key 不对/过期）
 *   登录态 **X-Netease-Cookie: MUSIC_U=…; __csrf=…**
 *          （★ 不能再拼 `?cookie=…` —— 服务端默认 cookie.allowQuery=false，
 *            查询串里的 cookie 会被 400 拒掉，见 os_api.cc）
 *
 *   歌单   /playlist/track/all?id=&limit=&offset=  → {"songs":[{id,name,dt,ar[],al.picUrl}]}
 *   搜索   /cloudsearch?keywords=&type=1&limit=     → {"result":{"songs":[…]}}（有封面）
 *          /search?keywords=&type=1&limit=          → 老格式（album 只有 picId，作次选）
 *   歌词   /lyric?id=                               → {"lrc":{"lyric":"[00:00.000] …"}}
 *   封面   /song/detail?ids=                        → {"songs":[{"al":{"picUrl":…}}]}
 *   直链   /song/url/v1?id=&level=                  → data[0].url/size/br/freeTrialInfo
 *   账号   /user/account、/user/playlist、/recommend/songs
 *
 * 落盘（需求："抓取歌单后把歌单列表二进制保存在存储卡内"）：
 *   /sdcard/online/pl_<歌单ID>.bin     定长头 + 定长记录数组（见 online_service.cc）
 *   /sdcard/online/audio/<歌曲ID>.<ext>  已下载的音频（播放的就是它）
 *
 * 封面 / 歌词**只缓存在 PSRAM，不写 SD 卡**（2026-10 改）：
 *   之前封面/歌词落 SD，切歌时读/写这些文件会让 SD 卡忙到"切歌阻塞"（日志里
 *   一排 sdmmc_req 0x107 超时，封面还下成 0 字节）。现在下载后整段留在 PSRAM：
 *     online_song_cover_data()  → 调用方拿到一张 malloc 的图片字节拷贝（JPEG/PNG）
 *     online_song_lyrics_data() → 调用方拿到一段 malloc 的 LRC 文本
 *   同一首歌二次请求直接命中内存缓存，不再碰网络与 SD。SD 上历史遗留的
 *   cover/ 目录与 *.lrc 文件在 online_service_init() 时清掉。
 *
 * 播放方式：**默认流式**（online_song_stream_url → audio_service_play_uri 边下边播），
 *   只有容器不支持从流中间打开（m4a/ogg/opus）或流地址拿不到时，才退回
 *   "整首下载到 SD 再当本地文件播"。早期（meting 时代）是反过来的，注释里的
 *   "先下载再播放"是那段历史。
 *
 * 线程模型：所有 API 都是**同步阻塞**的（内部走 HTTP），
 *   必须放在工作线程里调用，不要在 LVGL 任务里直接调。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_MAX_PLAYLISTS   8      /* 最多保存几个歌单 ID */
#define ONLINE_ID_MAX          24
#define ONLINE_NAME_MAX        40     /* 歌单显示名（用户可重命名，UTF-8 按字节算） */
#define ONLINE_TITLE_MAX       128
#define ONLINE_ARTIST_MAX      96
#define ONLINE_URL_MAX         320

/* 一首在线歌曲（内存态；落盘时用等长的定长记录） */
typedef struct {
    uint32_t id;                          /* 网易云歌曲 ID（接口直接给；旧歌单从 url 抠） */
    char     title[ONLINE_TITLE_MAX];
    char     artist[ONLINE_ARTIST_MAX];
    char     cover_url[ONLINE_URL_MAX];   /* al.picUrl；缺了会现场用 /song/detail 补 */
    char     audio_url[ONLINE_URL_MAX];   /* ⚠ 历史字段：旧歌单里的 meting 地址（已不再新填） */
    char     lrc_url[ONLINE_URL_MAX];     /* ⚠ 历史字段：歌词现在按 id 现取 /lyric，不用它 */
} online_song_t;

/* 初始化：建目录、读 NVS 里保存的歌单 ID 列表、清掉历史遗留的 SD 封面/歌词。
 * 可重复调用。 */
esp_err_t online_service_init(void);

/* 当前网络是否可用（WiFi 拿到 IP）——在线功能的前置条件 */
bool online_has_network(void);

/* ---------------------------------------------------------------- 歌单管理 */

size_t      online_playlist_count(void);
const char *online_playlist_id(size_t idx);
/*
 * 歌单的显示名（用户在界面上重命名过就是它）。
 * 没起过名返回**空串**（不是 NULL）—— 展示决策交给 UI：一般退回显示 ID。
 * idx 越界返回 NULL。
 */
const char *online_playlist_name(size_t idx);
/* 重命名（存 NVS）；name 传 NULL 或空串 = 清掉自定义名，退回显示 ID */
esp_err_t online_playlist_rename(size_t idx, const char *name);

/* ---------------------------------------------------------------- 搜索历史
 *
 * 存 NVS（最多 ONLINE_HISTORY_MAX 条，**新的在最前**，同一关键词去重后提前）。
 * 只存关键词本身，展示与交互（长按删单条 / 一键清空）由 UI 决定。
 */
#define ONLINE_HISTORY_MAX  10
#define ONLINE_KW_MAX       64     /* 搜索关键词上限（UTF-8 按字节） */

size_t      online_history_count(void);
/* 第 idx 条关键词（0 = 最新）；越界返回 NULL */
const char *online_history_at(size_t idx);
esp_err_t   online_history_add(const char *kw);
esp_err_t   online_history_remove(size_t idx);
esp_err_t   online_history_clear(void);

/*
 * 最近一次失败的可读原因（中文，给 UI 的 Toast 用）；没有失败过返回 NULL。
 * 例如 "服务器返回 HTTP 400" / "网络错误 ESP_ERR_HTTP_CONNECT" / "无法写入 SD 卡"。
 */
const char *online_last_error(void);

/*
 * 添加歌单：抓取 → 解析 → **二进制落盘** → 记 NVS。
 * 已存在则覆盖（重新抓取刷新）。
 */
esp_err_t online_playlist_add(const char *id);

/* 删除歌单：清 NVS + 删二进制文件 */
esp_err_t online_playlist_remove(const char *id);

/*
 * 把某个歌单读进内存（优先读本地二进制；本地没有就抓一次再落盘）。
 * 成功时 *songs 由本模块 malloc（PSRAM 优先），调用方用 free() 释放。
 */
esp_err_t online_playlist_load(const char *id, online_song_t **songs, size_t *count);

/*
 * 搜索歌曲。三级：
 *   1) /cloudsearch（有封面，首选）
 *   2) /search（老格式，album 没有 picUrl —— 封面由播放页现场补）
 *   3) 没网或服务端失败时：扫本机已保存歌单做子串匹配（歌名/歌手，不区分大小写）
 * *out 由本模块 malloc，调用方 free()。max 为返回上限（服务端单页上限 50）。
 */
esp_err_t online_search(const char *keyword, online_song_t **out, size_t *count, size_t max);

/* ---------------------------------------------------------------- 播放准备 */

/*
 * **流式播放**（默认路径，见文件头"设计选择"）：
 *   1) online_song_stream_url()      解析 302 拿到音频最终 https 地址（不下载）
 *   2) online_song_prepare_stream()  只抓封面/歌词（几十~几百 KB，进 PSRAM 缓存）
 *   3) 上层 audio_service_play_uri(最终地址) 边下边播
 */
esp_err_t online_song_stream_url(const online_song_t *song, char *out, size_t cap);
/*
 * 同上，但顺带把**总时长**估出来（读 4 KB 前缀拿 MP3 码率 × Content-Length）。
 * 在线流式播放拿不到 duration_ms（GMF 的 music info 恒为 "0 bps, 0 ms"），
 * 所以播放器页/列表的"总时长"要靠它。out_bytes/out_dur_ms 可为 NULL。
 */
esp_err_t online_song_stream_info(const online_song_t *song, char *out, size_t cap,
                                  int64_t *out_bytes, uint32_t *out_dur_ms);
esp_err_t online_song_prepare_stream(const online_song_t *song);

/*
 * 兜底路径：把整首歌下载到 SD（命中缓存直接返回），再当本地文件播放。
 * 流式失败时回退用它；也可以用来做离线缓存。
 */
esp_err_t online_song_prepare(const online_song_t *song, char *audio_path, size_t cap);

/*
 * 封面字节（PSRAM 缓存，不落 SD）。
 * 成功时 *out 是**调用方所有**的 malloc 缓冲（PSRAM 优先，JPEG/PNG 原样字节），
 * 用完 free()。同一首歌二次调用直接命中内存缓存（不再下载）。
 * 没封面 URL / 没网返回 ESP_ERR_NOT_FOUND；下载失败返回对应错误码。
 */
esp_err_t online_song_cover_data(const online_song_t *song, uint8_t **out, size_t *out_len);

/* ============================================================ 网易云账号登录
 *
 * 架构（2026-10）：设备端**不做 weapi 加密** —— 一律走自建的 API 服务
 * （参考工程 NeteaseCloudMusicApiEnhanced/api，加密/签名/cookie 都在服务端做）。
 * 接口契约（已核对它的 module/login_qr_*.js 源码）：
 *   GET {base}/login/qr/key                        → data.unikey
 *   GET {base}/login/qr/create?key=<k>&qrimg=true  → data.qrurl（手机扫的链接）
 *                                                    data.qrimg（"data:image/png;base64,…"）
 *   GET {base}/login/qr/check?key=<k>              → code 800/801/802/803
 * 803（登录成功）时响应里带 cookie → 存到 /sdcard/online/netease_cookie.txt，
 * 之后所有账号接口都带 **X-Netease-Cookie 请求头**调（见 os_api.cc）。
 *
 * ⚠ base 是**可配置**的（NVS 'online'/'nb'）：服务可能跑在你的 PC 上（局域网 IP:3000）
 *   或部署到公网，固件不硬编码。
 */
#define NET_QR_WAIT   801   /* 等待扫码 */
#define NET_QR_SCAN   802   /* 已扫码，待手机确认 */
#define NET_QR_OK     803   /* 登录成功 */
#define NET_QR_EXPIRE 800   /* 二维码失效（需要重新取 key） */

/* API 服务地址：set 写 NVS（传 NULL/空串 = 恢复默认）；get 永不返回 NULL */
esp_err_t online_netease_api_base_set(const char *base);
const char *online_netease_api_base(void);

/* 1) 取二维码 key（unikey）；out_key 至少 64 字节 */
esp_err_t online_netease_qr_key(char *out_key, size_t cap);
/* 2) 用 key 换二维码：out_url = 手机要扫的链接；qrimg = **PNG 原始字节**（调用方 free） */
esp_err_t online_netease_qr_create(const char *key, char *out_url, size_t url_cap,
                                   uint8_t **qrimg, size_t *qrimg_len);
/* 3) 轮询扫码结果：*out_code 取 NET_QR_* 四个值之一；成功时自动保存 cookie */
esp_err_t online_netease_qr_poll(const char *key, int *out_code);

/*
 * 登录态。cookie 有两个来源，按优先级：
 *   1) SD 上扫码登录存下来的 /sdcard/online/netease_cookie.txt（最新，优先）；
 *   2) 构建期编进固件的内置 cookie —— 见 main/Kconfig.projbuild 的
 *      CONFIG_ONLINE_NETEASE_CK_FILE（相对工程根目录的路径，默认 assets/ncm-ck.txt，
 *      留空 = 不启用）。有这个时开机即为已登录，不用扫码。
 * 两个都没有 = 未登录，界面走扫码流程。
 */
bool online_netease_logged_in(void);
const char *online_netease_cookie(void);      /* 未登录返回空串，不是 NULL */

/*
 * 重新从 SD 读一次 cookie（只在**当前判定为未登录**时才有意义，已登录时直接返回）。
 *
 * 为什么需要它：开机后马上进在线页时，SD 卡可能还没挂上 —— 那次 fopen 失败并不
 * 代表"没登录过"，但 net_cookie_load() 一旦下了结论就会一直沿用，界面于是永远停在
 * "未登录"，用户得重新扫码。UI 在重建账号页时调这个，挂载完成后就能自动恢复登录态。
 */
esp_err_t online_netease_cookie_reload(void);

/*
 * 已登录账号昵称（NVS 持久化，重启后立刻可用）。
 * 没有存过 / 已退出登录返回**空串**（不是 NULL）。
 *
 * 为什么要落 NVS：昵称只能靠 /user/account 请求拿到，重启后要等网络往返（几秒，
 * 没网就一直没有）—— 期间界面上"已登录：XX"会变成"已登录：网易云账号"，
 * 看起来就是"用户名丢了"。
 */
const char *online_netease_nickname(void);
/* 记下昵称（传 NULL/空串 = 清掉）；online_netease_account() 成功时会自动调它 */
esp_err_t online_netease_nickname_set(const char *name);
/* 退出登录：清内存 + 删 SD 上的 cookie 文件 */
esp_err_t online_netease_logout(void);

/* ---------------------------------------------------------------- 账号数据
 *
 * 下面这些都带 cookie 调（`X-Netease-Cookie` 头）。
 * cookie 失效时的表现已实测：/user/account 返回 {"account":null,"profile":null}
 * —— 所以 online_netease_account() 返 ESP_ERR_INVALID_STATE 就表示"要重新登录"。
 */

typedef struct {
    char uid[24];
    char nickname[64];
} online_account_t;

/* 账号信息（GET /user/account） */
esp_err_t online_netease_account(online_account_t *out);

typedef struct {
    char id[ONLINE_ID_MAX];
    char name[ONLINE_NAME_MAX];
    char cover_url[ONLINE_URL_MAX];
    int  track_count;
    bool self_created;        /* true = 自己创建，false = 收藏的歌单 */
} online_acct_playlist_t;

/* 账号里的歌单（GET /user/playlist?uid=<uid>&limit=…） */
esp_err_t online_netease_account_playlists(const char *uid, online_acct_playlist_t *out,
                                           size_t max, size_t *out_n);

/*
 * 歌单里的歌 —— **分页取**（懒加载用；见 app_online 的"每页 100 首"）。
 *
 *   GET /playlist/track/all?id=<歌单ID>&limit=100&offset=<offset+已取>
 *   内部按 PL_PAGE_SONGS(100) 首一页循环，直到取满 max 首或歌单结束：
 *     ⚠ 服务端这个接口返回的是**完整 song detail 对象**（ar/al/dt/h/m/l/privileges…，
 *       实测 ~2.3 KB/首）—— 一次要 300 首 ≈ 700 KB，超过单响应上限 NET_JSON_MAX(512 KB)
 *       就会整个失败。所以**绝不能**一次要太多：单页 100 首 ≈ 250 KB 才稳。
 *
 *   *out_n = 实际取到几首；**< max 就说明已经到歌单末尾**（调用方据此收尾）。
 *   只填 id / title / artist / cover_url —— audio_url 留空，播放时按需取直链
 *   （见 online_netease_song_url）。offset 为 0 且第一页就失败时，退回
 *   /playlist/detail 再试一次（老接口，只对小歌单有效）。
 */
#define ONLINE_PL_PAGE_SONGS  100
esp_err_t online_netease_playlist_page(const char *pl_id, size_t offset,
                                       online_song_t *out, size_t max, size_t *out_n);

/* 每日推荐（GET /recommend/songs → data.dailySongs[]），同上只填元数据 */
esp_err_t online_netease_daily_songs(online_song_t *out, size_t max, size_t *out_n);

/*
 * 带 cookie 取播放**直链**（GET /song/url/v1?id=<id>&level=standard）。
 * 实测返回的就是可直接播的 http 地址（不再 302），并带 size/br —— 顺便把总时长算出来
 * （之前"总时长算不出来"的老问题，在这条路上天然解决）。
 * out_bytes / out_dur_ms 可为 NULL。
 */
esp_err_t online_netease_song_url(const char *song_id, char *out, size_t cap,
                                  int64_t *out_bytes, uint32_t *out_dur_ms);

/*
 * ---------------------------- 红心（"我喜欢的音乐"） ----------------------------
 *
 * 服务端行为（2026-11 实测，见 os_like.cc 文件头）：
 *   - /like 必须带**有效** cookie：不带 → HTTP 500（服务端自己炸）；cookie 无效 →
 *     HTTP 301 {"msg":"需要登录","code":301}。而 os_http_client 对非 200 一律丢 body，
 *     所以这两种在返回值上都是错误码，调用方**先问 online_netease_logged_in()**。
 *   - /likelist 游客也能拿 200，只是 ids 为空 —— 别把它当"登录成功"的证据。
 */

/* 账号 uid（NVS 'online'/'uid'；未登录 = 空串）。/likelist 必填。 */
const char *online_netease_uid(void);
/* 写 uid 到 NVS（NULL/空串 = 擦除）。登录成功后由 online_netease_account() 调用。 */
esp_err_t online_netease_uid_set(const char *uid);

/*
 * 把 song_id 加进 / 移出"我喜欢"。
 *   ESP_OK                服务端 code=200，已生效
 *   ESP_ERR_INVALID_STATE 未登录 / cookie 已过期（code=301 或没 uid）
 *   其它                  网络或服务端故障
 */
esp_err_t online_netease_like(const char *song_id, bool like);

/*
 * 拉"我喜欢"的**全量** id（升序、去重），用于判断某首是否已收藏。
 * 成功时 *out_ids 是调用方 free() 的数组（PSRAM 优先），*out_n 是歌曲数；
 * 未登录 / 请求失败 → 错误码且 *out_ids = NULL。
 * ⚠ 一次最多几千条（实测 1 万首 ≈ 100 KB JSON），只在切歌/点红心时拉，别轮询。
 */
esp_err_t online_netease_likelist(uint64_t **out_ids, size_t *out_n);

/* 在 online_netease_likelist() 的结果里二分查 song_id（纯逻辑，无网络）。 */
bool online_netease_liked_has(const uint64_t *ids, size_t n, const char *song_id);

/*
 * 歌词文本（PSRAM 缓存，不落 SD）。
 * 成功时 *out 是**调用方所有**的 NUL 结尾 malloc 文本（UTF-8 原样，直接交给
 * media_meta_lrc_parse），用完 free()。二次调用命中缓存。
 * 没网 / 这首歌没有歌词（/lyric 返回空串）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t online_song_lyrics_data(const online_song_t *song, char **out, size_t *out_len);

/* 清掉下载缓存（音频 SD 文件 + PSRAM 封面/歌词缓存），歌单二进制保留 */
esp_err_t online_cache_clear(void);

#ifdef __cplusplus
}
#endif
