/*
 * music_spectrum.cc —— 音频可视化（esp-dsp FFT 频谱条）
 *
 * 数据通路（三条线程各管一段，谁也不等谁）：
 *
 *   输出任务                    LVGL 任务（40 ms lv_timer）
 *   write_out() ──PCM──▶ 环形缓冲 ──▶ Hann 窗 ──▶ esp-dsp FFT ──▶ 归一化
 *                          (SPSC)                  (N=1024)        │
 *                                                                  ▼
 *                                    26 根频谱柱 ◀── 线性映射 ◀── log 幅度
 *                                                   + 对数频率分带
 *
 * 为什么要喂"最终 PCM"（48k/16bit/2ch）而不是解码器出来的原始数据：
 * 一来输出级的音量/ASRC 都在 write_out() 之前完成，这里看到的就是用户听到的；
 * 二来不管本地文件是什么采样率，进到这里的永远是 48 kHz —— 频段映射只算一次。
 *
 * 用的是乐鑫官方 esp-dsp（`espressif/esp-dsp`，REQUIRES 里的 `espressif__esp-dsp`）：
 *   dsps_fft2r_init_fc32()  —— 建旋转因子表（只建一次）
 *   dsps_wind_hann_f32()    —— 汉宁窗系数
 *   dsps_fft2r_fc32()       —— 基 2 复数 FFT（S31 上走 PIE 汇编 arp4 实现）
 *   dsps_bit_rev_fc32()     —— 该 FFT 是 DIF，输出是位反序，必须重排
 *   dsps_sqrtf_f32()        —— 幅度 = sqrt(re²+im²)
 *
 * 处理链严格按需求：收 PCM → dsp FFT → 归一化 → log 幅度 → 对数频率缩放
 * → 线性映射成柱高（**不做 gamma 压平**）。
 */
#include "music_spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"      /* esp_ptr_external_ram()：日志里区分 PSRAM / 内部 RAM */
#include "nvs.h"

#include "dsps_fft2r.h"
#include "dsps_sqrt.h"
#include "dsps_wind_hann.h"

#include "ui_metrics.h"

#include "audio_service.h"
#include "music_theme.h"

static const char *TAG = "music_spec";

/* ---------------------------------------------------------------- 参数 */

/*
 * N 从 gmf_fft 时代的 512 提到 1024：esp-dsp 的 PIE 实现比 gmf_fft 快得多，
 * 1024 点在 48k 下是 46.9 Hz/bin，低频（60~200 Hz）才有几根 bin 可用；
 * 512 点时 60 Hz 附近只有一根 bin，柱子会一直贴着底边。
 */
#define SPEC_N            1024
#define SPEC_RING         8192      /* 环形缓冲样本数（单声道 48k ≈ 170 ms） */
#define SPEC_BANDS        26        /* 频段数（柱子根数） */
#define SPEC_PERIOD_MS    40        /* 刷新周期：25 fps，与 LVGL 主循环合拍 */
#define SPEC_BOX_H        UI_PX_H(38)
#define SPEC_BAR_W        UI_PX_W(8)
#define SPEC_BAR_MIN_H    UI_PX_H(2)
#define SPEC_F_LO_HZ      60.0f     /* 频带下沿 */
#define SPEC_F_HI_HZ      16000.0f  /* 频带上沿（48k 的 Nyquist 是 24k，再高全是空 bin） */
#define SPEC_DB_FLOOR     (-48.0f)  /* 低于满幅 -48 dB 的频段视为 0（再低全是噪声） */
#define SPEC_RMS_MUTE     30.0f     /* 整帧 RMS 低于此值（≈ -60 dBFS）当静音，直接衰减 */
#define SPEC_DECAY        0.72f     /* 回落速度：上升立刻，下降按这一系数（≈ 0.2 s 落完） */

#define SPEC_NVS_NS       "music_cfg"
#define SPEC_NVS_KEY      "fft"

/* "连续多久没收到 PCM 就报一次"：3 s = 75 个 40 ms 周期（够本地曲目起播解码） */
#define SPEC_STARVE_TICKS (3000 / SPEC_PERIOD_MS)

/*
 * 内部 RAM 的"最低保留量"：给频谱自己分配之后，至少还要剩这么多。
 * 详见下面"内存（懒分配）"那一节的注释 —— 这个数是从真机那次
 * esp-aes: Failed to allocate memory 换来的。
 *
 * 2026-11-16 从 48 KB 降到 16 KB：cJSON 节点改走 PSRAM（os_json_mem.cc）、
 * mbedTLS 也改走 PSRAM（CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC），内部 RAM 的
 * 常驻大头没了，频谱再卡 48 KB 只会让"在线播放时柱子不动"这个毛病延续下去。
 * 剩下的 16 KB 是留给 AES DMA（≤1600 B/次）和协议栈动态分配的安全垫。
 */
#define SPEC_INTERNAL_RESERVE (16 * 1024)

/*
 * esp-dsp 的 FFT 表长由 menuconfig 的 CONFIG_DSP_MAX_FFT_SIZE 决定（默认 4096），
 * 而 dsps_fft2r_init_fc32() 在 table_size 超限时返回 ESP_ERR_DSP_PARAM_OUTOFRANGE，
 * 表现是"开机日志一条警告、之后柱状图永远不动"。宁可在编译期就挡住。
 */
#if !defined(CONFIG_DSP_MAX_FFT_SIZE) || (CONFIG_DSP_MAX_FFT_SIZE < SPEC_N)
#error "CONFIG_DSP_MAX_FFT_SIZE 小于频谱 FFT 的 SPEC_N(1024)：menuconfig → DSP Library → Maximum FFT length 至少要 1024"
#endif

/* ---------------------------------------------------------------- 状态 */

static int16_t *s_ring;                 /* SPSC 环形缓冲（PSRAM 优先） */
static uint32_t s_head;                 /* 生产者写（输出任务） */
static uint32_t s_tail;                 /* 消费者写（LVGL 任务） */

/*
 * s_want_pcm 是"输出任务要不要往环形缓冲里塞样本"的开关。
 * 页面没开 / 用户关掉 FFT 时它是 false —— 这样即使没人在看，
 * 输出任务也不会白白做 48k 次/秒的取平均 + 写内存。
 */
static volatile bool s_want_pcm;
static bool s_enabled;                  /* 用户开关（NVS 持久化） */

static lv_obj_t *s_box;                 /* 谱图容器（页面销毁时随父对象一起没） */
static lv_obj_t *s_bars[SPEC_BANDS];
static int s_px[SPEC_BANDS];            /* 上次画的高度（px），没变就不动 UI */
static float s_level[SPEC_BANDS];       /* 平滑后的电平 0..1 */
static int s_lo_bin[SPEC_BANDS];
static int s_hi_bin[SPEC_BANDS];
static lv_timer_t *s_timer;

static bool  s_fft_ok;                  /* dsps_fft2r_init_fc32() 成功过 */
static float *s_fft;                    /* 2*SPEC_N 个 float：data[2k]=Re, data[2k+1]=Im */
static float *s_win;                    /* SPEC_N 个 float：汉宁窗 */
static int16_t s_frame[SPEC_N];         /* 取最新一帧（单声道 int16） */
static uint32_t s_accent = MUSIC_THEME_DEFAULT_RGB;

static bool s_fft_warned;               /* "内部 RAM 不够建 FFT 表"只吼一次 */
static uint32_t s_starve_ticks;         /* 连续多少个周期一个 PCM 字节都没收到 */
static bool s_starve_warned;            /* "没喂 PCM"只吼一次 */
static bool internal_room(size_t bytes);  /* 定义在文件后半部分的"内存（懒分配）" */

/* ---------------------------------------------------------------- 小工具 */

static void bands_build(void)
{
    /* 对数频率分段：每根柱子覆盖的带宽按等比递增（低频窄、高频宽） */
    const float bin_hz = 48000.0f / (float)SPEC_N;
    const float ratio  = SPEC_F_HI_HZ / SPEC_F_LO_HZ;
    for (int i = 0; i < SPEC_BANDS; i++) {
        const float f0 = SPEC_F_LO_HZ * powf(ratio, (float)i / (float)SPEC_BANDS);
        const float f1 = SPEC_F_LO_HZ * powf(ratio, (float)(i + 1) / (float)SPEC_BANDS);
        int lo = (int)(f0 / bin_hz);
        int hi = (int)(f1 / bin_hz);
        if (lo < 1) {
            lo = 1;                       /* bin 0 是直流 */
        }
        if (hi < lo) {
            hi = lo;
        }
        if (hi > SPEC_N / 2) {
            hi = SPEC_N / 2;
        }
        s_lo_bin[i] = lo;
        s_hi_bin[i] = hi;
    }
}

static void bars_apply_color(void)
{
    for (int i = 0; i < SPEC_BANDS; i++) {
        if (s_bars[i]) {
            lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(s_accent), LV_PART_MAIN);
        }
    }
}

/* ---------------------------------------------------------------- PCM 回调（输出任务） */

static void spectrum_pcm_cb(const int16_t *pcm, size_t frames, void *user)
{
    (void)user;
    if (!s_want_pcm || !s_ring || !pcm) {
        return;
    }
    const uint32_t head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    for (size_t i = 0; i < frames; i++) {
        /* 左右取平均：可视化只看能量，单声道足够（还省一半 FFT 输入） */
        const int32_t mono = ((int32_t)pcm[2 * i] + (int32_t)pcm[2 * i + 1]) / 2;
        s_ring[(head + (uint32_t)i) % SPEC_RING] = (int16_t)mono;
    }
    /* 先写数据、后更新 head（release），消费者靠它判断"读到哪是安全的" */
    __atomic_store_n(&s_head, head + (uint32_t)frames, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------- 取最新一帧 */

/* 只取"最新的 N 个样本"，更早的直接丢掉（可视化不需要每一帧都画） */
static int grab_latest(int16_t *dst)
{
    const uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint32_t tail = s_tail;
    uint32_t avail = head - tail;                  /* 无符号回绕安全 */

    if (avail > SPEC_RING) {                       /* 生产者跑太远：只留最近一圈 */
        tail  = head - SPEC_RING;
        avail = SPEC_RING;
    }
    if (avail > (uint32_t)SPEC_N) {
        tail  = head - (uint32_t)SPEC_N;
        avail = (uint32_t)SPEC_N;
    }

    const uint32_t pad = (uint32_t)SPEC_N - avail;
    memset(dst, 0, sizeof(int16_t) * pad);         /* 老样本补 0：能量从后往前对 */
    for (uint32_t i = 0; i < avail; i++) {
        dst[pad + i] = s_ring[(tail + i) % SPEC_RING];
    }
    s_tail = head;                                 /* 剩下的旧数据不要了 */
    return (int)avail;
}

/* ---------------------------------------------------------------- esp-dsp 初始化 */

static bool fft_ready(void)
{
    if (s_fft_ok) {
        return true;
    }
    if (!s_fft || !s_win) {
        return false;
    }

    /*
     * 库自己 malloc 的表（N 个 float 的旋转因子 + 位反转表）也在**内部 RAM**，
     * 所以先看余量：不够就先不建，别把 TLS 的 AES DMA / 蓝牙的最后一块抢走。
     */
    if (!internal_room(SPEC_N * sizeof(float))) {
        if (!s_fft_warned) {
            s_fft_warned = true;
            ESP_LOGW(TAG, "internal RAM 余量不足（largest=%u），本次不建 FFT 表",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        return false;
    }

    /*
     * 旋转因子表交给库自己 malloc（内部 RAM）。表长 = N，意味着最大支持 N 点。
     * 返回 ESP_ERR_DSP_PARAM_OUTOFRANGE 就说明 N 超出了 CONFIG_DSP_MAX_FFT_SIZE。
     */
    const esp_err_t err = dsps_fft2r_init_fc32(NULL, SPEC_N);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dsps_fft2r_init_fc32(%d) failed: 0x%x", SPEC_N, err);
        return false;
    }
    dsps_wind_hann_f32(s_win, SPEC_N);     /* 汉宁窗：压掉截断引起的旁瓣 */
    s_fft_ok = true;
    return true;
}

/* ---------------------------------------------------------------- 定时器（LVGL 任务） */

static void spec_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* s_fft / s_win 也判一次：detach 会把它们还回去，别让"表已建好"的缓存骗过去 */
    if (!s_box || !s_enabled || !s_ring || !s_fft || !s_win || !fft_ready()) {
        return;
    }

    const int got = grab_latest(s_frame);

    /*
     * 诊断：连续 3 s 一个样本都没收到，说明问题在**上游没喂 PCM**，
     * 不是 FFT 画不出来 —— 这两种故障在界面上长得一模一样（柱子贴底不动），
     * 没这行日志就只能靠猜。真机 2026-11 用户报"在线音乐上 FFT 不工作"就靠它定位。
     */
    if (got <= 0) {
        if (s_starve_ticks < SPEC_STARVE_TICKS) {
            s_starve_ticks++;
            if (s_starve_ticks == SPEC_STARVE_TICKS && !s_starve_warned) {
                s_starve_warned = true;
                ESP_LOGW(TAG, "3 s 没收到 PCM（want=%d fft_ok=%d ring=%p）：输出任务没在喂数据，"
                              "柱子不可能动 —— 先确认 audio_service 的 PCM 回调挂上了、且真的在出声",
                         (int)s_want_pcm, (int)s_fft_ok, (void *)s_ring);
            }
        }
    } else {
        s_starve_ticks = 0;
    }

    /* 整帧 RMS：静音时不要拿"最后一根噪声 bin"归一化，否则底噪也会跳 */
    float rms = 0.0f;
    for (int i = 0; i < SPEC_N; i++) {
        const float v = (float)s_frame[i];
        rms += v * v;
    }
    rms = sqrtf(rms / (float)SPEC_N);

    /* ① 收 PCM → 复数组：加窗后写实部，虚部清零 */
    for (int i = 0; i < SPEC_N; i++) {
        s_fft[2 * i]     = (float)s_frame[i] * s_win[i];
        s_fft[2 * i + 1] = 0.0f;
    }

    /* ② 调 dsp 库做 FFT（输入自然序 → 输出位反序），然后重排 */
    if (dsps_fft2r_fc32(s_fft, SPEC_N) != ESP_OK) {
        return;
    }
    dsps_bit_rev_fc32(s_fft, SPEC_N);

    /*
     * ③ 归一化基准：满幅正弦（A=32767）加汉宁窗后，峰值谱线 ≈ A*N/4
     *    （esp-dsp 的 complex FFT 不做 1/N 缩放，所以幅度是"真的"线性幅度）。
     */
    const float ref = 32767.0f * (float)SPEC_N * 0.25f;
    const bool  mute = (got <= 0) || (rms < SPEC_RMS_MUTE);
    const int   box_h = lv_obj_get_content_height(s_box);
    const float span  = 0.0f - SPEC_DB_FLOOR;      /* 48 dB 的动态范围 */

    for (int b = 0; b < SPEC_BANDS; b++) {
        float target = 0.0f;
        if (!mute) {
            /* 频带内取最大 bin（峰值包络比求和更贴合"柱子跟着鼓点跳"的观感） */
            float peak2 = 0.0f;
            for (int k = s_lo_bin[b]; k <= s_hi_bin[b]; k++) {
                const float re = s_fft[2 * k];
                const float im = s_fft[2 * k + 1];
                const float m2 = re * re + im * im;
                if (m2 > peak2) {
                    peak2 = m2;
                }
            }

            /* ④ log 幅度：线性幅度 → dB（相对满幅），再夹到 [floor, 0] */
            const float mag = dsps_sqrtf_f32(peak2) + 1.0f;
            const float db  = 20.0f * log10f(mag / ref);
            float lin = (db - SPEC_DB_FLOOR) / span;
            if (lin < 0.0f) {
                lin = 0.0f;
            } else if (lin > 1.0f) {
                lin = 1.0f;
            }
            /* ⑤ 线性映射到柱高（需求明确不做 gamma） */
            target = lin;
        }

        /* 上升立刻到位（打击乐要有"弹起来"的手感），下降慢慢回落 */
        s_level[b] = (target > s_level[b]) ? target
                                           : (s_level[b] * SPEC_DECAY + target * (1.0f - SPEC_DECAY));

        int h = (int)(s_level[b] * (float)box_h);
        if (h < SPEC_BAR_MIN_H) {
            h = SPEC_BAR_MIN_H;
        }
        if (h != s_px[b]) {
            s_px[b] = h;
            lv_obj_set_height(s_bars[b], h);
        }
    }
}

/* ---------------------------------------------------------------- 内存（懒分配） */

/*
 * ★ 频谱绝不能跟"必须拿内部 RAM"的东西抢内存。
 *
 * 只能放内部 RAM 的有两类，且都在开机阶段要：
 *   ① Wi-Fi / 蓝牙协议栈与控制器的内存池（main.cc 的 RADIO_MIN_INTERNAL = 32 KB，
 *      低于它宁可不起射频，否则控制器在错误路径里 panic）；
 *   ② mbedtls 的 AES/GCM —— 每次 TLS 写都要给 DMA 申请一块 16 字节对齐的内部缓冲
 *      （esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c:270 的
 *      heap_caps_aligned_alloc(MALLOC_CAP_DMA)）。
 *
 * 2026-11 真机就是这样炸的（用户报"歌曲拉取失败"）：
 *     E (12817) esp-aes: Failed to allocate memory
 *     E (12818) esp-tls-mbedtls: write error :-0x0084
 *     E (12822) HTTP_CLIENT: Error write request
 *     W (12828) online: GET failed (ESP_ERR_HTTP_WRITE_DATA): https://<api-host>/user/account
 * 同一时刻蓝牙也挂（BT_SDP: SDP - Rcvd conn cnf with error: 0x4 -> A2DP FAILED）——
 * 这不是网络问题，是开机那二三十秒（Wi-Fi 建链 + 蓝牙连 A2DP + 账号歌单拉取同时来）
 * 内部 RAM 见底。旧实现恰好在 init 里一次拿走约 36 KB 内部 RAM
 * （ring 16 KB + FFT 输入 8 KB + 窗 4 KB + dsp 表/rev 表 8 KB），而 init 跑在
 * main() 的 app_music_register_apps() 里，**比射频初始化还早** —— 等于先把射频的
 * 口粮吃掉，再让它去连设备。
 *
 * 现行纪律：
 *   - 开机（init）**零分配**，只在播放器页建出来（attach）时才要内存；
 *   - 环形缓冲 / FFT 输入 / 汉宁窗（16 + 8 + 4 = 28 KB）**一律优先 PSRAM**：
 *     它们都是"每帧整块读写一遍"的批量数据，放 PSRAM 只慢一点点，而内部 RAM
 *     是 Wi-Fi/蓝牙协议栈、TLS 的 AES DMA、任务栈的救命资源（2026-11 用户：
 *     "这把 SRAM 搞了都崩溃了"）。PSRAM 真的没有才退回内部 RAM，再不够就不画；
 *   - esp-dsp 自己的表（旋转因子 4 KB + 位反转 2 KB）由库内部 malloc，拿的是
 *     内部 RAM；**离开播放器页会调 dsps_fft2r_deinit_fc32() 还回去**，
 *     所以"听歌时"才占这 6 KB。
 */

/* 内部 RAM 够不够：要求"分配之后至少还剩 SPEC_INTERNAL_RESERVE" */
static bool internal_room(size_t bytes)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= bytes + SPEC_INTERNAL_RESERVE;
}

/* 分配失败也返回 false（调用方据此决定"这次不画"），已有的一律复用 */
static bool spec_alloc(void)
{
    if (s_ring && s_fft && s_win) {
        return true;
    }

    if (!s_ring) {
        s_ring = (int16_t *)heap_caps_malloc(SPEC_RING * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            s_ring = (int16_t *)heap_caps_malloc(SPEC_RING * sizeof(int16_t),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_ring) {
            s_head = 0;
            s_tail = 0;
        }
    }

    const size_t fft_bytes = 2u * SPEC_N * sizeof(float);
    const size_t win_bytes = SPEC_N * sizeof(float);

    /*
     * ★ 2026-11-16 改成 **PSRAM 优先**（用户："把 FFT 可视化的东西搬 PSRAM 去，
     *   这把 SRAM 搞了都崩溃了"）。
     *
     *   FFT 输入缓冲必须 16 字节对齐：PIE 汇编（dsps_fft2r_fc32_arp4）做 128 位
     *   向量读写，非对齐会直接异常，所以这里用 heap_caps_aligned_alloc(16, ...)。
     *
     *   PIE 读 PSRAM 是走 cache 的普通数据通路，架构上没问题；代价是每帧多读
     *   ~12 KB（25 fps 下 ≈300 KB/s，PSRAM 200 MHz 八线完全吃得下）。
     *   万一哪天在真机上发现 PIE 外部 RAM 取数有问题（现象是进播放器页立刻
     *   crash / 非法指令），把下面两个 MALLOC_CAP_SPIRAM 换成 MALLOC_CAP_INTERNAL
     *   即可退回（内部 RAM 里这块是 12 KB，撤掉 cJSON/mbedTLS 的大头之后放得下）。
     */
    if (!s_fft) {
        s_fft = (float *)heap_caps_aligned_alloc(16, fft_bytes,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fft && internal_room(fft_bytes)) {
            s_fft = (float *)heap_caps_aligned_alloc(16, fft_bytes,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_win) {
        s_win = (float *)heap_caps_aligned_alloc(16, win_bytes,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_win && internal_room(win_bytes)) {
            s_win = (float *)heap_caps_aligned_alloc(16, win_bytes,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }

    if (!s_ring || !s_fft || !s_win) {
        ESP_LOGW(TAG, "no memory for spectrum (ring=%p fft=%p win=%p, internal largest=%u)",
                 (void *)s_ring, (void *)s_fft, (void *)s_win,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }

    /* 每次进播放器页打一行：以后"音频卡顿 / TLS 又没内存"能直接看出缓冲落在哪 */
    ESP_LOGI(TAG, "spec mem: ring %uB(%s) fft %uB(%s) win %uB(%s) | internal free=%u largest=%u",
             (unsigned)(SPEC_RING * sizeof(int16_t)),
             esp_ptr_external_ram(s_ring) ? "psram" : "int",
             (unsigned)(2u * SPEC_N * sizeof(float)),
             esp_ptr_external_ram(s_fft) ? "psram" : "int",
             (unsigned)(SPEC_N * sizeof(float)),
             esp_ptr_external_ram(s_win) ? "psram" : "int",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

/*
 * 页面销毁时把内部 RAM 还给系统。**只还 FFT/窗**（它们只被 lv_timer 用，而 lv_timer
 * 已经在 detach 里删掉了）；环形缓冲留给输出任务的回调，删了会有 use-after-free 的
 * 窗口 —— 而且它本来就优先在 PSRAM，留着也不占内部 RAM。
 */
static void spec_free_internal(void)
{
    if (s_fft) {
        heap_caps_free(s_fft);
        s_fft = NULL;
    }
    if (s_win) {
        heap_caps_free(s_win);
        s_win = NULL;
    }
    /*
     * 还有一块容易被忘掉的内部 RAM：dsps_fft2r_init_fc32() 自己 malloc 的
     * 旋转因子表（N 个 float = 4 KB）+ 位反转表（约 2 KB）。它俩是**库内全局**，
     * 不调 deinit 就一直攥着，离开播放器页也不还。
     * 这里还回去（~6 KB），下次进页面由 fft_ready() 重新建。
     * 安全性：唯一的使用者是 s_timer，而 detach 已经先把它删掉了。
     */
    if (s_fft_ok) {
        dsps_fft2r_deinit_fc32();
        s_fft_ok = false;
    }
    s_fft_warned = false;   /* 下次进页面如果还不够内存，要重新吼一次（别被上次的静默骗了） */
}

void music_spectrum_init(void)
{
    /* 开关落 NVS（默认开）。用独立命名空间，别跟 online / wifi 的键混在一起。 */
    uint8_t v = 1;
    nvs_handle_t h;
    if (nvs_open(SPEC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, SPEC_NVS_KEY, &v) != ESP_OK) {
            v = 1;
        }
        nvs_close(h);
    }
    s_enabled = (v != 0);

    /* ★ 这里**一个字节都不分配**：本函数跑在 main() 的 app_music_register_apps()，
     *   比 Wi-Fi/蓝牙初始化更早（见上面 SPEC_INTERNAL_RESERVE 的注释）。 */

    /* 页面没打开时 s_want_pcm 保持 false：回调立刻返回，零开销 */
    audio_service_set_pcm_monitor(spectrum_pcm_cb, NULL);
}

void music_spectrum_attach(lv_obj_t *parent)
{
    if (!parent || s_box) {
        return;
    }
    /* 懒分配：第一次建页才要内存（开机时一个字节都不占，见 SPEC_INTERNAL_RESERVE） */
    if (!spec_alloc()) {
        return;                           /* 真没内存就先不建空壳，下次进页面再试 */
    }
    bands_build();
    s_starve_ticks = 0;
    s_starve_warned = false;
    (void)fft_ready();                    /* 顺手把 dsp 旋转因子表建好（失败则 timer 里不画） */

    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, LV_PCT(100), SPEC_BOX_H);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_ROW);
    /* 主轴均分、交叉轴贴底：柱子从底部往上长 */
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_CLICKABLE);   /* 不许挡住页面手势 */
    for (int i = 0; i < SPEC_BANDS; i++) {
        lv_obj_t *bar = lv_obj_create(s_box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, SPEC_BAR_W, SPEC_BAR_MIN_H);
        lv_obj_set_style_bg_color(bar, lv_color_hex(s_accent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, SPEC_BAR_W / 2, LV_PART_MAIN);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        s_bars[i] = bar;
        s_px[i]   = SPEC_BAR_MIN_H;
        s_level[i] = 0.0f;
    }

    if (!s_enabled) {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_timer) {
        s_timer = lv_timer_create(spec_timer_cb, SPEC_PERIOD_MS, NULL);
    } else {
        lv_timer_resume(s_timer);
    }
    s_want_pcm = s_enabled;
}

void music_spectrum_detach(void)
{
    s_want_pcm = false;
    if (s_timer) {
        lv_timer_delete(s_timer);         /* ★ 必须自己删：timer 的 user_data 不是对象，
                                           *   不随页面销毁而消失，留着就是野指针 */
        s_timer = NULL;
    }
    /* 容器与柱子由 LVGL 随父对象销毁 —— 这里只清指针，绝不 lv_obj_delete */
    s_box = NULL;
    memset(s_bars, 0, sizeof(s_bars));
    memset(s_px, 0, sizeof(s_px));
    memset(s_level, 0, sizeof(s_level));
    s_starve_ticks = 0;
    s_starve_warned = false;

    /* 把 FFT/窗那 12 KB 内部 RAM 还给系统（timer 已删，没人再用它们） */
    spec_free_internal();
}

bool music_spectrum_enabled(void)
{
    return s_enabled;
}

void music_spectrum_set_enabled(bool on)
{
    s_enabled = on;

    nvs_handle_t h;
    if (nvs_open(SPEC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, SPEC_NVS_KEY, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }

    if (s_box) {
        if (on) {
            lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_timer) {
        if (on) {
            lv_timer_resume(s_timer);
        } else {
            lv_timer_pause(s_timer);
        }
    }
    s_want_pcm = on && (s_box != NULL);
}

void music_spectrum_set_accent(uint32_t rgb)
{
    if (rgb == s_accent) {
        return;
    }
    s_accent = rgb;
    bars_apply_color();
}
