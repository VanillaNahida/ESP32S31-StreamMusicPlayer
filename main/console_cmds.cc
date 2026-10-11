/*
 * console_cmds.cc —— 串口终端命令：打印歌曲文件信息（给 AI 分析与性能优化用）
 *
 * 为什么加这个：排查"某个文件卡顿 / 速度不对 / 拖不动"时，
 * 光看播放日志看不出**文件本身**长什么样（采样率、位深、容器、标签有多大、
 * 音频数据从第几个字节开始……），而 AI（或人）拿到这些数才能判断该往哪优化。
 * 所以这里把 `media_meta` 的探测能力、标签读取与播放服务状态直接暴露成命令。
 *
 * 命令（全部输出 `key: value` 行，便于机器解析；一行一个字段）：
 *
 *   songinfo <绝对路径>    单个文件：容器/参数/标签/大小/音频数据偏移 + 性能提示
 *   songscan <目录>        递归扫描，一行一个文件（TSV），适合整库统计
 *   playstat               当前播放状态快照（码率/位置/输出路由/流进度）
 *
 * 注意：
 *   - 命令跑在 **REPL 任务**里（不是 LVGL 任务），所以**不能**碰任何 UI 对象；
 *     这里只调用 media_meta / audio_service / storage_service 的只读接口。
 *   - `songinfo`/`songscan` 会读 SD（同步），扫描大目录时命令会阻塞一会儿 ——
 *     这是有意的：诊断命令要的是"一口气把结果打完"。
 *   - REPL 走 `esp_console_new_repl_stdio()`（IDF 6.1），与日志共用 console UART
 *     （115200，见 AGENTS §3.1），日志和提示符会交错，属于正常现象。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_console.h"
#include "esp_log.h"

#include "console_cmds.h"     /* ★ 必须包含自己：main.cc 按 C 链接调用（extern "C"），
                               *   定义处若不带这个声明就会是 C++ 修饰名 → 链接期
                               *   undefined reference（本工程踩过一次同类坑） */
#include "audio_service.h"
#include "media_meta.h"

static const char *TAG = "console";

/* ---------------------------------------------------------------- 工具 */

/* 扩展名（小写，不带点）；没有则返回空串 */
static void path_ext_lower(const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    if (!path) {
        return;
    }
    const char *dot = strrchr(path, '.');
    if (!dot || dot[1] == '\0') {
        return;
    }
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < cap; p++) {
        out[n++] = (char)((*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p);
    }
    out[n] = '\0';
}

static const char *guess_container(const char *path)
{
    char ext[16];
    path_ext_lower(path, ext, sizeof(ext));
    if (!strcmp(ext, "mp3"))  return "MP3";
    if (!strcmp(ext, "flac")) return "FLAC";
    if (!strcmp(ext, "wav"))  return "WAV";
    if (!strcmp(ext, "aac"))  return "ADTS-AAC";
    if (!strcmp(ext, "m4a") || !strcmp(ext, "mp4")) return "MP4/M4A";
    if (!strcmp(ext, "ogg"))  return "OGG";
    if (!strcmp(ext, "opus")) return "OPUS";
    return ext[0] ? "未知" : "无扩展名";
}

static uint64_t file_size_of(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) == 0 && st.st_size > 0) {
        return (uint64_t)st.st_size;
    }
    return 0;
}

static void print_tags(const char *path)
{
    media_tags_t tags = {};
    const esp_err_t r = media_meta_read_tags(path, &tags);
    printf("tag_read: %s\n", r == ESP_OK ? "ok" : esp_err_to_name(r));
    if (r == ESP_OK) {
        printf("tag_title: %s\n", tags.title[0] ? tags.title : "(空)");
        printf("tag_artist: %s\n", tags.artist[0] ? tags.artist : "(空)");
        printf("tag_album: %s\n", tags.album[0] ? tags.album : "(空)");
        printf("tag_composer: %s\n", tags.composer[0] ? tags.composer : "(空)");
    }
}

/* ---------------------------------------------------------------- songinfo */

static int cmd_songinfo(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法: songinfo <绝对路径>   例: songinfo /sdcard/a.flac\n");
        return 1;
    }
    const char *path = argv[1];

    const uint64_t size = file_size_of(path);
    printf("path: %s\n", path);
    printf("container_guess: %s\n", guess_container(path));
    if (size == 0) {
        printf("file_size: 0   (打不开？路径要对，且 SD 已挂载)\n");
        return 1;
    }
    printf("file_size: %llu\n", (unsigned long long)size);

    /* 容器参数（media_meta 按魔数分派，不看扩展名） */
    audio_probe_t p = {};
    const esp_err_t pr = media_meta_probe_audio(path, &p);
    printf("probe: %s\n", pr == ESP_OK ? "ok" : esp_err_to_name(pr));
    if (pr == ESP_OK) {
        printf("sample_rate: %u\n", (unsigned)p.sample_rate);
        printf("channels: %u\n", (unsigned)p.channels);
        printf("bits: %u\n", (unsigned)p.bits);
        printf("duration_ms: %u\n", (unsigned)p.duration_ms);
        printf("bitrate_bps: %u\n", (unsigned)p.bitrate);
        if (p.duration_ms) {
            printf("duration_hms: %u:%02u\n", (unsigned)(p.duration_ms / 60000u),
                   (unsigned)((p.duration_ms / 1000u) % 60u));
        }
        if (p.bitrate && size) {
            /* 用平均码率反推"整首应当有多少字节"，与真实大小差得远 =
             * 容器里有大块非音频数据（封面/标签），或码率是估的 */
            const uint64_t expect = (uint64_t)p.bitrate * p.duration_ms / 8000ull;
            printf("expected_bytes_by_bitrate: %llu  (差值 %+lld = 非音频数据，多为封面/标签)\n",
                   (unsigned long long)expect,
                   (long long)((int64_t)size - (int64_t)expect));
        }
    }

    printf("audio_data_offset: %u  (音频数据从第几字节开始；这块越大，起播读得越多)\n",
           (unsigned)media_meta_audio_data_offset(path));
    print_tags(path);

    /* ---- 性能提示：这条链会不会走重采样、解码负载大概是 44.1k 的几倍 ---- */
    if (pr == ESP_OK && p.sample_rate) {
        const bool need_asrc = (p.sample_rate != 48000u || p.channels != 2u);
        printf("perf_need_asrc: %s   (源 %u Hz %uch -> 输出恒 48000 Hz 2ch)\n",
               need_asrc ? "yes" : "no", (unsigned)p.sample_rate, (unsigned)p.channels);
        printf("perf_samples_per_sec: %u   (采样率 × 声道；越大解码越吃力)\n",
               (unsigned)(p.sample_rate * (p.channels ? p.channels : 1u)));
        printf("perf_load_vs_44100: %.2f x\n",
               (double)p.sample_rate / 44100.0);
        if (p.bits > 16u) {
            printf("perf_extra_bit_convert: yes (源 %u bit -> 输出 16 bit)\n", (unsigned)p.bits);
        }
        if (p.duration_ms && size) {
            printf("perf_audio_bytes_per_sec: %llu   (每秒要读多少字节)\n",
                   (unsigned long long)(size * 1000ull / p.duration_ms));
        }
    } else {
        printf("perf_note: 探测失败 —— 时长/码率未知（seek 与进度条会不可用）\n");
    }

    /* 正在播的是不是这个文件（方便和播放日志对上） */
    {
        audio_info_t info = {};
        if (audio_service_get_info(&info) && info.sample_rate) {
            printf("playing_info: %d Hz %uch %u bit, %u ms\n", info.sample_rate,
                   (unsigned)info.channels, (unsigned)info.bits,
                   (unsigned)info.duration_ms);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- songscan */

static int scan_walk(const char *dir, int depth, int *count)
{
    if (depth > 6) {
        return 0;                     /* 防目录环 */
    }
    DIR *d = opendir(dir);
    if (!d) {
        printf("# 打不开目录: %s\n", dir);
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        /* ⚠ 每层递归都要一个路径缓冲：递归上限 6 层，缓冲取 512 字节
         *   （本地路径上限 512，见 storage_service）→ 最深 3 KB，配上 REPL 的
         *   8 KB 栈与探测函数的用量是安全的。别随手把这行放大。 */
        char full[512];
        const int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) {
            continue;                 /* 路径过长：跳过，别截断后去读错文件 */
        }

        if (de->d_type == DT_DIR) {
            scan_walk(full, depth + 1, count);
            continue;
        }
        char ext[16];
        path_ext_lower(full, ext, sizeof(ext));
        if (strcmp(ext, "mp3") && strcmp(ext, "flac") && strcmp(ext, "wav") &&
            strcmp(ext, "aac") && strcmp(ext, "m4a") && strcmp(ext, "mp4") &&
            strcmp(ext, "ogg") && strcmp(ext, "opus")) {
            continue;
        }

        audio_probe_t p = {};
        const esp_err_t r = media_meta_probe_audio(full, &p);
        const uint64_t  sz = file_size_of(full);
        /* TSV：容器 采样率 位深 声道 时长ms 码率bps 大小B 路径 */
        printf("%s\t%u\t%u\t%u\t%u\t%u\t%llu\t%s\n",
               r == ESP_OK ? ext : "probe_fail",
               (unsigned)p.sample_rate, (unsigned)p.bits, (unsigned)p.channels,
               (unsigned)p.duration_ms, (unsigned)p.bitrate,
               (unsigned long long)sz, full);
        (*count)++;
    }
    closedir(d);
    return 0;
}

static int cmd_songscan(int argc, char **argv)
{
    const char *dir = (argc >= 2) ? argv[1] : "/sdcard";
    printf("# songscan: 容器\\t采样率\\t位深\\t声道\\t时长ms\\t码率bps\\t大小B\\t路径\n");
    int count = 0;
    scan_walk(dir, 0, &count);
    printf("# 共 %d 个音频文件\n", count);
    return 0;
}

/* ---------------------------------------------------------------- playstat */

static int cmd_playstat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    audio_info_t info = {};
    const bool info_ok = audio_service_get_info(&info);
    uint32_t rate = 0;
    uint8_t  ch = 0, bits = 0;
    const bool src_ok = audio_service_source_format(&rate, &ch, &bits);

    printf("playing: %s\n", audio_service_is_playing() ? "yes" : "no");
    printf("output_active: %s   (PCM 到过 codec；缓冲结束的判据)\n",
           audio_service_output_active() ? "yes" : "no");
    printf("position_ms: %u\n", (unsigned)audio_service_position_ms());
    printf("source_rate: %u\n", (unsigned)rate);
    printf("source_channels: %u\n", (unsigned)ch);
    printf("source_bits: %u\n", (unsigned)bits);
    printf("source_ok: %s\n", src_ok ? "yes" : "no");
    printf("bitrate_bps: %u   (本地=探测值；在线=总字节÷时长+实测纠偏)\n",
           (unsigned)audio_service_bitrate());
    printf("seek_supported: %s\n", audio_service_seek_supported() ? "yes" : "no");
    if (info_ok) {
        printf("info: %d Hz %uch %u bit, %u ms, %d bps\n", info.sample_rate,
               (unsigned)info.channels, (unsigned)info.bits,
               (unsigned)info.duration_ms, info.bitrate);
    } else {
        printf("info: (空 —— 在线流没走文件探测，属正常)\n");
    }
    int pct = -1;
    if (audio_service_stream_progress(&pct)) {
        printf("stream_progress_pct: %d\n", pct);
    }
    int kbps = 0;
    if (audio_service_stream_speed(&kbps)) {
        printf("stream_speed_kbps: %d\n", kbps);
    }
    uint32_t land = 0;
    if (audio_service_seek_landing_ms(&land)) {
        printf("seek_landing_ms: %u\n", (unsigned)land);
    }
    return 0;
}

/* ---------------------------------------------------------------- 注册与启动 */

esp_err_t console_cmds_init(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "songinfo", .help = "打印单个歌曲文件的详细信息（给 AI 分析用）",
          .hint = "<绝对路径>", .func = &cmd_songinfo },
        { .command = "songscan", .help = "递归扫描目录并逐行输出音频文件参数（TSV）",
          .hint = "[目录，默认 /sdcard]", .func = &cmd_songscan },
        { .command = "playstat", .help = "打印当前播放状态快照（码率/位置/流进度）",
          .hint = NULL, .func = &cmd_playstat },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register '%s' failed: %s", cmds[i].command, esp_err_to_name(err));
            return err;
        }
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "music> ";
    cfg.max_cmdline_length = 512;         /* 路径很长（本地上限 512） */
    cfg.task_stack_size = 8 * 1024;       /* songscan 递归 6 层 × 512 B 路径 + 探测/打印 */

    esp_err_t err = esp_console_new_repl_stdio(&cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console repl create failed: %s（命令仍可用 esp_console_run 调）",
                 esp_err_to_name(err));
        return err;
    }
    (void)esp_console_register_help_command();
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console repl start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "串口命令就绪：songinfo <路径> / songscan [目录] / playstat（help 看全部）");
    return ESP_OK;
}
