/*
 * bt_service.cc —— 蓝牙音频（ESP-GMF esp_bt_audio / 经典蓝牙 A2DP Source）
 *
 * 分工：
 *   controller            ← 本文件（esp_bt_controller_init/enable CLASSIC_BT）
 *   Bluedroid + GAP + A2DP/AVRCP ← esp_bt_audio（host_config 传进去）
 *   SBC 编码              ← 本文件（esp_audio_codec 的编码器，见 feed_task 上方说明）
 *   PCM 打包与发送        ← esp_bt_audio 的 A2DP 源（打包 + RTP 时间戳 + 发送）
 *
 * 音频通路（本文件实现）：
 *   音频管线 out 回调 → bt_service_write_pcm() → StreamBuffer（PSRAM）
 *   → feed 任务 → 攒满一帧 PCM → SBC 编码 → acquire_write/release_write → A2DP
 *
 * 为什么要中间缓冲 + 独立任务：acquire_write/release_write 会等 A2DP 的发送窗口
 * （几十毫秒级），而 out 回调跑在解码任务里，在那里等会把解码拖停。
 * 缓冲满时**丢帧**（蓝牙侧宁可少一点数据，也不能让本地播放卡住）。
 */
#include "bt_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_stream.h"

/* SBC 编码器（esp_audio_codec）：A2DP 源要的是**编码后的帧**，不是 PCM，
 * 见下面 feed_task 上方那段说明 */
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_types.h"

/* 采样率转换（esp_asrc）：本地管线固定 48 kHz，而对端（耳机）多数只收 44.1 kHz，
 * 不重采样就会变调、而且每秒钟多出 8.8% 的帧喂不完（队列满 → 大把丢帧） */
#include "esp_asrc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "bt_svc";

#define DEV_NAME        "KORVO-1 音乐播放器"   /* 对端看到的蓝牙名 */
#define PIN_CODE        "0000"
#define PCM_BUF_BYTES   (64 * 1024)            /* 环形缓冲 ≈ 340 ms @48k/16b/2ch */
/* StreamBuffer 的"触发水位"：只影响 xStreamBufferSend 的返回值语义，
 * 真正的攒帧由 feed_task 按编码器要求（一个 SBC 帧的 PCM 字节数）来做 */
#define PCM_BUF_TRIGGER (512)
#define FEED_TASK_STACK (5 * 1024)
/* 统计日志间隔（编码帧数）：512 B PCM/帧 ≈ 2.7 ms @48k 立体声 → 1000 帧 ≈ 2.7 s */
#define FEED_LOG_EVERY_FRAMES (1000)

/* 总开关 + "上次连过的设备"（需求：记忆开关状态、开机自动连已保存设备） */
#define NVS_NS          "bt_cfg"
#define NVS_KEY_ON      "on"
#define NVS_KEY_LAST    "last"

/*
 * 设备名缓存（MAC → 名字）。
 *
 * 为什么要持久化：名字**只能**从 ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED 里拿到 ——
 * esp_bt_audio_event_connection_st_t 根本没有名字字段，组件也没给
 * read_remote_name 之类的接口（见 esp_bt_audio_classic.h）。于是
 * "开机自动连上次那台设备"这条路**永远**拿不到名字，s_peer_name 只能退回 MAC，
 * 用户看到的就是"设备名变成 MAC 地址"。把扫描时拿到的名字按 MAC 存起来，
 * 连接/重启后再按 MAC 反查即可。
 */
#define NVS_NS_NAME      "bt_name"
/*
 * NVS 键名上限 15 个字符（NVS_KEY_NAME_MAX_SIZE = 16，含结尾 0），
 * 而 "AA:BB:CC:DD:EE:FF" 有 17 个字符 —— 拿它当键会直接
 * ESP_ERR_NVS_KEY_TOO_LONG。所以键用去掉冒号的 12 位大写十六进制（如 "A1B2C3D4E5F6"）。
 */
#define BT_NAME_KEY_LEN  13
/* 名字缓存条数上限：满了淘汰最早写进去的那条，别让 NVS 被无限撑大 */
#define BT_NAME_KEEP_MAX 16
/* 开机自动连延时：控制器 + 协议栈起来后还要让 A2DP 侧稳定一下 */
#define AUTO_CONNECT_MS (3000)

/* 本地音频管线送进来的 PCM 格式（audio_service 侧固定，见 bt_service.h） */
#define A2DP_PCM_RATE   (48000)
#define A2DP_PCM_CH     (2)
#define A2DP_PCM_BITS   (16)
/* 一次喂给 ASRC 的采样帧数 = 一个 SBC 帧的时长（128 采样 @48k ≈ 2.7 ms） */
#define ASRC_IN_FRAMES  (128)
/* 重采样后的暂存区：够放"上一次的余量 + 一次 ASRC 的输出"就行 */
#define STAGE_BYTES     (2048)
/* 入队等待上限：队列满就短暂等一下（吸收抖动），但绝不长时间阻塞 ——
 * 真正的节拍已经由 bt_service_write_pcm() 的阻塞发送交给解码管线了 */
#define TX_WAIT_MS      (50)

static volatile bt_state_t s_state = BT_ST_UNINIT;
static volatile bool       s_inited;
static volatile bool       s_discovering;
static volatile bool       s_connected;
static volatile bool       s_streaming;
/* 总开关（NVS 记忆；关掉时控制器 + 协议栈一起 deinit 释放资源） */
static volatile bool       s_enabled = true;
static bool                s_settings_loaded;  /* 是否已经从 NVS 读过开关 */
static uint8_t             s_last_bda[6];      /* 上次连上的耳机/音箱 */
static bool                s_has_last;
static esp_timer_handle_t  s_auto_timer;       /* 开机自动连的一次性定时器 */

static bt_dev_t            s_dev[BT_DEV_MAX];
static volatile size_t     s_dev_count;
static bt_dev_t            s_bonded[BT_DEV_MAX];
static volatile size_t     s_bonded_count;

static char                s_peer_name[BT_NAME_MAX];
static char                s_peer_addr[18];
static uint8_t             s_peer_bda[6];

static esp_bt_audio_stream_handle_t s_stream;
static StreamBufferHandle_t         s_pcm;
static TaskHandle_t                 s_feed_task;
static volatile int                 s_rate;
static volatile int                 s_channels;
static volatile uint32_t            s_dropped;

/* ---- SBC 编码 + 重采样（只有 feed 任务碰，不需要锁） ---- */
static esp_audio_enc_handle_t s_enc;          /* 编码器句柄，随流起停开关 */
static int                    s_enc_in_bytes;  /* 一个 SBC 帧对应的 PCM 字节数 */
static int                    s_enc_out_bytes; /* 编码后一帧的建议缓冲大小 */
static uint8_t               *s_enc_frame;     /* 编码输出缓冲 */
static bool                   s_enc_registered;/* esp_audio_enc_register_default 只调一次 */
static uint32_t               s_tx_frames;     /* 已发出的编码帧数（统计） */
static uint32_t               s_tx_bytes;      /* 已发出的编码字节数（统计） */

static esp_asrc_handle_t      s_asrc;          /* 非 NULL = 需要 48k → 协商采样率 */
static uint16_t               s_asrc_in_smp;   /* ASRC 一个"采样帧"的输入字节数 */
static uint16_t               s_asrc_out_smp;  /* ASRC 一个"采样帧"的输出字节数 */
static uint32_t               s_asrc_in_bytes; /* 每次喂给 ASRC 的字节数 */
static uint8_t               *s_asrc_in;
static uint8_t               *s_asrc_out;
static uint32_t               s_asrc_out_cap_samples;
static uint8_t               *s_stage;         /* 重采样后的 PCM 暂存（凑够一帧再编码） */
static uint32_t               s_stage_len;
static uint32_t               s_asrc_err_logged;/* ASRC 失败计数（限频打日志用） */
static uint32_t               s_asrc_probe;     /* 前几次 ASRC 转换的"进/出"探测日志 */
static int64_t                s_tx_t0;          /* 吞吐统计窗口起点（us） */
static uint32_t               s_last_drop_log_ms;/* 环形缓冲满的上次告警时刻 */

static bt_media_ctrl_cb_t  s_ctrl_cb;
static void               *s_ctrl_user;

void bt_service_addr_str(const uint8_t bda[6], char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return;
    }
    snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool same_bda(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bt_dev_t *dev_find(const uint8_t *bda, bool create)
{
    for (size_t i = 0; i < s_dev_count; i++) {
        if (same_bda(s_dev[i].bda, bda)) {
            return &s_dev[i];
        }
    }
    if (!create || s_dev_count >= BT_DEV_MAX) {
        return NULL;
    }
    bt_dev_t *d = &s_dev[s_dev_count];
    memset(d, 0, sizeof(*d));
    memcpy(d->bda, bda, 6);
    /* volatile 上不能用 ++（C++20 deprecated，IDF 又是 -Werror） */
    s_dev_count = s_dev_count + 1;
    return d;
}

/* ------------------------------------------------------------ 设备名缓存（NVS） */

/* MAC → NVS 键：去掉冒号的 12 位大写十六进制（原因见 BT_NAME_KEY_LEN 的注释） */
static void bt_name_key(const uint8_t bda[6], char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = hex[(bda[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[bda[i] & 0x0F];
    }
    out[12] = '\0';
}

/*
 * 这个"名字"是不是真名字。
 *
 * 扫描表里的 name 拿不到真名时会被填成 MAC 串（DEVICE_DISCOVERED 里的兜底），
 * 那不是名字：既不该存进 NVS（会把真名覆盖掉），也不该当作已解析出的名字。
 */
static bool bt_name_is_real(const uint8_t bda[6], const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    char addr[18];
    bt_service_addr_str(bda, addr, sizeof(addr));
    return strcmp(name, addr) != 0;
}

/*
 * 枚举 bt_name 里的字符串键：返回实际拷进 keys 的条数（最多 cap 个），
 * *more 为真表示还有没装下的。
 *
 * ⚠ 迭代器不保证看得到同一次事务里**尚未 commit** 的写入，所以"腾地方"必须在
 * nvs_set_str **之前**做（见 bt_name_save 的顺序）。
 */
static size_t bt_name_enum(nvs_handle_t h, char keys[][BT_NAME_KEY_LEN],
                           size_t cap, bool *more)
{
    if (more) {
        *more = false;
    }
    nvs_iterator_t it = NULL;
    if (nvs_entry_find_in_handle(h, NVS_TYPE_STR, &it) != ESP_OK || !it) {
        nvs_release_iterator(it);          /* 允许传 NULL */
        return 0;
    }
    size_t n = 0;
    nvs_entry_info_t info;
    while (nvs_entry_info(it, &info) == ESP_OK) {
        if (n < cap) {
            /*
             * ⚠ 必须显式给出精度：`info.key` 是 `char[NVS_KEY_NAME_MAX_SIZE]`（16 字节，
             *   NVS 键名上限 15 字符），而 keys[] 的每个元素只有 BT_NAME_KEY_LEN(13)。
             *   直接 "%s" 会被 GCC 证明"最多写 16 字节进 13 字节"，在 -Werror 下直接
             *   编译失败（真机踩过）。本命名空间里的键**只有我们自己写的 12 位十六进制**，
             *   按 12 字节截断既安全又能让编译器证得出来。
             */
            snprintf(keys[n], BT_NAME_KEY_LEN, "%.*s", BT_NAME_KEY_LEN - 1, info.key);
            n++;
        } else if (more) {
            *more = true;
        }
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
    }
    nvs_release_iterator(it);
    return n;
}

/* 为即将写入的 keep_key 腾位置：满了就删最早写进去的那条（FIFO 淘汰） */
static void bt_name_make_room(nvs_handle_t h, const char *keep_key)
{
    for (int round = 0; round < 8; round++) {
        char keys[BT_NAME_KEEP_MAX + 1][BT_NAME_KEY_LEN];
        bool more = false;
        const size_t n = bt_name_enum(h, keys, BT_NAME_KEEP_MAX + 1, &more);

        bool have = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(keys[i], keep_key) == 0) {
                have = true;
                break;
            }
        }
        if (have) {
            return;                        /* 覆盖写：不占新条目，不用腾 */
        }
        if (n < BT_NAME_KEEP_MAX && !more) {
            return;                        /* 还有空位 */
        }
        /*
         * NVS 按条目索引顺序枚举（实测先写的先出来），所以第一个非 keep_key
         * 就是最旧的一条 —— 淘汰它。
         */
        size_t victim = n;                 /* n = 没找到可删的 */
        for (size_t i = 0; i < n; i++) {
            if (strcmp(keys[i], keep_key) != 0) {
                victim = i;
                break;
            }
        }
        if (victim == n || nvs_erase_key(h, keys[victim]) != ESP_OK) {
            return;                        /* 删不动就别在这儿死循环 */
        }
        ESP_LOGI(TAG, "bt name cache full: dropped '%s'", keys[victim]);
    }
}

/* 记住一台设备的名字（传进来的不是真名字时什么都不做） */
static void bt_name_save(const uint8_t bda[6], const char *name)
{
    if (!bt_name_is_real(bda, name)) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NAME, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[BT_NAME_KEY_LEN];
    bt_name_key(bda, key);

    char cur[BT_NAME_MAX];
    size_t len = sizeof(cur);
    if (nvs_get_str(h, key, cur, &len) == ESP_OK && strcmp(cur, name) == 0) {
        nvs_close(h);                      /* 名字没变：别反复擦写 flash */
        return;
    }
    bt_name_make_room(h, key);
    if (nvs_set_str(h, key, name) == ESP_OK && nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "bt name cached: %s = %s", key, name);
    }
    nvs_close(h);
}

/* 反查：MAC → 名字。命中返回 true（out 里是名字），否则 out 为空串 */
static bool bt_name_load(const uint8_t bda[6], char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NAME, NVS_READONLY, &h) != ESP_OK) {
        return false;                      /* 还没缓存过任何名字 */
    }
    char key[BT_NAME_KEY_LEN];
    bt_name_key(bda, key);
    size_t len = cap;
    const bool ok = (nvs_get_str(h, key, out, &len) == ESP_OK) && out[0];
    nvs_close(h);
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

/*
 * 对端显示名 + 地址。
 *
 * 三级回退：事件里带来的真名 → NVS 里缓存过的名字 → 地址。
 * 中间那一级就是"名称变成 MAC"的修法：连接事件里从来没有名字，只有扫描才有。
 */
static void make_addr_18(const uint8_t *bda, const char *name)
{
    bt_service_addr_str(bda, s_peer_addr, sizeof(s_peer_addr));
    if (bt_name_is_real(bda, name)) {
        snprintf(s_peer_name, sizeof(s_peer_name), "%s", name);
        return;
    }
    if (bt_name_load(bda, s_peer_name, sizeof(s_peer_name))) {
        return;
    }
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", s_peer_addr);
}

/* ---------------------------------------------------------------- 开关/记忆 */

static void bt_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                      /* 没存过 = 默认开、没有上次设备 */
    }
    uint8_t on = 1;
    s_enabled = (nvs_get_u8(h, NVS_KEY_ON, &on) == ESP_OK) ? (on != 0) : true;
    size_t len = sizeof(s_last_bda);
    s_has_last = (nvs_get_blob(h, NVS_KEY_LAST, s_last_bda, &len) == ESP_OK && len == 6);
    nvs_close(h);
}

static void bt_settings_save_enabled(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_ON, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void bt_settings_save_last(const uint8_t bda[6])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_KEY_LAST, bda, 6);
    nvs_commit(h);
    nvs_close(h);
    memcpy(s_last_bda, bda, 6);
    s_has_last = true;
}

/* 开机自动连上次的设备（延时到协议栈稳定后跑，esp_timer 任务上下文） */
static void bt_auto_connect_cb(void *arg)
{
    (void)arg;
    if (!s_enabled || s_connected || !s_has_last) {
        return;
    }
    ESP_LOGI(TAG, "auto connect to saved device");
    bt_service_connect(s_last_bda);
}

/* ---------------------------------------------------------------- PCM → A2DP */

/*
 * ⚠ A2DP 源要的是**编码后的帧**，不是 PCM —— 这是"推给蓝牙没声音"的根因。
 *
 * esp_bt_audio 的源头音流通路只是把调用方写进来的字节原样打包发出去
 * （bt_audio_classic_stream.c: release_write → xQueueSend(data_q)，
 *  bt_audio_a2dp_src.c: 从 data_q 收包 → esp_a2d_source_audio_data_send()），
 * **组件内部不做任何编码**。它按"一个包 = 一个编码帧"来算批次：
 *     max_batch_size = (MTU - 头) / sbc_frame_size
 *     data_size      = Σ 每个包的 data_len      // 超过 MTU 直接整批丢掉
 *     rtp_ts        += samples_per_frame × 包数
 * 我们要做的正是官方示例里 pipeline 的 `aud_enc` 那一级
 * （examples/bt_audio/main/stream_proc.c: esp_gmf_audio_enc_reconfig(...)
 *  用的就是 esp_bt_audio_stream_get_codec_info() 给的 codec_cfg），
 * 也就是：PCM → SBC 编码 → 一包一帧 → acquire_write/release_write。
 *
 * 如果直接把 2 KB 的 PCM 当一个包写进去，不但内容不是 SBC 帧（耳机解出来是噪音），
 * 每批的 data_size 还会超过 MTU 被整批丢弃 —— 现象就是**完全没声音**。
 *
 * codec_cfg 是组件按**协商结果**生成好的 esp_sbc_enc_config_t（采样率/声道/
 * block/subband/bitpool 都是对端定的），所以这里不能自己拼参数，
 * 必须拿 codec_cfg 去开编码器，否则耳机解不了。
 */

static void bt_enc_close(void)
{
    if (s_enc) {
        esp_audio_enc_close(s_enc);
        s_enc = NULL;
    }
    if (s_asrc) {
        esp_asrc_close(s_asrc);
        s_asrc = NULL;
    }
    free(s_asrc_in);    s_asrc_in    = NULL;
    free(s_asrc_out);   s_asrc_out   = NULL;
    free(s_enc_frame);  s_enc_frame  = NULL;
    free(s_stage);      s_stage      = NULL;
    s_stage_len         = 0;
    s_enc_in_bytes      = 0;
    s_enc_out_bytes     = 0;
    s_asrc_in_bytes     = 0;
    s_asrc_out_cap_samples = 0;
}

/*
 * 打开 48 kHz → dest_rate 的重采样器。
 *
 * ⚠ 这里刻意用**软件** ASRC，不用硬件（ESP_ASRC_PERF_TYPE_AUTO/HW_ONLY）：
 *   S31 的硬件 ASRC 走 GDMA，输入/输出缓冲要满足 cache-line 对齐与 PSRAM 一致性，
 *   组件把这类缓冲的分配交给调用方（asrc_mem.c 用 MALLOC_CAP_SPIRAM 问对齐），
 *   一旦缓冲的归属/同步没对齐，CPU 读到的就是没写完/过期的重采样数据 ——
 *   现场表现正是"先慢放、几秒后全是噪音"。
 *   48k→44.1k 立体声的软件重采样在 320 MHz 上有的是余量（每秒不到 200 KB），
 *   而且完全绕开 DMA/cache，先用可靠的软件路径；要上硬件 ASRC 就应该整体
 *   走官方 GMF 管线（io_file → aud_dec → aud_rate_cvt → aud_enc → io_bt），
 *   让 GMF 管缓冲，而不是在这里手工拼。
 */
static bool bt_asrc_open(uint32_t dest_rate)
{
    esp_asrc_cfg_t cfg = {};
    cfg.src_info.sample_rate     = A2DP_PCM_RATE;
    cfg.src_info.channel         = A2DP_PCM_CH;
    cfg.src_info.bits_per_sample = A2DP_PCM_BITS;
    cfg.dest_info                = cfg.src_info;
    cfg.dest_info.sample_rate    = dest_rate;
    cfg.perf_type                = ESP_ASRC_PERF_TYPE_SW_SPEED;
    cfg.complexity               = 3;
    cfg.timeout_ms               = 1000;

    if (esp_asrc_open(&cfg, &s_asrc) != ESP_ASRC_ERR_OK || !s_asrc) {
        s_asrc = NULL;
        return false;
    }
    if (esp_asrc_get_bytes_per_sample(s_asrc, &s_asrc_in_smp, &s_asrc_out_smp) != ESP_ASRC_ERR_OK ||
        s_asrc_in_smp == 0 || s_asrc_out_smp == 0) {
        bt_enc_close();
        return false;
    }
    s_asrc_in_bytes = (uint32_t)ASRC_IN_FRAMES * s_asrc_in_smp;

    uint32_t out_samples = 0;
    if (esp_asrc_get_out_sample_num(s_asrc, ASRC_IN_FRAMES, &out_samples) != ESP_ASRC_ERR_OK ||
        out_samples == 0) {
        bt_enc_close();
        return false;
    }
    s_asrc_out_cap_samples = out_samples + 32;      /* 余量：非整数比时输出数会浮动 */

    /* 软件路径不要求 cache 对齐，但放内部 RAM 更稳（热路径、无 PSRAM 延迟） */
    const size_t in_sz  = s_asrc_in_bytes;
    const size_t out_sz = s_asrc_out_cap_samples * s_asrc_out_smp;
    s_asrc_in  = (uint8_t *)heap_caps_aligned_alloc(64, in_sz,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_asrc_out = (uint8_t *)heap_caps_aligned_alloc(64, out_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_asrc_in) {
        s_asrc_in = (uint8_t *)malloc(in_sz);
    }
    if (!s_asrc_out) {
        s_asrc_out = (uint8_t *)malloc(out_sz);
    }
    if (!s_asrc_in || !s_asrc_out) {
        ESP_LOGE(TAG, "asrc buffer alloc failed");
        bt_enc_close();
        return false;
    }
    ESP_LOGI(TAG, "asrc ready (sw): %d Hz -> %u Hz, %u frames/call, out cap %u frames",
             A2DP_PCM_RATE, (unsigned)dest_rate, (unsigned)ASRC_IN_FRAMES,
             (unsigned)s_asrc_out_cap_samples);
    return true;
}

/* 按协商到的 codec_cfg 打开编码器；成功返回 true */
static bool bt_enc_open(esp_bt_audio_stream_handle_t h)
{
    esp_bt_audio_stream_codec_info_t info = {};
    if (esp_bt_audio_stream_get_codec_info(h, &info) != ESP_OK || !info.codec_cfg) {
        ESP_LOGW(TAG, "no codec info for stream, retry later");
        return false;
    }
    if (info.codec_type != ESP_BT_AUDIO_STREAM_CODEC_SBC) {
        /* 本工程只注册了 SBC（CONFIG_BT_A2DP_CODEC_AAC_ENABLED 未开），
         * 真协商到别的编解码器就只能放弃了，别硬编出错音 */
        ESP_LOGE(TAG, "unsupported A2DP codec type %d (only SBC)", (int)info.codec_type);
        return false;
    }

    if (!s_enc_registered) {
        esp_audio_err_t r = esp_audio_enc_register_default();
        if (r != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "register default encoders failed: %d", (int)r);
            return false;
        }
        s_enc_registered = true;
    }

    esp_audio_enc_config_t cfg = {};
    cfg.type   = ESP_AUDIO_TYPE_SBC;
    cfg.cfg    = info.codec_cfg;      /* 组件按协商结果生成的 esp_sbc_enc_config_t */
    cfg.cfg_sz = info.cfg_size;

    esp_audio_err_t err = esp_audio_enc_open(&cfg, &s_enc);
    if (err != ESP_AUDIO_ERR_OK || !s_enc) {
        ESP_LOGE(TAG, "sbc enc open failed: %d", (int)err);
        s_enc = NULL;
        return false;
    }
    if (esp_audio_enc_get_frame_size(s_enc, &s_enc_in_bytes, &s_enc_out_bytes) != ESP_AUDIO_ERR_OK ||
        s_enc_in_bytes <= 0 || s_enc_out_bytes <= 0) {
        ESP_LOGE(TAG, "sbc enc frame size query failed");
        bt_enc_close();
        return false;
    }

    s_enc_frame = (uint8_t *)malloc((size_t)s_enc_out_bytes);
    s_stage     = (uint8_t *)malloc(STAGE_BYTES);
    if (!s_enc_frame || !s_stage) {
        ESP_LOGE(TAG, "sbc enc buffer alloc failed");
        bt_enc_close();
        return false;
    }
    s_stage_len = 0;

    /* 协商参数以 codec_info 为准（channels 是声道**位掩码**，要 popcount 才是声道数） */
    s_rate     = (int)info.sample_rate;
    s_channels = __builtin_popcount((unsigned)info.channels);

    ESP_LOGI(TAG, "sbc enc ready: %d Hz, %d ch, PCM/frame %d B, SBC/frame %d B",
             (int)s_rate, (int)s_channels, s_enc_in_bytes, s_enc_out_bytes);

    /*
     * 本地管线固定 48 kHz。对端采样率不是 48k 时必须重采样：
     *   1) 不重采样 → 变调（44.1k 会被当 44.1k 播放，音调高 8.8%）；
     *   2) 而且按 48k 攒出来的帧数比链路能吃下的多 8.8%，
     *      队列很快满 → 每帧都要等/丢，实测"发 1000 帧丢 200 KB"。
     * 重采样失败时只记警告：宁可变调也要出声。
     */
    if (s_rate != A2DP_PCM_RATE && !bt_asrc_open((uint32_t)s_rate)) {
        ESP_LOGW(TAG, "ASRC 打开失败，退回不重采样：会有约 %.1f%% 的音调偏差",
                 ((float)A2DP_PCM_RATE / (float)s_rate - 1.0f) * 100.0f);
    }
    return true;
}

/* 把重采样后的 PCM 追加到暂存区（凑够一个编码帧才编码） */
static void bt_stage_append(const uint8_t *data, uint32_t len)
{
    if (!data || len == 0 || !s_stage) {
        return;
    }
    if (s_stage_len + len > STAGE_BYTES) {
        s_dropped = s_dropped + len;      /* 理论上到不了：一次输出 < 一个编码帧 */
        return;
    }
    memcpy(s_stage + s_stage_len, data, len);
    s_stage_len += len;
}

/* 编码暂存区里的一帧并交给 A2DP 源（一包 = 一个编码帧） */
static void bt_send_one_frame(esp_bt_audio_stream_handle_t h)
{
    esp_audio_enc_in_frame_t in = {};
    in.buffer = s_stage;
    in.len    = (uint32_t)s_enc_in_bytes;
    esp_audio_enc_out_frame_t out = {};
    out.buffer = s_enc_frame;
    out.len    = (uint32_t)s_enc_out_bytes;

    if (esp_audio_enc_process(s_enc, &in, &out) != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        s_dropped = s_dropped + (uint32_t)s_enc_in_bytes;
    } else {
        /*
         * 发送池被占满时（NO_MEM）稍等重试；队列满（release 超时）就丢掉这一帧。
         * 都只等很短的时间 —— 在这里长时间等会把 48k→A2DP 的差速全部压到
         * 环形缓冲上，最终表现为大面积丢帧。
         */
        for (int retry = 0; retry < 20; retry++) {
            esp_bt_audio_stream_packet_t pkt = {};
            if (esp_bt_audio_stream_acquire_write(h, &pkt, out.encoded_bytes) != ESP_OK || !pkt.data) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            memcpy(pkt.data, s_enc_frame, out.encoded_bytes);
            pkt.size = out.encoded_bytes;
            if (esp_bt_audio_stream_release_write(h, &pkt, TX_WAIT_MS) != ESP_OK) {
                s_dropped = s_dropped + out.encoded_bytes;
            } else {
                s_tx_frames++;
                s_tx_bytes += out.encoded_bytes;
                if (s_tx_frames == 1) {
                    /* 第一帧：一线"通了"的最短判据（排查没声音时先看这一行） */
                    ESP_LOGI(TAG, "first a2dp frame sent (%u B)", (unsigned)out.encoded_bytes);
                }
                if ((s_tx_frames % FEED_LOG_EVERY_FRAMES) == 0) {
                    /* 带上时间窗口算实际帧率：A2DP 每秒必须发够 rate/128 帧，
                     * 少了就是"慢放"（对端欠载），多了会被丢（对端过载/噪音） */
                    const int64_t now = esp_timer_get_time();
                    const uint32_t per_frame = (uint32_t)(s_enc_in_bytes /
                                              (s_channels > 0 ? s_channels * 2 : 2));
                    const double secs = (s_tx_t0 && now > s_tx_t0) ? (double)(now - s_tx_t0) / 1e6 : 0.0;
                    if (secs > 0.0) {
                        ESP_LOGI(TAG, "a2dp tx: %u frames, %.0f f/s (need %.0f f/s), dropped %u B",
                                 (unsigned)s_tx_frames,
                                 (double)FEED_LOG_EVERY_FRAMES / secs,
                                 (double)s_rate / (double)(per_frame ? per_frame : 1),
                                 (unsigned)s_dropped);
                    } else {
                        ESP_LOGI(TAG, "a2dp tx: %u frames, dropped %u B",
                                 (unsigned)s_tx_frames, (unsigned)s_dropped);
                    }
                    s_tx_t0 = now;
                }
            }
            break;
        }
    }

    /* 用掉一个编码帧的输入，把余量搬到前面 */
    if (s_stage_len > (uint32_t)s_enc_in_bytes) {
        const uint32_t rest = s_stage_len - (uint32_t)s_enc_in_bytes;
        memmove(s_stage, s_stage + s_enc_in_bytes, rest);
        s_stage_len = rest;
    } else {
        s_stage_len = 0;
    }
}

static void feed_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_bt_audio_stream_handle_t h = s_stream;
        if (!h || !s_streaming || !s_pcm) {
            /* 流没起来 / 已停：把编码器/重采样器收掉，等下次 STARTED 重建 */
            if (s_enc) {
                bt_enc_close();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_enc && !bt_enc_open(h)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 暂存里够一个编码帧就先编码发送 */
        if (s_stage_len >= (uint32_t)s_enc_in_bytes) {
            bt_send_one_frame(h);
            continue;
        }

        /* 不需要重采样（对端就是 48 kHz）：直接按编码帧长度取 PCM */
        if (!s_asrc) {
            if (xStreamBufferBytesAvailable(s_pcm) < (size_t)s_enc_in_bytes) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (xStreamBufferReceive(s_pcm, s_stage + s_stage_len,
                                     (size_t)s_enc_in_bytes, 0) != (size_t)s_enc_in_bytes) {
                continue;
            }
            s_stage_len += (uint32_t)s_enc_in_bytes;
            continue;
        }

        /* 需要重采样：先取一段 48 kHz PCM，转成对端采样率再进暂存区 */
        if (xStreamBufferBytesAvailable(s_pcm) < (size_t)s_asrc_in_bytes) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (xStreamBufferReceive(s_pcm, s_asrc_in, s_asrc_in_bytes, 0) != s_asrc_in_bytes) {
            continue;
        }
        uint32_t in_samples  = s_asrc_in_bytes / s_asrc_in_smp;
        uint32_t out_samples = s_asrc_out_cap_samples;
        if (esp_asrc_process(s_asrc, s_asrc_in, in_samples, s_asrc_out, &out_samples) !=
                ESP_ASRC_ERR_OK || out_samples == 0) {
            s_dropped = s_dropped + s_asrc_in_bytes;
            /* 别静默失败：重采样挂了等于又回到"没声音"，必须能看见 */
            if ((++s_asrc_err_logged % 200) == 1) {
                ESP_LOGW(TAG, "asrc process failed (%u in-samples, first at #%u)",
                         (unsigned)in_samples, (unsigned)s_asrc_err_logged);
            }
            continue;
        }
        /* 前几次把"进多少、出多少"打出来：48k→44.1k 应该是 128 进 ≈ 117~118 出。
         * 这个比例不对（比如出=进，或者出=0）就是重采样没生效，声音必然不对。 */
        if (s_asrc_probe < 5) {
            s_asrc_probe++;
            ESP_LOGI(TAG, "asrc probe #%u: %u in -> %u out frames",
                     (unsigned)s_asrc_probe, (unsigned)in_samples, (unsigned)out_samples);
        }
        bt_stage_append(s_asrc_out, out_samples * s_asrc_out_smp);
    }
}

esp_err_t bt_service_write_pcm(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 这段 PCM 在 48k/2ch/16bit 下代表多少毫秒（用于没处可写时"按时间睡"） */
    const uint32_t dur_ms = (uint32_t)(((uint64_t)len * 1000ULL) /
                                       ((uint64_t)A2DP_PCM_RATE * A2DP_PCM_CH * (A2DP_PCM_BITS / 8)));

    if (!s_pcm || !s_streaming) {
        /*
         * 蓝牙流还没起来 / 已经停了：丢掉这段音频，但**按它的播放时长睡一下**。
         * 不睡的话 GMF 解码管线会全速空转（见下面流控的说明），
         * 等流一起来，环形缓冲里只剩"最新的那一小段"，声音必然是抽稀过的噪音。
         */
        vTaskDelay(pdMS_TO_TICKS(dur_ms > 0 ? dur_ms : 1));
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★ 蓝牙通路的流控就在这一行：**阻塞**等环形缓冲腾出位置。
     *
     * 扬声器通路的节拍由 esp_codec_dev_write() 提供 —— 它写 I2S/codec 时会被
     * DMA 缓冲挡住，GMF 解码任务自然按 48 kHz 的实时速度推进。
     * 蓝牙这边如果"满了就丢帧并立刻返回成功"，GMF 会以为写成功了，
     * 于是全速解码：实测 8 秒灌进 1.5 MB，而链路每秒只吃得下 176 KB ——
     * 我们只消费到环形缓冲里最新的一小段，等于把音乐抽稀成 1/20 再按 44.1 kHz
     * 发出去，对端听到的就是**噪音**（日志里的 "pcm ring full: dropped ..." 就是它）。
     * 阻塞式发送把节拍还给解码管线，缓冲就不会被灌爆。
     *
     * 上限 200 ms：万一 A2DP 侧真的卡死，也只丢这一段，不会把管线永久挂住。
     */
    const size_t sent = xStreamBufferSend(s_pcm, data, len, pdMS_TO_TICKS(200));
    if (sent != len) {
        s_dropped = s_dropped + (uint32_t)(len - sent);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_drop_log_ms > 2000) {
            s_last_drop_log_ms = now_ms;
            ESP_LOGW(TAG, "pcm ring full after 200ms: dropped %u B (total %u B)",
                     (unsigned)(len - sent), (unsigned)s_dropped);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t bt_service_dropped_bytes(void) { return s_dropped; }

/* ---------------------------------------------------------------- 事件 */

static void bt_audio_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    (void)user_data;
    switch (event) {
    case ESP_BT_AUDIO_EVENT_DISCOVERY_STATE_CHG: {
        const esp_bt_audio_event_discovery_st_t *st = (esp_bt_audio_event_discovery_st_t *)event_data;
        s_discovering = st->discovering;
        if (!s_discovering) {
            ESP_LOGI(TAG, "discovery done: %u device(s)", (unsigned)s_dev_count);
            if (s_state == BT_ST_DISCOVERING) {
                s_state = BT_ST_READY;
            }
        } else {
            s_state = BT_ST_DISCOVERING;
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED: {
        const esp_bt_audio_event_device_discovered_t *d =
            (esp_bt_audio_event_device_discovered_t *)event_data;
        bt_dev_t *e = dev_find(d->addr, true);
        if (e) {
            /*
             * ★ 名字"只增不减"（2026-10 修"扫描时设备名有时变成 MAC 地址"）：
             *   一次查询会对**同一台设备上报多包**，而名字（EIR / 异步
             *   read_remote_name 的结果）往往**不在第一包里**。
             *   原来是每包都无条件重写 e->name，后面只要再来一包不带名字的，
             *   就会把已经拿到的好名字**覆盖成 MAC**（看起来"名字一会儿有一会儿没有"）。
             *   现在：有名字才写；仍然没名字（从没见过）才退回地址显示。
             */
            if (d->name[0] && strncmp(e->name, d->name, sizeof(e->name)) != 0) {
                snprintf(e->name, sizeof(e->name), "%s", d->name);
                /* 名字**只有**扫描里拿得到：立刻缓存到 NVS，否则下次开机自动连
                 * 这台设备时（不扫描）界面上只会显示 MAC 地址。 */
                bt_name_save(d->addr, e->name);
            }
            e->rssi     = (int8_t)d->rssi;
            e->is_audio = (((d->disc_data.classic.cod & ESP_BT_COD_MAJOR_DEV_BIT_MASK)
                            >> ESP_BT_COD_MAJOR_DEV_BIT_OFFSET) == ESP_BT_COD_MAJOR_DEV_AV);
            for (size_t i = 0; i < s_bonded_count; i++) {
                if (same_bda(s_bonded[i].bda, d->addr)) {
                    e->bonded = true;
                    break;
                }
            }
            if (e->name[0] == '\0') {
                /* 这一包还没带名字：先用 NVS 里缓存过的（"名字只增不减"的最后一道保险） */
                bt_name_load(e->bda, e->name, sizeof(e->name));
            }
            if (e->name[0] == '\0') {
                /* 从没见过名字的设备：退回地址显示（等后面的包带来真名） */
                bt_service_addr_str(e->bda, e->name, sizeof(e->name));
            }
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        const esp_bt_audio_event_connection_st_t *st =
            (esp_bt_audio_event_connection_st_t *)event_data;
        if (st->tech != ESP_BT_AUDIO_TECH_CLASSIC) {
            break;
        }
        s_connected = st->connected;
        if (st->connected) {
            char addr[18];
            bt_service_addr_str(st->addr, addr, sizeof(addr));
            const bt_dev_t *d = dev_find(st->addr, false);
            make_addr_18(st->addr, d ? d->name : NULL);
            /* 名字可能来自扫描表或 NVS：顺手同步进缓存（是 MAC 兜底时函数内部会跳过） */
            bt_name_save(st->addr, s_peer_name);
            memcpy(s_peer_bda, st->addr, 6);
            s_state = BT_ST_CONNECTED;
            ESP_LOGI(TAG, "a2dp connected: %s (%s)", s_peer_name, addr);
            /* 记住这台设备：下次开机自动连它（需求） */
            bt_settings_save_last(st->addr);
            bt_service_refresh_bonded();
        } else {
            s_streaming = false;
            s_stream   = NULL;
            s_state    = BT_ST_READY;
            ESP_LOGW(TAG, "a2dp disconnected");
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        const esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;
        switch (st->state) {
        case ESP_BT_AUDIO_STREAM_STATE_STARTED: {
            s_stream    = st->stream_handle;
            s_streaming = true;
            s_state     = BT_ST_STREAMING;
            if (xStreamBufferReset(s_pcm) != pdPASS) {
                ESP_LOGW(TAG, "pcm buffer reset failed");
            }
            /* 协商到的实际码流参数：采样率跟音源不一致会有音高偏差，
             * 先打出来，方便对照；重采样留给下一步（esp_asrc）。
             * channels 是声道**位掩码**，popcount 才是声道数。 */
            esp_bt_audio_stream_codec_info_t info = {};
            if (esp_bt_audio_stream_get_codec_info(st->stream_handle, &info) == ESP_OK) {
                s_rate     = (int)info.sample_rate;
                s_channels = __builtin_popcount((unsigned)info.channels);
                ESP_LOGI(TAG, "a2dp stream started: %d Hz, %d ch, %u bit, PCM/frame %u B",
                         (int)info.sample_rate, (int)s_channels,
                         (unsigned)info.bits, (unsigned)info.frame_size);
            }
            ESP_LOGI(TAG, "streaming, PCM buffer %d bytes", (int)PCM_BUF_BYTES);
            break;
        }
        case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
        case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
            if (s_stream == st->stream_handle) {
                s_stream    = NULL;
                s_streaming = false;
                s_state     = s_connected ? BT_ST_CONNECTED : BT_ST_READY;
            }
            ESP_LOGI(TAG, "a2dp stream stopped");
            break;
        default:
            break;
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_MEDIA_CTRL_CMD: {
        const esp_bt_audio_event_media_ctrl_t *cmd = (esp_bt_audio_event_media_ctrl_t *)event_data;
        ESP_LOGI(TAG, "remote media cmd: %d", (int)cmd->cmd);
        if (s_ctrl_cb) {
            s_ctrl_cb((int)cmd->cmd, s_ctrl_user);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 对外接口 */

esp_err_t bt_service_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    /* 先读开关与"上次设备"：开关关着就整个不初始化（射频与内存都不占） */
    bt_settings_load();
    if (!s_enabled) {
        ESP_LOGI(TAG, "bt disabled by switch, skip init");
        s_state = BT_ST_UNINIT;
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * 编译期护栏：经典蓝牙（BR/EDR）要求**控制器就是 BR/EDR Only**。
     * 若 sdkconfig 里把它留成默认的 BLE_ONLY（CONFIG_BTDM_CTRL_MODE_BLE_ONLY），
     * 下面的 esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) 拿不到经典蓝牙，
     * 而 esp_bluedroid_init() 会一直等 BR/EDR 控制器的 HCI 应答 —— 现象就是
     * "蓝牙页点扫描没反应"。修法见 sdkconfig.defaults 的蓝牙段落。
     */
#if !CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY
    ESP_LOGE(TAG, "控制器不是 BR/EDR Only 模式（CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY 未开），"
                  "经典蓝牙/A2DP 无法工作");
#endif

    /* 1) 控制器：经典蓝牙（esp_bt_audio 的 host 不负责这一步） */
    esp_bt_controller_config_t ccfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "bt init 1/3: controller init");
    esp_err_t ret = esp_bt_controller_init(&ccfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(ret));
        s_state = BT_ST_FAILED;
        return ret;
    }
    ESP_LOGI(TAG, "bt init 2/3: controller enable (classic)");
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(ret));
        s_state = BT_ST_FAILED;
        return ret;
    }

    /* 2) PCM 缓冲 + 喂数据任务（先建好，流一起来就能用） */
    if (!s_pcm) {
        s_pcm = xStreamBufferCreateWithCaps(PCM_BUF_BYTES, PCM_BUF_TRIGGER, MALLOC_CAP_SPIRAM);
        if (!s_pcm) {
            ESP_LOGW(TAG, "PSRAM stream buffer failed, fall back to internal");
            s_pcm = xStreamBufferCreate(PCM_BUF_BYTES, PCM_BUF_TRIGGER);
        }
        if (!s_pcm) {
            s_state = BT_ST_FAILED;
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_feed_task) {
        xTaskCreate(feed_task, "bt_feed", FEED_TASK_STACK, NULL, 6, &s_feed_task);
    }

    /* 3) Bluedroid + A2DP Source（esp_bt_audio 一把做完） */
    esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
    snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "%s", DEV_NAME);
    host_cfg.pin_type = ESP_BT_PIN_TYPE_FIXED;
    snprintf((char *)host_cfg.pin_code, sizeof(host_cfg.pin_code), "%s", PIN_CODE);
    /* IO_CAP_NONE = Just Works：耳机/音箱常见的无码配对 */
    host_cfg.iocap = ESP_BT_IO_CAP_NONE;

    esp_bt_audio_config_t cfg = {};
    cfg.host_config   = &host_cfg;
    cfg.event_cb      = bt_audio_event_cb;
    cfg.event_user_ctx = NULL;
    cfg.classic.roles = ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC;   /* A2DP 源（推流给耳机） */
    /* 发送任务：钉 core 0（core 1 是 LVGL），优先级 5、栈 4 KB */
    cfg.classic.a2dp_src_send_task_core_id    = 0;
    cfg.classic.a2dp_src_send_task_prio       = 5;
    cfg.classic.a2dp_src_send_task_stack_size = 4096;

    ESP_LOGI(TAG, "bt init 3/3: bluedroid + A2DP source host");
    ret = esp_bt_audio_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_audio_init: %s", esp_err_to_name(ret));
        s_state = BT_ST_FAILED;
        return ret;
    }

    bt_service_refresh_bonded();
    s_state   = BT_ST_READY;
    s_inited  = true;      /* 只有真正成功才算初始化过，失败允许重试 */
    ESP_LOGI(TAG, "bt audio ready (A2DP source), %u bonded", (unsigned)s_bonded_count);

    /* 开机自动连上次用过的设备（需求）；没有记录就什么都不做 */
    if (s_has_last) {
        if (!s_auto_timer) {
            esp_timer_create_args_t args = {};
            args.callback = bt_auto_connect_cb;
            args.name     = "bt_auto_conn";
            if (esp_timer_create(&args, &s_auto_timer) != ESP_OK) {
                s_auto_timer = NULL;
            }
        }
        if (s_auto_timer) {
            esp_timer_stop(s_auto_timer);
            esp_timer_start_once(s_auto_timer, (uint64_t)AUTO_CONNECT_MS * 1000ULL);
        }
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- 总开关 */

bool bt_service_is_enabled(void)
{
    /* 首次查询时懒加载 NVS：UI 在服务尚未 init 时也要能显示正确的开关状态 */
    if (!s_settings_loaded) {
        bt_settings_load();
        s_settings_loaded = true;
    }
    return s_enabled;
}

esp_err_t bt_service_set_enabled(bool on)
{
    if (on == s_enabled) {
        return ESP_OK;
    }
    s_enabled = on;
    bt_settings_save_enabled(on);
    ESP_LOGI(TAG, "bt switch -> %s", on ? "on" : "off");
    if (on) {
        /*
         * 重新打开：先等一下再 init。
         * esp_bt_audio_deinit() 之后，BTC 任务还要跑完它的收尾（A2DP 侧的
         * g_a2dp_on_deinit 由 BTC 异步清），立刻重新注册 SBC SEP 会撞
         * ESP_ERR_INVALID_STATE 并 abort。300 ms 足够它跑完；配合"钉在 BTC 核"
         * 一起用（见 app_settings 里的 radio_toggle_task）。
         */
        vTaskDelay(pdMS_TO_TICKS(300));
        return bt_service_init();
    }
    const esp_err_t ret = bt_service_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));      /* 让控制器 deinit 收尾，别立刻又 init */
    return ret;
}

/*
 * 关掉蓝牙：拆掉数据通路（PCM 缓冲 + feed 任务 + 编码器）→
 * esp_bt_audio_deinit()（内含 Bluedroid disable/deinit 与 profile 清理）→
 * 控制器 disable/deinit。
 *
 * 注意**不要**调 esp_bt_mem_release()：那会把控制器的内存永久归还，
 * 之后再也 init 不回来，而需求是"开关能来回切"。
 */
esp_err_t bt_service_deinit(void)
{
    if (s_auto_timer) {
        esp_timer_stop(s_auto_timer);
    }
    if (!s_inited && !s_pcm && !s_feed_task) {
        s_state = BT_ST_UNINIT;
        return ESP_OK;
    }

    s_streaming = false;
    s_connected = false;
    s_stream    = NULL;
    s_state     = BT_ST_UNINIT;

    bt_enc_close();

    if (s_feed_task) {
        vTaskDelete(s_feed_task);
        s_feed_task = NULL;
    }
    if (s_pcm) {
        vStreamBufferDelete(s_pcm);
        s_pcm = NULL;
    }

    if (s_inited) {
        esp_bt_audio_deinit();          /* 协议栈 + profiles（内部会 disable/deinit Bluedroid） */
        s_inited = false;
    }
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "bt deinit (switch off)");
    return ESP_OK;
}

bool bt_service_is_ready(void)
{
    return s_state != BT_ST_UNINIT && s_state != BT_ST_FAILED;
}

bt_state_t bt_service_state(void) { return s_state; }
bool bt_service_is_discovering(void) { return s_discovering; }
bool bt_service_is_connected(void) { return s_connected; }
bool bt_service_is_streaming(void) { return s_streaming; }
int  bt_service_stream_rate(void) { return s_rate; }
int  bt_service_stream_channels(void) { return s_channels; }
const char *bt_service_peer_name(void) { return s_peer_name; }
const char *bt_service_peer_addr(void) { return s_peer_addr; }
const uint8_t *bt_service_peer_bda(void) { return s_peer_bda; }

const char *bt_service_state_str(void)
{
    switch (s_state) {
    case BT_ST_UNINIT:        return "初始化中…";
    case BT_ST_DISCOVERING:   return "搜索中…";
    case BT_ST_CONNECTED:     return "已连接";
    case BT_ST_STREAMING:     return "正在推流";
    case BT_ST_FAILED:        return "蓝牙不可用";
    default:                  return "就绪";
    }
}

esp_err_t bt_service_start_discovery(void)
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_discovering) {
        return ESP_OK;
    }
    s_dev_count = 0;                    /* 每次搜索重新列表 */
    esp_err_t ret = esp_bt_audio_classic_discovery_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "discovery start: %s", esp_err_to_name(ret));
        return ret;
    }
    s_discovering = true;
    s_state = BT_ST_DISCOVERING;
    return ESP_OK;
}

esp_err_t bt_service_stop_discovery(void)
{
    if (!s_discovering) {
        return ESP_OK;
    }
    return esp_bt_audio_classic_discovery_stop();
}

size_t bt_service_dev_count(void) { return s_dev_count; }

const bt_dev_t *bt_service_dev(size_t idx)
{
    return (idx < s_dev_count) ? &s_dev[idx] : NULL;
}

esp_err_t bt_service_connect(const uint8_t bda[6])
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_bd_addr_t addr;
    memcpy(addr, bda, 6);
    const bt_dev_t *d = dev_find(bda, false);
    make_addr_18(bda, d ? d->name : NULL);
    /* A2DP 源连接：没配对过的设备在这一步顺手完成配对（Just Works） */
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "a2dp connect: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "connecting a2dp to %s", s_peer_name);
    return ESP_OK;
}

esp_err_t bt_service_disconnect(const uint8_t bda[6])
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_bd_addr_t addr;
    memcpy(addr, bda, 6);
    return esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
}

esp_err_t bt_service_media_start(void)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_streaming) {
        return ESP_OK;
    }
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "media start: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bt_service_media_stop(void)
{
    if (!s_connected) {
        return ESP_OK;
    }
    return esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
}

esp_err_t bt_service_refresh_bonded(void)
{
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        s_bonded_count = 0;
        return ESP_OK;
    }
    if (num > BT_DEV_MAX) {
        num = BT_DEV_MAX;
    }
    esp_bd_addr_t list[BT_DEV_MAX];
    int n = num;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
        return ESP_FAIL;
    }
    if (n < 0) {
        n = 0;
    }
    s_bonded_count = (size_t)n;
    for (int i = 0; i < n; i++) {
        bt_dev_t *b = &s_bonded[i];
        memset(b, 0, sizeof(*b));
        memcpy(b->bda, list[i], 6);
        b->bonded = true;
        const bt_dev_t *d = dev_find(list[i], false);
        if (d && bt_name_is_real(list[i], d->name)) {
            snprintf(b->name, sizeof(b->name), "%s", d->name);
        }
        if (d) {
            b->is_audio = d->is_audio;
            b->rssi     = d->rssi;
        }
        if (b->name[0] == '\0') {
            /* 本次没扫到 / 只扫到 MAC：查 NVS 缓存（开机直接看"已配对"也有名字） */
            bt_name_load(b->bda, b->name, sizeof(b->name));
        }
        if (b->name[0] == '\0') {
            bt_service_addr_str(b->bda, b->name, sizeof(b->name));
        }
        bt_dev_t *e = dev_find(list[i], false);
        if (e) {
            e->bonded = true;
        }
    }
    return ESP_OK;
}

size_t bt_service_bonded_count(void) { return s_bonded_count; }

const bt_dev_t *bt_service_bonded(size_t idx)
{
    return (idx < s_bonded_count) ? &s_bonded[idx] : NULL;
}

esp_err_t bt_service_remove_bond(const uint8_t bda[6])
{
    if (!bt_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_bd_addr_t addr;
    memcpy(addr, bda, 6);
    esp_err_t ret = esp_bt_gap_remove_bond_device(addr);
    if (ret == ESP_OK) {
        bt_service_refresh_bonded();
    }
    return ret;
}

void bt_service_set_media_ctrl_cb(bt_media_ctrl_cb_t cb, void *user)
{
    s_ctrl_cb   = cb;
    s_ctrl_user = user;
}
