/*
 * online_service.h —— 在线音乐（网易云 + QQ 音乐，都走同一个自建聚合 API）
 *
 * ⚠ 2026-11 增补：QQ 音乐（docs/qq-music.md）。同一份 online_song_t 用 `src` 区分音源，
 *   播放/封面/歌词/红心都按它分派；QQ 的实现在 os_qq*.cc，对外接口见文件末尾那一节。
 *   下面这段"网易云"的契约依旧是主体，两家共用鉴权（Bearer API Key）与服务地址。
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
 *   /sdcard/online/audio/<歌曲ID>.<ext>  **兜底下载**才有（流式播放不落盘）：
 *                                       m4a/ogg/opus 这类顺序流接不上的容器、
 *                                       或取流失败时，才整首下到这里当本地文件播。
 *                                       目录也是那时候才建（2026-11 起不再开机创建）
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

/* ------------------------------------------------------------------ 音源
 *
 * 同一份 online_song_t 现在承载**两家**在线音乐：网易云与 QQ 音乐。
 * 播放链路（取直链 / 封面 / 歌词 / 红心）都按这个字段分派，
 * 其余 UI 代码不需要知道歌是哪家的。
 */
typedef enum {
    ONLINE_SRC_NETEASE = 0,   /* 网易云（默认值：旧歌单文件里没有这个字段） */
    ONLINE_SRC_QQ      = 1,   /* QQ 音乐 */
} online_src_t;

/* 一首在线歌曲（内存态；落盘时用等长的定长记录） */
typedef struct {
    uint32_t id;                          /* 网易云歌曲 ID / QQ 的 songid（接口直接给） */
    char     title[ONLINE_TITLE_MAX];
    char     artist[ONLINE_ARTIST_MAX];
    char     cover_url[ONLINE_URL_MAX];   /* 封面地址；网易云缺了会现场用 /song/detail 补 */
    char     audio_url[ONLINE_URL_MAX];   /* ⚠ 历史字段：旧歌单里的 meting 地址（已不再新填） */
    char     lrc_url[ONLINE_URL_MAX];     /* ⚠ 历史字段：歌词现在按 id 现取，不用它 */
    /*
     * ★ 2026-11 新增（QQ 音乐）：
     *   src  —— 这首歌属于哪个平台（决定取直链 / 歌词 / 红心走哪条接口）；
     *   dur_sec —— 元数据时长（秒；QQ 的 interval、网易云的 dt÷1000），
     *              有它在线播放不用再读 4 KB 前缀估时长；
     *   mid  —— QQ 的歌曲 mid（取流/歌词的必填参数），网易云留空。
     * 三个字段都追加在末尾：os_pl_format 用定长记录的 rsv 区落盘（格式不升版本）。
     */
    uint8_t  src;
    uint16_t dur_sec;
    char     mid[48];
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
 * 同上，但按 **QQ 音乐**的歌单 ID 抓取（两家编号独立，由调用方选择）。
 * 落盘格式与网易云一样，记录里带 src/mid，读回来能正常播放。
 */
esp_err_t online_playlist_add_qq(const char *id);

/*
 * 把某个歌单读进内存（优先读本地二进制；本地没有就抓一次再落盘）。
 * 成功时 *songs 由本模块 malloc（PSRAM 优先），调用方用 free() 释放。
 */
esp_err_t online_playlist_load(const char *id, online_song_t **songs, size_t *count);

/*
 * "每天一份"的推荐列表缓存（每日推荐 / QQ 每日30首）。
 *
 * 为什么要它：QQ 的 /qq/recommend/daily 是**服务端串 6 次"猜你喜欢"拼出来的**
 * （见 ncm-api 的 qq-provider.md §5.4），每次请求都是新的 30 首 —— 同一个入口
 * 反复进出会看到不停变化的歌单。用户要的是"今天的 30 首"，所以这里按 key 落盘，
 * **由调用方在 key 里带日期**（例如 "qq30_20261124"），于是同一天命中、换天自然失效。
 *
 * 文件格式与歌单**完全相同**（/sdcard/online/pl_<key>.bin，定长头 + 定长记录），
 * 读出来的是 os_psram_alloc 的数组，所有权交给调用方（与 online_playlist_load 一致）。
 *
 *   load: ESP_ERR_NOT_FOUND = 没有这一天的缓存（调用方去请求网络）
 *   save: 失败只意味着"下次再取一次"，调用方可以忽略；成功后会顺手清掉旧日期的缓存
 */
esp_err_t online_feed_cache_load(const char *key, online_song_t **songs, size_t *count);
esp_err_t online_feed_cache_save(const char *key, const online_song_t *songs, size_t count);
/* 删掉某一天的缓存（换账号时用：每日推荐是按账号算的，别把上一个人的列表留给下一个人） */
void      online_feed_cache_forget(const char *key);

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
    /*
     * QQ 音乐的「目录 ID」：往歌单里加歌（online_qq_song_add）要的是它，**不是**
     * id。"我喜欢" 固定是 201（见 online_service.h 末尾的 QQ 一节）。
     * 网易云歌单没有这个概念，恒为空串。
     */
    char dirid[16];
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
 * 最近一次"加进我喜欢"的那首歌（id / 封面地址 / 音源 / mid），落 NVS。
 *
 * 为什么要它（2026-11 用户）："我喜欢的音乐的封面没有成功使用最新收藏的一首歌的
 * 封面作为封面"。歌单卡片原来的封面取的是**列表第一首**，而那首通常是"最早收藏"的
 * 那一首 —— 刚点亮红心的那首根本不会出现在封面上。所以收藏成功时在这里记一份，
 * 打开「我喜欢的音乐」时优先用它当封面（见 app_online 的 ao_page_acct.cc）。
 *
 * ⚠ 在 LVGL 任务里调即可（只有一次 NVS 写，不碰网络）；两个音源共用一份记录，
 *   取用方按 song->src 自己校验是不是当前平台的。
 */
void online_last_liked_song_set(const online_song_t *song);
/* 取这份记录：没有任何收藏记录 / 没登录过 → false（out 不动） */
bool online_last_liked_song_get(online_song_t *out);

/*
 * 歌词文本（PSRAM 缓存，不落 SD）。
 * 成功时 *out 是**调用方所有**的 NUL 结尾 malloc 文本（UTF-8 原样，直接交给
 * media_meta_lrc_parse），用完 free()。二次调用命中缓存。
 * 没网 / 这首歌没有歌词（/lyric 返回空串）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t online_song_lyrics_data(const online_song_t *song, char **out, size_t *out_len);

/* 清掉下载缓存（音频 SD 文件 + PSRAM 封面/歌词缓存），歌单二进制保留 */
esp_err_t online_cache_clear(void);

/* ================================================================ QQ 音乐
 *
 * 走的是同一个服务端（base 见 online_netease_api_base()），只是挂在 `/qq` 前缀下：
 *
 *   鉴权   Authorization: Bearer <同一个 API Key>（os_http_client 里统一加）
 *   登录态 **X-QQMusic-Credential: base64(credential JSON)**   ← 与网易云的 cookie 并列
 *   外壳   {"code":200,"msg":"ok","data":…}（服务端已把 sidecar 的 code:0 归一）
 *
 * 登录 = QQ / 微信扫码：
 *   GET  /qq/login/qrcode/qq             → data.data（base64 PNG）、data.identifier
 *   GET  /qq/login/qrcode/qq/status?identifier=  → data.event（0 完成 / 1 待扫 / 2 待确认
 *                                                  3 过期 / 4 拒绝）、data.credential
 *   POST /qq/login/refresh               → data.credential（服务端顺手做 L2 密文托管）
 *   POST /qq/login/recover               → 设备换板/重刷后按 musicid 取回托管凭证
 *
 * 凭证**整份 JSON 存 NVS**（一个 blob），重启后不用重新扫码；服务端文档建议的
 * 刷新策略见 docs/qq-provider.md §3.2（本模块实现：临期 10% 才刷、串行、成功先落盘）。
 *
 * 与网易云一样，本节所有 API 都是**同步阻塞**的，必须放在工作线程里调。
 */
#define QQ_QR_DONE     0   /* 登录成功（响应里带 credential） */
#define QQ_QR_WAIT     1   /* 等待扫码 */
#define QQ_QR_CONFIRM  2   /* 已扫码，等待手机确认 */
#define QQ_QR_EXPIRED  3   /* 二维码过期 */
#define QQ_QR_REFUSED  4   /* 手机上点了拒绝 */

/* 「我喜欢」的目录 ID（QQ 上游固定值）：往这里加歌 = 喜欢，删歌 = 取消喜欢 */
#define QQ_LIKE_DIRID  "201"
/* 「我喜欢」详情页在 app 里用的详情 Key（不是歌单 ID） */
#define ONLINE_QQ_FAV_KEY  "\x02" "qqfav"

/* 已登录（NVS 里有可用凭证） */
bool online_qq_logged_in(void);
/* 账号昵称（NVS 持久化；没取到 / 未登录 = 空串） */
const char *online_qq_nickname(void);
/* 写昵称（传 NULL/空串 = 清掉）；退出登录 / 换账号时用 */
esp_err_t online_qq_nickname_set(const char *name);
/* QQ 号 / 微信长数字 ID（未登录 = 空串）；服务端部分接口要它 */
const char *online_qq_musicid(void);
/* 退出登录：清内存 + NVS + 上游托管副本（不可逆，要重新扫码） */
esp_err_t online_qq_logout(void);
/* 重新从 NVS 读一次凭证（与 online_netease_cookie_reload 对称；NVS 无需等 SD） */
esp_err_t online_qq_cred_reload(void);

/* 取二维码：identifier（≤128 字节）+ **PNG 原始字节**（调用方 free） */
esp_err_t online_qq_qr_create(char *identifier, size_t id_cap, uint8_t **png, size_t *png_len);
/* 轮询扫码结果：*out_event = QQ_QR_*；成功（QQ_QR_DONE）时自动保存凭证到 NVS */
esp_err_t online_qq_qr_poll(const char *identifier, int *out_event);

/*
 * 账号歌单：先按 musicid 拉**自建歌单**，再按 encryptUin 拉**收藏歌单**，
 * 合并写进 out（self_created 区分）。一个列表失败不影响另一个。
 * 成功时 *out_n 是条数；两个都失败才返回错误。
 */
esp_err_t online_qq_account_playlists(online_acct_playlist_t *out, size_t max, size_t *out_n);

/*
 * 歌单曲目分页：**offset 必须是 100 的倍数**（服务端一页 100 首，见 .cc 的说明）。
 * 只填元数据（id/mid/title/artist/cover_url/dur_sec/src）。
 * *out_n < max 表示到歌单末尾。
 */
#define ONLINE_QQ_PAGE_SONGS  100
esp_err_t online_qq_playlist_page(const char *pl_id, size_t offset, online_song_t *out,
                                  size_t max, size_t *out_n);

/* 「我喜欢」的歌曲分页（dirid=201），语义同上 */
esp_err_t online_qq_fav_songs(size_t offset, online_song_t *out, size_t max, size_t *out_n);

/*
 * 「我喜欢」的全量 id（升序去重，用于播放器页的红心状态）。
 * 内部按 100 首一页翻，最多 QQ_LIKE_MAX_PAGES 页（够用且不至于把工作线程占太久）。
 * *out_ids 由调用方 free。
 */
#define QQ_LIKE_PAGE_SONGS  100
/*
 * 红心状态表最多翻几页（100 首/页）。原来 5 页 = 500 首，而真机账号实测有
 * **719 首**收藏 —— 后 219 首的心形会亮错。8 页 = 800 首，够个人账号用；
 * 代价只是"登录后第一次查收藏状态"多几个回环（之后在 RAM 里缓存）。
 */
#define QQ_LIKE_MAX_PAGES   8
esp_err_t online_qq_likelist(uint64_t **out_ids, size_t *out_n);
/* 在 online_qq_likelist() 的结果里二分查（纯逻辑） */
bool online_qq_liked_has(const uint64_t *ids, size_t n, const char *song_id);

/* 加 / 取消「我喜欢」（dirid=201）；未登录或凭证过期返回 ESP_ERR_INVALID_STATE */
esp_err_t online_qq_like(const char *song_id, bool like);

/*
 * 收藏到指定歌单。
 *   dirid       目标歌单的目录 ID（"我喜欢"传 QQ_LIKE_DIRID=201，自建歌单用
 *               online_acct_playlist_t.dirid）
 *   song_id     歌曲的 songid（十进制）
 *   playlist_id 歌单的 dissid（online_acct_playlist_t.id）；用于"是不是已经收藏过"
 *               的检查，不知道可传 NULL —— 那就跳过检查，只按"加成功"处理
 *   out_already 非 NULL 时返回"目标歌单里本来就有这首歌"（此时没有重复写入）
 *
 * 服务端会先查目标歌单（"我喜欢"按 fav/songs，其它按 songlist/{id}/detail），
 * 命中就直接返回 already，不再打上游的写接口。
 */
esp_err_t online_qq_song_add(const char *dirid, const char *song_id, const char *playlist_id,
                             bool *out_already);
/* 从歌单里删除一首歌（dirid 同上；用于"删除：从当前歌单移除"） */
esp_err_t online_qq_song_del(const char *dirid, const char *song_id);

/*
 * 「每日30首」与「私人雷达」（都是一次性推荐列表，服务端已拼好 30 首）：
 *   GET /qq/recommend/daily → {"data":{"songs":[…30],"source":"guess"}}
 *   GET /qq/recommend/radar → {"data":{"songs":[…30],"source":"radar"}}
 * 服务端把"猜你喜欢"（每次 5 首、随机）连拿 6 次 / 雷达取 3 页再合并去重 ——
 * 设备端只发一次请求。
 */
esp_err_t online_qq_daily_songs(online_song_t *out, size_t max, size_t *out_n);
esp_err_t online_qq_radar_songs(online_song_t *out, size_t max, size_t *out_n);

/* 网络搜索（/qq/search/general_search；*out 由本模块 malloc，调用方 free） */
esp_err_t online_qq_search(const char *keyword, online_song_t **out, size_t *count, size_t max);

/*
 * 播放地址：服务端 /qq/song/{mid}/play_url 已经把 CDN 拼好了（见 docs/qq-provider.md §5.1），
 * 固件直接拿来当流地址用。
 *   ESP_OK                 out 里是完整 https 地址
 *   ESP_ERR_NOT_FOUND      上游没有音源（无版权 / 非 VIP）—— 界面上提示"无播放权限"
 *   ESP_ERR_INVALID_STATE  未登录（QQ 匿名只能拿到免费歌）
 */
esp_err_t online_qq_song_url(const online_song_t *song, char *out, size_t cap,
                             int64_t *out_bytes, uint32_t *out_dur_ms);

/* 歌词：/qq/song/{mid}/lyric → data.lyric（纯 LRC 文本）。*out 由调用方 free。 */
esp_err_t online_qq_lyric(const online_song_t *song, char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
