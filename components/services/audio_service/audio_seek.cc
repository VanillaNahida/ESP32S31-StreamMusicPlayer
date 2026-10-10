/*
 * audio_seek.cc —— 拖动定位（本地与在线两条完全不同的路）
 *
 * ── 本地文件：用 espaudiocore 的库内 seek ─────────────────────────────────────
 * `espaudiocore_seek_ms()` 内部是"seek 源 → 复位解码器 → 继续解码"，串行在解码
 * 任务里完成，**帧对齐由解码器保证**。所以 GMF 时代那一整套
 * "平均码率 / 实测有效码率 / 时间→字节映射 / 落点核对 / 容器白名单"
 * （约 250 行）整体退休：本地文件不再有"定位失败：码率未知"这种失败模式。
 *
 * ── 在线流：ringbuf 输入不支持 seek，只能"重开会话" ──────────────────────────
 * 库的 API.md 明确写了 ringbuf 输入 `seek_ms()` 返回 ESP_ERR_NOT_SUPPORTED
 * （纯顺序流，源连 tell() 都没有）。所以在线定位走：
 *
 *   停解码会话 → 带 `Range: bytes=N-` 重开 HTTP → 重开解码会话
 *
 * N 由**平均码率**换算（Content-Length ÷ 时长，与 GMF 时代的绝对映射同一套算法）。
 * 两个已知边界：
 *   1. **只支持 MP3 / ADTS-AAC**：这两类解码器靠帧同步字就能从流中间接上；
 *      FLAC/WAV/M4A 需要容器头（STREAMINFO / RIFF / moov），从中间开始时解析不出来，
 *      所以对它们直接判"不可拖动"（宁可 UI 灰掉进度条，也不要拖过去没声音）。
 *   2. 落点是**最近的可解码帧**，与请求时刻可能差几十毫秒（MP3 一帧 26 ms）——
 *      比 GMF 时代"标签算进音频长度导致早到几秒"要准得多。
 *
 * 落点核对（GMF 时代用 `esp_gmf_io_get_pos()` 反推真实落点）不再需要：
 * 本地由解码器保证，在线的落点由 HTTP Range 保证。`audio_seek_landing_ms()`
 * 保留接口但恒返回 false，上层（app_music）拿到 false 就什么都不做。
 */
#include "audio_internal.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaudiocore.h"

static const char *TAG = "audio_seek";

/** 在线定位可用性：这类容器的解码器能从流中间靠同步字接上 */
static bool fmt_stream_seekable(int fmt)
{
    return fmt == ESPAUDIOCORE_FMT_MP3 || fmt == ESPAUDIOCORE_FMT_AAC;
}

bool audio_seek_supported(void)
{
    if (!audio_engine_session_active()) {
        return false;
    }
    if (!audio_engine_is_stream()) {
        return true;                     /* 本地文件：库内 seek，恒可用 */
    }
    if (!fmt_stream_seekable(audio_http_format())) {
        return false;
    }
    /* 在线要能算"时间 → 字节"：总字节（Content-Length）× 时长（上层告知） */
    return audio_engine_bitrate() != 0;
}

/** 本地：库内 seek + 丢掉 seek 前的残留 PCM + 重置时间轴基准 */
static esp_err_t seek_local(uint32_t pos_ms)
{
    const esp_err_t err = audio_engine_seek_ms((int64_t)pos_ms);
    if (err == ESP_OK) {
        /* seek 请求与真正生效之间，ringbuf 里还留着"旧位置"的 PCM；
         * 不丢掉的话听感是"先播一小段旧声音再跳到新位置" */
        audio_output_flush();
        audio_output_note_seek(pos_ms);
        ESP_LOGI(TAG, "local seek -> %u ms", (unsigned)pos_ms);
    } else {
        ESP_LOGW(TAG, "local seek %u ms failed: %s", (unsigned)pos_ms, esp_err_to_name(err));
    }
    return err;
}

/** 在线：停会话 → Range 重开 → 重开解码会话 */
static esp_err_t seek_stream(uint32_t pos_ms)
{
    const uint32_t bps = audio_engine_bitrate();
    if (!bps) {
        return ESP_ERR_NOT_SUPPORTED;    /* 算不出码率就没有字节位置可用 */
    }
    const uint64_t byte = (uint64_t)pos_ms * bps / 8000ull;
    ESP_LOGI(TAG, "stream seek -> %u ms = byte %llu (%u bps)", (unsigned)pos_ms,
             (unsigned long long)byte, (unsigned)bps);

    audio_core_lock();

    audio_engine_stop();                 /* 先彻底停掉旧会话（含输出任务收尾） */
    esp_err_t err = audio_http_start(byte);
    if (err == ESP_OK && !audio_http_wait_ready(8000)) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        err = audio_engine_start_stream(audio_http_format());
    }

    audio_core_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream seek failed: %s", esp_err_to_name(err));
        audio_http_stop();
        return err;
    }

    audio_output_flush();
    audio_output_note_seek(pos_ms);
    return ESP_OK;
}

esp_err_t audio_seek_to(uint32_t pos_ms)
{
    if (!audio_engine_session_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_engine_is_stream()) {
        /*
         * 本地 seek 也要持会话锁：`espaudiocore_seek_ms()` 读的是库内部的会话对象，
         * 而换曲（`audio_engine_start_file`）会在持锁状态下把它换掉 ——
         * 不持锁就可能 seek 到已经被释放的会话上。
         * 阻塞上限：库内是"登记请求 + 最多 200 ms 轮询"，所以持锁时间是有界的。
         */
        audio_core_lock();
        const esp_err_t err = seek_local(pos_ms);
        audio_core_unlock();
        return err;
    }
    return seek_stream(pos_ms);
}

bool audio_seek_landing_ms(uint32_t *out_ms)
{
    (void)out_ms;
    return false;    /* 见文件头"落点核对不再需要" */
}
