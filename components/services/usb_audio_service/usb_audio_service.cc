/*
 * usb_audio_service.cc —— USB 声卡（UAC 1.0 Speaker）输出
 *
 * 数据通路：audio_output.cc 的 out_task → usb_audio_write() → uac_host_device_write()
 *          → 等时端点（192 B/ms）→ USB 声卡。**与往板载 codec 写的完全是同一块 PCM**
 *          （输出级已经恒为 48 kHz/16bit/2ch），所以这里不做任何格式转换。
 *
 * 线程模型（照 usb_host_uac 官方 audio_player 例程）：
 *   usb_audio_task   —— usb_host_install() 之后泵 usb_host_lib_handle_events()，
 *                       并从事件队列里取"开/关声卡"的动作（打开设备会做控制传输，
 *                       不能在驱动回调里做，会卡住 UAC 的后台任务）；
 *   UAC 后台任务     —— uac_host_install(create_background_task=true) 建的，
 *                       驱动回调/设备回调都在这个任务里跑。
 *
 * ⚠ 拔出的处理是**两段式**的，这一点是需求（"必须立即切回内置输出"）：
 *   1. 设备回调（UAC 任务）里**立刻**把 s_ready 清 0 并通知上层 —— 只做标志，
 *      绝不去 close/重开（设备已经没了，控制传输会一直超时，那就不是"立即"了）；
 *   2. 真正的收尾（stop + close）交给 usb_audio_task 的事件队列，晚几十毫秒无妨。
 *   另外 audio_output 的写路径还有第三道保险：写失败就地回落板载（见 write_out）。
 */
#include "usb_audio_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/uac_host.h"

static const char *TAG = "usb_audio";

/* 与输出级的硬约定：进本组件的是 48 kHz / 2ch / **16-bit**（192 B/ms，全工程唯一时间轴） */
#define USB_AUDIO_RATE       48000
#define USB_AUDIO_CH         2
#define USB_AUDIO_BITS       16
#define USB_AUDIO_BYTES_MS   192

/* 泵 USB Host 库事件的任务（只做这一件事，绝不阻塞） */
#define USB_LIB_PRIO         5
#define USB_LIB_STACK        3072
/* 装 UAC 驱动 / 开关设备 / 收状态的任务（允许阻塞） */
#define USB_ACTION_PRIO      5
#define USB_ACTION_STACK     4096
/* UAC 驱动自带的事件任务 */
#define UAC_TASK_PRIO        5
#define UAC_TASK_STACK       4096

/*
 * ⚠ 设备侧位深**不一定**是 16：不少 UAC 声卡 / 无线耳机接收器只暴露 24-bit alt
 *   （2026-10 实测：291D:385D "MCHOSE V9 PRO" 只有 `alt 1: 2ch 24bit`）。
 *   这种设备我们把 16-bit PCM **左移 8 位扩成 24-bit**（3 字节/样本，288 B/ms）再推。
 *   只是装进更宽的容器、不增加信息量，但设备只认这个格式，没得选。
 *   16/24/32 都支持，subframe 字节数在 open 时按设备实际能力定（见 s_sub）。
 */

/*
 * UAC 环形缓冲：19200 B 满 / 9600 B 阈值。
 * 换算成时间随位深变：16bit ⇒ 100 ms、24bit ⇒ 66 ms、32bit ⇒ 50 ms，都够深。
 * 太浅（<20 ms）会被 USB 调度抖动抽干 → 断音；太深会让"切歌/暂停"有明显延迟。
 * 取这么深的另一个原因：输出级一次可能给来一块 >50 ms 的 PCM（直通路径直接用
 * ringbuf item），缓冲太浅就会 push 失败（返回 ESP_FAIL）。
 */
#define UAC_BUF_BYTES        19200
#define UAC_BUF_THRESHOLD    9600

/*
 * 每次真正 push 进 UAC 的块大小 = 10 ms（1920 B）。
 * 为什么要拆：`uac_host_device_write()` 是大块整推的（xRingbufferSend 要么全进要么
 * 全不进），一整块 50 ms 撞上"缓冲只剩 30 ms"时就会 ESP_FAIL —— 拆分后按 10 ms
 * 小步推进，天然跟着 USB 的消费速度走，既不断音也不误判。
 */
#define USB_WRITE_CHUNK      (USB_AUDIO_BYTES_MS * 10)

/* 写 USB 的阻塞上限：一块 192 B/ms、通常一块 20~50 ms 的数据，100 ms 足够，
 * 再长会把解码管线的节拍也拖住（跟 bt_service_write_pcm 的 200 ms 是同一个考虑）。 */
#define USB_WRITE_TIMEOUT_MS 100

typedef enum { EV_TX_CONNECTED = 1, EV_DEV_GONE } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint8_t   addr;
    uint8_t   iface;
} ev_t;

static volatile bool              s_ready;        /* 声卡已打开且在推流（写路径每块都读） */
static volatile bool              s_inited;
static uac_host_device_handle_t   s_dev;          /* 只在 usb_audio_task 里写 */
/*
 * 句柄锁：usb_audio_write() 在音频输出任务里跑，close_dev() 在 USB 任务里跑。
 * 没有这把锁，拔出瞬间可能"写"到正在被 close 的句柄上（use-after-free）。
 * 写最多阻塞 USB_WRITE_TIMEOUT_MS，所以 close 侧等 300 ms 足够。
 */
static SemaphoreHandle_t          s_dev_mtx;
static QueueHandle_t              s_evq;
static SemaphoreHandle_t          s_host_ready;   /* usb_lib 装完主机库后 give */
static usb_audio_state_cb_t       s_cb;
static void                      *s_cb_user;
static usb_audio_notice_cb_t      s_notice_cb;
static void                      *s_notice_user;
static char                       s_name[64] = "USB 声卡";
static int                        s_vol = 100;
/*
 * 设备实际的 subframe 字节数（2=16bit / 3=24bit / 4=32bit），open 时定，0=未起流。
 * 写路径按它把 16-bit PCM 扩成设备要的宽度。
 */
static volatile uint8_t           s_sub;

/*
 * 16-bit → 设备位深（24/32 位左对齐：高位在前，低位补 0）的转换缓冲。
 * 深度 = USB_WRITE_CHUNK/2 个样本 × 4 字节（960×4 = 3840 B，内部 RAM）。
 */
static uint8_t                    s_conv[USB_WRITE_CHUNK / 2 * 4];

/* ------------------------------------------------------------------ 小工具 */

/* 通知上层（只发一次状态，不做任何会阻塞的事） */
static void notify(bool ready)
{
    usb_audio_state_cb_t cb = s_cb;
    void *user = s_cb_user;
    if (cb) {
        cb(ready, user);
    }
}

/* 给用户的一句话（单生产者：USB 任务；单消费者：audio_service 抄走） */
static void notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notice(const char *fmt, ...)
{
    usb_audio_notice_cb_t cb = s_notice_cb;
    if (!cb) {
        return;
    }
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cb(buf, s_notice_user);
}

/* wchar_t（UCS-2/4）→ UTF-8。产品名基本都是 ASCII，遇到代理对就跳过。 */
static void wide_to_utf8(const wchar_t *w, char *out, size_t out_size)
{
    size_t o = 0;
    if (!out || out_size == 0) {
        return;
    }
    for (size_t i = 0; w && w[i] && o + 4 < out_size; i++) {
        uint32_t cp = (uint32_t)w[i];
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
        /* cp >= 0x10000（代理对/补平面）：直接跳过，产品名用不到 */
    }
    out[o] = '\0';
}

/* 某个 alt 是否支持 48 kHz（离散列表 or 连续范围） */
static bool alt_supports_48k(const uac_host_dev_alt_param_t *p)
{
    if (p->sample_freq_type > 0) {
        for (int i = 0; i < p->sample_freq_type; i++) {
            if (p->sample_freq[i] == USB_AUDIO_RATE) {
                return true;
            }
        }
        return false;
    }
    if (p->sample_freq_lower == 0 && p->sample_freq_upper == 0) {
        return false;     /* 没报范围 = 不可知，别猜 */
    }
    return USB_AUDIO_RATE >= p->sample_freq_lower && USB_AUDIO_RATE <= p->sample_freq_upper;
}

/* ------------------------------------------------------------------ 事件回调 */

/* 驱动回调：发现 UAC 播放/录音接口（UAC 后台任务上下文，不能阻塞） */
static void uac_driver_cb(uint8_t addr, uint8_t iface_num,
                          const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        ev_t e = { EV_TX_CONNECTED, addr, iface_num };
        if (s_evq) {
            xQueueSend(s_evq, &e, 0);
        }
    } else if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        /* 录音接口（麦克风）：本工程只做播放，忽略 */
        ESP_LOGI(TAG, "UAC mic interface %u found — ignored (playback only)", iface_num);
    }
}

/* 设备回调：**拔出通知的第一时间**就在这里 */
static void uac_device_cb(uac_host_device_handle_t dev, const uac_host_device_event_t event, void *arg)
{
    (void)dev;
    (void)arg;
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        /*
         * ★ 需求：播放中拔出必须**立即**切回内置输出。
         *   这里只清标志 + 通知（纯内存操作，微秒级），
         *   close/stop 交给 usb_audio_task（对已拔掉的设备做控制传输会一直超时）。
         */
        if (s_ready) {
            s_ready = false;
            ESP_LOGW(TAG, "USB audio %s -> not ready",
                     event == UAC_HOST_DRIVER_EVENT_DISCONNECTED ? "disconnected" : "transfer error");
            /*
             * 顺序要紧：先发"已拔出"（用户视角），
             * 再 notify(false) —— 如果当时正在用 USB，audio_service 会把它改写成
             * "USB 声卡已断开，已切回扬声器"，那句更准确。
             */
            notice("USB 声卡已拔出");
            notify(false);
        }
        {
            ev_t e = { EV_DEV_GONE, 0, 0 };
            if (s_evq) {
                xQueueSend(s_evq, &e, 0);
            }
        }
        break;
    default:
        break;      /* TX_DONE / RX_DONE：缓冲阈值事件，我们靠 write 阻塞即可，不处理 */
    }
}

/* ------------------------------------------------------------------ 开 / 关设备 */

static void close_dev(void)
{
    if (!s_dev) {
        return;
    }
    s_ready = false;      /* 先关门，新的写立刻返回 INVALID_STATE */
    if (s_dev_mtx) {
        (void)xSemaphoreTake(s_dev_mtx, pdMS_TO_TICKS(300));
    }
    (void)uac_host_device_stop(s_dev);
    (void)uac_host_device_close(s_dev);
    s_dev = NULL;
    s_sub = 0;
    if (s_dev_mtx) {
        xSemaphoreGive(s_dev_mtx);
    }
    snprintf(s_name, sizeof(s_name), "USB 声卡");
    ESP_LOGI(TAG, "USB audio device closed");
}

/*
 * 打开播放接口并起流。
 *
 * ⚠ **只允许在 usb_action_task 里调用**（里面对 AudioControl 的整定会阻塞若干毫秒到几秒）。
 *   **绝不能**在泵 `usb_host_lib_handle_events()` 的那个任务里做 —— 那会把 USB 主机库的
 *   设备状态机一起堵死，控制传输永远完不成（真机就是这么崩的，见文件头的说明）。
 */
static esp_err_t open_speaker(uint8_t addr, uint8_t iface)
{
    if (s_dev) {
        close_dev();          /* 换了一个设备：先收旧的 */
    }

    uac_host_device_handle_t dev = NULL;
    const uac_host_device_config_t cfg = {
        .addr             = addr,
        .iface_num        = iface,
        .buffer_size      = UAC_BUF_BYTES,
        .buffer_threshold = UAC_BUF_THRESHOLD,
        .callback         = uac_device_cb,
        .callback_arg     = NULL,
    };
    esp_err_t err = uac_host_device_open(&cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(err));
        return err;
    }

    uac_host_dev_info_t info;
    memset(&info, 0, sizeof(info));
    if (uac_host_get_device_info(dev, &info) == ESP_OK) {
        char nm[64] = {0};
        wide_to_utf8(info.iProduct, nm, sizeof(nm));
        if (nm[0]) {
            snprintf(s_name, sizeof(s_name), "%s", nm);
        }
        ESP_LOGI(TAG, "found UAC device %04X:%04X \"%s\" (iface %u, %u alt)",
                 info.VID, info.PID, s_name, iface, info.iface_alt_num);
    }

    /*
     * 挑 alt：条件只有三条 —— **2ch + 支持 48 kHz**，位深**16 优先**（最省带宽/CPU），
     * 设备只有 24/32 bit 的 alt 也接受（我们负责把 16-bit PCM 扩宽）。
     * `uac_host_device_start()` 内部会按 (channels, bit_resolution, sample_freq) 自己挑 alt。
     */
    /*
     * 选择顺序 = **24 → 16 → 32**（需求："先做判断，支持 24bit 就以 24bit 输出，
     * 不支持就回退到声卡默认"）。24 优先是因为绝大多数 UAC 声卡/耳机接收器都把它当
     * 主档位；没有 24 就退 16（最省），再没有就 32。都挑不到才判不可用。
     */
    static const uint8_t kBitsOrder[3] = { 24, 16, 32 };
    int alt_ok = -1;
    int bits_ok = 0;
    for (size_t oi = 0; oi < 3 && alt_ok < 0; oi++) {
        const int want_bits = kBitsOrder[oi];
        for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
            uac_host_dev_alt_param_t p;
            if (uac_host_get_device_alt_param(dev, alt, &p) != ESP_OK) {
                continue;
            }
            if (oi == 0) {
                ESP_LOGI(TAG, "  alt %u: %uch %ubit, %s",
                         alt, p.channels, p.bit_resolution,
                         p.sample_freq_type > 0 ? "discrete rates" : "continuous range");
            }
            if (p.channels == USB_AUDIO_CH && p.bit_resolution == want_bits && alt_supports_48k(&p)) {
                alt_ok = alt;
                bits_ok = want_bits;
                break;
            }
        }
    }
    if (alt_ok < 0) {
        ESP_LOGE(TAG, "this device has no 48 kHz/%uch/16|24|32-bit alt — unusable for us",
                 USB_AUDIO_CH);
        notice("USB 声卡不支持 48 kHz 立体声，无法使用");
        (void)uac_host_device_close(dev);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (bits_ok != USB_AUDIO_BITS) {
        ESP_LOGW(TAG, "picked %d-bit alt — 16-bit PCM will be widened (设备只认这个宽度)",
                 bits_ok);
    }

    const uac_host_stream_config_t scfg = {
        .channels       = USB_AUDIO_CH,
        .bit_resolution = (uint8_t)bits_ok,     /* 设备只认这个宽度就用这个宽度 */
        .sample_freq    = USB_AUDIO_RATE,
    };
    err = uac_host_device_start(dev, &scfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_start failed: %s", esp_err_to_name(err));
        (void)uac_host_device_close(dev);
        return err;
    }

    /* 音量交给设备侧衰减（与蓝牙 AVRCP 绝对音量同一个思路：不做逐样本软件缩放） */
    (void)uac_host_device_set_volume(dev, (uint8_t)s_vol);
    (void)uac_host_device_set_mute(dev, false);

    s_sub = (uint8_t)(bits_ok / 8);     /* 2/3/4 字节每样本 */
    s_dev = dev;
    s_ready = true;
    ESP_LOGI(TAG, "USB sound card ready: \"%s\" alt %d @ %u Hz/%uch/%ubit (sub %u B, %u B/ms)",
             s_name, alt_ok, USB_AUDIO_RATE, USB_AUDIO_CH, bits_ok,
             (unsigned)s_sub, (unsigned)(USB_AUDIO_CH * s_sub * (USB_AUDIO_RATE / 1000)));
    notice("USB 声卡已连接：%s（%dbit）", s_name, bits_ok);
    notify(true);
    return ESP_OK;
}

/* ------------------------------------------------------------------ 任务 */

/* ------------------------------------------------------------------ 任务
 *
 * ⚠⚠ **必须两个任务**，这是真机踩过的坑（2026-10-21）：
 *
 *   最初版只有一个任务：先 `usb_host_lib_handle_events(0, ...)` 顺手泵一下，
 *   再从这个队列取"开设备"的动作、**就地**调 `uac_host_device_open/start`。
 *   结果：open 里对 AudioControl 的整定（读 volume range 之类）一直等到 5 s 超时，
 *   而这 5 s 里**没人泵 USB Host 库事件** —— 主机库的设备状态机正是靠
 *   `usb_host_lib_handle_events()` 前进的，于是控制传输永远完不成：
 *
 *     E uac-host: Control Transfer Timeout
 *     E USB HOST: Get EP handle error: ESP_ERR_INVALID_ARG   ← EP0 没有 EP handle，HALT 必然失败
 *     E uac-host: Unable to submit control transfer          ← 上一个还挂在 in-flight
 *     E uac-host: uac_host_device_start failed: ESP_ERR_NOT_FINISHED
 *     assert failed: usbh_dev_close usbh.c:1334 (num_ctrl_xfers_inflight == 0)
 *
 *   正确结构（与 `usb_host_uac` 官方 audio_player 例程一致）：
 *     usb_lib_task    —— 只做 `usb_host_install()` + 阻塞式 `usb_host_lib_handle_events()`，
 *                        **绝不在里面做任何会阻塞的动作**；
 *     usb_action_task —— 装 UAC 驱动、收事件、open/close/push。会阻塞的活全在这里。
 * -------------------------------------------------------------------------*/

/* 只泵 USB Host 库事件（枚举 / 设备插拔 / 总线状态都靠它推进） */
static void usb_lib_task(void *arg)
{
    (void)arg;

    const usb_host_config_t hcfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&hcfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "usb_host_install: already installed (另一处已经装过 USB Host)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    if (s_host_ready) {
        xSemaphoreGive(s_host_ready);    /* 告诉 action 任务：可以装 UAC 驱动了 */
    }

    while (true) {
        uint32_t flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            (void)usb_host_device_free_all();
        }
    }
}

/* 打开失败时重试几次：第一次常常赶上枚举尾部还没彻底稳定 */
static void try_open(uint8_t addr, uint8_t iface)
{
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (open_speaker(addr, iface) == ESP_OK) {
            return;
        }
        if (attempt < 3) {
            ESP_LOGW(TAG, "open failed (%d/3), retry in 1.5 s", attempt);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    ESP_LOGE(TAG, "give up this USB sound card — unplug/replug to retry");
    notice("USB 声卡初始化失败，请拔插重试");
}

/* 处理"开/关设备"：这里阻塞没有关系（泵事件的是上面那个任务） */
static void usb_action_task(void *arg)
{
    (void)arg;

    if (!s_host_ready || xSemaphoreTake(s_host_ready, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGE(TAG, "usb host did not come up, UAC disabled");
        vTaskDelete(NULL);
        return;
    }

    const uac_host_driver_config_t ucfg = {
        .create_background_task = true,
        .task_priority          = UAC_TASK_PRIO,
        .stack_size             = UAC_TASK_STACK,
        .core_id                = 0,
        .callback               = uac_driver_cb,
        .callback_arg           = NULL,
    };
    esp_err_t err = uac_host_install(&ucfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UAC host installed, waiting for a USB sound card");

    while (true) {
        ev_t e;
        if (xQueueReceive(s_evq, &e, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        if (e.kind == EV_TX_CONNECTED) {
            try_open(e.addr, e.iface);
        } else if (e.kind == EV_DEV_GONE) {
            close_dev();
        }
    }
}

/* ------------------------------------------------------------------ 对外 API */

esp_err_t usb_audio_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_inited = true;

    s_evq = xQueueCreate(8, sizeof(ev_t));
    s_dev_mtx = xSemaphoreCreateMutex();
    s_host_ready = xSemaphoreCreateBinary();
    if (!s_evq || !s_dev_mtx || !s_host_ready) {
        ESP_LOGE(TAG, "event queue / mutex alloc failed");
        s_inited = false;
        return ESP_ERR_NO_MEM;
    }

    /* 先起 action（它等 usb_lib 装完主机库），再起泵事件的那个任务 */
    if (xTaskCreatePinnedToCore(usb_action_task, "usb_uac", USB_ACTION_STACK, NULL,
                                USB_ACTION_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "usb_uac task create failed");
        s_inited = false;
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", USB_LIB_STACK, NULL,
                                USB_LIB_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "usb_lib task create failed");
        s_inited = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool usb_audio_is_ready(void)
{
    return s_ready;
}

const char *usb_audio_device_name(void)
{
    return s_name;
}

int usb_audio_bit_depth(void)
{
    const uint8_t sub = s_sub;
    return sub ? (int)sub * 8 : 0;
}

/*
 * 把 16-bit 交错 PCM 扩成设备的 subframe 宽度（2=原样 / 3=24bit / 4=32bit）。
 * 24/32 位按**左对齐**：样本的 16 位放在高 16 位、低位补 0（这是 UAC 的通行做法，
 * Windows/macOS 把 16-bit 送进 24-bit 端点时也是这么补的）。
 * 逐字节读，避免 (int16_t*) 强转带来的对齐问题（pcm 来自 ringbuf，不保证 2 字节对齐）。
 */
static size_t pcm16_widen(const uint8_t *in, size_t samples, uint8_t sub, uint8_t *out)
{
    if (sub == 2) {
        memcpy(out, in, samples * 2);
        return samples * 2;
    }
    for (size_t i = 0; i < samples; i++) {
        const uint8_t lo = in[2 * i];
        const uint8_t hi = in[2 * i + 1];
        if (sub == 3) {
            *out++ = 0;      /* 低 8 位补 0 */
            *out++ = lo;
            *out++ = hi;
        } else {
            *out++ = 0;
            *out++ = 0;
            *out++ = lo;
            *out++ = hi;
        }
    }
    return samples * sub;
}

esp_err_t usb_audio_write(const void *pcm, size_t bytes, uint32_t timeout_ms)
{
    if (!pcm || bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只处理完整的 2ch/16bit 帧（4 字节）：半帧会让左右声道错位 */
    bytes &= ~(size_t)3;
    if (bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = USB_WRITE_TIMEOUT_MS;
    }
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = bytes;

    while (left > 0) {
        const size_t n16 = (left > USB_WRITE_CHUNK) ? USB_WRITE_CHUNK : left;
        const uint8_t sub = s_sub;
        const uint8_t *w = p;
        size_t n = n16;
        if (sub > 2) {                 /* 设备只认 24/32 bit：这里扩宽 */
            n = pcm16_widen(p, n16 / 2, sub, s_conv);
            w = s_conv;
        }
        if (s_dev_mtx && xSemaphoreTake(s_dev_mtx, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (s_ready && s_dev) {     /* 拿到锁后再确认一次：close_dev 可能刚跑过 */
            err = uac_host_device_write(s_dev, (uint8_t *)w, (uint32_t)n,
                                        pdMS_TO_TICKS(timeout_ms));
        }
        if (s_dev_mtx) {
            xSemaphoreGive(s_dev_mtx);
        }

        if (err == ESP_ERR_INVALID_STATE) {
            /*
             * ★ 只有这一个错误码代表"设备真的不在了"（接口状态不再是 ACTIVE）。
             *   立刻标记不可用 + 通知上层（audio_service 会把路由切回板载）。
             */
            if (s_ready) {
                s_ready = false;
                ESP_LOGW(TAG, "uac write: device gone -> not ready");
                notify(false);
            }
            return ESP_ERR_INVALID_STATE;
        }
        if (err != ESP_OK) {
            /*
             * ⚠ ESP_FAIL 是**环形缓冲满**（`_ring_buffer_push` 的 xRingbufferSend 失败），
             *   **不是**拔出！这种情况只丢这一小块，绝不能切路由 ——
             *   否则"USB 播放中偶尔抖一下"就会莫名其妙跳回扬声器。
             *   分块已经让这种情况很少见；真碰上就等下块再写。
             */
            ESP_LOGD(TAG, "uac write: ring full / %s, drop %u B", esp_err_to_name(err), (unsigned)n);
            return err;
        }
        p += n16;
        left -= n16;
    }
    return ESP_OK;
}

void usb_audio_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_vol = percent;
    if (s_dev) {
        (void)uac_host_device_set_volume(s_dev, (uint8_t)percent);
    }
}

void usb_audio_set_state_cb(usb_audio_state_cb_t cb, void *user)
{
    s_cb_user = user;
    s_cb      = cb;
}

void usb_audio_set_notice_cb(usb_audio_notice_cb_t cb, void *user)
{
    s_notice_user = user;
    s_notice_cb   = cb;
}
