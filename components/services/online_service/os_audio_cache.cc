/*
 * os_audio_cache.cc —— SD 音频缓存的命中查找与 LRU 清理
 *
 * 逐字搬自原 online_service.cc（find_cached_audio 1186–1211、
 * audio_cache_trim 1214–1257），只加 os_ 前缀。
 * 扩展名嗅探（ext_from_sniff）搬到了纯逻辑的 os_util.cc：os_ext_from_sniff()。
 */
#include "internal/os_audio_cache.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "internal/os_fs.h"
#include "internal/os_state.h"

/* 在音频缓存目录里找这首歌已下载的文件（扩展名不定，所以按前缀找） */
bool os_find_cached_audio(uint32_t song_id, char *out, size_t cap)
{
    DIR *d = opendir(ONLINE_AUDIO_DIR);
    if (!d) {
        return false;
    }
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%u.", (unsigned)song_id);
    const size_t plen = strlen(prefix);
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) != 0) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), ONLINE_AUDIO_DIR "/%s", ent->d_name);
        if (os_file_ok(path, 32 * 1024)) {
            snprintf(out, cap, "%s", path);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* 缓存太多就删最旧的几个（SDL 卡很大，但别无限涨） */
void os_audio_cache_trim(void)
{
    DIR *d = opendir(ONLINE_AUDIO_DIR);
    if (!d) {
        return;
    }
    /* 统计 + 找出最旧的那几个：文件数不多（<= 几百），直接两遍扫描 */
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] != '.') {
            n++;
        }
    }
    if (n <= AUDIO_CACHE_MAX) {
        closedir(d);
        return;
    }
    int to_del = n - AUDIO_CACHE_MAX;
    for (int k = 0; k < to_del; k++) {
        rewinddir(d);
        char oldest[320] = "";
        time_t oldest_t = 0;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char path[320];
            snprintf(path, sizeof(path), ONLINE_AUDIO_DIR "/%s", ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }
            if (oldest[0] == '\0' || st.st_mtime < oldest_t) {
                snprintf(oldest, sizeof(oldest), "%s", path);
                oldest_t = st.st_mtime;
            }
        }
        if (oldest[0]) {
            unlink(oldest);
        }
    }
    closedir(d);
}
