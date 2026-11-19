/*
 * music_state.h —— 音乐 APP 的**共享状态**（拆分重构后唯一的一份 static 状态）
 *
 * 这个头文件是 app_music.cc（3619 行）拆成多文件后"谁都不能再各自留一份 static"的落点：
 *   - **只装数据**（POD）：曲目与时间轴、进度记忆、在线队列句柄、看门狗与重试计数、
 *     seek/缓冲时序标志、事件中转标志、封面像素与描述符（跨线程，保持 volatile）；
 *   - **零依赖**：只 include <stddef.h>/<stdint.h>/<stdbool.h>，不 include lvgl.h，
 *     也不 include 任何服务头 —— online_song_t 是**匿名结构 typedef**（online_service.h:52），
 *     无法前置声明，所以这里存 `const void *`，由 music_online 自己解引用。
 *   - **不含 UI 控件指针**：播放器页/本地页的 lv_obj_t*、lv_timer_t*、弹窗句柄都在各自的
 *     视图与封面模块内部（music_view_internal.h / music_output_view.cc / music_cover.cc），
 *     结构上消灭"业务模块误删定时器、误写已销毁对象"的可能。
 *
 * 写入口径（拆分时刻意保留的粗粒度迁移函数，不做逐字段 setter）：
 *   - `music_state_begin_local()` / `music_state_begin_online()` / `music_state_begin_none()`
 *   - `music_state_leave_online()`（切回本地队列时清在线残留）
 *   - 其余字段由 owner 模块直接读写（music_player 是时间轴与看门狗的 owner，
 *     music_online 是在线队列与准备标志的 owner，music_cover 是封面跨线程标志的 owner）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 沿用原 app_music.cc 的两条缓冲上限（见原文件 90–98 行的注释）：
 *   本地路径  ≤ STORAGE_PATH_MAX (512)
 *   在线流地址 ≤ 560（CDN 签名 URL 很长，与 audio_service 的 uri[600] 对齐）
 * s_track_path 既喂给 audio_service_play_uri()，也是进度记忆在 NVS 里的键 ——
 * 一旦被 snprintf 截断，签名 URL 会直接 403、本地路径则指向不存在的文件。
 * （-Os 下 GCC 的 -Werror=format-truncation 正好抓到了 512 < 560 这一处。） */
#define MUSIC_TRACK_PATH_MAX 600
#define MUSIC_STREAM_MAX     560

/* music_state.like_pub 的"还没发布过任何结果"哨兵（与 like_seq 的取值空间不重叠） */
#define MUSIC_LIKE_PUB_NONE  0xFFFFFFFFu

typedef enum {
    MUSIC_MODE_LIST = 0,   /* 列表循环（默认）：顺序放完一轮再从第一首开始 */
    MUSIC_MODE_ONE,        /* 单曲循环：一首放完原样重来 */
    MUSIC_MODE_SHUFFLE,    /* 随机循环：每次随机挑一首不同的 */
} music_play_mode_t;

typedef struct {
    /* ---------------------------------------------------------- 曲目与时间轴 */
    int      index;                                   /* 当前曲目下标（-1 = 无） */
    char     track_path[MUSIC_TRACK_PATH_MAX];        /* 真正在播的路径（也是 NVS 进度键） */
    uint32_t elapsed_ms;                              /* 已播毫秒（只在真出声时累加） */
    uint32_t dur_ms;                                  /* 总时长（0 = 未知） */
    bool     playing;                                 /* 界面侧"在播"缓存 */
    music_play_mode_t play_mode;

    /* 续播位置（进度记忆）：>0 表示"这首要从这里继续" */
    uint32_t resume_ms;
    int      save_ticks;                              /* 进度落盘节流计数（10 tick = 5 s） */
    int      resume_wait;                             /* 等码率的 tick 计数（12 tick 超时） */

    /* ---------------------------------------------------------- 播放来源 */
    bool     on_mode;                                 /* 当前队列是否来自在线歌单 */
    char     on_stream[MUSIC_STREAM_MAX];             /* 待播地址（https 流 / 下载缓存路径） */
    bool     on_stream_local;                         /* true = 下载兜底（当本地文件播） */
    uint32_t on_dur_ms;                               /* 在线总时长（估出来的，0=未知） */

    /* ---------------------------------------------------------- 在线准备（跨线程） */
    volatile int  on_req;                             /* 待准备的曲目下标（-1 = 无） */
    volatile bool on_busy;                            /* 准备线程占用中 */
    bool     on_ready;                                /* 准备完成，等 LVGL 任务起播（最后置位） */
    int      on_ready_idx;
    uint32_t on_ready_seq;                            /* ready 对应的请求序号 */
    bool     on_failed;                               /* 准备失败，等 LVGL 任务提示 */
    bool     on_prefetch;                             /* 本次准备是预取（当前无入口置真，代码留着） */
    uint32_t on_seq;                                  /* 请求序号（只在 LVGL 任务里自增，工作线程只读一次比较） */
    const void *on_songs;                             /* 当前在线队列数组（online_song_t*，见文件头说明） */
    size_t   on_count;
    const void *on_owned;                             /* 托管中、等换队列时释放的旧在线队列 */
    /* 在线封面/歌词的**内存缓冲**（PSRAM，不落 SD）。
     * 归属规则：music_online 的工作线程写入新分配；**只有 LVGL 任务释放**
     * （起播时转移给封面解码请求 / 解析完歌词后 free；换歌 / 退页丢弃未消费的 ready 时 free）。 */
    uint8_t *on_cover_data;
    size_t   on_cover_len;
    char    *on_lrc_data;
    size_t   on_lrc_len;

    /* ---------------------------------------------------------- 看门狗 / 重刷 / 续播 */
    int      stall_ticks;                             /* 无 PCM 连续 tick 数 */
    int      stall_kicks;                             /* 本首踢管线次数（上限 3） */
    int      url_retry;                               /* 本首地址重刷次数（上限 2） */
    int      eof_retry;                               /* 提前结束续播次数（上限 3，只在真换曲时清零） */
    uint32_t on_resume_ms;                            /* 在线断点续播目标 */
    int      on_resume_wait;                          /* 等"总长 + 时长"就绪的 tick 计数 */
    int      fail_streak;                             /* 连续播放失败次数（3 次停下） */

    /* ---------------------------------------------------------- 缓冲 / seek 时序 */
    bool     buf_show;                                /* 正在显示"缓冲中"提示 */
    bool     seek_buf_pending;                        /* 这次缓冲是拖动定位引起的 */
    int      seek_landing_wait;                       /* 落点核对剩余 tick（10 次） */
    bool     seek_in_flight;                          /* seek 进行中（看门狗别误判） */
    int      seek_inflight_n;                         /* seek 无 PCM 计数（上限 20 s/500 ms） */

    /* ---------------------------------------------------------- 事件中转（跨任务） */
    volatile bool advance_req;                        /* 播完 → 换曲请求 */
    volatile bool err_req;                            /* 出错请求 */
    volatile int  media_cmd;                          /* 蓝牙媒体键（1–5，0 = 无） */

    /* ---------------------------------------------------------- 红心收藏（跨线程）
     *
     * 只有一个工作线程（music_like.cc 的 like_task）写、LVGL 任务读，
     * 发布顺序：先写 like_pub_id / like_state → 再写 like_pub（发布标记）→ 最后清 like_busy。
     * 消费方只在 like_pub == like_seq 时相信 like_state（代次不符 = 换过歌，整批丢弃）。
     *
     * 为什么要有"代次"：用户点了红心立刻切下一首是常态，若只按标志位消费，
     * 上一首的结果会贴到新歌上（红心亮错歌）。 */
    volatile bool     like_busy;          /* 工作线程占用中（UI 显示转圈） */
    volatile uint32_t like_seq;           /* 曲目代次（换歌 / 切回本地时 ++） */
    volatile uint32_t like_pub;           /* 已发布结果对应的代次（LIKE_PUB_NONE = 无） */
    volatile int      like_state;         /* 0 未知 / 1 已喜欢 / 2 未喜欢 */
    volatile int      like_result;        /* toast 队列：0 无 / 1 加成功 / 2 取消成功 / 3 未登录 / 4 失败 */
    volatile bool     like_failed;        /* 同步失败（LVGL 侧据此退避，别每 tick 重试） */
    char              like_pub_id[24];    /* 已发布结果对应的歌曲 id */
    char              like_cur_id[24];    /* LVGL 侧记住"上一次看到的曲目 id"（不变就不重算） */

    /* ---------------------------------------------------------- 封面（跨线程产权在 music_cover.cc）
     *
     * 像素与描述符**故意与页面生命周期错位**：页面销毁时不释放（下次进页直接贴回），
     * 真换曲时才由 cover_show(NULL,0,0) 释放。释放顺序不可改：
     * 先摘 lv_image 的 src → 再 free 描述符 → 最后 free 像素。 */
    void *cover_dsc;                                  /* lv_image_dsc_t*（堆上，每张新分配） */
    uint8_t *cover_pixels;                            /* 当前显示的封面像素（RGB565，PSRAM） */
    volatile int  cover_want;                         /* 待解码曲目（-1 = 无） */
    volatile bool cover_busy;                         /* 解码线程占用中 */
    volatile bool cover_ready;                        /* 工作线程已产出结果 */
    uint8_t *cover_out;                               /* 产出像素（等 LVGL 任务认领） */
    int      cover_out_w;
    int      cover_out_h;
    volatile int cover_out_idx;                       /* 产出对应的曲目下标（对不上就丢） */

    /*
     * 封面主色（0xRRGGBB，0 = 没有封面 / 纯黑白灰 → 用默认蔚蓝）。
     * 由**解码线程**在解出像素后写（music_theme_rgb_from_cover），
     * LVGL 侧只读（music_theme_accent_rgb），换曲释放封面时清零。
     * 写入点必须在"像素已就绪"之后、cover_ready 之前 —— 否则 UI 会先看到
     * 新封面配旧主题色（下一 tick 才对齐，肉眼可见的一下跳色）。
     */
    volatile uint32_t cover_accent;
} music_state_t;

/* 固件唯一实例 */
extern music_state_t g_music_state;

music_state_t *music_state(void);

/* ------------------------------------------------------------------ 迁移函数 */

/* 起播一首**本地**曲目：摆下标/路径/续播位置与时间轴，并清掉 seek 与节流计数。
 * 不动 UI（那是视图的事），也不动在线队列（由 music_state_leave_online 负责）。 */
void music_state_begin_local(music_state_t *st, int idx, const char *path, uint32_t resume_ms);

/* 起播一首**在线**曲目：时间轴归零、看门狗计数清零、记住真正要播的地址。 */
void music_state_begin_online(music_state_t *st, int idx, const char *uri, uint32_t dur_ms);

/* 切回本地 / 停止播放：下标清空（-1）。 */
void music_state_begin_none(music_state_t *st);

/* 切回本地队列：清掉"在线队列"的任何残留计数（地址重刷 / 断点续播 / 提前结束）。
 * 注意**不**释放封面/歌词缓冲 —— 那要走 music_online_discard_ready()。 */
void music_state_leave_online(music_state_t *st);

/* 拖动定位 / 续播定位刚刚受理：复位看门狗计数并把时间轴对齐到目标时刻。 */
void music_state_note_seek_accepted(music_state_t *st, uint32_t target_ms);

/* 定位期间"重新计数"：清卡死看门狗的两个计数（重新缓冲正是它盯的特征）。 */
void music_state_reset_watchdog(music_state_t *st);

/* 进度千分比（总时长未知时为 0）。 */
int32_t music_state_progress_permille(const music_state_t *st);

/* "未在播放 / 正在播放 / 已暂停"（日志用；playing 由调用方从 audio_service 取）。 */
const char *music_state_status_text(const music_state_t *st, bool playing);

#ifdef __cplusplus
}
#endif
