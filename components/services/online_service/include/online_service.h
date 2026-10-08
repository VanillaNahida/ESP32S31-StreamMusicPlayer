/*
 * online_service.h —— 在线音乐（网易云歌单，走 Meting API）
 *
 * 数据来源：Meting API（默认 http://meting.xcnahida.cn/api），接口约定：
 *   歌单   ?server=netease&type=playlist&id=<歌单ID>   → JSON 数组
 *   歌曲   ?server=netease&type=url&id=<歌曲ID>        → 302 到真实音频地址
 *   歌词   ?server=netease&type=lrc&id=<歌曲ID>        → 纯文本 LRC
 *   封面   ?server=netease&type=pic&id=<歌曲ID>        → 302 到 jpg
 * 每条歌记录形如：{"title","author","pic","url","lrc"}，歌曲 ID 藏在 url 里。
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
 * 为什么"先下载再播放"而不是直接流式播放：
 *   1) 播放通路与本地文件完全一致 —— 进度/拖动定位这些"和本地一样"的
 *      需求不用再写第二套；
 *   2) meting 的音频地址是 302 到 https CDN，先落地可以把 TLS/断流问题集中在
 *      下载这一处，播放时不再受网络抖动影响。
 * 代价是需要 SD 空间与几秒的首播等待，缓存命中后就是本地播放的速度。
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
    uint32_t id;                          /* 网易云歌曲 ID（从 url 里解析） */
    char     title[ONLINE_TITLE_MAX];
    char     artist[ONLINE_ARTIST_MAX];
    char     cover_url[ONLINE_URL_MAX];
    char     audio_url[ONLINE_URL_MAX];   /* meting type=url（会 302 到真实音频） */
    char     lrc_url[ONLINE_URL_MAX];
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
 * 搜索歌曲。
 *   服务端：?server=netease&type=search&id=<关键词>（关键词内部做百分号编码，
 *           支持中文；服务端返回真实搜索结果）
 *   兜底：没网或服务端失败时，扫本机已保存歌单做子串匹配（歌名/歌手，不区分大小写）
 * *out 由本模块 malloc，调用方 free()。max 为返回上限。
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

/*
 * 歌词文本（PSRAM 缓存，不落 SD）。
 * 成功时 *out 是**调用方所有**的 NUL 结尾 malloc 文本（UTF-8 原样，直接交给
 * media_meta_lrc_parse），用完 free()。二次调用命中缓存。
 * 没歌词 URL / 没网返回 ESP_ERR_NOT_FOUND；下载失败返回对应错误码。
 */
esp_err_t online_song_lyrics_data(const online_song_t *song, char **out, size_t *out_len);

/* 清掉下载缓存（音频 SD 文件 + PSRAM 封面/歌词缓存），歌单二进制保留 */
esp_err_t online_cache_clear(void);

#ifdef __cplusplus
}
#endif
