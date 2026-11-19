/*
 * online_service.cc —— 在线音乐（网易云原生 API）对外门面
 *
 * 这个文件是拆分后的**薄门面**：只留 init、查询与几个需要跨模块编排的动作。
 * 各域的实现分布（拆分蓝图 docs/refactor/03_online_service.md §6）：
 *
 *   os_state.cc       共享状态（歌单 ID 表 / init 标志 / last_err）+ 路径与 NVS 键
 *   os_util.cc        **纯逻辑**：UTF-8 安全截断 / ID 提取 / 百分号编码 / 魔数定扩展名
 *   os_fs.cc          PSRAM 分配 / 文件与目录小工具 / online_has_network
 *   os_http_client.cc 自管 302 的同步 HTTP（GET / 下载 / Location 拼回主机）
 *   os_json.cc        **纯逻辑**：cJSON 取值 + 网易云对象 → online_song_t
 *   os_pl_format.cc   **纯逻辑**：歌单二进制格式（pack/unpack + 读写盘）
 *   os_playlists.cc   歌单 NVS 列表 + 抓取 + 落盘编排
 *   os_search.cc      搜索（服务端 type=search + 本地歌单兜底）
 *   os_cache.cc       **纯逻辑**：通用 LRU 字节缓存容器
 *   os_media_cache.cc 封面 / 歌词（PSRAM，不落 SD）+ 播放前预热 + 兜底下载
 *   os_audio_cache.cc SD 音频缓存（命中查找 / LRU 清理）
 *   os_stream_url.cc  播放地址 302 解析（+ MP3 码率 → 时长估算）
 *   os_history.cc     搜索历史（NVS）
 *   os_api.cc         自建 API 服务：带 cookie 的 GET
 *   os_login.cc       cookie（内置 / SD / 压缩 / 存盘）+ 昵称 + 扫码登录 + 登出
 *   os_account.cc     账号 API base + 账号信息 / 歌单分页 / 每日推荐 / 直链
 *
 * 对外契约（include/online_service.h）一行未改；线程模型也不变：
 * 所有 API 都是**同步阻塞**的，必须放在工作线程里调用，不要在 LVGL 任务里直接调。
 */
#include "online_service.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_log.h"

#include "internal/os_fs.h"
#include "internal/os_history.h"
#include "internal/os_json_mem.h"
#include "internal/os_login.h"
#include "internal/os_media_cache.h"
#include "internal/os_playlists.h"
#include "internal/os_state.h"

static const char *TAG = "online";

/* ---------------------------------------------------------------- init */

esp_err_t online_service_init(void)
{
    if (os_state_ready()) {
        return ESP_OK;
    }
    /* ★ 必须在**任何 cJSON 调用之前**装 hooks：cJSON 的 malloc/free 是全局函数指针，
     *   装晚了就会"malloc 分配、heap_caps_free 释放"混用。likelist / 歌单详情的
     *   节点海量（3 万首 ≈ 2 MB），不搬 PSRAM 就会把内部 RAM 抽干（见 os_json_mem.cc）。 */
    os_json_use_psram();
    os_mkdir_if_needed(ONLINE_ROOT);
    os_mkdir_if_needed(ONLINE_AUDIO_DIR);
    /* 封面/歌词不再落 SD：把旧固件写下来的 cover/ 目录与 *.lrc 清掉，
     * 免得它们继续占用 SD 空间、也避免下次切歌再读到旧文件。 */
    os_wipe_dir(ONLINE_COVER_DIR);
    os_wipe_dir(ONLINE_LRC_DIR);
    {
        DIR *d = opendir(ONLINE_AUDIO_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') {
                    continue;
                }
                const char *dot = strrchr(ent->d_name, '.');
                if (dot && strcasecmp(dot, ".lrc") == 0) {
                    char path[320];
                    snprintf(path, sizeof(path), "%s/%s", ONLINE_AUDIO_DIR, ent->d_name);
                    unlink(path);
                }
            }
            closedir(d);
        }
    }
    os_playlists_nvs_load();
    os_history_nvs_load();
    os_nick_load();           /* 账号昵称：重启后不等网络就能显示"已登录：XX" */
    os_cookie_load();         /* 登录态：SD 已挂的话这里就能恢复，省得界面闪一下未登录 */
    os_state_set_ready();
    ESP_LOGI(TAG, "online service ready: %u playlist id(s)",
             (unsigned)os_state_playlist_count());
    return ESP_OK;
}

/* ---------------------------------------------------------------- 查询 */

size_t online_playlist_count(void)
{
    return os_state_playlist_count();
}

const char *online_last_error(void)
{
    return os_state_last_error();
}

const char *online_playlist_id(size_t idx)
{
    return os_state_playlist_id(idx);
}

const char *online_playlist_name(size_t idx)
{
    return os_state_playlist_name(idx);
}

esp_err_t online_playlist_rename(size_t idx, const char *name)
{
    if (idx >= os_state_playlist_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    os_state_playlist_rename(idx, name);
    os_playlists_nvs_save();
    ESP_LOGI(TAG, "playlist '%s' renamed to '%s'", online_playlist_id(idx), online_playlist_name(idx));
    return ESP_OK;
}

/* ---------------------------------------------------------------- 缓存清理 */

esp_err_t online_cache_clear(void)
{
    /* SD 上的音频缓存（封面/歌词早已不进 SD） */
    DIR *d = opendir(ONLINE_AUDIO_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            /* 目录名 + '/' + FATFS 最长 255 字节文件名：给够空间，别让编译器担心截断 */
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", ONLINE_AUDIO_DIR, ent->d_name);
            unlink(path);
        }
        closedir(d);
    }
    /* PSRAM 里的封面/歌词缓存 */
    os_media_cache_clear();
    return ESP_OK;
}
