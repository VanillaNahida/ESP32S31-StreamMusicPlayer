/*
 * app_music.cc —— 音乐 APP：播放器页（横屏左右分栏）+ 本地音乐列表页
 *
 * 播放器页（开发指南 5.10）：
 *   左栏 240px：专辑封面（内嵌图，解不出来就用占位色块）+ 下沿提示
 *   右栏：歌名 / 歌手 / **歌词区（在封面右侧）** / 进度条（可拖动定位）/ 已播·总时长 / 控制排
 *   轻点封面 → 在封面右侧展开歌词，再点收起
 *
 * M2 已知限制（后续里程碑修正）：
 *   - esp_audio_simple_player 不提供播放位置 API：进度用"已播墙钟时间"累计，
 *     总时长用 MUSIC_INFO 的 bitrate 与文件大小估算（CBR 准，VBR 有偏差）。
 *     拖动定位 = 暂停 → esp_gmf_pipeline_seek(字节位置) → 恢复，
 *     字节位置同样按平均码率估算，所以 MP3/AAC/TS 这类流式格式可靠，
 *     M4A(MP4 容器) 直接不给拖（见 track_seek_unsupported）。
 *   - 封面只解 JPEG（MP3 的 ID3v2 APIC/PIC、FLAC 的 PICTURE 块）；
 *     PNG 封面、M4A 的 covr、OGG 的 METADATA_BLOCK_PICTURE 留到后续里程碑。
 */
#include "app_music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"   /* 目录条目缓冲优先放 PSRAM（heap_caps_realloc） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_core.h"
#include "ui_metrics.h"
#include "ui_app_registry.h"
#include "esp_lv_adapter.h"

#include "bsp.h"
#include "audio_service.h"
#include "storage_service.h"
#include "media_meta.h"
#include "play_state.h"    /* 音量/曲目/进度记忆（NVS） */
#include "bt_service.h"    /* 输出设备弹窗要显示蓝牙连接/推流状态 */
#include "online_service.h"/* 在线歌单队列（在线音乐 app 交过来） */

static const char *TAG = "app_music";

/*
 * 歌词列最多建多少个标签。
 * ★ 从 80 提到 240（2026-10 用户报"歌词显示不全"）：网易云的 LRC 经常带翻译/罗马音
 *   双语行，一首 4 分钟的歌轻松超过 80 行 —— 超出的部分以前**根本不建标签**，
 *   所以后面整段歌词都看不到。240 个指针的数组只有 ~1 KB，可接受。
 */
#define LYRIC_VIEW_MAX_LINES 240
#define TICK_MS              500   /* 进度/歌词刷新周期 */
#define PROGRESS_SAVE_TICKS  10    /* 每 10 个 tick（5 秒）把播放进度记一次 */
/* 续播最多等这么多个 tick（12 × 500 ms = 6 秒）还拿不到码率就放弃定位 */
#define RESUME_WAIT_MAX_TICKS 12
/* 在线曲目下载任务的栈：HTTP + TLS 不浅。
 * 实测 6 KB 会在 TLS/证书校验里爆 "Stack protection fault"
 * （Guru 定位到 heap_caps_aligned_alloc ← mbedtls 握手），所以给 12 KB；
 * 任务只在下载期间存在，跑完就 vTaskDelete。 */
#define ONLINE_TASK_STACK    (12 * 1024)

/* ------------------------------------------------------------------ 状态 */

static int       s_index = -1;
static uint32_t  s_elapsed_ms;
static uint32_t  s_dur_ms;
static bool      s_playing;
static lrc_t     s_lrc;
static int       s_lyric_cur = -1;
/*
 * 续播位置（进度记忆）：>0 表示"这首要从这里继续"。
 * 字节定位要等 MUSIC_INFO 报出码率才算得出来，所以先记在这里，
 * 由 tick_cb 在码率到手后执行，见 tick_cb 里的说明。
 */
static uint32_t  s_resume_ms;
static int       s_save_ticks;      /* 进度记忆的节流计数（每 5 秒写一次 RAM 态） */
static int       s_resume_wait;     /* 等码率来定位续播位置的 tick 计数（兜底用） */
/*
 * 当前**真正在播**的曲目路径。
 *
 * 为什么不直接用 storage_service_track(s_index)->path：文件浏览页每进一个目录都会
 * 调 storage_service_load_dir() 把播放队列换成那个目录的内容（这是需求：
 * 上一首/下一首在当前目录里轮转）。于是"正在播的曲子"和"队列的第 s_index 个"
 * 会分家 —— 拿队列去记进度，记下来的就是另一首歌。进度记忆必须以这条路径为准。
 */
/*
 * 这两条缓冲的长度上限必须能互相装下，别各自拍脑袋定：
 *   本地路径  ≤ STORAGE_PATH_MAX (512)
 *   在线流地址 ≤ 560（CDN 签名 URL 很长，见 s_on_stream / audio_service 的 uri[600]）
 * s_track_path 既喂给 audio_service_play_uri()，也是进度记忆在 NVS 里的键 ——
 * 一旦被 snprintf 截断，签名 URL 会直接 403、本地路径则指向不存在的文件。
 * （-Os 下 GCC 的 -Werror=format-truncation 正好抓到了 512 < 560 这一处。）
 */
#define TRACK_PATH_MAX  600
static char      s_track_path[TRACK_PATH_MAX];

/* 播放器页控件 */
static lv_obj_t *s_p_title;
static lv_obj_t *s_p_artist;
static lv_obj_t *s_p_seek;          /* lv_bar：进度条本体（纯显示，触摸由抓取区管） */
static lv_obj_t *s_p_seek_hit;      /* 进度条抓取区：自己算比例，松手才真定位 */
static lv_obj_t *s_p_cur;
static lv_obj_t *s_p_tot;
static lv_obj_t *s_p_btn;
static lv_obj_t *s_p_cover_box;     /* 左栏封面容器 */
static lv_obj_t *s_p_cover_img;     /* 真封面（没解出来时隐藏） */
static lv_obj_t *s_p_lyric_box;     /* 右栏歌词区（封面右侧） */
static lv_obj_t *s_p_lyric_hint;    /* 未展开时的提示 */
static lv_obj_t *s_p_lyric_col;
static lv_obj_t *s_p_lyric_labels[LYRIC_VIEW_MAX_LINES];
static size_t    s_p_lyric_n;
static bool      s_lyric_mode;      /* true = 歌词已展开 */
static bool      s_seeking;         /* 手指正在拖进度条：期间别让 tick 覆盖它 */
static lv_timer_t *s_tick;

/* --- 封面：异步解码（读文件 + 软解 JPEG 不能放在 LVGL 任务里） --- */
#define COVER_MAX_DIM    UI_PX_H(240)   /* 封面显示边长（超出的按 1/2^n 降采样） */
/* JPEG 软解用栈不浅（项目里出过 0xa5a5a5a5 栈溢出，见 11_遇到的问题.md F3），
 * 给足 8 KB；像素缓冲走 PSRAM（CONFIG_SPIRAM_USE_MALLOC），不占内部 RAM */
#define COVER_TASK_STACK (8 * 1024)

typedef struct {
    int  idx;
    bool from_mem;                  /* true = data/len 是内存里的图片字节（在线歌曲） */
    uint8_t *data;                  /* from_mem 时有效：本请求**独占**，用完 free */
    size_t   len;
    char path[STORAGE_PATH_MAX];    /* from_mem=false：本地歌曲音频路径 */
} cover_req_t;

static lv_image_dsc_t *s_cover_dsc;     /* 堆上的描述符，指向 s_cover_pixels（RGB565） */
static uint8_t *s_cover_pixels;         /* 当前显示的封面像素，media_meta_cover_free 释放 */
static volatile int  s_cover_want = -1; /* 待解码曲目（-1 = 无） */
static volatile bool s_cover_busy;
static volatile bool s_cover_ready;     /* 工作线程已产出结果 */
static uint8_t *s_cover_out;
static int      s_cover_out_w;
static int      s_cover_out_h;
static volatile int s_cover_out_idx = -1;

/* ------------------------------------------------------------- 在线曲目队列
 *
 * 在线音乐 app 把"当前在线歌单"的数组指针交给这里，播放器就用同一套 UI
 * （封面 / 歌词 / 进度 / 上下曲）播它。
 *
 * 约定：songs 数组在 app_music 切到别的队列之前必须一直有效 ——
 * app_online 负责在把新数组交过来之后再释放旧数组（见 app_online.cc）。
 *
 * 播放方式：**流式**（默认）。工作线程先解析出音频的最终 https 地址、抓好封面与
 * 歌词，然后 audio_service_play_uri() 交给 GMF 的 HTTP IO 边下边播 —— 点歌到出声
 * 只要一两秒，不再等整首下完。流地址拿不到（版权/网络）时自动退回"下载到 SD
 * 再当本地文件播"（s_on_stream_local = true），这时进度记忆照常工作。
 */
static const online_song_t *s_on_songs;
static size_t  s_on_count;
static bool    s_on_mode;                    /* 当前队列是否来自在线歌单 */
/* 要播放的地址：流式是 https 地址，下载兜底是本地缓存路径
 * （audio_service_play_uri() 两种都认） */
static char    s_on_stream[560];
static bool    s_on_stream_local;            /* true = 下载兜底（本地文件） */
static uint32_t s_on_dur_ms;                 /* 在线歌曲总时长（估出来的，0=未知） */
/* 请求序号：点歌会立刻 ++，工作线程只在序号没变时才认自己的结果 ——
 * 否则"点第二首时第一首的下载刚好完成"，tick 会先把旧的那首起播，
 * 表现为"点了歌但没切过去"（先放旧的、几秒后才换）。 */
static uint32_t s_on_seq;   /* 只在 LVGL 任务里改，工作线程只读一次比较 */

/*
 * 在线封面/歌词的**内存缓冲**（PSRAM，不落 SD）。
 * 归属规则：online_prepare_task（工作线程）写入新分配；**只有 LVGL 任务释放**
 * （start_track_online_at 消费时转移给封面解码请求 / 解析完歌词后 free；
 * play_next / 点歌 / 退页丢弃未消费的 ready 时 free）。
 * cover_kick 会把 s_on_cover_data **转移**进封面解码请求（请求独占，线程内释放），
 * 所以这里释放的唯一竞态点被消掉了。
 */
static uint8_t *s_on_cover_data;             /* 已下载好的封面字节（JPEG/PNG） */
static size_t   s_on_cover_len;
static char    *s_on_lrc_data;               /* 已下载好的歌词文本（UTF-8） */
static size_t   s_on_lrc_len;

/*
 * 进度记忆的唯一出口：**在线流式不记**。
 * 流地址是 CDN 带时效签名的 URL（几分钟后失效），存下来下次也用不了，
 * 反而会把"上次听到哪首本地歌"覆盖掉。下载兜底（本地文件）照常记。
 */
static void save_progress(uint32_t pos_ms)
{
    if (s_on_mode && !s_on_stream_local) {
        return;
    }
    if (!s_track_path[0]) {
        return;
    }
    play_state_track_set(s_track_path, pos_ms);
}
static volatile int  s_on_req = -1;          /* 待下载的曲目下标（-1 = 无） */
static volatile bool s_on_busy;
static bool    s_on_ready;                   /* 下载完成，等 LVGL 任务起播 */
static int     s_on_ready_idx = -1;
/* ready 对应的请求序号：tick 消费前必须与 s_on_seq 一致，否则是过期 ready
 * （用户中间又点了别的歌），只做丢弃、不起播 —— 见 tick_cb 与 online_prepare_task */
static uint32_t s_on_ready_seq;
static bool    s_on_failed;                  /* 下载失败，等 LVGL 任务提示 */
/*
 * 预取下一首的封面/歌词：**目前刻意不启用**（它占住唯一的准备槽位，用户这时点歌
 * 就得干等它跑完，实测 1~3 秒 —— 为了"点了就切"的及时性去掉了入口，
 * 见 tick_cb 里的说明）。这套标志与 online_prepare_task 的 prefetch 分支都还留着，
 * 以后想在"网络空闲/缓冲充足"时重新启用，把入口调用加回来即可。
 */
static bool    s_on_prefetch;                /* 本次下载是预取（不起播） */

/*
 * 丢弃"已下载好、还没起播"的在线结果（封面/歌词缓冲也一并释放）。
 * 只在 LVGL 任务里调：缓冲由工作线程发布、LVGL 任务独占释放，
 * 发布点在工作线程最后几条指令里，最坏情况（发布窗口内恰好切歌）由
 * s_on_ready_seq 的序号校验兜住 —— 过期的 ready 在 tick 里只会被丢弃释放，
 * 不会再被消费。
 */
static void online_discard_ready(void)
{
    s_on_ready     = false;
    s_on_ready_idx = -1;
    if (s_on_cover_data) {
        free(s_on_cover_data);
        s_on_cover_data = NULL;
        s_on_cover_len  = 0;
    }
    if (s_on_lrc_data) {
        free(s_on_lrc_data);
        s_on_lrc_data = NULL;
        s_on_lrc_len  = 0;
    }
}

/* 前向声明（start_track / tick_cb 在它们定义之前） */
static void lyric_rebuild(void);
static void lyric_apply_mode(void);
static void update_lyric_highlight(void);
static void cover_show(uint8_t *px, int w, int h);
static void cover_reap(void);
static void cover_kick(void);
static void online_prepare_kick(void);
static void set_buffering_hint(void);
/* 正在显示"缓冲中"提示（tick_cb 里刷百分比、出声后撤掉） */
static bool s_buf_show;
static void start_track_online_at(int idx, const char *uri);
/* 在线切歌的"界面先行"：先把播放器页切成目标歌曲（见实现处的说明） */
static void online_switch_ui_to(int idx);

/* ------------------------------------------------------------------ 播放模式 */

typedef enum {
    PLAY_MODE_LIST = 0,   /* 列表循环（默认）：顺序放完一轮再从第一首开始 */
    PLAY_MODE_ONE,        /* 单曲循环：一首放完原样重来 */
    PLAY_MODE_SHUFFLE,    /* 随机循环：每次随机挑一首不同的 */
} play_mode_t;

static play_mode_t s_play_mode = PLAY_MODE_LIST;
static lv_obj_t   *s_p_mode_btn;     /* 模式按钮里的图标 label */
static lv_obj_t   *s_p_mode_badge;   /* 单曲循环的 "1" 角标 */

/*
 * 三个模式的图标：
 *   列表循环 → LV_SYMBOL_LOOP
 *   单曲循环 → LV_SYMBOL_LOOP + 右上角 "1" 角标（等价 repeat-one）
 *   随机循环 → LV_SYMBOL_SHUFFLE
 *
 * 为什么不用"真正的" repeat-one 字形：随固件编译的位图字体 ui_font_sc16
 * 只收了生成时指定的一批 FontAwesome5 码点（见 components/ui_font/
 * ui_font_sc16.c 头部 Opts），其中没有 repeat-one，也没有 repeat。
 * 直接用会渲染成缺字方框，所以用 LOOP + "1" 角标等价表达；
 * 三个图形外形差异明显，再叠加 Toast 文案，不会认错。
 */
static const char *play_mode_name(void)
{
    switch (s_play_mode) {
    case PLAY_MODE_ONE:     return "单曲循环";
    case PLAY_MODE_SHUFFLE: return "随机循环";
    default:                return "列表循环";
    }
}

/* 把当前模式画到按钮上（页面尚未创建时是空操作） */
static void apply_play_mode_ui(void)
{
    if (s_p_mode_btn) {
        lv_label_set_text(s_p_mode_btn, (s_play_mode == PLAY_MODE_SHUFFLE)
                                            ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP);
    }
    if (s_p_mode_badge) {
        if (s_play_mode == PLAY_MODE_ONE) {
            lv_obj_remove_flag(s_p_mode_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_p_mode_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 点击循环模式按钮：列表循环 → 单曲循环 → 随机循环 → 列表循环 */
static void on_mode_click(lv_event_t *e)
{
    (void)e;
    switch (s_play_mode) {
    case PLAY_MODE_LIST:    s_play_mode = PLAY_MODE_ONE;     break;
    case PLAY_MODE_ONE:     s_play_mode = PLAY_MODE_SHUFFLE; break;
    default:                s_play_mode = PLAY_MODE_LIST;    break;
    }
    apply_play_mode_ui();
    ui_toast(play_mode_name(), 1200);
    ESP_LOGI(TAG, "play mode -> %s", play_mode_name());
}

/*
 * 播放事件 → LVGL 任务的中转标志。
 * 事件回调运行在 GMF 播放任务上下文（audio_service.h 明确要求别做重活），
 * 所以那里只置标志，UI 更新与换曲都放到 tick_cb（LVGL 任务）里做。
 */
static volatile bool s_advance_req;
static volatile bool s_err_req;
/* 蓝牙耳机按键（AVRCP）：非 0 = 待处理的媒体命令，见 app_music.h */
static volatile int  s_media_cmd;
static lv_timer_t   *s_media_timer;
static int           s_fail_streak;   /* 连续播放失败次数，防止坏文件无限跳过 */

/* ------------------------------------------------------------------ 工具 */

static void fmt_time(uint32_t ms, char *buf, size_t sz)
{
    if (ms == 0 || ms > 24u * 3600u * 1000u) {
        snprintf(buf, sz, "--:--");
        return;
    }
    uint32_t total_s = ms / 1000;
    snprintf(buf, sz, "%u:%02u", (unsigned)(total_s / 60), (unsigned)(total_s % 60));
}

static const char *current_status_text(void)
{
    if (s_index < 0) {
        return "未在播放";
    }
    return audio_service_is_playing() ? "正在播放" : "已暂停";
}

/* ------------------------------------------------------------------ 播放控制 */

/*
 * 起播一首：resume_ms > 0 表示"从这里的进度继续"（进度记忆 / 开机续播）。
 * 换曲一律传 0，只有 on_play_pause 重新起播当前曲目、以及开机恢复时才带位置。
 */
static void start_track_at(int idx, uint32_t resume_ms)
{
    const track_t *t = storage_service_track((size_t)idx);
    if (!t) {
        ui_toast("曲目不存在", 1200);
        return;
    }

    /* 切回本地队列：清掉"在线队列"的任何残留（封面/歌词缓冲也要释放） */
    s_on_mode     = false;
    online_discard_ready();

    s_index      = idx;
    s_resume_ms  = resume_ms;
    /* 先把进度条摆到续播位置，别让它从 0 跳一下（真正的定位见 tick_cb） */
    s_elapsed_ms = resume_ms;
    s_dur_ms     = 0;
    s_seeking    = false;
    s_lyric_cur  = -1;
    s_save_ticks = 0;
    s_resume_wait = 0;
    snprintf(s_track_path, sizeof(s_track_path), "%s", t->path);

    /*
     * 歌词三级查找（media_meta 内部按顺序试）：
     *   1) 歌曲同目录同名 .lrc
     *   2) /sdcard/lrc/<同名>.lrc
     *   3) 内嵌 ID3 歌词（USLT / SYLT）→ 取到后自动落盘到 /sdcard/lrc
     */
    if (media_meta_lyrics_load(t->path, &s_lrc) != ESP_OK) {
        ESP_LOGD(TAG, "no lyrics for %s", t->path);
    }

    if (s_p_title) {
        lv_label_set_text(s_p_title, t->title);
    }
    if (s_p_artist) {
        lv_label_set_text(s_p_artist, t->artist[0] ? t->artist : "未知歌手");
    }

    /* 歌词：换曲要重建（页面可能已经存在，例如播完自动续播） */
    lyric_rebuild();
    if (s_lyric_mode) {
        update_lyric_highlight();
    }
    s_buf_show = false;    /* 本地文件：立刻出声，不要"缓冲中"提示 */

    /* 封面：先退回占位（别把上一首的封面留在屏上），再交给工作线程解 */
    cover_show(NULL, 0, 0);
    cover_reap();
    s_cover_want = idx;
    cover_kick();

    /*
     * 进度条归零：时长这时还不知道（要等 MUSIC_INFO），进度条先按 0 摆好。
     * 续播位置在定位成功后随 s_elapsed_ms 一起由 tick_cb 换算成比例显示。
     */
    if (s_p_seek) {
        lv_bar_set_value(s_p_seek, 0, LV_ANIM_OFF);
    }
    if (s_p_cur) {
        char buf[16];
        fmt_time(s_elapsed_ms, buf, sizeof(buf));
        lv_label_set_text(s_p_cur, buf);
    }

    /* 进度记忆：换曲就是要记住"现在放的是哪首、放到哪了"，立刻落盘 */
    save_progress(resume_ms);
    play_state_commit();

    if (audio_service_play_path(t->path) != ESP_OK) {
        ui_toast("播放失败：格式不支持或文件损坏", 2000);
        return;
    }
    s_playing = true;
    if (s_p_btn) {
        lv_label_set_text(lv_obj_get_child(s_p_btn, 0), LV_SYMBOL_PAUSE);
    }
}

static void start_track(int idx)
{
    start_track_at(idx, 0);
}

static void play_next(int dir)
{
    /* 在线队列：换曲要先去把音频下下来（工作线程），不能在这里阻塞 LVGL */
    if (s_on_mode && s_on_songs && s_on_count > 0) {
        int idx = s_index;
        if (idx < 0) {
            idx = 0;
        } else if (s_play_mode == PLAY_MODE_SHUFFLE && s_on_count > 1) {
            int r = idx;
            while (r == idx) {
                r = (int)(esp_random() % (uint32_t)s_on_count);
            }
            idx = r;
        } else {
            idx = (idx + dir + (int)s_on_count) % (int)s_on_count;
        }
        s_index = idx;               /* 目标歌先记上：界面/状态立刻指向它 */
        s_on_seq++;
        online_discard_ready();      /* 清掉还没起播的旧结果（缓冲一并释放） */
        s_on_failed = false;
        s_on_req = idx;
        /*
         * 和点歌走同一套顺序（见 app_music_play_online）：
         * 停旧歌 → 界面切到目标歌 → 再显示缓冲信息。
         * 自动续播时旧歌本来就播完了，多调一次 stop 无害。
         */
        audio_service_stop();
        s_playing = false;
        if (s_p_btn) {
            lv_label_set_text(lv_obj_get_child(s_p_btn, 0), LV_SYMBOL_PLAY);
        }
        online_switch_ui_to(idx);
        online_prepare_kick();
        if (s_on_busy) {
            ui_toast("缓冲中，请稍后...", 1200);
            set_buffering_hint();
        }
        return;
    }

    size_t n = storage_service_count();
    if (n == 0) {
        return;
    }
    int idx = s_index;
    if (idx < 0) {
        idx = 0;
    } else if (s_play_mode == PLAY_MODE_SHUFFLE && n > 1) {
        /* 随机循环：挑一首和当前不同的（n == 1 时退化为原曲） */
        int r = idx;
        while (r == idx) {
            r = (int)(esp_random() % (uint32_t)n);
        }
        idx = r;
    } else {
        idx = (idx + dir + (int)n) % (int)n;
    }
    start_track(idx);
}

static void on_play_pause(lv_event_t *e)
{
    (void)e;
    if (s_index < 0) {
        ui_toast("请先在本地音乐里选一首", 1600);
        return;
    }
    /*
     * audio_service_toggle() 只在 RUNNING / PAUSED 之间切；管线处于 STOPPED
     * （开机恢复的上次曲目、或手动停止过）时它返回错误 —— 这时要**从记忆的
     * 进度重新起播**，否则"开机后按播放没反应"。
     */
    if (audio_service_toggle() != ESP_OK) {
        /* 在线模式：音频文件已在 SD 上，直接重起播即可（不用再下载） */
        if (s_on_mode && s_on_stream[0]) {
            start_track_online_at(s_index, s_on_stream);
        } else {
            start_track_at(s_index, s_elapsed_ms);
        }
        return;
    }
    s_playing = audio_service_is_playing();
    if (s_p_btn) {
        lv_label_set_text(lv_obj_get_child(s_p_btn, 0),
                          s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    /* 暂停就立刻把进度落盘（用户可能马上断电） */
    if (!s_playing && s_track_path[0]) {
        save_progress(s_elapsed_ms);
        play_state_commit();
    }
}

static void on_prev(lv_event_t *e) { (void)e; play_next(-1); }
static void on_next(lv_event_t *e) { (void)e; play_next(+1); }

/* 音量键 → 从右边缘滑出竖向音量条（ui_volume_panel.cc） */
static void on_volume(lv_event_t *e)
{
    (void)e;
    ui_volume_show();
}

/* ------------------------------------------------------------------ 进度条定位 */

/* MP4/M4A 是随机访问容器：按字节 seek 后解码器对不上帧，直接不给拖 */
static bool track_seek_unsupported(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot && (strcasecmp(dot, ".m4a") == 0);
}

/* 触点 x 在抓取区里的比例 → 千分比（0~1000） */
static int32_t seek_pointer_permille(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !s_p_seek_hit) {
        return 0;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(s_p_seek_hit, &a);
    int w = lv_area_get_width(&a);
    if (w <= 0) {
        return 0;
    }
    int rel = (int)p.x - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    return (int32_t)((int64_t)rel * 1000 / w);
}

/* 按下即跟手：拖动中只更新进度条与"已播"时间，**松手才真定位**
 * （GMF 的 seek 要暂停/恢复管线，边拖边定位会一顿一顿）
 *
 * ★ 时长未知（s_dur_ms == 0）时**也允许进入拖动**（2026-10 修"进度条完全不可用"）：
 *   原来这里 `|| s_dur_ms == 0` 直接 return —— 于是 s_seeking 一直是 false，
 *   拇指不跟手，而且 on_seek_release() 在 `if (!s_seeking) return;` 就退出，
 *   **下面那句"时长未知，暂时无法拖动定位"的提示永远执行不到**。
 *   表现就是"按下去毫无反应、也没有任何解释"。
 *   现在：照样进拖动（拇指跟手，让用户看到控件是活的），松手时由 on_seek_release
 *   给出原因。视觉归位由 tick / 松手分支负责。
 */
static void on_seek_press(lv_event_t *e)
{
    (void)e;
    if (s_index < 0 || !s_p_seek) {
        return;
    }
    s_seeking = true;
    const int32_t permille = seek_pointer_permille();
    if (s_dur_ms == 0) {
        /* 时长未知：没法换算时间，但拇指照样跟手（纯视觉反馈） */
        lv_bar_set_value(s_p_seek, permille, LV_ANIM_OFF);
        return;
    }
    lv_bar_set_value(s_p_seek, permille, LV_ANIM_OFF);
    if (s_p_cur) {
        char buf[16];
        fmt_time((uint32_t)((uint64_t)permille * s_dur_ms / 1000ULL), buf, sizeof(buf));
        lv_label_set_text(s_p_cur, buf);
    }
}

/* 手指滑出抓取区：只把"正在拖动"清掉，不做定位 */
static void on_seek_lost(lv_event_t *e)
{
    (void)e;
    s_seeking = false;
}
static void on_seek_release(lv_event_t *e)
{
    (void)e;
    if (!s_seeking) {
        return;
    }
    s_seeking = false;
    if (s_index < 0) {
        return;
    }
    if (s_dur_ms == 0) {
        ui_toast("时长未知，暂时无法拖动定位", 1600);
        if (s_p_seek) {
            lv_bar_set_value(s_p_seek, 0, LV_ANIM_OFF);
        }
        return;
    }

    const int32_t permille = seek_pointer_permille();
    const uint32_t target  = (uint32_t)((uint64_t)permille * s_dur_ms / 1000ULL);

    const track_t *t = s_on_mode ? NULL : storage_service_track((size_t)s_index);
    if (t && track_seek_unsupported(t->path)) {
        ui_toast("M4A 暂不支持拖动定位", 2000);
        return;
    }
    if (audio_service_seek_ms(target) != ESP_OK) {
        ui_toast("定位失败：码率未知或格式不支持", 1800);
        return;
    }

    /* 位置本来靠墙钟累计，定位成功后要对齐过去，否则下一次 tick 会把进度拉回原处 */
    s_elapsed_ms = target;
    s_lyric_cur  = -1;
    /* 进度记忆跟着用户的手动定位走（落盘由 play_state 防抖合并） */
    if (s_track_path[0]) {
        save_progress(target);
    }
    if (s_p_seek) {
        lv_bar_set_value(s_p_seek, permille, LV_ANIM_OFF);
    }
    if (s_p_cur) {
        char buf[16];
        fmt_time(target, buf, sizeof(buf));
        lv_label_set_text(s_p_cur, buf);
    }
    if (s_lyric_mode) {
        update_lyric_highlight();
    }
}

/* 轻点封面：在封面**右侧**展开 / 收起歌词 */
static void on_toggle_view(lv_event_t *e)
{
    (void)e;
    s_lyric_mode = !s_lyric_mode;
    lyric_apply_mode();
    if (s_lyric_mode) {
        s_lyric_cur = -1;               /* 强制重算高亮与居中 */
        update_lyric_highlight();
    }
}

/* ------------------------------------------------------------- 输出设备选择弹窗
 *
 * 需求：播放器页放一个输出设备按钮，点开列出可选的输出设备。
 *
 * 实现要点（都踩过坑，别改）：
 *   - 弹窗挂在 lv_layer_top()，不挂在页面 screen 上：页面被 pop（异步销毁）时
 *     弹窗不该跟着页面一起消失（那是未定义行为），页面 DELETE 里显式关掉它。
 *   - 关弹窗一律 lv_obj_delete_async()：回调是**弹窗自己的子对象**发出来的，
 *     同步删会在 LVGL 事件派发器还在遍历这棵子树时把它抹掉 → hard fault（F4）。
 *   - 切换输出只调 audio_service_set_output()，按钮图标由 1 s 的 tick 统一刷新，
 *     避免"点了没反应/图标和实际不一致"。
 */

static lv_obj_t  *s_out_btn_icon;                 /* 控制排上输出按钮里的图标 */
static lv_obj_t  *s_out_overlay;                  /* 输出设备弹窗（lv_layer_top 上） */
static audio_out_t s_out_shown = (audio_out_t)-1; /* 按钮图标当前画的是哪个输出 */

static const char *out_icon_of(audio_out_t o)
{
    switch (o) {
    case AUDIO_OUT_BLUETOOTH: return LV_SYMBOL_BLUETOOTH;
    case AUDIO_OUT_NONE:      return LV_SYMBOL_MUTE;
    default:                  return LV_SYMBOL_VOLUME_MAX;
    }
}

static const char *out_name_of(audio_out_t o)
{
    switch (o) {
    case AUDIO_OUT_BLUETOOTH: {
        /*
         * 需求：这里显示"蓝牙音频设备 <设备名> 已连接"，而不是干巴巴一个类别名。
         * 名字来自 bt_service_peer_name()（A2DP 连上时写进去的对端名）。
         * 用 static 缓冲返回：调用方都在 LVGL 任务里、拿到就立刻拷进 label，单线程安全。
         * 没连上（或还没拿到名字）时退回原来的类别名。
         */
        static char bt_desc[BT_NAME_MAX + 32];
        const char *nm = bt_service_peer_name();
        if (nm && nm[0]) {
            snprintf(bt_desc, sizeof(bt_desc), "蓝牙音频设备 %s 已连接", nm);
            return bt_desc;
        }
        return "蓝牙音频设备";
    }
    case AUDIO_OUT_NONE:      return "静音";
    default:                  return "板载扬声器";
    }
}

static void out_picker_close(void)
{
    if (s_out_overlay) {
        lv_obj_delete_async(s_out_overlay);
        s_out_overlay = NULL;
    }
}

static void on_out_pick(lv_event_t *e)
{
    const audio_out_t o = (audio_out_t)(intptr_t)lv_event_get_user_data(e);
    esp_err_t ret = audio_service_set_output(o);
    if (ret == ESP_ERR_INVALID_STATE) {
        ui_toast("先在「设置 → 蓝牙」里连接设备", 2000);   /* 弹窗留着，方便连完再来 */
        return;
    }
    if (ret != ESP_OK) {
        ui_toast("切换失败", 1500);
        return;
    }
    ui_toast(out_name_of(o), 1200);
    out_picker_close();
}

static void on_out_scrim(lv_event_t *e)
{
    (void)e;
    out_picker_close();
}

/* 一行可选输出：图标块 + 名字 + 状态（当前在用的标"使用中"） */
static void out_picker_row(lv_obj_t *col, audio_out_t o, const char *sym, uint32_t accent)
{
    const bool cur = (audio_service_get_output() == o);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, cur ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* 当前输出那行加一圈琥珀描边（比只靠底色深浅更好认） */
    lv_obj_set_style_border_width(row, cur ? UI_PX_H(2) : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_ACCENT, LV_PART_MAIN);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(40), UI_PX_H(40));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(nm, out_name_of(o));
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, UI_PX_H(52), 0);

    const char *st = NULL;
    if (o == AUDIO_OUT_BLUETOOTH) {
        st = bt_service_is_connected()
                 ? (bt_service_is_streaming() ? "已连接 · 推流中" : "已连接 · 未推流")
                 : "未连接设备";
    } else if (o == AUDIO_OUT_SPEAKER) {
        st = "ES8389 + NS4150B";
    } else {
        st = "关功放（完全静音）";
    }
    lv_obj_t *sv = lv_label_create(row);
    lv_obj_set_style_text_font(sv, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(sv, cur ? UI_COLOR_ACCENT : UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(sv, cur ? "使用中" : st);
    lv_obj_align(sv, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(row, on_out_pick, LV_EVENT_CLICKED, (void *)(intptr_t)o);
}

/* 开/关弹窗（再点一次按钮就关掉） */
static void on_output_click(lv_event_t *e)
{
    (void)e;
    if (s_out_overlay) {
        out_picker_close();
        return;
    }
    ui_volume_hide();      /* 焦点转移：把音量条收掉 */

    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, LV_PART_MAIN);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, on_out_scrim, LV_EVENT_CLICKED, NULL);
    s_out_overlay = scrim;

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, UI_PCT_X(58));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_PX_H(16), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, UI_PX_H(14), LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* 吃掉点击，别穿透到遮罩 */
    lv_obj_center(panel);

    lv_obj_t *title = lv_label_create(panel);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(title, "选择输出设备");

    out_picker_row(panel, AUDIO_OUT_SPEAKER,   LV_SYMBOL_VOLUME_MAX, 0xE8973A);
    out_picker_row(panel, AUDIO_OUT_BLUETOOTH, LV_SYMBOL_BLUETOOTH,  0x4A6FA5);
    out_picker_row(panel, AUDIO_OUT_NONE,      LV_SYMBOL_MUTE,       0x6E6E78);

    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(hint, "点空白处关闭");
}

/* 按钮图标跟着实际输出走（由 tick_cb 调用，改动才写 LVGL） */
static void out_btn_sync(void)
{
    if (!s_out_btn_icon) {
        return;
    }
    const audio_out_t cur = audio_service_get_output();
    if (cur != s_out_shown) {
        s_out_shown = cur;
        lv_label_set_text(s_out_btn_icon, out_icon_of(cur));
    }
}

/* ------------------------------------------------------------------ 1s 刷新 */

/* 播放服务事件（GMF 任务上下文）：只置标志，别碰 LVGL */
static void on_audio_evt(audio_evt_t evt, void *user)
{
    (void)user;
    switch (evt) {
    case AUDIO_EVT_FINISHED:
        s_advance_req = true;
        break;
    case AUDIO_EVT_ERROR:
        /* 解码失败 / 格式不支持：先报错再跳过，不然界面看着像卡死 */
        s_err_req     = true;
        s_advance_req = true;
        break;
    default:
        break;
    }
}

/*
 * 文本没变就别写 label。
 *
 * lv_label_set_text() 每次都会 free 旧串 + malloc 新串，并对整块区域
 * lv_obj_invalidate()（LVGL 9.5 的 set_text_internal）。而 tick_cb 是 500 ms
 * 一次的常驻定时器，"当前时间 / 总时长 / 播放按钮"里总时长和按钮图标绝大多数
 * 时候是不变的 —— 无条件重写等于让界面和堆每 500 ms 抖一次。
 *
 * 这里直接读 label 当前文本做比较，不额外维护一份缓存：缓存很容易和别处的
 * 直接写入（切歌、暂停等路径）不同步，反而会漏掉该有的刷新。
 */
static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

static void update_lyric_highlight(void)
{
    /* 纯文本歌词（内嵌 USLT 没有时间轴）：静态展示，不高亮也不滚动 */
    if (!s_lrc.timed) {
        return;
    }
    int idx = media_meta_lrc_index_at(&s_lrc, s_elapsed_ms);
    if (idx == s_lyric_cur || !s_p_lyric_col) {
        return;
    }
    const int prev = s_lyric_cur;
    s_lyric_cur = idx;

    /*
     * 只重设"上一行"和"当前行"。
     *
     * 歌词标签建的时候统一是 UI_FONT_SMALL + UI_COLOR_FG_DIM（见下面重建歌词
     * 标签那段），不变式是"只有第 s_lyric_cur 行是强调色 + 大一号字"，
     * 所以换行时动这两行就够。
     *
     * 原来对全部（最多 LYRIC_VIEW_MAX_LINES = 80）行调 set_style_text_font：
     * 改字号等于改行高，会触发整列 flex 重新布局 —— 每换一行歌词就卡一下。
     */
    const int touched[2] = { prev, idx };
    for (size_t k = 0; k < 2; k++) {
        const int i = touched[k];
        if (i < 0 || (size_t)i >= s_p_lyric_n || !s_p_lyric_labels[i]) {
            continue;
        }
        const bool cur = (i == idx);
        lv_obj_set_style_text_color(s_p_lyric_labels[i],
                                    cur ? UI_COLOR_ACCENT : UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_p_lyric_labels[i],
                                   cur ? UI_FONT_TEXT : UI_FONT_SMALL, LV_PART_MAIN);
    }

    /*
     * 用 translate_y 把整列平移到"当前行居中"。
     * 居中基准必须是**歌词区自己的高度**：歌词区现在在封面右侧（右栏），
     * 用 UI_H/2 会把整列推到屏幕外 —— 这正是旧版"看不到歌词"的原因。
     */
    if (idx >= 0 && idx < (int)s_p_lyric_n && s_p_lyric_box) {
        lv_area_t a;
        lv_obj_get_coords(s_p_lyric_box, &a);
        const int32_t box_h  = lv_area_get_height(&a);
        const int32_t line_h = UI_PX_H(26);
        const int32_t target = box_h / 2 - line_h / 2 - (int32_t)idx * line_h;
        lv_obj_set_style_translate_y(s_p_lyric_col, target, LV_PART_MAIN);
    }
}

/* 蓝牙媒体命令：跑在 LVGL 任务里（由 s_media_timer 每 100 ms 取一次） */
static void media_cmd_tick(lv_timer_t *t)
{
    (void)t;
    const int cmd = s_media_cmd;
    if (cmd == 0) {
        return;
    }
    s_media_cmd = 0;
    switch (cmd) {
    case 1:      /* ESP_BT_AUDIO_MEDIA_CTRL_CMD_PLAY */
        if (s_index >= 0) {
            /* 管线是 STOPPED（开机恢复的曲目）时 resume 无效，要从记忆进度起播 */
            if (audio_service_resume() != ESP_OK) {
                if (s_on_mode && s_on_stream[0]) {
                    start_track_online_at(s_index, s_on_stream);
                } else {
                    start_track_at(s_index, s_elapsed_ms);
                }
            }
            s_playing = true;
        }
        break;
    case 2:      /* PAUSE */
        audio_service_pause();
        s_playing = false;
        break;
    case 3:      /* STOP */
        audio_service_stop();
        s_playing = false;
        break;
    case 4:      /* NEXT */
        play_next(+1);
        break;
    case 5:      /* PREV */
        play_next(-1);
        break;
    default:
        break;
    }
    /* 暂停 / 停止（含耳机上按的）：立刻把进度落盘 */
    if ((cmd == 2 || cmd == 3) && s_track_path[0]) {
        save_progress(s_elapsed_ms);
        play_state_commit();
    }
    if (s_p_btn) {
        lv_label_set_text(lv_obj_get_child(s_p_btn, 0),
                          s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    ESP_LOGI(TAG, "media ctrl %d applied", cmd);
}

void app_music_on_media_ctrl(int cmd)
{
    s_media_cmd = cmd;      /* 只置标志：这里可能是蓝牙栈的任务上下文 */
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;

    /* 在线曲目：下载完成 → 起播；失败 → 提示（都在 LVGL 任务里处理） */
    if (s_on_ready) {
        const int idx = s_on_ready_idx;
        s_on_ready = false;
        s_on_ready_idx = -1;
        if (idx >= 0 && s_on_ready_seq == s_on_seq) {
            start_track_online_at(idx, s_on_stream);
        } else {
            /* 过期 ready（用户中间又点了别的歌，工作线程发布窗口里的旧结果）：
             * 只把它的缓冲释放掉，不起播。 */
            online_discard_ready();
        }
    } else if (s_on_req >= 0) {
        online_prepare_kick();
    } else if (s_on_failed) {
        s_on_failed = false;
        ui_toast("在线歌曲获取失败（网络/版权限制？）", 2200);
        /* 缓冲提示不能留在屏上：把歌词区恢复成 歌词 / 暂无歌词
         * （s_buf_show 也要清掉，否则下一个 tick 又把"缓冲中…"写回去） */
        s_buf_show = false;
        lyric_apply_mode();
    }
    /* 注：原先这里会"顺手预取下一首的封面/歌词"，但它会占住唯一的准备槽位，
     * 用户这时点歌就得干等它跑完（实测 1~3 秒）——为了"点了就切"的及时性，去掉。 */

    /*
     * 在线歌缓冲中：把**下载百分比**刷到右栏说明行（需求："缓冲的时候…
     * 显示加载百分比"）。
     *
     * ★ 显示窗口 = 从 set_buffering_hint() 到**真的出声**
     *   （audio_service_output_active）。为什么不用 s_on_busy 判：
     *   s_on_busy 只覆盖"解析地址 + 抓封面/歌词"那一段，它在
     *   audio_service_play_uri() 之前就结束了 —— 那时音频的 HTTP 流还没开始，
     *   进度恒为 0，百分比几乎永远显示不出来。而 s_playing 又太早：
     *   pipeline 一 run() 就是 playing，声音还没出来。所以用"PCM 到过 codec"
     *   这个信号（见 audio_service_output_active 的注释）。
     *   服务端分块传输（没有 Content-Length）时 stream_progress() 返回 false，
     *   那就保持 set_buffering_hint() 写的那句"缓冲中，请稍后..."。
     */
    if (s_buf_show) {
        if (audio_service_output_active()) {
            s_buf_show = false;
            lyric_apply_mode();          /* 出声了：说明行交回 歌词 / 暂无歌词 */
        } else if (s_p_lyric_hint) {
            int pct = 0;
            if (audio_service_stream_progress(&pct)) {
                lv_label_set_text_fmt(s_p_lyric_hint, "缓冲中… %d%%", pct);
            }
        }
    }

    /*
     * 播放结束 / 解码出错 → 换下一首。
     *
     * 为什么不在事件回调里直接换：事件回调跑在 GMF 播放任务里，在那里调
     * esp_audio_simple_player_run() 等于在播放器自己的回调里重入播放器。
     * 所以回调只置标志，真正的动作放到这里（LVGL 任务上下文）。
     */
    bool err  = s_err_req;
    bool done = s_advance_req;
    s_err_req     = false;
    s_advance_req = false;

    if (err) {
        s_fail_streak++;
        if (s_fail_streak >= 3) {
            ui_toast("连续几首都放不了，先停下", 2200);
            done = false;
            s_index = -1;
        } else {
            ui_toast("这首放不了，已跳过", 1600);
        }
    }
    /* 真的播起来一会儿才算这首没问题，清掉失败计数（否则一首坏文件会一直跳） */
    if (s_playing && s_elapsed_ms > 3000) {
        s_fail_streak = 0;
    }
    if (done && storage_service_count() > 0) {
        if (s_play_mode == PLAY_MODE_ONE && s_index >= 0) {
            /* 单曲循环：同一首从头再放（坏文件由上面的 fail_streak 兜住，
             * 连续失败 3 次会把 done 置假并停下，不会死循环） */
            start_track(s_index);
        } else if (storage_service_count() > 1) {
            play_next(+1);
        }
    } else if (done && s_fail_streak >= 3) {
        s_fail_streak = 0;
    }

    /*
     * ★ 墙钟只在**真的出声之后**才累加（2026-10 修"歌词与歌曲不同步"）：
     *   simple_player 没有位置 API，进度只能靠墙钟累加；而在线歌从 run() 到
     *   真正出声之间还有 1~3 秒的缓冲 —— 以前这段时间也被算进 s_elapsed_ms，
     *   于是歌词**整体提前**（缓冲越久偏得越多）。
     *   这里加一道 audio_service_output_active()（PCM 真的写到 codec/蓝牙那一刻
     *   才为真，见 audio_service.h），起播前不计时，误差就消失了。
     *   顺带把暂停/卡顿期间也排除了（同样的道理）。
     */
    if (audio_service_is_playing() && audio_service_output_active()) {
        /* s_resume_ms > 0 期间先不累计：这时音源还在 0 附近走，等下面
         * 定位完成后再把 s_elapsed_ms 对齐到续播位置，否则显示会来回跳。 */
        if (s_resume_ms == 0) {
            s_elapsed_ms += TICK_MS;
        }
    }
    s_playing = audio_service_is_playing();

    /*
     * 进度记忆（需求）：从"上次听到的位置"续播。
     *
     * 为什么放在这里而不是 start_track_at()：按字节定位需要平均码率，
     * 而码率要等播放器发出 MUSIC_INFO 事件之后才有（见 audio_service_seek_ms
     * 的限制说明）。所以起播时先把位置记在 s_resume_ms，这里每 500 ms 试一次，
     * 码率一到就定位，然后清掉标志（只尝试一次，失败不反复 seek）。
     */
    if (s_resume_ms > 0 && s_playing && s_track_path[0]) {
        const uint32_t target = s_resume_ms;
        if (track_seek_unsupported(s_track_path)) {
            /* M4A 是随机访问容器，按字节 seek 后解码器对不上帧 → 只能从头放 */
            s_resume_ms  = 0;
            s_elapsed_ms = 0;
            ESP_LOGW(TAG, "resume unsupported for m4a, start from 0");
        } else {
            audio_info_t info;
            const bool info_ok = audio_service_get_info(&info) && info.bitrate > 0;
            if (++s_resume_wait > RESUME_WAIT_MAX_TICKS) {
                /* 兜底：一直等不到码率（容器/格式报不出 bitrate）就放弃定位，
                 * 否则 s_elapsed_ms 会被"续播位置"钉死，进度条与时间都不再走。
                 * 真实已播 = 起播到现在经过的墙钟。 */
                s_resume_ms  = 0;
                s_elapsed_ms = (uint32_t)s_resume_wait * TICK_MS;
                ESP_LOGW(TAG, "no bitrate for resume, continue from wall clock");
            } else if (info_ok) {
                s_resume_ms = 0;
                if (audio_service_seek_ms(target) == ESP_OK) {
                    s_elapsed_ms = target;
                    s_lyric_cur  = -1;
                    ESP_LOGI(TAG, "resumed at %u ms", (unsigned)target);
                } else {
                    s_elapsed_ms = 0;      /* 定位失败：界面别停在假进度上 */
                }
            }
        }
    }

    /*
     * 进度记忆：每 5 秒把"当前曲目 + 已播毫秒"同步到 play_state 的 RAM 态，
     * 真正写 flash 由 play_state 内部按 8 秒防抖合并，不会狂写 NVS。
     */
    if (++s_save_ticks >= PROGRESS_SAVE_TICKS) {
        s_save_ticks = 0;
        if (s_playing && s_track_path[0]) {
            save_progress(s_elapsed_ms);
        }
    }

    /* 总时长：优先用探测到的准确值（Xing/Info 帧数），否则按 文件大小/码率 估 */
    if (s_dur_ms == 0) {
        audio_info_t info;
        const track_t *tr = storage_service_track((size_t)(s_index < 0 ? 0 : s_index));
        if (s_index >= 0 && tr && audio_service_get_info(&info)) {
            if (info.duration_ms >= 1000) {
                s_dur_ms = info.duration_ms;
            } else if (info.bitrate > 0) {
                s_dur_ms = (uint32_t)((uint64_t)tr->size_bytes * 8ULL * 1000ULL / (uint64_t)info.bitrate);
                if (s_dur_ms < 1000) {
                    s_dur_ms = 0;
                }
            }
        }
    }

    /* 进度：千分比显示，跟总时长解耦（时长未知也不会跳） */
    if (s_p_seek && !s_seeking) {
        int32_t permille = 0;
        if (s_dur_ms > 0) {
            permille = (int32_t)((uint64_t)s_elapsed_ms * 1000ULL / s_dur_ms);
        }
        lv_bar_set_value(s_p_seek, permille, LV_ANIM_OFF);
    }
    if (s_p_cur && !s_seeking) {
        char buf[16];
        fmt_time(s_elapsed_ms, buf, sizeof(buf));
        label_set_if_changed(s_p_cur, buf);
    }
    if (s_p_tot) {
        char buf[16];
        fmt_time(s_dur_ms, buf, sizeof(buf));
        label_set_if_changed(s_p_tot, buf);
    }
    if (s_p_btn) {
        label_set_if_changed(lv_obj_get_child(s_p_btn, 0),
                             s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    /* 输出设备可能被设置页/蓝牙事件改过：按钮图标跟着走 */
    out_btn_sync();
    if (s_lyric_mode) {
        update_lyric_highlight();
    }

    /* 封面：先把工作线程的产出搬上屏（下标对不上就丢掉），再按需拉起下一次解码 */
    if (s_cover_ready) {
        s_cover_ready = false;
        if (s_cover_out_idx == s_index) {
            cover_show(s_cover_out, s_cover_out_w, s_cover_out_h);
        } else if (s_cover_out) {
            media_meta_cover_free(s_cover_out);
        }
        s_cover_out = NULL;
    }
    cover_kick();
}

static void player_on_delete(lv_event_t *e)
{
    (void)e;
    /* 弹窗挂在 top layer 上，不随页面销毁：离开播放器页就显式关掉它，
     * 否则它会一直盖在别的页面上 */
    out_picker_close();
    s_out_btn_icon = NULL;
    s_out_shown    = (audio_out_t)-1;
    /* 离开播放器页就把进度落盘：用户很可能紧接着就断电/退出 */
    if (s_track_path[0]) {
        save_progress(s_elapsed_ms);
        play_state_commit();
    }
    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    if (s_media_timer) {
        lv_timer_delete(s_media_timer);
        s_media_timer = NULL;
    }
    s_p_title = s_p_artist = s_p_cur = s_p_tot = NULL;
    s_p_seek = s_p_seek_hit = s_p_btn = NULL;
    s_p_cover_box = s_p_cover_img = NULL;
    s_p_lyric_box = s_p_lyric_hint = s_p_lyric_col = NULL;
    s_p_mode_btn = s_p_mode_badge = NULL;
    s_lyric_mode = false;
    s_seeking    = false;
    s_p_lyric_n  = 0;
    memset(s_p_lyric_labels, 0, sizeof(s_p_lyric_labels));

    /* 离开播放器页：没起播的在线结果直接丢弃（封面/歌词缓冲一并释放） */
    online_discard_ready();

    /*
     * ★ 解码好的封面**故意不释放**（2026-10 用户报"切页面回来封面就没了"）：
     *   s_cover_pixels 是 PSRAM 里的 RGB565（240×240 ≈ 115 KB），s_cover_dsc 是
     *   指向它的堆描述符 —— 两者都不属于 LVGL 对象，页面销毁后依然有效。
     *   留着它，下次进播放器页直接 lv_image_set_src() 贴回去，不用重新下载/解码；
     *   真换曲时 start_track_*() 会 cover_show(NULL,...) 把它释放掉（有界，不会累积）。
     *   s_p_cover_img 归 LVGL，随页面销毁，这里只把指针清掉。
     */
    s_p_cover_box = NULL;
    s_p_cover_img = NULL;
}

/* ------------------------------------------------------------------ 播放器页 */

static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb,
                                lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, UI_PX_H(56), UI_PX_H(56));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, fg, LV_PART_MAIN);
    lv_label_set_text(l, sym);
    lv_obj_center(l);

    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    }
    return b;
}

/* ------------------------------------------------------------------ 封面 */

static void cover_task(void *arg);   /* 前向声明：cover_kick 在它上面 */

/* 丢掉没人认领的解码结果（换曲太快、页面已退出等） */
static void cover_reap(void)
{
    if (s_cover_ready) {
        s_cover_ready = false;
        if (s_cover_out) {
            media_meta_cover_free(s_cover_out);
            s_cover_out = NULL;
        }
    }
}


/* 有请求且空闲就起解码任务（必须在 LVGL 任务上下文调用） */
static void cover_kick(void)
{
    if (s_cover_busy || s_cover_want < 0) {
        return;
    }
    const int idx = s_cover_want;
    s_cover_want = -1;

    cover_req_t *req = (cover_req_t *)malloc(sizeof(cover_req_t));
    if (!req) {
        return;
    }
    req->idx = idx;
    if (s_on_mode) {
        /* 在线歌曲：封面字节已经在 PSRAM 里（不落 SD）。
         * 把缓冲**转移**给请求：请求独占、解码线程用完释放，
         * 之后切换任何歌曲都不会再碰它。 */
        if (!s_on_cover_data) {
            free(req);              /* 没抓到封面：不排队解码（占位图兜底） */
            return;
        }
        req->from_mem = true;
        req->data     = s_on_cover_data;
        req->len      = s_on_cover_len;
        s_on_cover_data = NULL;
        s_on_cover_len  = 0;
    } else {
        /* 本地歌曲：从音频文件里取内嵌封面。
         * 把路径**拷进请求结构**再交给工作线程：线程只认这块内存，
         * 不再去读 storage_service 的曲目表（扫描任务可能正在重建它） */
        const track_t *t = storage_service_track((size_t)idx);
        if (!t) {
            free(req);
            return;
        }
        req->from_mem = false;
        req->data     = NULL;
        req->len      = 0;
        snprintf(req->path, sizeof(req->path), "%s", t->path);
    }

    s_cover_busy = true;
    if (xTaskCreate(cover_task, "cover", COVER_TASK_STACK, req, 2, NULL) != pdPASS) {
        s_cover_busy = false;
        free(req);
        ESP_LOGW(TAG, "cover task create failed");
    }
}

/*
 * 封面解码任务：只读文件（本地）/ 解内存缓冲（在线）+ 解 JPEG/PNG + 写全局标志，
 * **绝不碰 LVGL**。结果由 tick_cb（LVGL 任务）搬到对象上。
 */
static void cover_task(void *arg)
{
    cover_req_t *req = (cover_req_t *)arg;
    uint8_t *px = NULL;
    int w = 0, h = 0;

    esp_err_t ret = req->from_mem
                        ? media_meta_image_mem_rgb565(req->data, req->len,
                                                      COVER_MAX_DIM, &px, &w, &h)
                        : media_meta_cover_rgb565(req->path, COVER_MAX_DIM, &px, &w, &h);
    if (req->data) {
        free(req->data);        /* 内存封面：本请求独占的拷贝，用完释放 */
    }
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "no cover for idx=%d (%s)", req->idx, esp_err_to_name(ret));
        px = NULL;
    }
    s_cover_out     = px;
    s_cover_out_w   = w;
    s_cover_out_h   = h;
    s_cover_out_idx = req->idx;
    s_cover_ready   = true;
    s_cover_busy    = false;
    free(req);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- 在线曲目下载 */

typedef struct {
    online_song_t song;      /* 拷贝一份，工作线程不再读共享数组 */
    int           idx;
    bool          prefetch;  /* 只是提前缓存，不下发起播 */
    uint32_t      seq;       /* 发起时的请求序号 */
} online_req_t;

static void online_prepare_task(void *arg);

/* 起一个下载任务（LVGL 任务里调；HTTP+TLS 不能放在 LVGL 任务里） */
static void online_prepare_kick(void)
{
    if (s_on_busy || s_on_req < 0 || !s_on_songs) {
        return;
    }
    const int idx = s_on_req;
    s_on_req = -1;
    if (idx < 0 || (size_t)idx >= s_on_count) {
        return;
    }
    online_req_t *req = (online_req_t *)malloc(sizeof(online_req_t));
    if (!req) {
        s_on_failed = true;
        return;
    }
    req->song = s_on_songs[idx];
    req->idx  = idx;
    req->prefetch = s_on_prefetch;
    req->seq      = s_on_seq;
    s_on_prefetch = false;
    s_on_busy = true;
    if (xTaskCreate(online_prepare_task, "on_fetch", ONLINE_TASK_STACK, req, 2, NULL) != pdPASS) {
        s_on_busy   = false;
        s_on_failed = true;
        free(req);
        ESP_LOGW(TAG, "online fetch task create failed");
    }
}

static void online_prepare_task(void *arg)
{
    online_req_t *req = (online_req_t *)arg;
    char stream[560] = {0};
    char path[STORAGE_PATH_MAX] = {0};

    /* 预取只抓"下一首"的封面/歌词（几十 KB），音频等到真要播时再解析 */
    if (req->prefetch) {
        online_song_prepare_stream(&req->song);
        ESP_LOGI(TAG, "prefetched meta for '%s'", req->song.title);
        s_on_busy = false;
        free(req);
        vTaskDelete(NULL);
        return;
    }

    /* 1) 解析 302 拿最终音频地址（不下载）→ 交给播放器流式播放 */
    s_on_dur_ms = 0;
    esp_err_t ret = online_song_stream_info(&req->song, stream, sizeof(stream),
                                             NULL, &s_on_dur_ms);
    uint8_t local_only = 0;
    if (ret != ESP_OK) {
        /* 2) 流式地址拿不到（版权/网络）→ 退回"先下载再播" */
        ESP_LOGW(TAG, "stream url failed (%s), fallback to download", esp_err_to_name(ret));
        ret = online_song_prepare(&req->song, path, sizeof(path));
        local_only = 1;
    } else {
        /* 只有封面与歌词要提前抓好（进 PSRAM 缓存，不落 SD） */
        online_song_prepare_stream(&req->song);
    }
    /* 封面/歌词各取一份拷贝（PSRAM），起播时直接给解码/解析用 */
    uint8_t *jpg = NULL;
    size_t   jpg_len = 0;
    char    *lrc = NULL;
    size_t   lrc_len = 0;
    online_song_cover_data(&req->song, &jpg, &jpg_len);
    online_song_lyrics_data(&req->song, &lrc, &lrc_len);

    /* 期间用户又点了别的歌：立刻收工，把"正在准备"让出去，
     * 让新的那首尽快（下一个 tick）开始解析 */
    if (req->seq != s_on_seq) {
        ESP_LOGI(TAG, "abort stale online fetch (idx=%d)", req->idx);
        free(jpg);
        free(lrc);
        s_on_busy = false;
        free(req);
        vTaskDelete(NULL);
        return;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "online prepare failed: %s", esp_err_to_name(ret));
        free(jpg);
        free(lrc);
        s_on_failed = true;
    } else if (req->seq != s_on_seq) {
        ESP_LOGI(TAG, "drop stale online result (idx=%d)", req->idx);
        free(jpg);
        free(lrc);
    } else {
        snprintf(s_on_stream, sizeof(s_on_stream), "%s", local_only ? path : stream);
        s_on_stream_local = (local_only != 0);
        /* 封面/歌词缓冲：**所有权转给 LVGL 任务**（只有它在消费时释放，
         * 见 s_on_cover_data 的注释与 online_discard_ready） */
        s_on_cover_data = jpg;
        s_on_cover_len  = jpg_len;
        s_on_lrc_data   = lrc;
        s_on_lrc_len    = lrc_len;
        s_on_ready_idx  = req->idx;
        s_on_ready_seq  = req->seq;
        s_on_ready      = true;      /* 最后置位：上面的字段先写好 */
    }
    s_on_busy = false;
    free(req);
    vTaskDelete(NULL);
}

/*
 * 起播一首在线歌（LVGL 任务上下文）。
 * uri 可能是 https 流地址（s_on_stream）或本地缓存路径（下载兜底）：
 * audio_service_play_uri() 两种都认。
 * 封面/歌词走内存缓冲（s_on_cover_data / s_on_lrc_data，PSRAM，不落 SD）：
 *   歌词在这里解析完就释放；封面转交给 cover_kick 的解码请求（请求独占）。
 */
static void start_track_online_at(int idx, const char *uri)
{
    if (!s_on_songs || idx < 0 || (size_t)idx >= s_on_count || !uri || !uri[0]) {
        return;
    }
    const online_song_t *song = &s_on_songs[idx];

    s_on_mode    = true;
    s_index      = idx;
    s_resume_ms  = 0;
    s_elapsed_ms = 0;
    s_dur_ms     = s_on_dur_ms;      /* 在线总时长（估算值；未知则为 0） */
    s_seeking    = false;
    s_lyric_cur  = -1;
    s_save_ticks = 0;
    s_resume_wait = 0;
    snprintf(s_track_path, sizeof(s_track_path), "%s", uri);

    /* 歌词：直接从内存里的 LRC 文本解析（不再读 SD 上的 .lrc 文件）。
     * 解析会把每一行拷进 s_lrc，所以这里解析完就可以释放缓冲。 */
    bool lrc_ok = false;
    if (s_on_lrc_data) {
        lrc_ok = (media_meta_lrc_parse(s_on_lrc_data, &s_lrc) == ESP_OK && s_lrc.count > 0);
        free(s_on_lrc_data);
        s_on_lrc_data = NULL;
        s_on_lrc_len  = 0;
    }
    /* 下载兜底（本地音频文件）：音频文件旁/全局 lrc 目录若还有边车歌词，兜底找一次 */
    if (!lrc_ok && s_on_stream_local) {
        lrc_ok = (media_meta_lyrics_load(uri, &s_lrc) == ESP_OK && s_lrc.count > 0);
    }
    if (!lrc_ok) {
        media_meta_lrc_reset(&s_lrc);
    }
    if (s_p_title) {
        lv_label_set_text(s_p_title, song->title);
    }
    if (s_p_artist) {
        lv_label_set_text(s_p_artist, song->artist[0] ? song->artist : "未知歌手");
    }
    lyric_rebuild();
    if (s_lyric_mode) {
        update_lyric_highlight();
    }

    /*
     * ★ 重新挂上"缓冲中"提示：上面的 lyric_rebuild()/lyric_apply_mode() 会把说明行
     * 交回歌词，但这时音频的 HTTP 流**还没开始下**（audio_service_play_uri 就在
     * 下面几行）—— 而"缓冲中 xx%"最有意义的就是接下来这一段。tick_cb 会把它刷成
     * 百分比，等真的出声（audio_service_output_active）再交回 lyric_apply_mode()。
     */
    set_buffering_hint();

    cover_show(NULL, 0, 0);
    cover_reap();
    s_cover_want = idx;
    cover_kick();

    if (s_p_seek) {
        lv_bar_set_value(s_p_seek, 0, LV_ANIM_OFF);
    }
    if (s_p_cur) {
        lv_label_set_text(s_p_cur, "0:00");
    }

    /* 进度记忆只对下载兜底（本地文件）做：流地址是带时效签名的，存下来没意义 */
    if (s_on_stream_local) {
        save_progress(0);
        play_state_commit();
    }

    if (audio_service_play_uri(uri) != ESP_OK) {
        ui_toast("在线歌曲播放失败（网络或格式不支持）", 2200);
        return;
    }
    s_playing = true;
    if (s_p_btn) {
        lv_label_set_text(lv_obj_get_child(s_p_btn, 0), LV_SYMBOL_PAUSE);
    }
    ESP_LOGI(TAG, "online track %d/%u: %s - %s (%s)",
             idx + 1, (unsigned)s_on_count, song->title, song->artist,
             s_on_stream_local ? "cached" : "streaming");
}

/*
 * ★ 在线切歌的"界面先行"：**先把播放器页切成目标歌曲，再显示缓冲信息**。
 *
 * 为什么要这样（用户 2026-10 提的）：点歌到真正起播之间有 1~3 秒
 * （解析 302 地址 + 抓封面 + 取歌词，都在工作线程里），这段时间如果界面上
 * 还留着**上一首**的标题/歌手/封面/歌词，看起来就是"点了没反应、切歌没生效"。
 * 现在点下去就先把目标歌曲的信息铺上去，随后再由 tick_cb 在右栏说明行上
 * 打"缓冲中… xx%"。
 *
 * 只动 UI 与几个状态量，**不碰播放器**：停旧歌由调用方负责（先停再调本函数），
 * 封面/歌词等音频就绪后由 start_track_online_at() 再写一遍同一套字段（幂等）。
 */
static void online_switch_ui_to(int idx)
{
    if (!s_on_songs || idx < 0 || (size_t)idx >= s_on_count) {
        return;
    }
    const online_song_t *song = &s_on_songs[idx];

    if (s_p_title) {
        lv_label_set_text(s_p_title, song->title);
    }
    if (s_p_artist) {
        lv_label_set_text(s_p_artist, song->artist[0] ? song->artist : "未知歌手");
    }
    /* 总时长/进度都还不知道：给占位值（tick 里对 s_dur_ms == 0 处处有保护） */
    if (s_p_tot) {
        lv_label_set_text(s_p_tot, "--:--");
    }
    if (s_p_cur) {
        lv_label_set_text(s_p_cur, "0:00");
    }
    if (s_p_seek) {
        lv_bar_set_value(s_p_seek, 0, LV_ANIM_OFF);
    }
    s_elapsed_ms = 0;
    s_dur_ms     = 0;
    s_buf_show   = false;      /* 让 set_buffering_hint() 重新置位（下面调用方会调） */
    s_seeking    = false;

    /* 封面：先退回占位（别把上一首的封面留在屏上），就绪后再解新的 */
    cover_show(NULL, 0, 0);
    cover_reap();
    s_cover_want = idx;

    /* 歌词：先清空并重画（此时新歌词还没抓到），别继续显示上一首的歌词；
     * lyric_rebuild() 会顺带把右栏说明行切成"暂无歌词"，紧接着调用方的
     * set_buffering_hint() 会把它改成"缓冲中…"。 */
    media_meta_lrc_reset(&s_lrc);
    lyric_rebuild();
}

/*
 * 在线音乐 app 的入口：把一份在线歌单交给播放器并从第 idx 首开始播。
 * **必须在 LVGL 任务上下文调用**（会更新播放器页控件）。
 */
void app_music_play_online(const online_song_t *songs, size_t count, size_t idx)
{
    if (!songs || count == 0 || idx >= count) {
        return;
    }
    s_on_songs = songs;
    s_on_count = count;
    s_on_mode  = true;
    s_index    = (int)idx;
    s_on_seq++;
    online_discard_ready();      /* 点歌即弃：旧结果（若有）不再起播 */
    s_on_failed = false;
    s_on_req   = (int)idx;

    /*
     * ★ 点击必须**立刻有反馈**：
     *   1) 先把正在播的那首停掉（否则旧歌会一直唱到新歌的"地址解析 + 封面 +
     *      歌词"全都就绪，实测这张网要 1~3 秒，听感就是"点了没反应"）；
     *   2) **再把播放器页切成目标歌曲**（标题/歌手/封面占位/歌词清空/进度归零）；
     *   3) 最后才显示缓冲信息（说明行"缓冲中… xx%"，tick 里刷百分比）。
     * 顺序很关键：用户要的是"先看到切到了哪首，再看到它在缓冲"。
     */
    audio_service_stop();
    s_playing = false;
    if (s_p_btn) {
        lv_label_set_text(lv_obj_get_child(s_p_btn, 0), LV_SYMBOL_PLAY);
    }
    online_switch_ui_to((int)idx);
    ui_toast("缓冲中，请稍后...", 1500);
    set_buffering_hint();
    online_prepare_kick();
}

/*
 * 把封面像素搬到对象上；px 为 NULL 时回到占位色块。必须在 LVGL 任务里调用。
 *
 * 描述符**每张封面新分配一块**（而不是复用同一块改内容）：lv_image 只认这个
 * 指针，LVGL 的图片缓存也是按指针做键的，复用同一块会让它拿到旧内容；
 * 换指针还能避开 LVGL 的内部缓存 API（lv_image_cache_drop 不在公开头文件里）。
 */
static void cover_show(uint8_t *px, int w, int h)
{
    if (s_p_cover_img) {
        lv_image_set_src(s_p_cover_img, NULL);   /* 先摘掉旧的，才能安全释放 */
    }
    if (s_cover_dsc) {
        free(s_cover_dsc);
        s_cover_dsc = NULL;
    }
    if (s_cover_pixels) {
        media_meta_cover_free(s_cover_pixels);
        s_cover_pixels = NULL;
    }

    if (px && s_p_cover_img && w > 0 && h > 0) {
        lv_image_dsc_t *dsc = (lv_image_dsc_t *)calloc(1, sizeof(lv_image_dsc_t));
        if (!dsc) {
            media_meta_cover_free(px);
            lv_obj_add_flag(s_p_cover_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        s_cover_pixels = px;
        dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
        dsc->header.w      = (uint32_t)w;
        dsc->header.h      = (uint32_t)h;
        dsc->header.stride = (uint32_t)w * 2u;
        dsc->data_size     = (uint32_t)w * 2u * (uint32_t)h;
        dsc->data          = s_cover_pixels;
        s_cover_dsc        = dsc;

        lv_image_set_src(s_p_cover_img, dsc);
        lv_obj_remove_flag(s_p_cover_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (px) {
        media_meta_cover_free(px);      /* 页面/对象没了：没人要 */
    }
    if (s_p_cover_img) {
        lv_obj_add_flag(s_p_cover_img, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ 左栏：封面 */

static void build_cover_view(lv_obj_t *parent)
{
    lv_obj_t *cover = lv_obj_create(parent);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(cover, UI_PX_H(16), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cover, UI_COLOR_ICON_LOCAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);   /* 点击交给外层 left */
    s_p_cover_box = cover;

    /* 占位：还没解出封面时显示 */
    lv_obj_t *sym = lv_label_create(cover);
    lv_obj_set_style_text_font(sym, UI_FONT_CLOCK, LV_PART_MAIN);
    lv_obj_set_style_text_color(sym, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(sym, LV_SYMBOL_AUDIO);
    lv_obj_center(sym);

    /* 真封面：RGB565，按 COVER 等比铺满（裁掉多余），圆角与容器一致 */
    s_p_cover_img = lv_image_create(cover);
    lv_obj_set_size(s_p_cover_img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_p_cover_img);
    lv_obj_set_style_radius(s_p_cover_img, UI_PX_H(16), LV_PART_MAIN);
    lv_image_set_inner_align(s_p_cover_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_remove_flag(s_p_cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_p_cover_img, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ 右栏：歌词（封面右侧） */

/* 按当前 s_lrc 重建歌词行；换曲与首次建页都要调 */
static void lyric_rebuild(void)
{
    if (s_p_lyric_col) {
        lv_obj_clean(s_p_lyric_col);
        lv_obj_set_style_translate_y(s_p_lyric_col, 0, LV_PART_MAIN);
    }
    s_p_lyric_n = 0;
    s_lyric_cur = -1;
    memset(s_p_lyric_labels, 0, sizeof(s_p_lyric_labels));

    if (!s_p_lyric_col) {
        return;
    }
    size_t n = s_lrc.count < LYRIC_VIEW_MAX_LINES ? s_lrc.count : LYRIC_VIEW_MAX_LINES;
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *l = lv_label_create(s_p_lyric_col);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(l, LV_PCT(100));
        /* ★ WRAP 而不是 DOT：歌词一句可能很长，DOT 会把它截成"…"（"显示不全"的另一半原因） */
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, s_lrc.lines[i].text);
        s_p_lyric_labels[i] = l;
    }
    s_p_lyric_n = n;
    lyric_apply_mode();     /* 行数变了，提示/歌词列的可见性要跟着变 */
}

/*
 * 在线歌的"解析地址 + 抓封面 + 取歌词"是异步的（实测 1~3 秒）。
 * 这段时间右栏说明行显示"缓冲中，请稍后..."，让用户看到明确的等待状态；
 * 起播时 start_track_online_at() 会走 lyric_rebuild() → lyric_apply_mode()
 * 把它恢复成 歌词 / 暂无歌词。
 *
 * 置 s_buf_show 之后，tick_cb 会把它刷成"缓冲中… xx%"（在线流式的下载进度），
 * 并在真的出声时交回 lyric_apply_mode()。
 */
static void set_buffering_hint(void)
{
    if (!s_p_lyric_hint) {
        return;
    }
    s_buf_show = true;
    lv_obj_remove_flag(s_p_lyric_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_p_lyric_hint, "缓冲中，请稍后...");
}

/*
 * 歌词**常驻显示**（需求：不再用"点封面显示/隐藏"）。
 * 有歌词就显示歌词列、没歌词就显示"暂无歌词"，封面那块也不再是切换按钮，
 * 只是封面本身。s_lyric_mode 保留但恒为 true，历史分支不再改可见性。
 */
static void lyric_apply_mode(void)
{
    const bool has_lyric = (s_p_lyric_n > 0);
    s_lyric_mode = true;

    if (s_p_lyric_hint) {
        if (has_lyric) {
            lv_obj_add_flag(s_p_lyric_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_p_lyric_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_p_lyric_hint, "暂无歌词");
        }
    }
    if (s_p_lyric_col) {
        if (has_lyric) {
            lv_obj_remove_flag(s_p_lyric_col, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_p_lyric_col, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void build_lyric_view(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);       /* 吃掉标题/歌手与控制排之间的剩余高度 */
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    s_p_lyric_box = box;

    s_p_lyric_hint = lv_label_create(box);
    lv_obj_set_style_text_font(s_p_lyric_hint, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_p_lyric_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_p_lyric_hint, "轻点封面显示歌词");
    lv_obj_center(s_p_lyric_hint);

    /* 歌词列：整列用 translate_y 平移让当前行居中（逐行滚动会抖） */
    s_p_lyric_col = lv_obj_create(box);
    lv_obj_remove_style_all(s_p_lyric_col);
    lv_obj_set_width(s_p_lyric_col, LV_PCT(100));
    lv_obj_set_height(s_p_lyric_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_p_lyric_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_p_lyric_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_p_lyric_col, UI_PX_H(6), LV_PART_MAIN);
    lv_obj_remove_flag(s_p_lyric_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_p_lyric_col, LV_OBJ_FLAG_CLICKABLE);

    lyric_rebuild();
    lyric_apply_mode();
}

static lv_obj_t *page_player(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /*
     * 顺序要紧：lv_obj_set_style_pad_all() 展开成上下左右四个 pad，
     * 写在 pad_top 之后会把"给状态栏留的边距"覆盖掉 —— 之前就是这么写的，
     * 结果歌名/歌手钻到 32px 状态栏底下。必须先 pad_all，再补 pad_top。
     */
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PAGE_PAD, LV_PART_MAIN);

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PX_W(20), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 左栏：专辑封面（整块可点 → 在**封面右侧**展开/收起歌词） */
    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, UI_PX_H(240), UI_PX_H(240));
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(left, on_toggle_view, LV_EVENT_CLICKED, NULL);

    build_cover_view(left);

    /* 右栏：信息 + 进度 + 控制 */
    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* 播放器页会被路由压栈打开（首页/本地/在线都会进这里）：返回键由
     * ui_core 的全局返回键自动叠在顶层，这里不再自己画。标题行仍按
     * UI_HEADER_TEXT_X 起排，给左侧按钮留位。 */

    /* 标题与副标题用独立的 block 元素，不要塞进同一个 label 的 inline span */
    s_p_title = lv_label_create(right);
    lv_obj_set_style_text_font(s_p_title, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_p_title, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(s_p_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_p_title, LV_PCT(100));
    if (s_on_mode && s_on_songs && s_index >= 0 && (size_t)s_index < s_on_count) {
        lv_label_set_text(s_p_title, s_on_songs[s_index].title);      /* 在线曲目 */
    } else {
        lv_label_set_text(s_p_title, (s_index >= 0 && storage_service_track((size_t)s_index))
                                         ? storage_service_track((size_t)s_index)->title
                                         : "未在播放");
    }

    s_p_artist = lv_label_create(right);
    lv_obj_set_style_text_font(s_p_artist, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_p_artist, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_p_artist, LV_PCT(100));
    lv_label_set_long_mode(s_p_artist, LV_LABEL_LONG_DOT);
    if (s_on_mode && s_on_songs && s_index >= 0 && (size_t)s_index < s_on_count) {
        lv_label_set_text(s_p_artist, s_on_songs[s_index].artist[0]
                                          ? s_on_songs[s_index].artist : "未知歌手");
    } else if (s_index >= 0) {
        const track_t *t = storage_service_track((size_t)s_index);
        lv_label_set_text(s_p_artist, (t && t->artist[0]) ? t->artist : "未知歌手");
    } else {
        lv_label_set_text(s_p_artist, "请在「本地音乐」里选一首");
    }

    /* 歌词区：在封面右侧，占掉标题/歌手与控制排之间的剩余高度 */
    build_lyric_view(right);

    /*
     * 进度条：外层 30px 透明"抓取区"（自己按触点 x 算比例，松手才真定位），
     * 里面 10px 的 lv_bar 只负责画：轨道 + 已播填充。
     * 用 lv_bar 而不是 lv_slider：按需求去掉拖拽小圆点，只保留进度条本体。
     */
    s_p_seek_hit = lv_obj_create(right);
    lv_obj_remove_style_all(s_p_seek_hit);
    lv_obj_set_size(s_p_seek_hit, LV_PCT(100), UI_PX_H(30));
    lv_obj_set_style_bg_opa(s_p_seek_hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_p_seek_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_p_seek_hit, LV_OBJ_FLAG_CLICKABLE);
    /* 手指横向滑出抓取区也不断手势（与边缘热区同理） */
    lv_obj_add_flag(s_p_seek_hit, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_p_seek_hit, on_seek_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_p_seek_hit, on_seek_press, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_p_seek_hit, on_seek_release, LV_EVENT_RELEASED, NULL);
    /* 手指滑出抓取区时 LVGL 只发 PRESS_LOST，不发 RELEASED：不处理的话
     * s_seeking 会永久停在 true，tick 里的进度更新全被跳过 —— 表现就是
     * "进度条概率性不再动/像消失了"。这里一并按松手处理。 */
    lv_obj_add_event_cb(s_p_seek_hit, on_seek_lost, LV_EVENT_PRESS_LOST, NULL);

    /* 只要进度条本体：用 lv_bar（没有 knob 部件），不要拖拽小圆点 */
    s_p_seek = lv_bar_create(s_p_seek_hit);
    lv_obj_remove_flag(s_p_seek, LV_OBJ_FLAG_CLICKABLE);   /* 触摸都交给抓取区 */
    lv_obj_set_width(s_p_seek, LV_PCT(100));
    lv_obj_set_height(s_p_seek, UI_PX_H(10));
    lv_obj_center(s_p_seek);
    lv_bar_set_range(s_p_seek, 0, 1000);                   /* 千分比，与时长解耦 */
    lv_bar_set_value(s_p_seek, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_p_seek, UI_COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_p_seek, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_p_seek, UI_PX_H(5), LV_PART_MAIN);
    /* 描边保证"时长未知 / 进度为 0"时轨道也一定看得见 */
    lv_obj_set_style_border_width(s_p_seek, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_p_seek, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_p_seek, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_p_seek, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_p_seek, UI_PX_H(5), LV_PART_INDICATOR);

    lv_obj_t *times = lv_obj_create(right);
    lv_obj_remove_style_all(times);
    lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(times, LV_OBJ_FLAG_SCROLLABLE);

    s_p_cur = lv_label_create(times);
    lv_obj_set_style_text_font(s_p_cur, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_p_cur, UI_COLOR_FG_DIM, LV_PART_MAIN);
    {
        /* 开机恢复的曲目：这里直接显示记忆到的进度，而不是 0:00 */
        char buf[16];
        fmt_time(s_elapsed_ms, buf, sizeof(buf));
        lv_label_set_text(s_p_cur, buf);
    }

    s_p_tot = lv_label_create(times);
    lv_obj_set_style_text_font(s_p_tot, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_p_tot, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_label_set_text(s_p_tot, "--:--");

    /* 控制排 */
    lv_obj_t *ctl = lv_obj_create(right);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_size(ctl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);

    /* 循环模式：点击在 列表循环/单曲循环/随机循环 之间切换（图标 + Toast） */
    lv_obj_t *mode_btn = make_round_btn(ctl, LV_SYMBOL_LOOP, on_mode_click,
                                        UI_COLOR_PILL, UI_COLOR_FG);
    s_p_mode_btn = lv_obj_get_child(mode_btn, 0);

    /* 单曲循环角标：LOOP 图标右上角的 "1"（字体里没有 repeat-one 字形） */
    s_p_mode_badge = lv_label_create(mode_btn);
    lv_obj_set_style_text_font(s_p_mode_badge, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_p_mode_badge, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_text(s_p_mode_badge, "1");
    lv_obj_align(s_p_mode_badge, LV_ALIGN_TOP_RIGHT, -UI_PX_W(8), UI_PX_H(6));
    apply_play_mode_ui();
    make_round_btn(ctl, LV_SYMBOL_PREV, on_prev, UI_COLOR_PILL, UI_COLOR_FG);
    s_p_btn = make_round_btn(ctl, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
                             on_play_pause, UI_COLOR_ACCENT, lv_color_hex(0x14141A));
    make_round_btn(ctl, LV_SYMBOL_NEXT, on_next, UI_COLOR_PILL, UI_COLOR_FG);
    make_round_btn(ctl, LV_SYMBOL_VOLUME_MAX, on_volume, UI_COLOR_PILL, UI_COLOR_FG);

    /* 输出设备选择：图标跟着当前输出走（扬声器/蓝牙/静音），点开是选择弹窗 */
    lv_obj_t *out_btn = make_round_btn(ctl, out_icon_of(audio_service_get_output()),
                                       on_output_click, UI_COLOR_PILL, UI_COLOR_FG);
    s_out_btn_icon = lv_obj_get_child(out_btn, 0);
    s_out_shown    = audio_service_get_output();

    lv_obj_add_event_cb(scr, player_on_delete, LV_EVENT_DELETE, NULL);

    if (!s_tick) {
        s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    }
    if (!s_media_timer) {
        /* 蓝牙按键要"跟手"一点，单独用一个 100 ms 的轻量定时器取标志 */
        s_media_timer = lv_timer_create(media_cmd_tick, 100, NULL);
    }

    /*
     * 进播放器页时的封面：
     *   1) 已经有解好的封面（PSRAM 里留着，见 player_on_delete 的说明）→ **直接贴回来**，
     *      不重新下载、不重新解码。这就是"切页面回来封面还在"的关键；
     *   2) 还没有（开机恢复的曲目：start_track_at 排了队但没走起播流程）→ 补排一次解码。
     *
     * 为什么不用记"这块封面属于哪一首"：换曲一定走 start_track_at()/start_track_online_at()
     * 或 online_switch_ui_to()，它们都会 cover_show(NULL,…) 先把旧封面释放掉 ——
     * 所以页面创建时还留着像素，就一定是当前这首的。
     */
    if (s_cover_dsc && s_cover_pixels && s_p_cover_img) {
        lv_image_set_src(s_p_cover_img, s_cover_dsc);
        lv_obj_remove_flag(s_p_cover_img, LV_OBJ_FLAG_HIDDEN);
    } else if (s_index >= 0 && !s_cover_busy) {
        s_cover_want = s_index;
    }

    ESP_LOGI(TAG, "player page created (%s)", current_status_text());
    return scr;
}

/* ------------------------------------------------------------------ 本地文件浏览
 *
 * 需求：进"本地音乐"就是**浏览 SD 卡**——
 *   - 只列当前目录这一层的文件（不递归）
 *   - 目录 / 音乐文件 / 其它文件三种图标
 *   - 非根目录第一行给 ".." 返回上一级
 *   - 点音乐文件：把**当前目录这一层**的音频设为播放队列并开始播放
 *   - 右侧有滚动条指示位置
 */

static lv_obj_t *s_list_col;
static lv_obj_t *s_list_hint;
static char      s_cur_dir[STORAGE_PATH_MAX] = "/sdcard";
static fs_entry_t *s_entries;                 /* 目录条目缓冲（PSRAM，按需扩容） */
static size_t    s_entries_cap;               /* s_entries 的容量（条） */
static int        s_entry_n;                  /* 当前目录的条目总数 */
static int        s_page;                     /* 当前页（从 0 开始） */

#define LOCAL_ROOT   "/sdcard"

/*
 * 一页最多画这么多行。
 *
 * 为什么要分页而不是一次把 2000 条全建出来：每行是 5~6 个 LVGL 对象，2000 条就是
 * 上万个对象 —— 建页要几百毫秒、滚动每帧都在遍历它们，越"全"越卡。分页后对象数
 * 有上界，任何规模的目录都能秒开，而且**所有歌都翻得到**（完整性问题不能靠截断解决，
 * 靠分页解决）。播放队列仍然是整个目录（见 storage_service_load_dir），所以
 * 上一首/下一首在整目录里轮转，跟翻不翻页无关。
 */
#define LOCAL_PAGE_ROWS   120

/* local_add_row 的 sel 取值：>=0 是 s_entries 的下标，负数是这几种动作行 */
#define LOCAL_SEL_NONE    (-1)   /* 提示行，不可点 */
#define LOCAL_SEL_PARENT  (-2)   /* ".." 上一级 */
#define LOCAL_SEL_PREV    (-3)   /* 上一页 */
#define LOCAL_SEL_NEXT    (-4)   /* 下一页 */

static void local_rebuild(void);
static void on_local_entry(lv_event_t *e);

/*
 * 确保条目缓冲至少能装 need 条。按 64 → 翻倍 → 上限的顺序扩容，优先 PSRAM
 * （一条 fs_entry_t 264 字节，2000 条就是 ~516 KB，放内部 RAM 会把 WiFi/音频挤掉）。
 * 返回能否满足。
 */
#define LOCAL_ENTRIES_MAX  8192

static bool local_reserve_entries(size_t need)
{
    if (need <= s_entries_cap) {
        return true;
    }
    if (need > LOCAL_ENTRIES_MAX) {
        need = LOCAL_ENTRIES_MAX;
    }
    size_t cap = s_entries_cap ? s_entries_cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    fs_entry_t *p = (fs_entry_t *)heap_caps_realloc(s_entries, cap * sizeof(fs_entry_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (fs_entry_t *)realloc(s_entries, cap * sizeof(fs_entry_t));
    }
    if (!p) {
        ESP_LOGE(TAG, "local: entry buffer grow to %u failed", (unsigned)cap);
        return false;
    }
    s_entries     = p;
    s_entries_cap = cap;
    ESP_LOGI(TAG, "local: entry buffer -> %u entries", (unsigned)cap);
    return s_entries_cap >= need;
}

/* 一行：图标 + 名字（+ 文件大小）。sel 见 LOCAL_SEL_* / s_entries 下标 */
static void local_add_row(const char *name, bool is_dir, bool is_audio, uint32_t size, int sel)
{
    if (!s_list_col) {
        return;
    }
    lv_obj_t *row = lv_obj_create(s_list_col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    /* 图标底色按类型区分：目录=琥珀，音乐=蓝，其它=灰 */
    const uint32_t accent = is_dir ? 0xE8973A : (is_audio ? 0x5B6FB5 : 0x4A4A52);
    /*
     * 三种图标（需求：音乐=音乐图标 / 其它文件=文件图标 / 文件夹=文件夹图标）：
     *   LV_SYMBOL_DIRECTORY = U+F07B、LV_SYMBOL_AUDIO = U+F001、LV_SYMBOL_FILE = U+F158
     * 三个码点都在 ui_font_sc16 的生成码点表里（见文件头 Opts），不会渲染成方框。
     */
    const char    *sym    = is_dir ? LV_SYMBOL_DIRECTORY
                                   : (is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, UI_PX_H(40), UI_PX_H(40));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon, UI_PX_H(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *is = lv_label_create(icon);
    lv_obj_set_style_text_font(is, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(is, lv_color_hex(0x14141A), LV_PART_MAIN);
    lv_label_set_text(is, sym);
    lv_obj_center(is);

    lv_obj_t *name_l = lv_label_create(row);
    lv_obj_set_style_text_font(name_l, UI_FONT_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_l, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(name_l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_l, UI_PCT_X(62));
    lv_label_set_text(name_l, name);
    lv_obj_align(name_l, LV_ALIGN_LEFT_MID, UI_PX_H(52), 0);

    /* 文件大小（目录不显示） */
    if (!is_dir && size > 0) {
        lv_obj_t *sz = lv_label_create(row);
        lv_obj_set_style_text_font(sz, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(sz, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text_fmt(sz, "%.1f MB", (double)size / (1024.0 * 1024.0));
        lv_obj_align(sz, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_add_event_cb(row, on_local_entry, LV_EVENT_CLICKED, (void *)(intptr_t)sel);
}

/* 只重画"当前页"的行（不碰数据）：翻页走这里，不重读 SD 目录，翻页是瞬时的 */
static void local_render(void)
{
    if (!s_list_col) {
        return;
    }
    lv_obj_clean(s_list_col);

    const int pages = (s_entry_n + LOCAL_PAGE_ROWS - 1) / LOCAL_PAGE_ROWS;
    if (pages <= 0) {
        s_page = 0;
    } else if (s_page >= pages) {
        s_page = pages - 1;
    } else if (s_page < 0) {
        s_page = 0;
    }
    const int first = s_page * LOCAL_PAGE_ROWS;
    int last = first + LOCAL_PAGE_ROWS;
    if (last > s_entry_n) {
        last = s_entry_n;
    }

    if (s_list_hint) {
        if (pages > 1) {
            lv_label_set_text_fmt(s_list_hint, "%s · 第 %d/%d 页 · 共 %d 项",
                                  s_cur_dir, s_page + 1, pages, s_entry_n);
        } else {
            lv_label_set_text_fmt(s_list_hint, "%s · 共 %d 项", s_cur_dir, s_entry_n);
        }
    }

    if (strcmp(s_cur_dir, LOCAL_ROOT) != 0) {
        local_add_row("..", true, false, 0, LOCAL_SEL_PARENT);   /* 返回上一级（根目录不给） */
    }
    for (int i = first; i < last; i++) {
        local_add_row(s_entries[i].name, s_entries[i].is_dir,
                      s_entries[i].is_audio, s_entries[i].size, i);
    }
    if (s_page > 0) {
        local_add_row(LV_SYMBOL_LEFT " 上一页", false, false, 0, LOCAL_SEL_PREV);
    }
    if (last < s_entry_n) {
        local_add_row("下一页 " LV_SYMBOL_NEXT, false, false, 0, LOCAL_SEL_NEXT);
    }
    if (s_entry_n == 0) {
        /* 空目录：根目录直接提示；子目录此时上面已经有 ".." 那一行，
         * 再补一条说明，免得看着像一个坏掉/空的列表 */
        local_add_row("（这个目录没有文件）", false, false, 0, LOCAL_SEL_NONE);
    }
}

/* 换目录：重读条目 → 重建播放队列 → 重画第一页 */
static void local_rebuild(void)
{
    if (!s_list_col) {
        return;
    }

    /*
     * 取目录条目：先按当前容量取一次，browse 返回的是**真实条数**（可能大于容量）。
     * 装不下就扩容重来 —— 容量翻倍增长，所以最多几轮就够，任何规模的目录都能列全。
     * （旧代码把容量写死 64 条且不看返回值，目录一大就静默只剩前 64 条。）
     */
    s_entry_n = -1;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (!s_entries && !local_reserve_entries(64)) {
            break;
        }
        const int got = storage_service_browse(s_cur_dir, s_entries, (int)s_entries_cap);
        if (got < 0) {
            s_entry_n = -1;
            break;
        }
        s_entry_n = got;
        if ((size_t)got <= s_entries_cap) {
            break;                                  /* 全装下了 */
        }
        const size_t cap_before = s_entries_cap;
        if (!local_reserve_entries((size_t)got)) {
            break;                                  /* 分配失败：按现有容量列 */
        }
        if (s_entries_cap == cap_before) {
            break;   /* 已到 LOCAL_ENTRIES_MAX，再扫一遍目录也是同样结果 */
        }
    }
    if (s_entry_n < 0) {
        s_entry_n = 0;
        lv_obj_clean(s_list_col);
        local_add_row("读取目录失败", false, false, 0, LOCAL_SEL_NONE);
        return;
    }
    /*
     * 不变量：s_entry_n 必须 ≤ 缓冲容量 —— 下面按页取 s_entries[i] 的下标，
     * 一旦 n 大于容量就会越界读（目录条目超过 LOCAL_ENTRIES_MAX 时只列前
     * LOCAL_ENTRIES_MAX 条，并留下日志）。
     */
    if ((size_t)s_entry_n > s_entries_cap) {
        ESP_LOGW(TAG, "local: %d entries but buffer holds %u — listing the first %u",
                 s_entry_n, (unsigned)s_entries_cap, (unsigned)s_entries_cap);
        s_entry_n = (int)s_entries_cap;
    }

    /* 队列 = 当前目录这一层的**全部**音频（上一首/下一首在整目录里轮转，与翻页无关） */
    storage_service_load_dir(s_cur_dir);

    local_render();
}

/*
 * 进目录前的"加载态"：把列表换成转圈 + "载入中…"，并**立刻刷一帧**。
 *
 * ★ 为什么需要（用户 2026-10 反馈"点进去卡在当前页面，加载完才进去"）：
 *   进目录走的是 local_rebuild() → storage_service_browse()，而它是**同步**读 SD
 *   （在 LVGL 任务里），大目录要几百毫秒甚至更久。这期间如果屏上还是**上一个目录**
 *   的内容，看起来就是"点了没反应/卡住"，然后新目录突然出现。
 *   所以先把加载态铺上屏、lv_refr_now() 强制送一帧，再去做那次阻塞读。
 *
 * ⚠ 转圈是**静止**的：读卡期间 LVGL 任务被占住，spinner 的动画转不起来。
 *   想让它真的转，得把目录读取挪到工作线程（见 AGENTS.md 里"下一步可做"那条）。
 */
static void local_show_loading(void)
{
    if (s_list_col) {
        lv_obj_clean(s_list_col);
        /* 一个转圈 + 一行字，居中显示（s_list_col 是纵向 flex，用容器包一下居中） */
        lv_obj_t *box = lv_obj_create(s_list_col);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, UI_PX_H(24), LV_PART_MAIN);
        lv_obj_set_style_pad_row(box, UI_PX_H(12), LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sp = lv_spinner_create(box);
        lv_obj_set_size(sp, UI_PX_H(48), UI_PX_H(48));
        lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, UI_PX_H(5), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, UI_COLOR_PILL, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, UI_COLOR_ACCENT, LV_PART_INDICATOR);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *t = lv_label_create(box);
        lv_obj_set_style_text_font(t, UI_FONT_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(t, UI_COLOR_FG_DIM, LV_PART_MAIN);
        lv_label_set_text(t, "载入中…");
    }
    if (s_list_hint) {
        lv_label_set_text_fmt(s_list_hint, "%s · 载入中…", s_cur_dir);
    }
}

static void on_local_entry(lv_event_t *e)
{
    const int sel = (int)(intptr_t)lv_event_get_user_data(e);
    if (sel == LOCAL_SEL_NONE) {
        return;
    }
    if (sel == LOCAL_SEL_PARENT) {                   /* ".." 上一级 */
        char *slash = strrchr(s_cur_dir, '/');
        if (slash && slash != s_cur_dir) {
            *slash = '\0';
        } else {
            snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", LOCAL_ROOT);
        }
        s_page = 0;
        local_show_loading();       /* 先把加载态铺上屏 */
        lv_refr_now(NULL);          /* 强制送一帧：别让用户盯着旧目录发呆 */
        local_rebuild();
        return;
    }
    if (sel == LOCAL_SEL_PREV || sel == LOCAL_SEL_NEXT) {
        s_page += (sel == LOCAL_SEL_NEXT) ? 1 : -1;
        if (s_page < 0) {
            s_page = 0;
        }
        local_render();          /* 只重画一页：不重读 SD 目录，翻页是瞬时的 */
        return;
    }
    if (sel < 0 || sel >= s_entry_n) {
        return;
    }
    const fs_entry_t *en = &s_entries[sel];

    char full[STORAGE_PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", s_cur_dir, en->name);
    if (n <= 0 || n >= (int)sizeof(full)) {
        ui_toast("路径太长", 1500);
        return;
    }

    if (en->is_dir) {
        snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", full);
        s_page = 0;
        /* ★ 先把加载态铺上屏并强制刷一帧，再去做同步的 SD 目录读取：
         * 否则读卡这几百毫秒里屏上还是**上一个目录**，看起来就是"卡住" */
        local_show_loading();
        lv_refr_now(NULL);
        local_rebuild();
        return;
    }
    if (!en->is_audio) {
        ui_toast("不是可播放的音频文件", 1500);
        return;
    }

    const int idx = storage_service_index_of_path(full);
    if (idx < 0) {
        ui_toast("文件不在当前播放队列里", 1500);
        return;
    }
    start_track(idx);
    /* 进播放器页（与首页点"音乐"同一个页面） */
    page_desc_t page = {};
    page.id     = "music";
    page.create = page_player;
    ui_router_push(&page);
}

/* 列表页销毁时把全局指针清掉（定时器/回调别再往已销毁对象里写） */
static void local_page_watch(lv_event_t *e)
{
    (void)e;
    s_list_col  = NULL;
    s_list_hint = NULL;
}

static lv_obj_t *page_local(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* pad_all 必须在 pad_top 之前（否则标题被状态栏压住） */
    lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PAGE_PAD, LV_PART_MAIN);

    /* 本地音乐页由路由压栈进入：返回键由 ui_core 全局返回键自动叠在顶层，
     * 这里不再自己画。标题按 UI_HEADER_TEXT_X 起排，给左侧按钮留位。 */
    lv_obj_t *head = lv_label_create(scr);
    lv_obj_set_style_text_font(head, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(head, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(head, "本地音乐");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, UI_HEADER_TEXT_X, 0);

    s_list_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_list_hint, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_list_hint, UI_COLOR_FG_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_list_hint, UI_PCT_X(62));
    lv_label_set_long_mode(s_list_hint, LV_LABEL_LONG_DOT);
    lv_obj_align_to(s_list_hint, head, LV_ALIGN_OUT_RIGHT_MID, UI_PX_W(14), 0);

    s_list_col = lv_obj_create(scr);
    lv_obj_remove_style_all(s_list_col);
    lv_obj_set_size(s_list_col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_top(s_list_col, UI_PX_H(40), LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list_col, UI_PX_H(8), LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_list_col, UI_PX_W(10), LV_PART_MAIN);   /* 给滚动条让位 */
    lv_obj_set_flex_flow(s_list_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_col, LV_SCROLLBAR_MODE_AUTO);
    /* remove_style_all 把滚动条样式也清了，这里显式补一条细滚动条 */
    lv_obj_set_style_width(s_list_col, UI_PX_W(6), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list_col, UI_COLOR_FG_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list_col, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list_col, UI_PX_W(3), LV_PART_SCROLLBAR);

    /* （返回键已由 ui_core 全局返回键承担，且它挂在 lv_layer_top 上、
     *   z 序永远高于本页的 s_list_col，不会再出现"列表盖住返回键"的问题） */

    lv_obj_add_event_cb(scr, local_page_watch, LV_EVENT_DELETE, NULL);

    if (!bsp_sdcard_is_mounted()) {
        lv_label_set_text(s_list_hint, "未检测到 SD 卡");
        local_add_row("未检测到 SD 卡，仅在线音乐可用", false, false, 0, LOCAL_SEL_NONE);
        return scr;
    }
    if (!local_reserve_entries(64)) {
        local_add_row("内存不足，无法列出文件", false, false, 0, LOCAL_SEL_NONE);
        return scr;
    }
    s_page = 0;
    snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", bsp_sdcard_mount_point());
    local_rebuild();
    ESP_LOGI(TAG, "local browser at %s", s_cur_dir);
    return scr;
}


/* ------------------------------------------------------------------ 注册 */

static void on_music_enter(void) {}
static void on_music_exit(void) {}

/*
 * 打开播放器页（压栈）。给别的 APP 用（在线音乐点歌之后直接进播放器看封面/歌词）。
 * 描述符是拷贝进路由栈的，所以这里的栈上临时量是安全的（见 ui_router_push 注释）。
 */
void app_music_open_player(void)
{
    page_desc_t page = { "music", page_player, on_music_enter, on_music_exit };
    ui_router_push(&page);
}

/*
 * 开机恢复上次的曲目与进度（进度记忆）。
 *
 * 只把状态摆好、**不自动出声**：进播放器页就能看到上次那首歌和它的进度，
 * 按 ▶ 才从记忆位置继续（on_play_pause → start_track_at(s_index, s_elapsed_ms)，
 * 真正的字节定位在 tick_cb 里等码率到手后执行）。
 * 断电前的音量则是在 audio_service_init() 里恢复的（不需要 UI）。
 */
static void restore_last_session(void)
{
    char path[STORAGE_PATH_MAX];
    uint32_t pos = 0;
    if (play_state_track_get(path, sizeof(path), &pos) != ESP_OK) {
        return;
    }
    if (!bsp_sdcard_is_mounted()) {
        ESP_LOGW(TAG, "restore skipped: sdcard not mounted");
        return;
    }

    /* 队列按"曲目所在目录这一层"装载，与浏览页点歌时的语义一致 */
    char dir[STORAGE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), "%s", LOCAL_ROOT);
    }

    if (storage_service_load_dir(dir) <= 0) {
        ESP_LOGW(TAG, "restore: '%s' 里没有可播的曲目", dir);
        return;
    }
    const int idx = storage_service_index_of_path(path);
    if (idx < 0) {
        ESP_LOGW(TAG, "restore: 记忆的曲目已不在（%s）", path);
        return;
    }

    s_index      = idx;
    s_elapsed_ms = pos;
    s_dur_ms     = 0;
    s_seeking    = false;
    s_resume_ms  = pos;
    s_save_ticks = 0;
    s_resume_wait = 0;
    snprintf(s_track_path, sizeof(s_track_path), "%s", path);
    media_meta_lyrics_load(path, &s_lrc);      /* 页面建起来时 lyric_rebuild 直接用 */
    ESP_LOGI(TAG, "restored last track: %s @ %u ms", path, (unsigned)pos);
}

void app_music_register_apps(void)
{
    static const app_desc_t music = {
        "music", "音乐", LV_SYMBOL_AUDIO, 0xE8973A,
        page_player, on_music_enter, on_music_exit, 10, 0
    };
    static const app_desc_t local = {
        "local", "本地音乐", LV_SYMBOL_SD_CARD, 0x5B6FB5,
        page_local, NULL, NULL, 30, 0
    };

    ui_app_register(&music);
    ui_app_register(&local);

    /* 订阅播放事件：播完自动下一首，解码失败则报错并跳过 */
    audio_service_set_event_cb(on_audio_evt, NULL);

    restore_last_session();
}
