/*
 * os_pl_format.h —— 歌单二进制文件的格式契约 + 编解码（纯内存 pack/unpack）
 *
 * 二进制歌单文件格式（定长，方便直接 mmap/定点读写，也符合"二进制保存"的需求）：
 *   struct online_pl_hdr_t { uint32_t magic; uint32_t version; uint32_t count; uint32_t rsv; }
 *   紧接着 count 条 online_rec_t（定长 **1252** 字节）
 * 小端（ESP32 本身就是小端，不做额外字节序转换）。无 CRC、无文件长度字段。
 *
 * 路径：/sdcard/online/pl_<歌单ID>.bin（os_pl_path()）
 *
 * ⚠ 文档漂移（原文件头注释第 6 行写"定长 896 字节"）：实际
 *   4+128+96+320+320+320+64 = **1252**，896 应是早期 ONLINE_URL_MAX 更小时的残留。
 *   本文件用 _Static_assert 把真实布局钉死；改 ONLINE_*_MAX 会立刻编译失败
 *   （否则旧文件会**静默**读不出来 —— 头部只校验 magic/version）。
 *
 * 原文件里的名字：online_rec_t / online_pl_hdr_t（118–133）、pl_save（811–848）、
 * pl_load（851–899）；search_local（1076–1101）里那套重复的读盘循环也收敛到这里。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_PL_MAGIC    0x4F504C31u   /* 'OPL1' */
#define ONLINE_PL_VERSION  1u

/* 落盘用的定长记录 */
typedef struct {
    uint32_t id;
    char     title[ONLINE_TITLE_MAX];
    char     artist[ONLINE_ARTIST_MAX];
    char     cover_url[ONLINE_URL_MAX];
    char     audio_url[ONLINE_URL_MAX];
    char     lrc_url[ONLINE_URL_MAX];
    uint8_t  rsv[64];
} online_rec_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t rsv;
} online_pl_hdr_t;

/*
 * 布局断言：定长记录的字节偏移就是磁盘格式，任何一处改动都要连带升 version
 * （字段偏移见拆分蓝图 §C.2 —— 0 id / 4 title / 132 artist / 228 cover_url /
 *  548 audio_url / 868 lrc_url / 1188 rsv）。
 */
_Static_assert(sizeof(online_rec_t) == 1252, "online_rec_t 布局变了：磁盘格式要升 version");
_Static_assert(sizeof(online_pl_hdr_t) == 16, "online_pl_hdr_t 布局变了");

/* 歌单文件路径：/sdcard/online/pl_<id>.bin */
void os_pl_path(const char *id, char *out, size_t cap);

/* 把内存里的歌曲数组写成二进制文件（**非原子**，任一步失败即 unlink 后返回失败） */
esp_err_t os_pl_save(const char *id, const online_song_t *songs, size_t count);

/* 读二进制文件到内存数组（头部校验 + 空标题记录跳过压紧）。
 * 成功时 *out 由本模块分配（PSRAM 优先），调用方 free()。 */
esp_err_t os_pl_load(const char *id, online_song_t **out, size_t *out_count);

/* 一条记录 → online_song_t（字段逐一 os_str_copy，先清零） */
void os_pl_rec_to_song(const online_rec_t *rec, online_song_t *out);

/*
 * 打开歌单文件、读头并校验。**失败返回 NULL**（文件不存在 / 头不合法）。
 * ⚠ 只校验 magic + version + 1 ≤ count ≤ 5000：与老 pl_load 的判据一致，
 *   而老的 search_local 只校验 magic —— 见 os_pl_open_for_scan()。
 */
FILE *os_pl_open(const char *id);

/*
 * 搜索用的宽松版：不校验 version/count（照搬老 search_local 的判据），
 * 文件不存在或 magic 不对就返回 NULL。条数从 *out_count 取。
 */
FILE *os_pl_open_for_scan(const char *id, uint32_t *out_count);

/* 读一条定长记录；false = 读不出来（截断 / EOF），等价于老的 `fread(...) != 1` */
bool os_pl_read_record(FILE *f, online_rec_t *rec);

#ifdef __cplusplus
}
#endif
