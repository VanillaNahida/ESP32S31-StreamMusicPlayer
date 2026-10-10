/*
 * os_media_cache.h —— 封面 / 歌词的 PSRAM 缓存（**不落 SD**）+ 播放前的轻量准备
 *
 * 原文件里的名字：cover_format（1325–1346，纯逻辑魔数判定）、
 * online_song_cover_data（1348–1440）、online_song_lyrics_data（1446–1495）、
 * psram_cache_clear（1764–1774）。
 *
 * ---------------------------------------------------------------------------
 * ⚠ 所有权契约（拆分前写在 online_service.h / 文件头，**一个字都不能变**）：
 *
 *   缓存里的内存**永远不直接外发**：对外 API 返回的始终是"调用方新 malloc 的拷贝"，
 *   所以 LRU 淘汰不会踩到播放端正在用的内存。
 *
 *   | API | 命中缓存 | 未命中 | 返回内存归属 | 释放 |
 *   | --- | --- | --- | --- | --- |
 *   | online_song_cover_data | psram_alloc(len) + memcpy | 下载 → 校验格式 →
 *   |   os_cache_put（把下载缓冲交给缓存）→ 再拷贝一份给调用方 | **调用方所有** | free() |
 *   | online_song_lyrics_data | psram_alloc(len+1) + memcpy + 补 '\0' | 同上（纯文本） |
 *   |   **调用方所有** | free() |
 *
 *   封面**格式白名单按魔数**判定（不信 URL 后缀、不信 Content-Type；解码端
 *   media_meta 的 image_to_rgb565() 同样按魔数分派），不支持的格式返回
 *   ESP_ERR_NOT_SUPPORTED 并把中文原因写进 last_err。
 *
 *   失败路径保持原样：下载成功但拷贝失败 → ESP_ERR_NO_MEM（缓存里仍留着数据）；
 *   http_get_text 失败 / 长度 < 64 → free(raw)，不泄漏。
 *
 *   与 media_meta 的耦合：COVER_MAX_BYTES(4 MB) 必须 ≤ MEDIA_META_COVER_FILE_MAX，
 *   否则会出现"下下来了却解不了"。
 * ---------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "online_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 封面字节上限。解码端 media_meta 的 MEDIA_META_COVER_FILE_MAX 必须 ≥ 这个值，
 * 否则会出现"下下来了却解不了"。两个都放在 4 MB：
 * 封面正常只有 100~300 KB，但**平台的原图可能大得离谱**（实测某首网易云封面
 * 原图 7.6 MB），所以下面还挂了 ?param= 让服务端先缩图，这个上限只当兜底。 */
#define COVER_MAX_BYTES    (4u * 1024u * 1024u)

/*
 * 封面请求的"服务端缩图"参数：网易云 CDN 认 ?param=WxH（也接受 y 分隔）。
 *
 * ★ 为什么必须加（2026-10 修"部分歌曲封面不显示"）：
 *   实测 https://p2.music.126.net/.../109951169045313754.jpg
 *     - 后缀是 .jpg、Content-Type 也写 image/jpg，**下来的却是 PNG**
 *       （文件头 89 50 4E 47 0D 0A 1A 0A）；
 *     - 而且原图 **7,951,295 字节（7.6 MB）** —— 远超下载上限，封面永远出不来。
 *   加上 ?param=300y300 之后只有 ~100 KB（同样是 PNG），秒下。
 *   界面上的封面框是 240 px、解码目标 COVER_MAX_DIM 也是 240，300 已经够用。
 *
 * ⚠ 两个尺寸**都必须给**，少一个参数会被 CDN 静默忽略、原样返回 7.6 MB 原图
 *   （实测 2026-10 同一条链接）：
 *     ?param=200x200  → 100,539 B  ✓（x / y 分隔符都认）
 *     ?param=200y200  → 100,539 B  ✓
 *     ?param=200      → 7,951,295 B ✗ ← 只有一维 = 没生效，别"简化"成这个写法
 */
#define ONLINE_COVER_PARAM  300

/* ---- PSRAM 封面/歌词缓存（不落 SD，见 online_service.h 文件头） ---- */
#define COVER_CACHE_MAX       8
#define COVER_CACHE_BYTES_MAX (4u * 1024u * 1024u)
#define LRC_CACHE_MAX         8
#define LRC_CACHE_BYTES_MAX   (256u * 1024u)

/* 清掉 PSRAM 里的封面/歌词缓存（online_cache_clear / logout 用） */
void os_media_cache_clear(void);

/*
 * 封面格式：**只认文件头魔数**，绝不看 URL 后缀或 Content-Type。
 *
 * ★ 平台这两个都会撒谎（2026-10 实测网易云）：
 *     URL 后缀 .jpg、Content-Type: image/jpg，实际字节是 PNG。
 *   解码端 media_meta 的 image_to_rgb565() 同样是按魔数分派的（PNG 走 libpng、
 *   其余按 JPEG 走 esp_new_jpeg），所以这里判定一致，不会出现"按后缀选了错的解码器"。
 *
 * 返回可打印的格式名；不认识的（WebP / GIF / HTML 错误页…）返回 NULL 并给出名字。
 */
const char *os_cover_format(const uint8_t *d, size_t n, const char **why);

#ifdef __cplusplus
}
#endif
