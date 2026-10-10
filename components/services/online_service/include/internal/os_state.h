/*
 * os_state.h —— 在线音乐服务的跨模块共享状态（路径 / NVS 键 / 歌单 ID 表 / last_err）
 *
 * 原 `online_service.cc` 拆分前的文件头契约（**原样保留**）：
 *
 *   落盘（需求："抓取歌单后把歌单列表二进制保存在存储卡内"）：
 *     /sdcard/online/pl_<歌单ID>.bin     定长头 + 定长记录数组（见 os_pl_format.h）
 *     /sdcard/online/audio/<歌曲ID>.<ext>  已下载的音频（兜底路径播放的就是它）
 *
 *   封面 / 歌词**只缓存在 PSRAM，不写 SD 卡**（2026-10 改）：
 *     之前封面/歌词落 SD，切歌时读/写这些文件会让 SD 卡忙到"切歌阻塞"（日志里
 *     一排 sdmmc_req 0x107 超时，封面还下成 0 字节）。现在下载后整段留在 PSRAM：
 *       online_song_cover_data()  → 调用方拿到一张 malloc 的图片字节拷贝（JPEG/PNG）
 *       online_song_lyrics_data() → 调用方拿到一段 malloc 的 LRC 文本
 *     同一首歌二次请求直接命中内存缓存，不再碰网络与 SD。SD 上历史遗留的
 *     cover/ 目录与 *.lrc 文件在 online_service_init() 时清掉。
 *
 *   HTTP 说明：
 *     - meting 的 API/歌词/封面都支持 **http**，走明文省掉 TLS 与时间同步依赖；
 *     - 音频是 302 到 https CDN，必须 TLS 校验（证书包已开），
 *       所以下载音频前要确认系统时间已经同步（SNTP 在 WiFi 拿到 IP 后启动）。
 *
 * 这里只放"多个 .cc 都要看"的常量与访问器；实现见 os_state.cc。
 * ⚠ 状态本身**无锁**（拆分前后一致）：所有 API 都是同步阻塞的，必须放在工作线程里
 *   调用 —— 见 online_service.h 的线程模型说明。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "online_service.h"   /* ONLINE_MAX_PLAYLISTS / ONLINE_ID_MAX / ONLINE_NAME_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 路径与文件名 */
#define ONLINE_ROOT        "/sdcard/online"
#define ONLINE_AUDIO_DIR   ONLINE_ROOT "/audio"
/* 历史遗留（不再写入，只在 init 时清理）： */
#define ONLINE_COVER_DIR   ONLINE_ROOT "/cover"
#define ONLINE_LRC_DIR     ONLINE_ROOT "/lrc"
#define ONLINE_PL_FMT      ONLINE_ROOT "/pl_%s.bin"

/* ---------------------------------------------------------------- NVS 键名 */
#define NVS_NS             "online"
#define NVS_KEY_CNT        "n"
#define NVS_KEY_ID_FMT     "id%d"      /* id0..id7 */
/* 歌单显示名（用户可重命名；空 = 还没起过名，UI 就显示 ID 或抓到的歌单名） */
#define NVS_KEY_NAME_FMT   "nm%d"      /* nm0..nm7 */
/* 搜索历史（最多 ONLINE_HISTORY_MAX 条，新的在最前，按关键词去重） */
#define NVS_KEY_SH_CNT     "shn"
#define NVS_KEY_SH_FMT     "sh%d"      /* sh0..sh9 */
/* 已登录账号昵称（重启后立刻能显示，不用等网络往返） */
#define NVS_KEY_NICK       "nick"

/* ---------------------------------------------------------------- Meting 接口
 * 搜索接口在另一个路径上，而且关键词放在 id= 里：
 *   http://meting.xcnahida.cn/meting/api?server=netease&type=search&id=<关键词>
 * （/api?type=search&keywords=xxx 在该实例上会忽略关键词、返回固定列表，别用） */
#define API_BASE           "http://meting.xcnahida.cn/api"
#define API_SEARCH_BASE    "http://meting.xcnahida.cn/meting/api"

/* ---------------------------------------------------------------- init 幂等标志 */
bool os_state_ready(void);
void os_state_set_ready(void);

/* ---------------------------------------------------------------- 歌单 ID 表
 * 读写都在这一层收敛：其它 .cc 不再直接摸 s_ids/s_names/s_id_count。 */
size_t os_state_playlist_count(void);
const char *os_state_playlist_id(size_t idx);          /* 越界 NULL */
const char *os_state_playlist_name(size_t idx);        /* 越界 NULL；没起名 = 空串 */

/* 改显示名（NULL/空串 = 清除自定义名）；越界什么都不做 */
void os_state_playlist_rename(size_t idx, const char *name);

/* 追加一个歌单 ID（表满返回 false，不写任何东西） */
bool os_state_playlist_append(const char *id);

/* 从表里删掉下标 idx 的那一条（后面元素前移；名字跟着挪） */
void os_state_playlist_remove_at(size_t idx);

/* 在已保存 ID 里线性查下标（跨整张表的原始比较），找不到 -1 */
int os_state_playlist_index(const char *id);

/* NVS 读取专用：整表清空重填 */
void os_state_playlist_reset(void);
/* NVS 读取专用：追加一条（id 非空才计入），成功写进 out 缓冲；返回是否写入了 */
bool os_state_playlist_load_one(const char *id, const char *name);

/* ---------------------------------------------------------------- last_err
 * 最近一次失败的可读原因（给 UI 的 Toast 用；空串 = 没失败过）。
 * ⚠ 跨任务覆盖、无锁 —— 与拆分前 s_last_err 的语义完全一致。 */
void        os_state_set_last_error(const char *msg);
const char *os_state_last_error(void);          /* NULL = 没失败过 */

#ifdef __cplusplus
}
#endif
