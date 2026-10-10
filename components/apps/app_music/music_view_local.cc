/*
 * music_view_local.cc —— 本地音乐列表页（视图层）
 *
 * 搬迁自 app_music.cc 的 2878–2881、2939–3108、3265–3509。
 * 注释原样保留：分页为什么必须有、加载态为什么必须有、
 * 页面销毁为什么**必须删** SD 定时器（与播放器页的"永不删"正好相反）。
 */
#include "music_view_local.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "bsp.h"
#include "play_state.h"
#include "storage_service.h"
#include "ui_core.h"
#include "ui_metrics.h"

#include "music_local_source.h"
#include "music_player.h"
#include "music_state.h"
#include "music_view_internal.h"

static const char *TAG = "app_music";

static lv_obj_t *s_list_col;                  /* 列表列（flex + 滚动条） */
static lv_obj_t *s_list_hint;                 /* 路径/页数提示行 */
static int       s_page;                      /* 当前页（从 0 开始） */
static lv_timer_t *s_local_sd_timer;          /* SD 热插拔轮询（页面存活期间） */

static void local_rebuild(void);
static void on_local_entry(lv_event_t *e);

/* 一行：图标 + 名字（+ 文件大小）。sel 见 LOCAL_SEL_* / 条目下标 */
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

    local_dir_view_t dv;
    local_source_view(&dv);
    const int entry_n = dv.n;

    const int pages = (entry_n + LOCAL_PAGE_ROWS - 1) / LOCAL_PAGE_ROWS;
    if (pages <= 0) {
        s_page = 0;
    } else if (s_page >= pages) {
        s_page = pages - 1;
    } else if (s_page < 0) {
        s_page = 0;
    }
    const int first = s_page * LOCAL_PAGE_ROWS;
    int last = first + LOCAL_PAGE_ROWS;
    if (last > entry_n) {
        last = entry_n;
    }

    if (s_list_hint) {
        if (pages > 1) {
            lv_label_set_text_fmt(s_list_hint, "%s · 第 %d/%d 页 · 共 %d 项",
                                  local_source_dir(), s_page + 1, pages, entry_n);
        } else {
            lv_label_set_text_fmt(s_list_hint, "%s · 共 %d 项", local_source_dir(), entry_n);
        }
    }

    if (strcmp(local_source_dir(), LOCAL_ROOT) != 0) {
        local_add_row("..", true, false, 0, LOCAL_SEL_PARENT);   /* 返回上一级（根目录不给） */
    }
    for (int i = first; i < last && dv.entries; i++) {
        local_add_row(dv.entries[i].name, dv.entries[i].is_dir,
                      dv.entries[i].is_audio, dv.entries[i].size, i);
    }
    if (s_page > 0) {
        local_add_row(LV_SYMBOL_LEFT " 上一页", false, false, 0, LOCAL_SEL_PREV);
    }
    if (last < entry_n) {
        local_add_row("下一页 " LV_SYMBOL_NEXT, false, false, 0, LOCAL_SEL_NEXT);
    }
    if (entry_n == 0) {
        /* 空目录：根目录直接提示；子目录此时上面已经有 ".." 那一行，
         * 再补一条说明，免得看着像一个坏掉/空的列表 */
        local_add_row("（这个目录没有文件）", false, false, 0, LOCAL_SEL_NONE);
    }
}

/*
 * 进目录/换目录前的"加载态"：把列表换成转圈 + "载入中…"。
 *
 * ★ 为什么需要（用户 2026-10 反馈"点进去卡在当前页面，加载完才进去"）：
 *   进目录要读 SD（列目录 + 建播放队列两遍 walk），大目录要几百毫秒到几秒。
 *   这期间如果屏上还是**上一个目录**的内容，看起来就是"点了没反应/卡住"。
 *
 * ★ 2026-10 改：读目录已经搬到**工作线程**（见 music_source_local.cc），所以
 *   - 页面/目录**立刻**切过来（不再等读卡）；
 *   - 转圈是**真的在转**（以前 LVGL 任务被同步读卡占住，动画根本刷不出来）；
 *   - 不再需要 lv_refr_now() 去"抢一帧"。
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
        lv_label_set_text_fmt(s_list_hint, "%s · 载入中…", local_source_dir());
    }
}

/* 扫描失败：列表换成一行错误提示（数据侧已把条目数清零） */
static void local_apply_scan(void)
{
    if (!s_list_col) {
        return;
    }
    if (local_source_last_failed()) {
        lv_obj_clean(s_list_col);
        local_add_row("读取目录失败", false, false, 0, LOCAL_SEL_NONE);
        return;
    }
    local_render();
}

/*
 * 换目录/插卡/重进页面都走它：铺加载态 + 起异步扫描，**立刻返回**。
 * 渲染由 500 ms 的 SD 轮询在看到新结果时做（见 local_sd_tick）。
 */
static void local_rebuild(void)
{
    if (!s_list_col) {
        return;
    }
    local_show_loading();
    local_source_scan_start(local_source_dir());
}

static void on_local_entry(lv_event_t *e)
{
    const int sel = (int)(intptr_t)lv_event_get_user_data(e);
    if (sel == LOCAL_SEL_NONE) {
        return;
    }
    if (sel == LOCAL_SEL_PARENT) {                   /* ".." 上一级 */
        char dir[STORAGE_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", local_source_dir());
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
        } else {
            snprintf(dir, sizeof(dir), "%s", LOCAL_ROOT);
        }
        local_source_set_dir(dir);
        s_page = 0;
        /* 上一级：同样走异步扫描（见 local_rebuild） */
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
    local_dir_view_t dv;
    local_source_view(&dv);
    if (sel < 0 || sel >= dv.n || !dv.entries) {
        return;
    }
    const fs_entry_t *en = &dv.entries[sel];

    char full[STORAGE_PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", local_source_dir(), en->name);
    if (n <= 0 || n >= (int)sizeof(full)) {
        ui_toast("路径太长", 1500);
        return;
    }

    if (en->is_dir) {
        local_source_set_dir(full);
        s_page = 0;
        /* 换目录：铺加载态 + 起**异步**扫描（工作线程读卡），页面立刻切过去、
         * 转圈真的在转，结果由 500 ms 轮询取回后渲染。 */
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
    music_player_start_track(idx);
    /* 进播放器页（与首页点"音乐"同一个页面） */
    music_router_open_player();
}

/* 列表页销毁时把全局指针清掉（定时器/回调别再往已销毁对象里写） */
static void local_page_watch(lv_event_t *e)
{
    (void)e;
    s_list_col  = NULL;
    s_list_hint = NULL;
    local_source_invalidate();   /* 作废在飞的扫描：结果不该留给下一个页面 */
    /* ★ 与播放器页相反：这个定时器**必须随页面删**（页面不在了就没人需要它） */
    if (s_local_sd_timer) {
        lv_timer_delete(s_local_sd_timer);
        s_local_sd_timer = NULL;
    }
}

/* ------------------------------------------------------------------ SD 热插拔
 *
 * 板子没有卡检测脚，storage_service 里有个任务在轮询，每次插入/拔出把代次 +1。
 *
 * 这里只是跟着代次走：拔卡 → 列表换成"未检测到"；插卡（或换了一张卡）→
 * 从根目录重新列一遍。**换卡必须走这一步**：不重新列的话，列表和播放队列
 * 还是旧卡的内容（旧挂载点没释放，读到的永远是旧卡）。
 *
 * 提示音交给状态栏那个统一入口去弹（插卡成功时它还会报容量），这里只在
 * "播放被拔卡打断"时补一句 —— 那件事只有播放器自己知道。
 */
static void local_sd_apply_removed(void)
{
    if (!s_list_col) {
        return;
    }
    const music_state_t *st = music_state();
    if (!st->on_mode && st->playing) {
        music_player_stop();
        ui_toast("SD 卡已拔出，播放已停止", 2000);
    }
    local_source_set_entry_count(0);
    s_page = 0;
    local_source_invalidate();          /* 作废在飞的扫描（拔卡了，旧结果不能再用） */
    local_source_set_dir(LOCAL_ROOT);
    if (s_list_hint) {
        lv_label_set_text(s_list_hint, "未检测到 SD 卡");
    }
    lv_obj_clean(s_list_col);
    local_add_row("未检测到 SD 卡，插好后会自动刷新", false, false, 0, LOCAL_SEL_NONE);
}

static void local_sd_apply_inserted(void)
{
    if (!s_list_col) {
        return;
    }
    /* 插上了：回到挂载点重新列一遍（换卡也能认出来） */
    s_page = 0;
    local_source_set_dir(bsp_sdcard_mount_point());
    if (s_list_hint) {
        lv_label_set_text(s_list_hint, "载入中…");
    }
    local_rebuild();
}

static void local_sd_tick(lv_timer_t *t)
{
    (void)t;

    /* ① 异步扫描的结果（见 music_source_local.cc）；② 排队请求；③ SD 热插拔代次 */
    local_poll_result_t r;
    local_source_poll(&r);

    if (r.scan_ready) {
        local_apply_scan();
    }
    if (r.sd_removed) {
        local_sd_apply_removed();
    } else if (r.sd_inserted) {
        local_sd_apply_inserted();
    }
}

lv_obj_t *music_view_local_build(void)
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

    /* SD 热插拔轮询：进来时有没有卡都要建 —— 页面开着的时候插卡也要能刷新 */
    local_source_sd_sync();
    if (!s_local_sd_timer) {
        s_local_sd_timer = lv_timer_create(local_sd_tick, LOCAL_SD_POLL_MS, NULL);
    }

    /*
     * 新页面：先把上一次留下的扫描状态作废。
     * ⚠ 必须 **s_scan_seq++** —— 若上一次页面关掉时还有一个扫描在飞（同一个 seq），
     *   它的结果会被轮询认成"当前目录的结果"画到新页面上：列的是旧目录的文件名，
     *   点下去却按新 s_cur_dir 拼路径 → 放到别的文件去了。
     */
    local_source_invalidate();
    local_source_set_entry_count(0);

    if (!storage_service_sd_is_mounted()) {
        lv_label_set_text(s_list_hint, "未检测到 SD 卡");
        local_add_row("未检测到 SD 卡，仅在线音乐可用", false, false, 0, LOCAL_SEL_NONE);
        return scr;
    }
    if (!local_source_reserve(64)) {
        local_add_row("内存不足，无法列出文件", false, false, 0, LOCAL_SEL_NONE);
        return scr;
    }
    s_page = 0;
    local_source_set_dir(bsp_sdcard_mount_point());
    local_rebuild();
    ESP_LOGI(TAG, "local browser at %s", local_source_dir());
    return scr;
}
