/*
 * mm_lyrics_file.cc —— 歌词查找（三级）与落盘 + 独立 .lrc 文件
 *
 * 从 media_meta.cc 拆出（拆分蓝图 docs/refactor/05_media_meta.md §6.2）：
 * 原 1578–1750 行。**纯搬迁**，注释随函数一起搬。
 *
 * ⚠ 优先级与落盘路径一个字都不能变（蓝图 R9）：同目录 .lrc → <SD>/lrc/<同名>.lrc
 *   → 内嵌 ID3（取到后**自动写入 /lrc**，下次直接命中第二档）。
 *   落盘失败只告警，不影响返回；目录不存在时 mkdir（已存在返回 EEXIST，忽略）。
 */
#include "internal/mm_lyrics_file.h"
#include "internal/mm_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "internal/mm_id3.h"
#include "internal/mm_lrc.h"

static const char *TAG = "media_meta";

esp_err_t mm_embedded_lyrics(const char *audio_path, char **text)
{
    if (!audio_path || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    *text = NULL;
    const char *dot = strrchr(audio_path, '.');
    if (!dot || strcasecmp(dot, ".mp3") != 0) {
        return ESP_ERR_NOT_SUPPORTED;       /* FLAC/OGG 的 LYRICS 字段后续补 */
    }
    FILE *f = fopen(audio_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = mm_id3_lyrics(f, text);
    fclose(f);
    return ret;
}

void mm_lyrics_free(char *text)
{
    free(text);
}

/* ---------------------------------------------------------- 歌词查找/落盘 */

/* "/sdcard/xx/yy.mp3" → "/sdcard/lrc" */
bool mm_lyrics_dir_for(const char *audio_path, char *dir, size_t cap)
{
    if (!audio_path || audio_path[0] != '/') {
        return false;
    }
    const char *second = strchr(audio_path + 1, '/');
    if (!second) {
        return false;
    }
    int n = snprintf(dir, cap, "%.*s/lrc", (int)(second - audio_path), audio_path);
    return (n > 0) && (n < (int)cap);
}

/* "/sdcard/xx/yy.mp3" → "/sdcard/lrc/yy.lrc" */
bool mm_lyrics_cache_path(const char *audio_path, char *path, size_t cap)
{
    char dir[600];
    if (!mm_lyrics_dir_for(audio_path, dir, sizeof(dir))) {
        return false;
    }
    const char *base = strrchr(audio_path, '/');
    base = base ? base + 1 : audio_path;
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    int n = snprintf(path, cap, "%s/%.*s.lrc", dir, (int)stem, base);
    return (n > 0) && (n < (int)cap);
}

esp_err_t mm_lyrics_load(const char *audio_path, lrc_t *out)
{
    if (!audio_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    mm_lrc_reset(out);

    /* 1) 歌曲同目录的同名 .lrc */
    if (mm_lrc_load_sidecar(audio_path, out) == ESP_OK && out->count > 0) {
        ESP_LOGI(TAG, "lyrics from sidecar next to the song");
        return ESP_OK;
    }

    /* 2) <挂载点>/lrc/<同名>.lrc */
    char dir[600]  = { 0 };
    char lrc[600]  = { 0 };
    const bool cache_ok = mm_lyrics_cache_path(audio_path, lrc, sizeof(lrc));
    if (cache_ok) {
        mm_lyrics_dir_for(audio_path, dir, sizeof(dir));
        if (mm_lrc_load_file(lrc, out) == ESP_OK && out->count > 0) {
            ESP_LOGI(TAG, "lyrics from %s", lrc);
            return ESP_OK;
        }
    }

    /* 3) 内嵌歌词：解析 + **落盘到 /lrc**，下次直接命中第 2 档 */
    char     *text = NULL;
    esp_err_t ret  = mm_embedded_lyrics(audio_path, &text);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "no embedded lyrics (%s)", esp_err_to_name(ret));
        return ret;
    }
    ret = mm_lrc_parse(text, out);
    if (ret == ESP_OK && out->count > 0 && cache_ok) {
        /* 目录可能不存在：SD 卡根下建一个 */
        if (dir[0]) {
            mkdir(dir, 0777);               /* 已存在返回 EEXIST，忽略 */
        }
        FILE *f = fopen(lrc, "wb");
        if (f) {
            size_t n = strlen(text);
            if (fwrite(text, 1, n, f) != n) {
                ESP_LOGW(TAG, "write %s failed (short write)", lrc);
            }
            fclose(f);
            ESP_LOGI(TAG, "lyrics from ID3, saved to %s (%u bytes)", lrc, (unsigned)n);
        } else {
            ESP_LOGW(TAG, "cannot save lyrics to %s", lrc);
        }
    }
    mm_lyrics_free(text);
    return ret;
}

/*
 * 独立 .lrc 文件 → lrc_t（在线流式播放用）。
 * 音频在网络上，本地只有我们缓存的歌词文件，所以不能走
 * "按音频路径找同名 .lrc"那条路；直接读文件 + 复用 lrc_parse。
 */
esp_err_t mm_lyrics_load_file(const char *lrc_path, lrc_t *out)
{
    if (!lrc_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(lrc_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const long sz = ftell(f);
    if (sz <= 0 || sz > (long)(512 * 1024)) {      /* 歌词不该大于 512 KB */
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = (char *)mm_alloc_big((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    rewind(f);
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    const esp_err_t ret = mm_lrc_parse(buf, out);
    free(buf);
    return ret;
}
