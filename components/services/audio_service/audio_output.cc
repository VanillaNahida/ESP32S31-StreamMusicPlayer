/*
 * audio_output.cc —— 输出级：把解码器吐出的 PCM 送到扬声器 / 蓝牙 / 静音
 *
 * 数据通路（spa：espaudiocore 的解码任务 → 本文件的任务）：
 *
 *   espaudiocore 解码任务
 *     └─ AudioSinkRingbuf::write_i32()        ← cfg.output_bits=16，恒定 int16
 *          └─ PCM ringbuf（AUDIO_PCM_RING_BYTES，PSRAM）
 *               └─ out_task（本文件）
 *                    ├─ esp_asrc：源采样率/声道 → 48 kHz / 2ch（16-bit）
 *                    └─ 按 s_route 分流
 *                         ├─ SPEAKER    : esp_codec_dev_write() → ES8389(TDM) → NS4150B
 *                         ├─ BLUETOOTH  : 软件音量 → bt_service_write_pcm()
 *                         └─ NONE       : 只走时间轴，不写硬件
 *
 * ── 为什么必须有这一层（而不是直接用 espaudiocore 的 I2S 输出）────────────────
 * 1. 本板 I2S 是 **TDM**，espaudiocore 的 I2S sink 用 STD API 重配时钟，配不下去
 *    （详见 audio_engine.cc 的说明）；
 * 2. espaudiocore **不做重采样、不做声道转换**，而工程的不变式是
 *    "写硬件的 PCM 恒为 48 kHz/16bit/2ch"（进度条、看门狗、seek 都按 192 B/ms 算）；
 * 3. 输出三路（扬声器/蓝牙/静音）与功放、静音时序是板级需求，库里没有。
 *
 * ── 重采样：**硬件** ASRC（2026-10 换，软件路径已被真机证伪）─────────────
 * S31 自带 ASRC 外设（`SOC_ASRC_SUPPORTED`，2 个 stream、每 stream 1~2 通道），
 * `espressif/esp_asrc` 的 `ESP_ASRC_PERF_TYPE_AUTO` 会把它挂到 GDMA 上跑。
 *
 * 为什么必须换：软件路径（`ESP_ASRC_PERF_TYPE_SW_SPEED`）= esp_audio_effects 的
 * `esp_ae_rate_cvt`，44.1k→48k 立体声在真机上**吃掉一个整核**，现场日志：
 *
 *     audio_out: play stats: 产 2125 ms / 墙 5018 ms (42.3%)
 *                | asrc 5047 ms (cpu 5047 ms)        ← 100% 的墙钟
 *     task_wdt: IDLE1 没喂狗，CPU 1: audio_out，MEPC 在 fa_resample_process
 *
 * 换成硬件后同一个换算的 CPU 占用≈0（任务阻塞在 DMA 完成事件上）。
 *
 * 硬件路径有两条硬约束，下面代码全部照做：
 *   1. **缓冲必须是内部 RAM（或 cache-line 对齐的 PSRAM）**：`esp_asrc_hw_process()`
 *      用 GDMA 读写，PSRAM 要先 msync、地址/长度还要按 cache line 对齐。我们把
 *      in/out 一律放**内部 RAM**并按 `esp_asrc_get_buffer_alignment()` 对齐 ——
 *      GDMA 完全不碰 PSRAM，也就没有"先慢放、几秒后全是噪音"那类一致性问题
 *      （bt_service 当年就踩过，见 bts_audio.cc 里 bt_asrc_open() 的长注释）。
 *   2. **S31 的硬件 ASRC 只认 16-bit / 1~2ch / 6 种标准采样率**
 *      （`esp_asrc/hw/caps/esp32s31/asrc_caps_check.c`）。ringbuf 本来就是 16-bit
 *      （espaudiocore `cfg.output_bits=16`），所以这条路成立；22050 Hz 这种不在
 *      表里的率由 AUTO 自动退回软件（日志写 `asrc ready (sw)`）。
 *
 * ⚠ 硬件 ASRC 一共只有 **2 个 stream**，立体声一占就是 2 个 —— 也就是"同时只能有
 *   一路立体声硬件重采样"。bt_service 那条（48k → A2DP 协商率）刻意继续走软件，
 *   免得跟这里抢；万一抢了也只是 AUTO 自动退回软件，不会失败。
 *
 * 选哪条路是 **menuconfig** 的 `Audio Service → 输出级 ASRC 性能模式`
 * （AUTO / HW_ONLY / SW_SPEED），默认 AUTO。
 *
 * ── 时间轴 ─────────────────────────────────────────────────────────────────
 * s_played_bytes 统计的是**送去硬件的 48k/16/2 字节数**，所以
 * `ms = bytes / AUDIO_OUT_BYTES_PER_MS`（192）恒成立。这是全工程唯一的时间轴口径，
 * 不要在别处再按源采样率算一套。
 */
#include "audio_internal.h"

#include <string.h>

#include "esp_asrc.h"
#include "esp_cpu.h"        /* esp_cpu_get_cycle_count()：区分"被抢 CPU"还是"自己变慢" */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "bsp.h"
#include "bt_service.h"
#include "espaudiocore.h"

static const char *TAG = "audio_out";

/*
 * 一次读 ringbuf / 喂 ASRC 的**最大**帧数。两条路径的判据完全不同，别合并成一个数。
 *
 * 【软件路径 ASRC_SW_FRAMES = 512】受"硬件缓冲只有 46.4 − 37.2 ≈ 9.2 ms"约束。
 * 现场日志（44100 Hz/2ch/16bit FLAC，route 0，当时软件重采样）：
 *     play stats: 产 5015/5015 ms (100.0%) | codec 4019 ms, asrc 962 ms, wait 7 ms,
 *                 calls 108
 * 每行 108 次调用 / 5015 ms → **每次迭代 46.4 ms**：读 8192 B（= 2048 帧 @44.1k
 * = 46.4 ms 音频）→ ASRC 8.9 ms（软件重采样 44.1k→48k）→ 阻塞写 37.2 ms。
 * 写成 37.2 ms 就能收下 46.4 ms 的音频，反推硬件侧（I2S DMA + codec_dev 内部缓冲）
 * **总深度只有 46.4 − 37.2 ≈ 9.2 ms**。
 *
 * 于是一次迭代里：硬件缓冲在开头是满的（9.2 ms），接着 8.9 ms 全花在 ASRC 计算上，
 * 这期间**一点新数据都喂不进去** → 缓冲被排到只剩 0.3 ms → 任何抖动都打穿 DMA。
 * 每 46 ms 断一次粮（≈21 次/秒）就是听感上的"异常卡顿"。
 * （也解释了为什么只有需要重采样的 44.1k/32k/22.05k 卡：48k 直通时 asrc = 0 ms。）
 *
 * 所以软件路径的判据是 **每次迭代的 ASRC 计算时间 << 9.2 ms 的硬件余量**：
 *     2048 帧 → 8.9 ms   ← 余量 0.3 ms，等于没有（旧值）
 *      512 帧 → 2.2 ms   ← 余量 7.0 ms
 *      256 帧 → 1.1 ms   ← 余量 8.1 ms（迭代次数更多，固定开销开始显出来）
 * 软件路径只在"硬件不可用"（22050 Hz、或 hardware stream 被抢）时才走。
 *
 * 【硬件路径 ASRC_HW_FRAMES = 1024】硬件是**突发**转换：数据由 GDMA 喂进 ASRC
 * 外设，esp_asrc_hw_process() 等到 RX 收帧中断就返回，**不按实时时钟节流**。
 * 所以"算得慢"不再是约束；块大一点只是为了摊薄每次调用的固定开销
 * （GDMA 描述符挂载 + 队列等待）。1024 帧 = 23 ms 音频，一次调用通常几百微秒。
 * ⚠ 1024 只是**上限**：asrc_open() 还会按 ASRC_DMA_BLOCK_MAX(4080 B) 把实际分片
 *   压下来（44.1k/2ch 时 → 920 帧 ≈ 21 ms），原因见下面那条长注释。
 *
 * ⚠ 别改成 8192 这种巨块：esp_asrc_process() 是在**会话锁内**调的，
 *   单次持锁时间要压住，否则 LVGL 那边调 audio_service_* 会被一起拖住。
 * ⚠ 想改大之前先看 stats 里的 `asrc` 列：它必须明显小于 (迭代时长 − codec 时长)。
 */
#define ASRC_SW_FRAMES    512
#define ASRC_HW_FRAMES    1024
/*
 * ⚠⚠ 硬件路径的**单次转换块上限 = 4080 字节**（输入、输出都算），这条不是性能，
 *   是**正确性**：esp_asrc 的 GDMA 描述符按 4080 B 一块切（esp_asrc_adapter.c 里
 *   ASRC_HW_GDMA_DESC_BUFFER_MAX_SIZE = DMA_DESCRIPTOR_BUFFER_MAX_SIZE_16B_ALIGNED
 *   = 4080），而它**复用**描述符表时只看"历史最大块数"：
 *       asrc_hw_gdma_create_link_list(byte_cnt, ...)   // 只有 desc_num > max 才重建
 *       asrc_hw_gdma_mount_link_list(list, max_desc_num, buf, byte_cnt)
 *   于是"先来一个大块（max=2）、再来一个小块"时，它仍按 2 块挂载：
 *   第一块固定写 4080 B、第二块拿 byte_cnt-4080（**无符号下溢成 4 GB**）——
 *   TX 会越界读、RX 的 EOF 永远等不到（表现：`E ESP_HW_ASRC: HW ASRC process
 *   timeout` + 丢块卡顿）。
 * 把**输入和输出都压在 4080 B 以内**，描述符个数就恒为 1、挂载长度恒等于实际
 * 字节数，块大块小都正确 —— 于是 ringbuf 这次给了多少帧（1024 帧的整片、还是
 * 解码器刚写的半片）都不再是问题。下面 asrc_open() 里的 clamp 就是算这个。
 */
#define ASRC_DMA_BLOCK_MAX  4080

/** 单次 esp_asrc_process() 的等待上限（只对硬件路径有意义）。
 *  超时 = 这次 DMA 没收到帧；200 ms 远大于真实转换耗时（亚毫秒级），
 *  又足够短，不会把输出任务卡在那儿一整秒。 */
#define ASRC_TIMEOUT_MS   200

/*
 * 走硬件还是软件：由 menuconfig 的
 *   Audio Service → 输出级 ASRC 性能模式
 * 决定（默认 AUTO）。三个分支的语义：
 *   AUTO    —— esp_asrc 自己判：硬件能干的都交给硬件（真换采样率的场合），
 *              只做声道上混（48k mono→stereo）时落回软件的声道转换（很便宜）；
 *              **硬件 open 失败会自动退回软件，不会没声**。
 *   HW_ONLY —— 只走硬件；失败时下面 asrc_open() 也会退回软件（宁可慢，不能变调）。
 *   SW_SPEED—— 旧行为，只走软件（44.1k→48k 要吃一个整核，仅作对照用）。
 */
#if defined(CONFIG_AUDIO_OUT_ASRC_PERF_HW_ONLY)
#define ASRC_PERF_DEFAULT  ESP_ASRC_PERF_TYPE_HW_ONLY
#elif defined(CONFIG_AUDIO_OUT_ASRC_PERF_SW_SPEED)
#define ASRC_PERF_DEFAULT  ESP_ASRC_PERF_TYPE_SW_SPEED
#else
#define ASRC_PERF_DEFAULT  ESP_ASRC_PERF_TYPE_AUTO
#endif
/** 健康度日志窗口 */
#define STATS_WINDOW_MS   5000
/** 输出任务栈（只做 memcpy/重采样/写 codec，不要大） */
#define OUT_TASK_STACK    4096
#define OUT_TASK_PRIO     6
#define OUT_TASK_CORE     1     /* 与解码任务（core 0）分开，避免互相饿死 */

/* ===========================================================================
 * 状态
 * =========================================================================*/

static RingbufHandle_t s_pcm_rb;
static TaskHandle_t    s_task;

static volatile uint32_t s_gen;          /**< 0 = 没有会话 */
static volatile bool     s_eos;          /**< 解码侧已到末尾，等输出抽干 */
static volatile bool     s_flowing;      /**< 本会话已经真的写过硬件 */
static volatile uint64_t s_played_bytes; /**< 送去硬件的 48k/16/2 字节数（时间轴） */
static volatile uint32_t s_tl_base_ms;   /**< 时间轴基准（换曲 0；seek 后 = 目标时刻） */
static volatile audio_out_t s_route = AUDIO_OUT_SPEAKER;
static volatile int      s_volume = 55;

/* 重采样 */
static esp_asrc_handle_t s_asrc;
static uint32_t          s_src_rate;
static uint8_t           s_src_ch;
static uint8_t           s_src_bits;   /**< 解码源位深（24 表示源是 24-bit，ringbuf 仍是 16-bit） */
static uint16_t          s_in_smp_bytes;   /**< 源：每帧字节数（ch×2） */
static uint16_t          s_out_smp_bytes;  /**< 目标：每帧字节数（恒 4） */
static uint8_t          *s_in_buf;
static size_t            s_in_cap;         /**< ASRC 输入缓冲容量（字节） */
static uint8_t          *s_out_buf;
static uint32_t          s_out_frames_cap;
static uint32_t          s_chunk_frames;   /**< 一次喂给 ASRC 的输入帧数（软 512 / 硬 ≤1024，再按 4080 B 压） */
static bool              s_asrc_hw;        /**< 本会话的重采样是否真的在硬件上跑（只影响日志/诊断） */
static uint32_t          s_asrc_err;       /**< esp_asrc_process 失败次数 */

/* ★ 2026-10 诊断（用户报 96k/24bit FLAC "播放速度过快"）：
 * 统计 ASRC 的**累计输入/输出帧数**。稳态下 out/in 必须等于 输出率/输入率
 * （96000→48000 就是 0.500）；若明显偏大（例如 0.875），说明每块喂给输出链的
 * 样本比应有值多 ⇒ 内容比真实时间跑得快，而"产/墙 100%"的表象并不矛盾
 * （那只说明 I2S 一直有数据）。*/
static uint32_t          s_asrc_in_frames;
static uint32_t          s_asrc_out_frames;
static uint32_t          s_asrc_calls;
static uint32_t          s_asrc_sat;       /**< 输出缓冲被写满（样本被截断）的次数 */
/**
 * ASRC 累计消耗的 **CPU 周期**。
 *
 * ★★ 2026-10 加：用来区分"重采样被抢了 CPU"和"重采样自己变慢了"。
 *   这是真机上唯一能分清两者的量：`s_stats_asrc_us` 是**墙钟**，任务被抢占时它照样走；
 *   周期计数只算这颗核真正执行过的周期。于是
 *       墙钟 107 ms / 周期折算 9 ms  → 92% 的时间在被别人抢（去找抢占者）
 *       墙钟 107 ms / 周期折算 103 ms → 代码真的慢 12 倍（cache/总线/库的问题）
 *   背景日志（蓝牙播 44.1k MP3）：
 *       play stats: 产 2135/5033 ms (42.4%) | asrc 5063 ms, calls 47, route 1
 *       task_wdt: IDLE1 (CPU 1) 没喂狗；MEPC 在 fa_resample_process
 *   板载输出同一份 ASRC 只要 8.9 ms/次 —— 差 12 倍，必须量一下才知道是哪种。
 */
static volatile uint64_t s_stats_asrc_cyc;
/** mono→stereo 的权重：esp_asrc 硬件路径要求显式给 {1,1} 才认这条路 */
static float             s_weight[2] = {1.0f, 1.0f};

/** 蓝牙路软件音量用的暂存（按需增长） */
static uint8_t          *s_bt_stage;
static size_t            s_bt_stage_cap;

/**
 * 重采样输出的暂存（按需增长）。**只属于输出任务**，引擎的会话拆除永远不碰它 ——
 * 这样"写硬件"才能放到会话锁之外做（见 prepare_block 与 out_task 的注释）。
 */
static uint8_t          *s_stage;
static size_t            s_stage_cap;

/* 健康度统计 */
static uint32_t s_stats_ms;
static uint64_t s_stats_bytes;
static uint64_t s_stats_codec_us;
static uint64_t s_stats_asrc_us;
static uint64_t s_stats_wait_us;
static uint32_t s_stats_calls;
static uint32_t s_stats_gap_max_ms;
static uint32_t s_stats_last_ms;
static uint32_t s_codec_err;             /**< 写 codec 失败次数（统计窗口内） */

/* ===========================================================================
 * 小工具
 * =========================================================================*/

static void ring_drain(void)
{
    if (!s_pcm_rb) {
        return;
    }
    for (;;) {
        size_t got = 0;
        uint8_t *it = (uint8_t *)xRingbufferReceiveUpTo(s_pcm_rb, &got, 0, 4096);
        if (!it) {
            break;
        }
        vRingbufferReturnItem(s_pcm_rb, it);
    }
}

/** 16-bit PCM 就地软件音量缩放（带削波保护）。**只给蓝牙路用**：
 *  扬声器路的音量走 ES8389 的数字音量寄存器（更干净、也不占 CPU）。*/
static void pcm16_scale(uint8_t *buf, size_t samples, int percent)
{
    if (percent >= 100) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    int16_t *p = (int16_t *)buf;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = ((int32_t)p[i] * percent) / 100;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        p[i] = (int16_t)v;
    }
}

static uint8_t *bt_stage(size_t need)
{
    if (s_bt_stage_cap >= need) {
        return s_bt_stage;
    }
    uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        return NULL;
    }
    if (s_bt_stage) {
        heap_caps_free(s_bt_stage);
    }
    s_bt_stage = p;
    s_bt_stage_cap = need;
    return s_bt_stage;
}

/** 重采样输出暂存，按需增长（只被 out_task 调用） */
static bool stage_ensure(size_t need)
{
    if (s_stage_cap >= need) {
        return s_stage != NULL;
    }
    uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        ESP_LOGE(TAG, "stage alloc %u B failed", (unsigned)need);
        return false;
    }
    if (s_stage) {
        heap_caps_free(s_stage);
    }
    s_stage = p;
    s_stage_cap = need;
    return true;
}

/* ===========================================================================
 * 重采样（esp_asrc）
 * =========================================================================*/

static void asrc_close(void)
{
    if (s_asrc) {
        /*
         * ⚠ 这两行是**崩溃定位专用**（2026-10 真机两次 tlsf 双重释放都发生在
         * "拆除用过 ASRC 的会话"时）。esp_asrc 的软件路径最终落到**预编译库**
         * `libesp_audio_effects.a` 的 `esp_ae_rate_cvt`，我们看不到它的 close 实现；
         * 有了 "asrc close: h=…" 与 "asrc closed" 这一对，下次日志就能判断
         * 崩溃到底发生在 `esp_asrc_close()` 里面（只有前一行）还是后面的某一步。
         * 定位完成后可以降成 LOGD。
         */
        ESP_LOGI(TAG, "asrc close: h=%p (src %u Hz %uch)", s_asrc,
                 (unsigned)s_src_rate, s_src_ch);
        esp_asrc_close(s_asrc);
        s_asrc = NULL;
        ESP_LOGI(TAG, "asrc closed");
    }
    if (s_in_buf) {
        heap_caps_free(s_in_buf);
        s_in_buf = NULL;
    }
    if (s_out_buf) {
        heap_caps_free(s_out_buf);
        s_out_buf = NULL;
    }
    s_in_cap = 0;
    s_out_frames_cap = 0;
    s_chunk_frames = 0;
    s_asrc_hw = false;
    /* ★ 换曲/会话结束时清掉 ASRC 统计：它们是**跨会话累计**的，不清零会把上一首
     * （比如 44.1k->48k，比值 1088）混进这一首，日志里 662→559 那种"漂移"就是这么来的 ✗。
     * 真实比值要看**增量**（每次 +460800 in / +230400 out = 精确 0.500 ✓）。*/
    s_asrc_in_frames = 0;
    s_asrc_out_frames = 0;
    s_asrc_calls = 0;
}

/* S31 硬件 ASRC 支持的采样率（esp_asrc/hw/caps/esp32s31/asrc_caps_check.c 的
 * IS_VALID_SR，缺一不可）。不在表里的率 AUTO 会自动落回软件，这里只是为了让
 * 日志能说清"这一首走的是哪条路"。 */
static bool asrc_hw_rate_ok(uint32_t hz)
{
    return hz == 8000 || hz == 16000 || hz == 24000 || hz == 32000 ||
           hz == 44100 || hz == 48000;
}

/*
 * ASRC 的输入/输出缓冲：**内部 RAM + 按 cache line 对齐**。
 *
 * 为什么不像别处一样优先用 PSRAM：S31 的硬件 ASRC 是**经 GDMA 读写内存**的
 * （esp_asrc_hw.c: esp_asrc_hw_gdma_*），PSRAM 缓冲在每次 process 前后都要
 * esp_cache_msync()，而且地址/长度必须 cache-line 对齐，否则返回
 * ESP_ASRC_ERR_INVALID_ALIGNMENT。缓冲放内部 RAM 之后：GDMA 不碰 PSRAM、
 * 两条 msync 都变成空操作（asrc_adapter.c 里 esp_ptr_external_ram() 为假），
 * 也就没有"先慢放、几秒后全是噪音"那类一致性隐患。
 * 代价很小：512~1024 帧的 in/out 一共 ~14 KB 内部 RAM。
 *
 * 内部 RAM 真不够时才退回 PSRAM —— 此时地址与长度都已按 align 对齐，
 * 组件的对齐检查仍然能过（是合法配置，只是多两次 cache 操作）。
 */
static uint8_t *asrc_buf_alloc(size_t size, size_t align, const char *what)
{
    if (align < 4) {
        align = 4;
    }
    uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(
        align, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "%s: 内部 RAM 分配 %u B 失败，退回 PSRAM（含 cache 同步）",
                 what, (unsigned)size);
        p = (uint8_t *)heap_caps_aligned_alloc(
            align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return p;
}

static bool asrc_open(uint32_t src_rate, uint8_t src_ch)
{
    esp_asrc_cfg_t cfg = {};
    cfg.src_info.sample_rate     = src_rate;
    cfg.src_info.channel         = src_ch;
    cfg.src_info.bits_per_sample = AUDIO_OUT_BITS;
    cfg.dest_info.sample_rate    = AUDIO_OUT_RATE;
    cfg.dest_info.channel        = AUDIO_OUT_CHANNELS;
    cfg.dest_info.bits_per_sample = AUDIO_OUT_BITS;
    /* ★ 2026-10：默认硬件（见文件头）。ASRC_PERF_DEFAULT 来自 menuconfig，
     * 硬件 open 失败时下面会退回 SW_SPEED（宁可慢，不能变调）。 */
    cfg.perf_type = ASRC_PERF_DEFAULT;
    /*
     * complexity 1~3：3 = 质量最好、最慢；1 = 最快、质量最差。
     *
     * ⚠ esp_asrc 的软件路径其实就是 **esp_audio_effects 的 `esp_ae_rate_cvt`**
     *   （`sw/src/asrc_sw_rate_cvt_ops.c` 只是薄封装）—— 也就是 GMF 时代被测出
     *   "约 20% 实时"的同一个软件重采样器。2026-10 真机日志显示 complexity=3 时
     *   它把整条链拖到 **97.8% 实时**（每 63 ms 音频要 64.5 ms 才产得出来），
     *   表现就是"部分文件（需要重采样的 44.1k/32k/22.05k）一卡一卡"，同时它还占着
     *   core 1 把 LVGL 挤掉三成 CPU。
     *
     * 取 2 是折中：比 3 明显省 CPU，质量损失在 44.1k→48k 这种比率上基本听不出来。
     * 若日志里 `asrc` 那一列仍然很大，下一步是**换更省的重采样**（自研定点多相 FIR，
     * 或按 esp_asrc.h 的官方对齐 API 走 S31 的硬件 ASRC），不要再往 1 降质量。
     */
    cfg.complexity = 2;                    /* 只在软件路径生效（硬件忽略） */
    cfg.timeout_ms = ASRC_TIMEOUT_MS;      /* 只在硬件路径生效；0 是非法值 */
    if (src_ch != AUDIO_OUT_CHANNELS) {
        /* 单声道 → 立体声：显式给权重，硬件路径也认这种配置 */
        cfg.weight = s_weight;
        cfg.weight_len = 2;
        s_weight[0] = 1.0f;
        s_weight[1] = 1.0f;
    }

    /*
     * ★ 打开重采样器（硬件优先）。
     *
     * 硬件路径会**占用 S31 的 ASRC stream**（共 2 个，立体声一次吃满 2 个），
     * 并且只认 16-bit / 1~2ch / 6 种标准率 —— 任何一条不满足，esp_asrc_open() 就
     * 返回 NOT_SUPPORT / FAIL。这种情况（22050 Hz 的音源、bt_service 已经占着
     * 硬件、AUTO 判硬件不划算…）必须**退回软件**，否则 prepare_block() 会因为
     * s_asrc == NULL 而当成"直通"，44.1k 按 48k 时钟播 = 快 8.8% = 明显变调。
     */
    esp_asrc_err_t orc = esp_asrc_open(&cfg, &s_asrc);
    if ((orc != ESP_ASRC_ERR_OK || !s_asrc) && cfg.perf_type != ESP_ASRC_PERF_TYPE_SW_SPEED) {
        ESP_LOGW(TAG, "asrc: 硬件路径开不起来 (%d, %u Hz %uch)，退回软件重采样",
                 (int)orc, (unsigned)src_rate, src_ch);
        cfg.perf_type = ESP_ASRC_PERF_TYPE_SW_SPEED;
        orc = esp_asrc_open(&cfg, &s_asrc);
    }
    if (orc != ESP_ASRC_ERR_OK || !s_asrc) {
        s_asrc = NULL;
        ESP_LOGE(TAG, "asrc open failed (%d): %u Hz %uch -> %d Hz %dch",
                 (int)orc, (unsigned)src_rate, src_ch, AUDIO_OUT_RATE, AUDIO_OUT_CHANNELS);
        return false;
    }
    if (esp_asrc_get_bytes_per_sample(s_asrc, &s_in_smp_bytes, &s_out_smp_bytes) != ESP_ASRC_ERR_OK ||
        s_in_smp_bytes == 0 || s_out_smp_bytes == 0) {
        asrc_close();
        return false;
    }

    /*
     * 走没走硬件：AUTO 只在"真的换了采样率 + 采样率在硬件表里 + 1~2ch + 16bit"
     * 时才让硬件上（esp_asrc.c 的 asrc_hw_pre_check）；只做声道上混（48k mono→stereo）
     * 时它落回软件的声道转换 —— 那个只做复制，很便宜，不需要麻烦硬件。
     * 这里的判断只用于**日志与分片大小**，不参与功能决策。
     */
    s_asrc_hw = (cfg.perf_type != ESP_ASRC_PERF_TYPE_SW_SPEED) &&
                (src_rate != AUDIO_OUT_RATE) && asrc_hw_rate_ok(src_rate) && src_ch <= 2;
    /*
     * 分片大小：软件路径固定 512 帧；硬件路径还要再按 ASRC_DMA_BLOCK_MAX 压一层。
     *
     * 硬件路径的输入字节数 = 帧数 × 源每帧字节，输出字节数 =（帧数 × 目标率/源率 + 16）
     * × 目标每帧字节 —— **两者都必须 ≤ 4080**，否则 GDMA 描述符会超过 1 块，
     * 之后就踩上库的"按历史最大块数挂载"那个陷阱（详见 ASRC_DMA_BLOCK_MAX 的注释）。
     * 16 是库里 RX DMA 额外挂的补偿帧数（ASRC_NUM_BUF_COMP_SZ，内部常量）。
     */
    uint32_t chunk = s_asrc_hw ? ASRC_HW_FRAMES : ASRC_SW_FRAMES;
    if (s_asrc_hw) {
        uint32_t lim_in  = ASRC_DMA_BLOCK_MAX / s_in_smp_bytes;
        uint32_t lim_out = ASRC_DMA_BLOCK_MAX / s_out_smp_bytes;
        lim_out = (lim_out > 16) ? (lim_out - 16) : 1;
        const uint32_t by_out = (uint32_t)((uint64_t)lim_out * src_rate / AUDIO_OUT_RATE);
        const uint32_t lim = (lim_in < by_out) ? lim_in : by_out;
        if (chunk > lim) {
            chunk = lim;
        }
        chunk &= ~7u;               /* 取 8 帧整数倍，日志好读 */
        if (chunk < 8) {
            chunk = 8;              /* 兜底：极端比率下也不能是 0 */
        }
    }
    s_chunk_frames = chunk;

    uint32_t out_frames = 0;
    if (esp_asrc_get_out_sample_num(s_asrc, s_chunk_frames, &out_frames) != ESP_ASRC_ERR_OK ||
        out_frames == 0) {
        asrc_close();
        return false;
    }
    /*
     * ★★ 2026-10 **回退**：这里曾经"自己算一遍、取较大者"（理想值兜底），结果真机上
     *    把 SW 重采样器害得空转：
     *
     *      W audio_out: asrc out cap 2357 < ideal 2358 (44100 Hz -> 48000 Hz): 用理想值兜底
     *      I audio_out: asrc ready (hw): 44100 Hz 2ch -> 48000 Hz 2ch, in 2048 frames -> out cap 2422
     *      I audio_out: play stats: PCM 41 KB/s | 产 1143/5115 ms (22.3%) | asrc 5166 ms
     *      E task_wdt: IDLE1 did not reset ... CPU 1: audio_out
     *      --- fa_resample_process  ← 一直卡在重采样里
     *
     *    容量给大之后，这个库会**试图填满整个输出缓冲**（而不是"按输入算该出多少"），
     *    于是每块都多跑一大截、100% 占核 → 生产端只剩 22%、蓝牙侧 `7 f/s`。
     *    教训：这个值的语义是"库自己算好的**最大**需要多少"，比它大并不安全。
     *
     *    所以：**以库给的值为主，只加固定小余量（+64 帧，就是老代码的取值）**，
     *    绝不按"理论最大值"去放大。当年怀疑它偏小（怕静默截断）的顾虑，改由下面
     *    `esp_asrc_process()` 之后的**饱和自检**（`out_frames >= cap` 时打日志）来兜 ——
     *    真截断了我们能从日志看见，而不是靠猜着放大缓冲。
     */
    /*
     * 输出容量：库算出来的最大输出帧 + 一点余量，再**按 cache line 向上取整**。
     *
     * 取整是为了硬件路径：esp_asrc_hw_process() 会拿 "*out_sample_num × 每帧字节"
     * 去做 PSRAM 对齐检查（asrc_adapter.c: asrc_hw_check_buffer_alignment），
     * 长度不是 cache line 整数倍就直接返回 ESP_ASRC_ERR_INVALID_ALIGNMENT。
     * 我们把缓冲放内部 RAM 时这条检查会被跳过，但取整之后"万一退回 PSRAM"也依然合法。
     *
     * 硬件路径还会额外要 ASRC_NUM_BUF_COMP_SZ(16) 帧的补偿空间（RX DMA 的描述符
     * 按 out_num+16 帧挂），而库对 BYPASS 类型不会把 16 帧算进 get_out_sample_num ——
     * 所以余量给 64 帧，两种类型都够。
     */
    size_t align = 64;
    esp_asrc_buffer_alignment_t al = {};
    if (esp_asrc_get_buffer_alignment(&al) == ESP_ASRC_ERR_OK && al.outbuf_addr_align >= 4) {
        align = al.outbuf_addr_align;
    }
    size_t cap_sz = ((size_t)out_frames + 64) * s_out_smp_bytes;
    cap_sz = (cap_sz + align - 1) & ~(align - 1);
    /* 取整可能切出半个采样帧（align 不是每帧字节的整数倍时，例如 align=32 而
     * 每帧 24 字节）。本工程输出恒为 16bit×2ch=4 字节、align 又是 2 的幂，所以
     * 这里不会进循环；留着是为了不让这类组合把任务卡死。 */
    for (int guard = 0; guard < 16 && (cap_sz % s_out_smp_bytes) != 0; guard++) {
        cap_sz += align;
    }
    s_out_frames_cap = (uint32_t)(cap_sz / s_out_smp_bytes);
    s_in_cap = (size_t)s_chunk_frames * s_in_smp_bytes;

    s_in_buf  = asrc_buf_alloc(s_in_cap, align, "asrc in");
    s_out_buf = asrc_buf_alloc(cap_sz, align, "asrc out");
    if (!s_in_buf || !s_out_buf) {
        ESP_LOGE(TAG, "asrc buffer alloc failed (in %u B / out %u B)",
                 (unsigned)s_in_cap, (unsigned)cap_sz);
        asrc_close();
        return false;
    }

    /* 把"进/出帧数"一起打出来：非整数比时 out/in 应当 ≈ 目标率 ÷ 源率
     * （44.1k→48k ≈ 1.088）。这个比值不对 = 重采样配置错了，声音必然不对。
     * `asrc ready (hw)` = 真的走 S31 的 ASRC 外设；`(sw)` = 软件路径（会吃 CPU）。 */
    ESP_LOGI(TAG, "asrc ready (%s): %u Hz %uch -> %d Hz %dch | in %u frames/call, "
                  "out cap %u frames | buf in %u B, out %u B, align %u, timeout %d ms",
             s_asrc_hw ? "hw" : "sw", (unsigned)src_rate, src_ch,
             AUDIO_OUT_RATE, AUDIO_OUT_CHANNELS,
             (unsigned)s_chunk_frames, (unsigned)s_out_frames_cap,
             (unsigned)s_in_cap, (unsigned)cap_sz, (unsigned)align, ASRC_TIMEOUT_MS);
    return true;
}

/**
 * 采样率/声道随时可能就绪或变化（解码器是异步打开的，AAC 甚至逐帧上报），
 * 所以每轮都问一次；只有真的变了才重建 ASRC。
 *
 * ⚠ **调用者必须已经持有会话锁**（`out_task` 就是这么做的）：
 * `espaudiocore_get_format()` 读的是库内部的会话对象，而 `audio_engine_stop()` 会在
 * 持锁状态下把它释放掉；同时这里还会 `asrc_close()/asrc_open()` 动 ASRC 与它的缓冲。
 * 不持锁就会与那次拆除撞车（2026-10 真机 double-free，见 out_task 上面的长注释）。
 */
static void pump_format(void)
{
    uint32_t rate = 0;
    uint8_t ch = 0, bits = 0;

    const esp_err_t ferr = espaudiocore_get_format(&rate, &ch, &bits);
    if (ferr != ESP_OK || rate == 0 || ch == 0) {
        return;
    }
    if (rate == s_src_rate && ch == s_src_ch) {
        return;
    }

    asrc_close();
    s_src_rate = rate;
    s_src_ch = ch;
    s_src_bits = bits;
    ESP_LOGI(TAG, "source format: %u Hz %uch (bits=%u; ringbuf 恒为 16-bit)", (unsigned)rate, ch, bits);

    if (rate == AUDIO_OUT_RATE && ch == AUDIO_OUT_CHANNELS) {
        return;     /* 直通，不需要重采样 */
    }
    asrc_open(rate, ch);
}

/* ===========================================================================
 * 写硬件
 * =========================================================================*/

/*
 * 把一块 PCM 写到硬件。**必须在会话锁之外调用**（见 out_task 的说明）：
 * 写 I2S/蓝牙会阻塞几十毫秒，锁里做会把 LVGL 一起卡住。
 * route/vol 由调用方在锁内取好快照传进来（差一块的延迟无所谓）。
 */
static void write_out(const uint8_t *pcm, size_t bytes, audio_out_t route, int vol)
{
    const int64_t t0 = esp_timer_get_time();

    if (route == AUDIO_OUT_SPEAKER) {
        esp_codec_dev_handle_t dev = bsp_audio_codec();
        if (dev && esp_codec_dev_write(dev, (void *)pcm, bytes) != ESP_OK) {
            s_codec_err++;
        }
    } else if (route == AUDIO_OUT_BLUETOOTH) {
        if (vol > 0) {
            /*
             * 音量有两条路，**只能走一条**（同时走就是平方衰减）：
             *   - AVRCP 绝对音量可用（bt_service_volume_is_avrcp()）：音量早就以
             *     SetAbsoluteVolume 发给耳机了，**由对端自己衰减** —— 这里原样送 PCM。
             *   - 不可用（AVRCP Controller 没连上 / 对端不支持）：退回逐样本软件缩放。
             * 判据是 bt_service 里上一次 set_absolute 的返回值，见 bts_volume.cc。
             */
            if (vol < 100 && !bt_service_volume_is_avrcp()) {
                uint8_t *dst = bt_stage(bytes);
                if (dst) {
                    memcpy(dst, pcm, bytes);
                    pcm16_scale(dst, bytes / 2, vol);
                    bt_service_write_pcm(dst, bytes);
                } else {
                    bt_service_write_pcm(pcm, bytes);
                }
            } else {
                bt_service_write_pcm(pcm, bytes);
            }
        }
        /* vol == 0：静音，不推数据，但时间轴照走（与 GMF 时代一致） */
    }
    /* AUDIO_OUT_NONE：只走时间轴 */

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_played_bytes += bytes;
    /*
     * "已出声"标志**无条件**置位（连 AUDIO_OUT_NONE 静音路也算）：
     * 它的语义是"这一首的 PCM 已经流起来了、解码链是通的"，UI 靠它把
     * "缓冲中…"换回歌词。静音时若一直不置位，界面会永远停在"缓冲中"。
     * （GMF 时代的 out_write_cb 也是在函数开头无条件置位的。）
     */
    s_flowing = true;

    s_stats_bytes += bytes;
    s_stats_calls++;
    s_stats_codec_us += (uint64_t)(esp_timer_get_time() - t0);
    if (s_stats_last_ms) {
        const uint32_t gap = now - s_stats_last_ms;
        if (gap > s_stats_gap_max_ms) {
            s_stats_gap_max_ms = gap;
        }
    }
    s_stats_last_ms = now;
}

/**
 * 每 5 秒一行播放健康度：确认"是不是真的在按时出声、卡在哪一段"。
 *
 * 关键一列是 `产 N ms / 墙 W ms`（交付的音频时长 ÷ 窗口时长）：
 *   - ≈100%：跟得上；
 *   - <99%：**产能不足**，DMA 会被慢慢抽干 → 一卡一卡。这时看后面三列定位：
 *       `asrc` 大 ⇒ 重采样太贵。先看末尾标记是 `(asrc-hw)` 还是 `(asrc-sw)`：
 *                  `(asrc-hw)`：它应该只有几百微秒/窗口，括号里的 `cpu` 也≈0
 *                  （任务阻塞在 GDMA 完成事件上，不占核）。若它反而很大，说明
 *                  卡在 DMA/中断上，去看 `E ESP_HW_ASRC: HW ASRC process timeout`；
 *                  `(asrc-sw)`：再看括号里的 `cpu` ——
 *                  `asrc` ≈ `cpu` ⇒ 软件重采样本身就这么慢（正是这次被换掉的东西）；
 *                  `asrc` ≫ `cpu` ⇒ **CPU 被同核的别人抢走了**（去找那个任务/中断）；
 *       `wait` 大 ⇒ 解码端供不上（SD/解码/任务被抢，常见于 Wi-Fi 在 core 0 忙）；
 *       `codec` 接近 100% 而前两项都不大 ⇒ 正常（写硬件本来就是被 DMA 节流的）。
 */
static void stats_tick(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_stats_ms == 0) {
        s_stats_ms = now;
        return;
    }
    const uint32_t win = now - s_stats_ms;
    if (win < STATS_WINDOW_MS) {
        return;
    }
    const uint32_t audio_ms = (uint32_t)(s_stats_bytes / AUDIO_OUT_BYTES_PER_MS);
    const uint32_t permille = win ? (uint32_t)((uint64_t)audio_ms * 1000ull / win) : 0;
    const uint32_t kbps = (uint32_t)((uint64_t)s_stats_bytes * 1000ull / win / 1024ull);
    const uint32_t ring_free = s_pcm_rb ? (uint32_t)xRingbufferGetCurFreeSize(s_pcm_rb) : 0;
    /* ASRC 真正吃掉的 CPU 时间（周期数换算；被抢占的时段不计）。
     * 频率用编译期常量：本项目 CONFIG_PM_ENABLE 没开（无 DFS），CPU 一直跑在这个频率上。 */
    const uint32_t cpu_khz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000u;
    const uint64_t asrc_cpu_ms = cpu_khz ? (s_stats_asrc_cyc / cpu_khz) : 0;
    ESP_LOGI(TAG, "play stats: PCM %u KB/s | 产 %u ms / 墙 %u ms (%u.%u%%) | codec %llu ms, "
                  "asrc %llu ms (cpu %llu ms), wait %llu ms | gap max %u ms, calls %u, "
                  "ring free %u B, route %d, src %u Hz %uch%s%s",
             (unsigned)kbps, (unsigned)audio_ms, (unsigned)win,
             (unsigned)(permille / 10), (unsigned)(permille % 10),
             (unsigned long long)(s_stats_codec_us / 1000),
             (unsigned long long)(s_stats_asrc_us / 1000),
             (unsigned long long)asrc_cpu_ms,
             (unsigned long long)(s_stats_wait_us / 1000),
             (unsigned)s_stats_gap_max_ms, (unsigned)s_stats_calls, (unsigned)ring_free,
             (int)s_route, (unsigned)s_src_rate, s_src_ch,
             s_asrc ? (s_asrc_hw ? " (asrc-hw)" : " (asrc-sw)") : "",
             s_codec_err ? " [codec write errors!]" : "");

    /*
     * 硬件路径的承诺是"ASRC 每 5 秒只花几百微秒"。若请求了硬件却仍占掉两成以上
     * 墙钟，说明 AUTO 在 esp_asrc 内部**已经退回软件**（典型原因：S31 只有 2 个
     * ASRC stream，两路都被 bt_service 之类占满；或者该采样率后来落到软件模块上）——
     * 上面那行日志还写着 (asrc-hw)，所以这里必须点名，免得被标记误导。
     */
    const uint64_t win_us = (uint64_t)win * 1000ull;
    if (s_asrc_hw && win_us && (s_stats_asrc_us * 100ull) > (win_us * 20ull)) {
        ESP_LOGW(TAG, "asrc: 标称硬件但本窗口重采样占 %llu%% 墙钟（应亚毫秒级）——"
                      "AUTO 很可能在 esp_asrc 内部退回了软件（硬件 stream 被占？）",
                 (unsigned long long)(s_stats_asrc_us * 100ull / win_us));
    }

    s_stats_ms = now;
    s_stats_bytes = 0;
    s_stats_codec_us = 0;
    s_stats_asrc_us = 0;
    s_stats_asrc_cyc = 0;
    s_stats_wait_us = 0;
    s_stats_calls = 0;
    s_stats_gap_max_ms = 0;
}

/* ===========================================================================
 * 输出任务
 * =========================================================================*/

/**
 * 锁内调用：把一块源 PCM 变成"可以直接写硬件的 48k/16/2 PCM"。
 *
 * 返回的指针**在锁外使用也是安全的**：
 *   - 直通（48k/2ch）时返回 ringbuf item 本身 —— 那块内存归 ringbuf 所有，
 *     直到 out_task 调 `vRingbufferReturnItem` 才失效（写完之后才还）；
 *   - 需要重采样时，把 ASRC 的输出**拷进 s_stage**（本任务私有、引擎永不释放），
 *     而不是直接用 `s_out_buf`（它会被 `audio_output_session_end()` 释放）。
 * 这一步就是为了"写硬件不用占着会话锁"——写 I2S/蓝牙要阻塞几十毫秒，
 * 占锁会把 LVGL 线程一起拖住（就是之前"UI 卡死"的来源之一）。
 */
static const uint8_t *prepare_block(uint8_t *data, size_t bytes, size_t *out_bytes)
{
    const size_t frame_bytes = (size_t)(s_src_ch ? s_src_ch : AUDIO_OUT_CHANNELS) * 2;
    bytes -= bytes % frame_bytes;      /* 半帧不处理（正常不会出现） */
    if (bytes == 0) {
        return NULL;
    }

    if (s_asrc && s_out_buf) {
        /*
         * ★★ 2026-10 修（真机现象："放久了卡顿 → asrc 100% CPU → task_wdt"）：
         *
         *   `s_out_frames_cap` 是 asrc_open() 按 **s_chunk_frames 个输入帧**
         *   向库要来的容量（日志 `asrc ready ... in 1024 frames/call, out cap N frames`），
         *   而 ringbuf 一次交给我们的块可能是 **1024 帧**。直接整块喂进去，库当场拒绝：
         *     E ESP_AE_RATE_CVT: The output resample sample buffer is too small(749).
         *     W audio_out: asrc process failed (1024 in-frames, #1)
         *   更要命的是：**这个实例一旦失败过就不再恢复** —— 之后每次都空转
         *   （`asrc 4985 ms / 5003 ms` ≈ 100% 一个核）→ 生产端只剩 40% → 卡顿 →
         *   两个 IDLE 都喂不到狗 → `task_wdt`。
         *
         *   所以：**按库声明的块大小分片喂**（每片输入帧数 = 要容量时用的那个数），
         *   容量就永远够、永远不会踩上面那条错误。
         */
        const uint32_t chunk_max_bytes = (uint32_t)s_chunk_frames * (uint32_t)frame_bytes;
        size_t done     = 0;      /* 已处理的输入字节数 */
        size_t produced = 0;      /* 已写进 s_stage 的输出字节数 */

        while (done < bytes) {
            const size_t left  = bytes - done;
            const size_t chunk = (left > chunk_max_bytes) ? chunk_max_bytes : left;
            const uint32_t in_frames = (uint32_t)(chunk / frame_bytes);
            uint32_t out_frames = s_out_frames_cap;

            if (in_frames == 0 || !s_in_buf ||
                (size_t)in_frames * frame_bytes > s_in_cap) {
                break;
            }
            /*
             * 先整片拷进本任务私有的 s_in_buf（内部 RAM），再喂 ASRC：
             *   - 硬件路径：GDMA 读的必须是内部 RAM（或按 cache line 对齐的 PSRAM），
             *     而 PCM ringbuf 在 PSRAM 里、切出来的片也不保证对齐 —— 拷一遍就把
             *     "地址/长度对齐 + cache 一致性"整类问题挡在门外；
             *   - 软件路径：只是让每次调用都从对齐地址起步（4 KB ≈ 50 µs，可忽略）。
             */
            memcpy(s_in_buf, data + done, (size_t)in_frames * frame_bytes);

            const int64_t  t0 = esp_timer_get_time();
            const uint32_t c0 = esp_cpu_get_cycle_count();
            const esp_asrc_err_t rc = esp_asrc_process(s_asrc, s_in_buf, in_frames,
                                                      s_out_buf, &out_frames);
            /* 周期只统计"这颗核真的执行了"的时间（被抢占的时段不计），见 s_stats_asrc_cyc */
            s_stats_asrc_cyc += (uint32_t)(esp_cpu_get_cycle_count() - c0);
            s_stats_asrc_us  += (uint64_t)(esp_timer_get_time() - t0);

            if (rc != ESP_ASRC_ERR_OK || out_frames == 0) {
                s_asrc_err = s_asrc_err + 1;  /* volatile 上不能用 ++（C++20 起 -Werror=volatile） */
                if ((s_asrc_err % 200) == 1) {
                    ESP_LOGW(TAG, "asrc process failed (%u in-frames, cap %u, #%u)",
                             (unsigned)in_frames, (unsigned)s_out_frames_cap,
                             (unsigned)s_asrc_err);
                }
                return NULL;      /* 这一块丢掉：宁可少一块，也绝不让它在失败态里空转 */
            }

            /*
             * ★ 截断自检（2026-10 加）：`esp_asrc_process()` 若"产出正好等于容量"，说明
             *   缓冲区被写满、**多出来的样本被静默丢掉** —— 那正是"卡顿 / 速度不对"的
             *   来源之一。撞上就报出来（限频）。
             */
            if (out_frames >= s_out_frames_cap) {
                s_asrc_sat = s_asrc_sat + 1;
                if ((s_asrc_sat % 200) == 1) {
                    ESP_LOGW(TAG, "asrc output saturated (%u in-frames -> cap %u, #%u)："
                                  "本块样本被截断", (unsigned)in_frames,
                             (unsigned)s_out_frames_cap, (unsigned)s_asrc_sat);
                }
            }

            const size_t n = (size_t)out_frames * s_out_smp_bytes;
            if (!stage_ensure(produced + n)) {
                return NULL;
            }
            memcpy(s_stage + produced, s_out_buf, n);
            produced += n;

        /* 见 s_asrc_in_frames 的说明：每 ~900 次调用（≈5 秒）打一次采样率比 */
        s_asrc_in_frames += in_frames;
        s_asrc_out_frames += out_frames;
        s_asrc_calls++;
        if ((s_asrc_calls % 900u) == 0u) {
            const uint32_t permille = (uint32_t)((uint64_t)s_asrc_out_frames * 1000ull /
                                                (uint64_t)(s_asrc_in_frames ? s_asrc_in_frames : 1u));
            ESP_LOGI(TAG, "asrc ratio: %u calls, in %u frames -> out %u frames, "
                     "out/in = %u/1000（96k->48k 应为 500，44.1k->48k 应为 1088）",
                     (unsigned)s_asrc_calls, (unsigned)s_asrc_in_frames,
                     (unsigned)s_asrc_out_frames, (unsigned)permille);
        }
            done     += chunk;
        }

        *out_bytes = produced;
        return s_stage;
    }

    *out_bytes = bytes;
    return data;
}

/*
 * 输出任务
 *
 * ★★ 2026-10 真机崩过一次（`assert failed: tlsf_free ... "block already marked as
 * free"`，正好在"换曲"那一瞬间），根因就是这里**没有和会话拆除互斥**：
 *
 *   - 引擎在 `audio_engine_stop_locked()` 里持锁做 `espaudiocore_stop()`（释放库内部
 *     的会话对象）→ `audio_output_session_end()` → `asrc_close()`（释放 ASRC 与它的
 *     输入/输出缓冲）；
 *   - 而本任务当时可能正在 `pump_format()` 里调 `espaudiocore_get_format()`
 *     （读的正是那个被释放的对象），或者正在 `play_block()` 里用 `esp_asrc_process()`
 *     跑同一份 ASRC 状态、往同一块 `s_out_buf` 里写。
 *
 * 两条都是 use-after-free / double-free。**光是"检查一下再调用"不够**：检查通过之后
 * 对象仍可能被拆掉（TOCTOU）。所以规矩是：
 *
 *   凡是"读库 / 用 ASRC"的动作，**必须持会话锁，并在锁内重新确认会话还在**；
 *   引擎侧的拆除本来就持着这把锁，于是两者被严格串行化，窗口彻底消失。
 *
 * ⚠ 但**写硬件不占锁**（2026-10 真机"UI 卡死"之后改的）：`esp_codec_dev_write()` /
 *   `bt_service_write_pcm()` 会阻塞几十毫秒（被 DMA/链路节流），占着锁会把任何
 *   顺手调 `audio_service_*` 的 LVGL 调用一起拖住 —— 表现就是界面卡住不动。
 *   所以流程是：**锁内**取数 + 重采样（结果拷进本任务私有的 s_stage）→ **锁外**写硬件。
 */
static void out_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "output task started on core %d", xPortGetCoreID());

    for (;;) {
        const uint32_t gen = s_gen;
        if (gen == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* 暂停时不消费：缓冲里那点音频留着，恢复后接着播（见 audio_internal.h） */
        if (!audio_engine_flow_gate()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t want = s_in_cap ? s_in_cap : 4096;
        size_t got = 0;
        const int64_t tw = esp_timer_get_time();
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_pcm_rb, &got, pdMS_TO_TICKS(100), want);
        if (!item || got == 0) {
            /* 取不到数据：统计"等数据"的耗时（这一列大 = 解码端供不上，不是重采样的锅） */
            s_stats_wait_us += (uint64_t)(esp_timer_get_time() - tw);
            /* 如果是"解码已到末尾"，说明积压播完了 → 上报 FINISHED */
            if (s_eos && gen == s_gen) {
                s_eos = false;
                audio_engine_output_drained(gen);
            }
            stats_tick();
            continue;
        }
        s_stats_wait_us += (uint64_t)(esp_timer_get_time() - tw);

        const uint8_t *pcm = NULL;
        size_t pcm_bytes = 0;
        audio_out_t route = AUDIO_OUT_SPEAKER;
        int vol = 100;

        audio_core_lock();
        if (s_gen == gen && audio_engine_flow_gate()) {
            pump_format();
            /*
             * 本地文件的"总时长兜底"：media_meta 的容器探测只覆盖 MP3/FLAC/WAV/ADTS，
             * M4A / OGG / Opus 那些歌的时长一直是 0（界面 `--:--`、进度条不动）——
             * 这里在锁内问一次解码库（它自己能算）。拿到后内部短路，不再重复问。
             */
            audio_engine_poll_duration();
            pcm = prepare_block(item, got, &pcm_bytes);
            route = s_route;
            vol = s_volume;
        }
        audio_core_unlock();

        if (pcm && pcm_bytes) {
            write_out(pcm, pcm_bytes, route, vol);   /* ← 锁外：写几十毫秒也不拖 UI */
        }
        vRingbufferReturnItem(s_pcm_rb, item);
        stats_tick();
    }
}

/* ===========================================================================
 * 对外（组件内部）接口
 * =========================================================================*/

esp_err_t audio_output_init(void)
{
    if (s_pcm_rb) {
        return ESP_OK;      /* 幂等 */
    }
    /* PCM 缓冲放 PSRAM：内部 RAM 要留给射频（见 main.cc 的 RADIO_MIN_INTERNAL 检查） */
    s_pcm_rb = xRingbufferCreateWithCaps(AUDIO_PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_rb) {
        ESP_LOGW(TAG, "PSRAM ringbuf failed, falling back to internal RAM");
        s_pcm_rb = xRingbufferCreate(AUDIO_PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    }
    if (!s_pcm_rb) {
        ESP_LOGE(TAG, "PCM ringbuf create failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(out_task, "audio_out", OUT_TASK_STACK, NULL, OUT_TASK_PRIO,
                                &s_task, OUT_TASK_CORE) != pdPASS) {
        vRingbufferDelete(s_pcm_rb);
        s_pcm_rb = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

RingbufHandle_t audio_output_pcm_ring(void)
{
    return s_pcm_rb;
}

void audio_output_session_begin(uint32_t gen)
{
    ring_drain();
    asrc_close();
    s_src_rate = 0;
    s_src_ch = 0;
    s_eos = false;
    s_flowing = false;
    s_played_bytes = 0;
    s_tl_base_ms = 0;
    s_asrc_err = 0;
    s_asrc_sat = 0;
    s_stats_ms = 0;
    s_stats_bytes = 0;
    s_stats_codec_us = 0;
    s_stats_asrc_us = 0;
    s_stats_asrc_cyc = 0;
    s_stats_wait_us = 0;
    s_stats_calls = 0;
    s_stats_gap_max_ms = 0;
    s_stats_last_ms = 0;
    s_codec_err = 0;
    if (s_route == AUDIO_OUT_SPEAKER) {
        bsp_audio_set_mute(false);      /* 起播解锁静音（stop 时才会静音） */
    }
    s_gen = gen;                        /* 最后置：置上之后任务才开始消费 */
}

void audio_output_session_end(void)
{
    ESP_LOGI(TAG, "session end (gen %u): drain + asrc teardown", (unsigned)s_gen);
    s_gen = 0;
    s_eos = false;
    asrc_close();
    s_src_rate = 0;
    s_src_ch = 0;
    ring_drain();
    ESP_LOGI(TAG, "session end done");
}

void audio_output_note_eos(uint32_t gen)
{
    if (gen == s_gen) {
        s_eos = true;
    }
}

void audio_output_flush(void)
{
    ring_drain();
}

void audio_output_note_seek(uint32_t pos_ms)
{
    s_tl_base_ms = pos_ms;
    s_played_bytes = 0;
    s_flowing = false;      /* UI 据此重新挂上"缓冲中…" */
    s_eos = false;
}

uint32_t audio_output_position_ms(void)
{
    return s_tl_base_ms + (uint32_t)(s_played_bytes / AUDIO_OUT_BYTES_PER_MS);
}

/*
 * 当前解码源格式（解码器上报的，不是探测值）——"歌曲信息"弹窗用。
 *
 * 为什么要这个而不是 `audio_service_get_info()`：本地文件的 info 来自 media_meta 探测，
 * 而**在线流的 info 是空的**（探测只对文件做）—— 弹窗里采样率/声道/位深就会显示
 * "--"。这里读的是输出级每块都在刷的 `s_src_*`（`pump_format()` 写的），在线/本地
 * 都有值。纯读，不加锁（它是几个字节的写，读到半新半旧也只是显示上的一帧）。
 */
bool audio_output_source_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    if (!s_src_rate) {
        return false;
    }
    if (rate) {
        *rate = s_src_rate;
    }
    if (channels) {
        *channels = s_src_ch;
    }
    if (bits) {
        *bits = s_src_bits;
    }
    return true;
}

void audio_output_set_route(audio_out_t out)
{
    if (out == s_route) {
        return;
    }

    switch (out) {
    case AUDIO_OUT_SPEAKER:
        bt_service_media_stop();
        bsp_audio_set_mute(false);
        audio_output_pa(audio_engine_running());
        break;

    case AUDIO_OUT_BLUETOOTH:
        /* 先掐掉板载出声再起蓝牙（左右共用一个功放使能，同时出声就是回声） */
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        bt_service_media_start();
        /*
         * 切到蓝牙就把当前音量下发给对端（AVRCP 绝对音量）。
         * 不做这一步，用户之前在扬声器下把音量调到 30% 再切蓝牙，耳机仍按它
         * 自己的音量放 —— 一耳朵就是巨响（这也是"音量为平方"之外最容易被忽略的一处）。
         * 若此刻 AVRCP 还没连上，bt_service 只缓存不发，流启动时会再补一次。
         */
        bt_service_set_volume(s_volume);
        break;

    case AUDIO_OUT_NONE:
    default:
        bsp_audio_set_mute(true);
        bsp_audio_pa_enable(false);
        break;
    }

    s_route = out;
    ESP_LOGI(TAG, "output route -> %d", (int)out);
}

audio_out_t audio_output_route(void)
{
    return s_route;
}

void audio_output_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (s_route == AUDIO_OUT_SPEAKER) {
        bsp_audio_set_volume(percent);   /* 扬声器：ES8389 数字音量寄存器 */
    } else if (s_route == AUDIO_OUT_BLUETOOTH) {
        /*
         * 蓝牙：**AVRCP 绝对音量**（esp_bt_audio_vol.h，见 bt_service/bts_volume.cc）。
         * 由对端去衰减，我们不碰 PCM；只有它不可用时 write_out() 才退回软件缩放。
         */
        bt_service_set_volume(percent);
    }
}

/*
 * 对端（耳机按键 / 手机）通过 AVRCP 改了音量：只同步本地状态。
 *
 * ⚠ 这里**绝对不能**再调 bt_service_set_volume()：那会把值原样发回对端，
 *   对端又回一条 VOLUME_CHANGE 通知 —— 音量在两个设备之间来回刷（打乒乓）。
 *   发起的那个方向（用户在本机改音量）才是唯一的"指令源"。
 */
void audio_output_note_peer_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
}

int audio_output_volume(void)
{
    return s_volume;
}

bool audio_output_flowing(void)
{
    return s_flowing;
}

uint64_t audio_output_played_bytes(void)
{
    return s_played_bytes;
}

void audio_output_pa(bool on)
{
    bsp_audio_pa_enable(on);
}
