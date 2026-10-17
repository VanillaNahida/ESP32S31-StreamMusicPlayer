/*
 * ui_status_bar.cc —— 常驻状态栏（高 32px）
 *
 * 左：时钟；右：Wi-Fi → 蓝牙 → 输出通道 → microSD（**不画电池**：本板无电池、
 * 无电量计，见 AGENTS.md 第 5 节）。
 *
 * 图标显示逻辑（2026-10 需求）
 * ---------------------------
 * 四个图标全部由**本文件自己的 500 ms 定时器**从各服务读状态后画出来，
 * 没有人从外面 set（老版本的那几个 ui_status_set_* 是"永远没人调"的占位，
 * 结果就是 Wi-Fi/蓝牙图标常年一动不动）：
 *
 *   Wi-Fi：开关关着 → **不显示**；开着但没连上 → wifi_off（暗）；
 *          已连接 → 按 RSSI 选 wifi_s1..s4（亮）
 *   蓝牙 ：开关关着 → **不显示**；已连接/推流中 → bt_connected（**蓝**）；
 *          搜索中 → bt_searching（亮）；其余（空闲）→ bt_idle（暗）
 *   SD   ：没插卡 → **不显示**；插着且挂上了 → sd_ok（**白**）；
 *          卡在但读不出来 → sd_warn（橙，强调色）
 *
 * 配色口径（都在 ui_metrics.h 的"状态栏图标配色"那一块）：
 *   橙是**警告色**，只给 SD 读不出来这种"要注意的事"；蓝牙连接成功是正常状态，
 *   所以用蓝（UI_COLOR_STATUS_BT_LINK）。sd_ok 用白而不是灰 —— 那个字形是空心
 *   描边，配灰色会比旁边实心的 Wi-Fi / 蓝牙暗一档，看着像"没点亮"。
 *
 * 三张图都是**单色位图**（assets/icons 下的 SVG 按白色烘进 ui_icons.c），
 * 颜色由 lv_obj_set_style_image_recolor() 决定 —— 这就是"同一个图标既能画成
 * 暗的也能画成强调色"的做法。
 *
 * 隐藏用 LV_OBJ_FLAG_HIDDEN：图标排在一条 flex row 里，而 LVGL 的 flex
 * **跳过隐藏的子对象**（lv_flex.c），所以"Wi-Fi 关掉"时右边那三个会自己往右
 * 收拢，不会留一个空洞。
 */
#include "ui_core.h"
#include "ui_internal.h"
#include "ui_metrics.h"
#include "ui_icons.h"

#include <stdio.h>

#include "esp_log.h"

#include "storage_service.h"   /* SD 热插拔：三态 + 代次（图标 + Toast） */
#include "bsp.h"               /* bsp_sdcard_capacity_kb（插入时报个容量） */
#include "wifi_service.h"      /* Wi-Fi 开关 / 连接状态 / RSSI */
#include "bt_service.h"        /* 蓝牙开关 / 连接 / 搜索状态 */

static const char *TAG = "ui_status";

#define STATUS_POLL_MS 500     /* 轮询间隔：都只是读几个缓存字段，开销可忽略 */

static lv_obj_t *s_clock;
static lv_obj_t *s_row;        /* 右侧图标行（flex，从右往左：Wi-Fi→蓝牙→输出→SD） */
static lv_obj_t *s_wifi;
static lv_obj_t *s_bt;
static lv_obj_t *s_out;        /* 输出通道（还是文字符号，未接状态源） */
static lv_obj_t *s_sd;
static lv_timer_t *s_poll;

/* SD 上次见到的 (代次, 三态)：用来决定"要不要弹 Toast / 刷图标" */
static uint32_t s_sd_gen_seen;
static int      s_sd_state_seen = -1;
static bool     s_wifi_enabled_seen;
static bool     s_bt_enabled_seen;

static void poll_cb(lv_timer_t *t);

/* 单色位图图标：创建即隐藏，由第一次 poll 填上 */
static lv_obj_t *make_icon(lv_obj_t *parent)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_image_recolor(img, UI_COLOR_FG, LV_PART_MAIN);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

/*
 * 上色 + 显隐的唯一出口。
 *   src == NULL → 隐藏（"开关关着" / "没插卡"三种情况都是它）
 * 只在**真的变了**的时候才写：lv_image_set_src / set_style 都会触发重绘，
 * 每秒无条件重设等于每秒重绘一次状态栏。
 */
static void icon_set(lv_obj_t *img, const void *src, lv_color_t color)
{
    if (!img) {
        return;
    }
    if (!src) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    if (lv_image_get_src(img) != src) {
        lv_image_set_src(img, src);
    }
    if (!lv_color_eq(lv_obj_get_style_image_recolor(img, LV_PART_MAIN), color)) {
        lv_obj_set_style_image_recolor(img, color, LV_PART_MAIN);
    }
}

/* ---------------------------------------------------------------- Wi-Fi */

/*
 * RSSI(dBm) → 1~4 格。
 *
 * 分档按常见系统状态栏口径（-55 以上满格、每 10 dBm 掉一格）。
 * ⚠ wifi_service_rssi() **未知时返回 0**（不是有效 dBm），不能当成"0 dBm 超强"
 * 也不能当成"没信号"：显示满格 —— 连上了却画一格会让人以为要断线。
 *
 * ⚠ 2026-10：服务原来**只在连上那一刻**取一次 RSSI，图标永远不变 —— 现在
 *   wifi_service 里有个 2 s 的定时器持续刷新（见 wifi_rssi_cb），这里拿到的就是
 *   当前信号强度了。另外加了一层**迟滞**（见下），否则站在两格交界处会看到
 *   图标 2↔3 每秒闪一次。
 */
#define WIFI_RSSI_HYST 3   /* dBm：升/降档都要"多走"这么多，防止边界抖动 */

/* 每档的下限：level >= k 需要 rssi >= WIFI_LEVEL_MIN[k]（1 档没有下限，用 -128 占位） */
static const int WIFI_LEVEL_MIN[5] = { -128, -128, -75, -65, -55 };

static int rssi_to_level(int rssi)
{
    if (rssi == 0)      return 4;
    if (rssi >= -55)    return 4;
    if (rssi >= -65)    return 3;
    if (rssi >= -75)    return 2;
    return 1;
}

/* 当前画到第几格：-1 = 还没画过（断开 / 关开关时复位，重连后按新信号直接落位） */
static int s_wifi_level = -1;

/*
 * 带迟滞的取档：从当前格出发，只有"越过边界 + WIFI_RSSI_HYST"才动。
 *   - 升档：rssi >= 上一档下限 + HYST（-55 边缘要 -52 才显示满格）
 *   - 降档：rssi <  本档下限 - HYST（满格要掉到 -58 以下才降到 3 格）
 * 这样同一个读数不会被两个相邻档同时"抢"。
 */
static int wifi_level_update(int rssi)
{
    if (rssi == 0) {                 /* 未知：满格（和 rssi_to_level 一致） */
        s_wifi_level = 4;
        return s_wifi_level;
    }
    if (s_wifi_level < 1) {          /* 首次（刚连上）：直接落位，不走迟滞 */
        s_wifi_level = rssi_to_level(rssi);
        return s_wifi_level;
    }
    while (s_wifi_level < 4 && rssi >= WIFI_LEVEL_MIN[s_wifi_level + 1] + WIFI_RSSI_HYST) {
        s_wifi_level++;
    }
    while (s_wifi_level > 1 && rssi < WIFI_LEVEL_MIN[s_wifi_level] - WIFI_RSSI_HYST) {
        s_wifi_level--;
    }
    return s_wifi_level;
}

static const void *wifi_icon_src(int level)
{
    switch (level) {
    case 1:  return &wifi_s1;
    case 2:  return &wifi_s2;
    case 3:  return &wifi_s3;
    default: return &wifi_s4;
    }
}

/* 上次打日志时的 dBm：只在"换格"或"信号动了 8 dBm 以上"时打一行，
 * 既不刷屏，又能证明数据真的在变（近距离时 RSSI 常常一直满格，
 * 光看图标会以为"还是不动"） */
#define WIFI_RSSI_LOG_STEP 8
static int s_wifi_rssi_logged;

static void apply_wifi(void)
{
    /* 开关关着 = 射频根本没起（wifi_service_set_enabled(false) 会 deinit）：
     * 需求是"不显示图标"，而不是画一个 wifi_off —— 那时候没有 Wi-Fi 这件事 */
    if (!wifi_service_is_enabled()) {
        s_wifi_level = -1;
        s_wifi_rssi_logged = 0;
        icon_set(s_wifi, NULL, UI_COLOR_FG);
        return;
    }
    /* 已连接 = 拿到 IP（WIFI_ST_CONNECTED 与 has_ip 通常同时成立，兜一层保险） */
    const bool connected = (wifi_service_state() == WIFI_ST_CONNECTED) ||
                           wifi_service_has_ip();
    if (!connected) {
        s_wifi_level = -1;
        s_wifi_rssi_logged = 0;
        icon_set(s_wifi, &wifi_off, UI_COLOR_FG_DIM);
        return;
    }
    const int prev = s_wifi_level;
    const int rssi = wifi_service_rssi();
    const int level = wifi_level_update(rssi);
    /* 日志：换格必打；没换格但信号明显动过也打一行（现场判断"UI 没画"还是
     * "服务没刷新"就看这行 —— 只有这行在刷而图标不动，才是 UI 的问题） */
    if (level != prev || rssi <= s_wifi_rssi_logged - WIFI_RSSI_LOG_STEP ||
        rssi >= s_wifi_rssi_logged + WIFI_RSSI_LOG_STEP) {
        ESP_LOGD(TAG, "wifi rssi %d dBm -> %d/4 格", rssi, level);
        s_wifi_rssi_logged = rssi;
    }
    icon_set(s_wifi, wifi_icon_src(level), UI_COLOR_FG);
}

/* ---------------------------------------------------------------- 蓝牙 */

static void apply_bt(void)
{
    if (!bt_service_is_enabled()) {
        icon_set(s_bt, NULL, UI_COLOR_FG);
        return;
    }
    const bt_state_t st = bt_service_state();
    if (st == BT_ST_CONNECTED || st == BT_ST_STREAMING || bt_service_is_connected()) {
        /* 蓝色（不是强调色橙）：橙是"警告"色，留给 sd_warn / 音量条那一类 */
        icon_set(s_bt, &bt_connected, UI_COLOR_STATUS_BT_LINK);
    } else if (st == BT_ST_DISCOVERING || bt_service_is_discovering() ||
               st == BT_ST_CONNECTING) {
        /* 搜索中 / **连接中**（2026-10 新增状态）：都是"正在忙"，用同一个亮色图标 ——
         * 连接最长会到 15 s ×(1+2 次重试)，状态栏上给个亮的不然像卡住了 */
        icon_set(s_bt, &bt_searching, UI_COLOR_FG);
    } else if (st == BT_ST_CONNECT_FAILED) {
        /* 连接失败：暗色即可（橙是警告色，留给 SD 读不出来那一类；失败原因在
         * 设置页的蓝牙页里写明，状态栏不抢注意力） */
        icon_set(s_bt, &bt_idle, UI_COLOR_FG_DIM);
    } else {
        /* 开着但空闲（含"还在初始化"）：这是蓝牙"平常"的样子 */
        icon_set(s_bt, &bt_idle, UI_COLOR_FG_DIM);
    }
}

/* ---------------------------------------------------------------- microSD */

static void apply_sd(void)
{
    switch (storage_service_sd_state()) {
    case SD_STATE_READY:
        /* 白色（= UI_COLOR_FG，和 Wi-Fi 已连接同色）：sd_card 是空心描边字形，
         * 用灰色（FG_DIM）会比旁边实心的图标明显暗一档，像"没点亮" */
        icon_set(s_sd, &sd_ok, UI_COLOR_FG);
        break;
    case SD_STATE_ERROR:
        /* 卡在但读不出来：用强调色（橙）—— 这是要用户看一眼的颜色 */
        icon_set(s_sd, &sd_warn, UI_COLOR_ACCENT);
        break;
    case SD_STATE_ABSENT:
    default:
        icon_set(s_sd, NULL, UI_COLOR_FG_DIM);
        break;
    }
}

/* 插入时报一句容量（GB / MB 两档，与设置页口径一致，都从 KB 换算） */
static void toast_sd_inserted(void)
{
    uint32_t total_kb = 0;
    if (bsp_sdcard_capacity_kb(&total_kb, NULL) != ESP_OK || total_kb == 0) {
        ui_toast("已插入 SD 卡", 2000);
        return;
    }
    char buf[48];
    if (total_kb >= 1024u * 1024u) {
        snprintf(buf, sizeof(buf), "已插入 SD 卡（%.1f GB）",
                 (double)total_kb / (1024.0 * 1024.0));
    } else {
        snprintf(buf, sizeof(buf), "已插入 SD 卡（%u MB）", (unsigned)(total_kb / 1024u));
    }
    ui_toast(buf, 2000);
}

/*
 * SD 三态变化时的 Toast。
 *
 * 只在**状态真的变了**（或换了一张卡 = 代次变了）时弹，监控任务每秒都在刷新
 * 状态，不能每秒弹一句。首帧不弹（开机时本来就知道卡在不在，弹一句等于噪声）。
 *
 * 只报"整件事"级别的三种变化：
 *   → READY          ：插卡成功（换卡也算，代次会变）
 *   ABSENT → ERROR   ：插进来一张读不出来的卡
 *   → ABSENT（非首次）：拔卡
 * "已挂载的卡突然不应答"（READY → ERROR）**故意不弹** —— 单次 CMD13 超时是
 * 会有的抖动，连续两次才真卸载；那种情况只让图标变橙，别打扰正在听歌的人。
 */
static void poll_sd(void)
{
    const int      st  = (int)storage_service_sd_state();
    const uint32_t gen = storage_service_sd_generation();
    if (st == s_sd_state_seen && gen == s_sd_gen_seen) {
        return;
    }
    const bool first = (s_sd_state_seen < 0);
    const int  prev  = s_sd_state_seen;
    s_sd_state_seen = st;
    s_sd_gen_seen   = gen;

    apply_sd();
    if (first) {
        return;
    }
    if (st == (int)SD_STATE_READY) {
        toast_sd_inserted();
    } else if (st == (int)SD_STATE_ERROR && prev == (int)SD_STATE_ABSENT) {
        ui_toast("SD 卡无法识别，请检查卡", 2500);
    } else if (st == (int)SD_STATE_ABSENT && prev != (int)SD_STATE_ABSENT) {
        ui_toast("SD 卡已拔出", 2000);
    }
}

/* ---------------------------------------------------------------- 轮询 */

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    apply_wifi();
    apply_bt();

    /*
     * 两个无线开关的状态：开关一关，service 会把资源整个释放掉，图标要立刻
     * 跟着消失（用户点完开关马上去看状态栏，别等下一次 poll 的错觉 —— 其实
     * poll 就是 500 ms，这里只是顺手打个日志，方便排查"图标不对"）。
     */
    const bool we = wifi_service_is_enabled();
    const bool be = bt_service_is_enabled();
    if (we != s_wifi_enabled_seen || be != s_bt_enabled_seen) {
        s_wifi_enabled_seen = we;
        s_bt_enabled_seen   = be;
        ESP_LOGD(TAG, "radio switch: wifi=%d bt=%d", (int)we, (int)be);
    }

    poll_sd();
}

lv_obj_t *ui_status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_W, UI_STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x08080C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, UI_PX_W(12), LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：时钟 */
    s_clock = lv_label_create(bar);
    lv_obj_set_style_text_font(s_clock, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);

    /*
     * 右：图标行。用 flex 而不是"后一个 align_to 前一个"：
     *   - 隐藏的图标会被 flex 直接跳过，右边几个自动收拢（align_to 会留空洞）；
     *   - 间距只有一处 pad_column，加减图标不用改每个图标的偏移。
     * 顺序（左→右）＝ SD → 输出 → 蓝牙 → Wi-Fi，正好是屏幕右侧"从内到外"。
     *
     * 宽 LV_SIZE_CONTENT（由子对象撑开）、高取状态栏高：交叉轴**不能**是
     * CONTENT —— LVGL 的 flex 在交叉轴为 CONTENT 时会强制把它当 START
     * （lv_flex.c:`track_cross_place`），文字符号（LV_SYMBOL_AUDIO，比位图矮）
     * 就贴到上边缘了；给死高度后 FLEX_ALIGN_CENTER 才真的居中。
     */
    s_row = lv_obj_create(bar);
    lv_obj_remove_style_all(s_row);
    lv_obj_set_size(s_row, LV_SIZE_CONTENT, UI_STATUSBAR_H);
    lv_obj_align(s_row, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(s_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_row, UI_PX_W(10), LV_PART_MAIN);
    lv_obj_remove_flag(s_row, LV_OBJ_FLAG_SCROLLABLE);

    s_sd   = make_icon(s_row);
    s_out  = lv_label_create(s_row);
    lv_obj_set_style_text_font(s_out, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_out, UI_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(s_out, LV_SYMBOL_AUDIO);
    s_bt   = make_icon(s_row);
    s_wifi = make_icon(s_row);

    /*
     * 一个 500 ms 定时器把三张图都刷一遍（原来只有一个"SD 代次"轮询，
     * Wi-Fi/蓝牙根本没人问）。所有取值都是服务里的缓存字段，不涉及网络/文件系统，
     * 在 LVGL 任务里跑是安全的（wifi_service.h 的约定就是"UI 用 lv_timer 轮询"）。
     */
    s_wifi_enabled_seen = wifi_service_is_enabled();
    s_bt_enabled_seen   = bt_service_is_enabled();
    s_sd_gen_seen       = storage_service_sd_generation();
    poll_cb(NULL);              /* 先画一帧，别让状态栏空 500 ms */
    s_poll = lv_timer_create(poll_cb, STATUS_POLL_MS, NULL);

    ESP_LOGD(TAG, "status bar created h=%d icon=%d", (int)UI_STATUSBAR_H,
             (int)UI_STATUS_ICON);
    return bar;
}

void ui_status_set_clock(const char *hhmm, const char *date_text)
{
    (void)date_text;   /* 日期在首页大字下方显示 */
    if (s_clock && hhmm) {
        lv_label_set_text(s_clock, hhmm);
    }
}
