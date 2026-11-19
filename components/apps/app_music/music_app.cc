/*
 * music_app.cc —— 对外门面：5 个 API + APP 注册 + 两个常驻定时器
 *
 * 从 app_music.cc 的 305–318、1312–1315、2306–2314、2321–2374、3502–3619 搬出来。
 *
 * ★ 对外符号（app_music.h 里声明，app_online.cc / main.cc 在用）**名字与签名一个都不能变**：
 *     app_music_register_apps / app_music_play_online / app_music_online_detach
 *     app_music_open_player  / app_music_on_media_ctrl
 *
 * 本文件是唯一同时看见"业务层（music_player/music_online/music_local_source）"与
 * "视图层（music_view_*）"的翻译单元，但它自己只做注册、转发与定时器创建，不含逻辑。
 */
#include "app_music.h"

#include <stdio.h>

#include "esp_log.h"

/*
 * ⚠ 必须包含自己的内部头：`music_view_player_ensure_timers()` 在 music_app.h 里是
 * **`extern "C"`** 声明（music_view_player.cc 也用 C 名去调它），而 C++ 里函数的
 * 链接名取决于**是否有声明可见**：不 include 这个头，本文件里的定义就会拿到
 * C++ 修饰名（`_Z31music_view_player_ensure_timersv`），于是链接期报
 * "undefined reference to `music_view_player_ensure_timers`"。
 * 以后在这个文件里新增"给别的 .cc 用的函数"时，同样要保证声明可见。
 */
#include "music_app.h"

#include "audio_service.h"
#include "ui_app_registry.h"   /* app_desc_t / ui_app_register */
#include "ui_core.h"
#include "ui_metrics.h"

#include "music_local_source.h"
#include "music_logic.h"
#include "music_online.h"
#include "music_player.h"
#include "music_player_bg.h"
#include "music_spectrum.h"   /* FFT 可视化：开机读一次 NVS 开关 + 挂 PCM 监视 */
#include "music_state.h"
#include "music_view_local.h"
#include "music_view_player.h"

/* 本文件只做注册/转发/建定时器，不打日志 —— 原来这里有个未使用的 TAG，
 * 编译出 -Wunused-variable；真要加日志时再补回来（TAG 用 "app_music"）。 */

static lv_timer_t *s_tick;          /* 500 ms 常驻心跳（**任何地方都不删**） */
static lv_timer_t *s_media_timer;   /* 100 ms 蓝牙媒体键（**任何地方都不删**） */

static void on_music_enter(void) {}
static void on_music_exit(void) {}

/* ------------------------------------------------------------------ 定时器回调 */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    music_player_tick();
}

static void media_cmd_tick(lv_timer_t *t)
{
    (void)t;
    music_player_media_cmd_tick();
}

/*
 * 确保两个常驻定时器存在（播放器页与 app_music_register_apps 双保险：谁先跑谁建）。
 * 页面销毁时**绝不要**删它们 —— 见 app_music_register_apps 里的长注释。
 */
void music_view_player_ensure_timers(void)
{
    if (!s_tick) {
        s_tick = lv_timer_create(tick_cb, MUSIC_TICK_MS, NULL);
    }
    if (!s_media_timer) {
        /* 蓝牙按键要"跟手"一点，单独用一个 100 ms 的轻量定时器取标志 */
        s_media_timer = lv_timer_create(media_cmd_tick, 100, NULL);
    }
}

/* ------------------------------------------------------------------ 对外 API */

void app_music_on_media_ctrl(int cmd)
{
    music_player_on_media_ctrl(cmd);
}

void app_music_online_detach(const online_song_t *songs)
{
    music_online_detach(songs);
}

/*
 * 在线音乐 app 的入口：把一份在线歌单交给播放器并从第 idx 首开始播。
 * **必须在 LVGL 任务上下文调用**（会更新播放器页控件）。
 */
void app_music_play_online(const online_song_t *songs, size_t count, size_t idx)
{
    music_online_play(songs, count, idx);
}

/*
 * 打开播放器页（压栈）。给别的 APP 用（在线音乐点歌之后直接进播放器看封面/歌词）。
 * 描述符是拷贝进路由栈的，所以这里的栈上临时量是安全的（见 ui_router_push 注释）。
 */
void app_music_open_player(void)
{
    music_router_open_player();
}

void app_music_register_apps(void)
{
    static const app_desc_t music = {
        "music", "音乐", LV_SYMBOL_AUDIO, 0xE8973A,
        music_view_player_build, on_music_enter, on_music_exit, 10, 0
    };
    static const app_desc_t local = {
        "local", "文件管理", LV_SYMBOL_DIRECTORY, 0x5B6FB5,
        music_view_local_build, NULL, NULL, 30, 0
    };

    ui_app_register(&music);
    ui_app_register(&local);

    /* 订阅播放事件：播完自动下一首，解码失败则报错并跳过 */
    audio_service_set_event_cb(music_player_on_audio_evt, NULL);

    /*
     * ★★ 播放状态的"心跳"定时器必须**开机就建、且不随页面销毁**（2026-10 用户报：
     *    "后台播放时歌放完了没有按循环逻辑续播，要进播放页才继续"）。
     *
     * 根因：tick 原先由 page_player() 创建、并在页面 LV_EVENT_DELETE 里删除 ——
     * 于是**不在播放器页时根本没有人消费 advance_req**（AUDIO_EVT_FINISHED 只是
     * 在播放器任务里置了这个标志）。歌放完 → 标志挂着 → 一直等到用户进播放页才被处理，
     * 表现就是"停住了"。错误处理、卡死看门狗、断点续播、进度记忆落盘同理，全都停摆。
     *
     * tick 里所有碰 UI 的地方本来就有空指针保护（控件表在退页时被整体清零），
     * 所以它可以脱离页面一直跑 —— 页面只是"显示"，状态机跑在页面之外。
     */
    music_view_player_ensure_timers();

    /*
     * 播放器页背景图：开机就在 core 0 后台解（800x480 JPEG 几百毫秒），
     * 进页面时直接能用；否则第一次进播放页会先看到纯色再"啪"一下变出图。
     */
    music_player_bg_start();

    /*
     * 音频可视化（FFT 频谱柱）：与页面无关地开机初始化一次 ——
     *   - 读 NVS(music_cfg/fft) 拿到开关（设置弹窗里改，重启后保持）；
     *   - 注册 audio_service 的 PCM 监视回调（输出任务里只做 memcpy 进环形缓冲）；
     *   - 建 FFT 的常数表与 Hann 窗（esp-dsp），失败时内部降级成"不显示柱子"。
     * 真正的 FFT 在 LVGL 任务的 40 ms 定时器里做，这里不占时间。
     */
    music_spectrum_init();

    music_player_restore_last_session();
}
