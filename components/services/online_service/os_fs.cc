/*
 * os_fs.cc —— PSRAM 分配 / 文件与目录小工具 / 网络判定
 *
 * 逐字搬自原 online_service.cc（psram_alloc 254–258、file_ok 273–280、
 * mkdir_if_needed 282–289、wipe_dir 1806–1823、online_has_network 1798–1801）。
 * 只改了名字：加 os_ 前缀。
 */
#include "internal/os_fs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#include "wifi_service.h"    /* online_has_network() */

void *os_psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

bool os_file_ok(const char *path, size_t min_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return (size_t)st.st_size >= min_size;
}

esp_err_t os_mkdir_if_needed(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    return (mkdir(path, 0777) == 0) ? ESP_OK : ESP_FAIL;
}

/*
 * 删掉一个目录里的全部内容，并尝试 rmdir（用于清理历史遗留的 cover/、lrc/ 目录）。
 */
void os_wipe_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

bool online_has_network(void)
{
    return wifi_service_has_ip();
}
