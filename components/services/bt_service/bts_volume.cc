/*
 * bts_volume.cc —— bt_service 内部：AVRCP 绝对音量（走 esp_bt_audio 的
 * esp_bt_audio_vol.h，而不是自己发 AVRCP 命令）
 *
 * 事件流（真机验证时按这个顺序看日志）：
 *
 *   audio_service_set_volume(60)            ← 用户按 VOL+ / 拖滑条
 *     → bt_service_set_volume(60)
 *        → esp_bt_audio_vol_set_absolute(60)   ← AVRCP SetAbsoluteVolume（0~100）
 *           → 对端回 ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT
 *           → 对端再发 ESP_AVRC_RN_VOLUME_CHANGE
 *              → 组件派发 ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE
 *                 → bts_volume_on_peer() → 上层回调（同步 UI/NVS，**不再回发**）
 *
 * 为什么必须"不再回发"：对端收到 SetAbsoluteVolume 后会回一条 VOLUME_CHANGE 通知，
 * 若这里再调一次 set_absolute 就又触发一条通知 —— 来回打乒乓。
 * 所以 on_peer() 只更新缓存，值没变时连回调都不发。
 */

#include "bts_volume.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt_audio_vol.h"

#include "bts_state.h"

static const char *TAG = "bt_vol";

/* 应用设定的音量（0~100）；-1 = 从没设过（开机第一次由 audio_service 下发） */
static volatile int  s_vol = -1;
/* 上一次 esp_bt_audio_vol_set_absolute() 是否成功。false ⇒ audio_output 退回软件缩放 */
static volatile bool s_avrcp_ok = false;

static bts_volume_cb_t s_peer_cb;
static void           *s_peer_user;

/*
 * 限流：音量面板/控制中心的滑条每来一个 PRESSING 事件就调一次 set_volume
 * （实测 60~100 Hz），而每一次都会：① 往 BTC 投一条 AVRCP 命令 ② 让组件打一行
 * ESP_LOGI（"CT: Set volume to N"）。不挡一下，拖一次滑条就是上千行日志、
 * 115200 的 console 直接被刷爆（UART 是阻塞的，反过来还会拖慢 LVGL）。
 *
 * 判据：距上次下发 ≥60 ms，或本次跳变 ≥5%（快拖时立刻跟上）。
 * 被限流丢掉的那几次只更新缓存，**值不会丢** —— 下一次调用或
 * bts_volume_reapply()（流启动/切路由）会把最新值补发出去。
 */
#define BTS_VOL_MIN_INTERVAL_MS  60
#define BTS_VOL_MIN_DELTA        5
static int     s_last_sent = -1;   /* 上次真正发出去的音量 */
static int64_t s_last_ms;          /* 上次真正发出去的时刻（ms） */

static int vol_clamp(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

void bts_volume_reset(void)
{
    /* 只清"可用"标记：音量本身是用户设置，下一台设备也要用。
     * s_last_sent 也要清 —— 否则重连后第一次 set 到同一个值会被限流当成"没变化"。 */
    s_avrcp_ok  = false;
    s_last_sent = -1;
}

int bts_volume_get(void)
{
    return s_vol < 0 ? 0 : s_vol;
}

bool bts_volume_avrcp_ok(void)
{
    return s_avrcp_ok;
}

void bts_volume_set_peer_cb(bts_volume_cb_t cb, void *user)
{
    s_peer_user = user;
    s_peer_cb   = cb;
}

void bts_volume_set(int percent)
{
    percent = vol_clamp(percent);
    s_vol = percent;

    /*
     * 没连上就别发：esp_bt_audio_vol_set_absolute() 在 CT 没注册时会返回
     * ESP_ERR_INVALID_STATE，并在组件里打一行 E 级日志
     * （Operation failed: vol_ops.set_absolute is not registered）。
     * 用户长按 VOL± 的时候那条日志会刷屏，所以这里先自己挡掉。
     */
    if (!bts_connected) {
        s_avrcp_ok  = false;
        s_last_sent = -1;       /* 连上之后同一个值也要能发出去 */
        return;
    }

    const int64_t now = esp_timer_get_time() / 1000;
    const bool    due = (percent != s_last_sent) &&
                        ((now - s_last_ms) >= BTS_VOL_MIN_INTERVAL_MS ||
                         s_last_sent < 0 ||
                         (percent > s_last_sent ? percent - s_last_sent
                                                : s_last_sent - percent) >= BTS_VOL_MIN_DELTA);
    if (!due) {
        return;                 /* 限流：值已缓存，下一拍再发 */
    }

    const esp_err_t err = esp_bt_audio_vol_set_absolute((uint32_t)percent);
    const bool      ok  = (err == ESP_OK);
    s_last_sent = percent;
    s_last_ms   = now;
    /* 只在状态翻转时打日志：拖动滑条会调用很多次 */
    if (ok != s_avrcp_ok) {
        ESP_LOGI(TAG, "AVRCP 绝对音量 %s（vol=%d%%）",
                 ok ? "可用：音量交给对端" : "不可用：退回软件缩放", percent);
    }
    s_avrcp_ok = ok;
}

void bts_volume_reapply(void)
{
    if (s_vol < 0 || !bts_connected) {
        return;
    }
    const esp_err_t err = esp_bt_audio_vol_set_absolute((uint32_t)s_vol);
    s_avrcp_ok  = (err == ESP_OK);
    s_last_sent = s_vol;
    s_last_ms   = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "下发音量 %d%% 给对端（AVRCP %s）", s_vol,
             s_avrcp_ok ? "ok" : "还没连上，暂用软件缩放");
}

void bts_volume_on_peer(int percent)
{
    percent = vol_clamp(percent);
    if (percent == s_vol) {
        /* 我们自己刚设的值被回显（对端收到 SetAbsoluteVolume 会再发一条通知）：
         * 值没变，直接吞掉 —— 这就是防止来回打乒乓的那道门。 */
        return;
    }
    s_vol = percent;
    /* 对端能发 VOLUME_CHANGE ⇒ 这条 AVRCP 链路肯定是通的 */
    s_avrcp_ok = true;
    ESP_LOGI(TAG, "对端 AVRCP 把音量改成 %d%%", percent);
    if (s_peer_cb) {
        s_peer_cb(percent, s_peer_user);   /* ⚠ 蓝牙栈上下文：上层只能更新状态 */
    }
}

/* ---------------------------------------------------------------- 对外（bt_service.h） */

esp_err_t bt_service_set_volume(int percent)
{
    bts_volume_set(percent);
    return ESP_OK;
}

int bt_service_get_volume(void)
{
    return bts_volume_get();
}

bool bt_service_volume_is_avrcp(void)
{
    return bts_volume_avrcp_ok();
}

void bt_service_set_volume_cb(bt_volume_cb_t cb, void *user)
{
    bts_volume_set_peer_cb(cb, user);
}
