/*
 * ui_router.cc —— 页面栈路由
 *
 * 维护页面栈，避免 lv_screen_load 的不可控性；返回手势需要"上一层"做视差背景，
 * 所以栈必须自己管理（保留上一屏对象而不是立刻删除）。
 */
#include "ui_core.h"
#include "ui_internal.h"

#include "esp_log.h"
#include "ui_metrics.h"

static const char *TAG = "ui_router";

#define UI_ROUTER_MAX 8

/*
 * 页面切换动画时长（ms）。
 *
 * 用 LVGL 自带的屏幕切换动画（lv_screen_load_anim）而不是自己驱动 translate_x：
 * 只有它会把"上一屏"挂到 disp->prev_scr 上，lv_refr 才会**同时渲染两层**
 * （lv_refr.c 的 render_screen 分别处理 act_scr 与 prev_scr）。自己改
 * translate_x 时 prev_scr 是 NULL，页面滑出去露出来的只有黑底。
 * 顺带这段合成走 LVGL 的 blend/fill，打开 PPA（CONFIG_LV_USE_PPA）就是硬件加速的。
 *
 * 两个方向各用哪种动画（语义见 lv_display.c 里 switch(anim_type) 的取值）：
 *   push → OVER_LEFT：新页从右侧滑入、压在旧页上；旧页**不动**（x 保持 0），
 *          这样返回手势用的 translate_x 视差基准不会被破坏。
 *   pop  → OUT_RIGHT：旧页（= 被退掉的那页）在**上层**向右滑出，露出下面
 *          已经归位的前一页 —— 与返回手势的观感一致。
 */
#define UI_PAGE_ANIM_MS 220

typedef struct {
    lv_obj_t   *scr;
    page_desc_t desc;        /* 按值保存：调用方常传栈上临时量，存指针会变悬垂指针 */
    bool        has_desc;
} route_entry_t;

static route_entry_t s_stack[UI_ROUTER_MAX];
static int s_depth;

extern lv_obj_t *ui_home_create(void);   /* ui_home.cc */

int ui_router_depth(void)
{
    return s_depth;
}

static void entry_exit(route_entry_t *e)
{
    if (e->has_desc && e->desc.on_exit) {
        e->desc.on_exit();
    }
}

static void entry_enter(route_entry_t *e)
{
    if (e->has_desc && e->desc.on_enter) {
        e->desc.on_enter();
    }
}

esp_err_t ui_router_push(const page_desc_t *page)
{
    if (!page || !page->create) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_depth >= UI_ROUTER_MAX) {
        ESP_LOGW(TAG, "router stack full (%d)", UI_ROUTER_MAX);
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *scr = page->create();
    if (!scr) {
        ESP_LOGE(TAG, "page '%s' create() failed", page->id ? page->id : "?");
        return ESP_FAIL;
    }

    /* 换页 = 音量条失去了它的"焦点页面"，立刻缩回右边缘外（不等自动缩回计时） */
    ui_volume_hide();

    if (s_depth > 0) {
        entry_exit(&s_stack[s_depth - 1]);
        /*
         * 这里**故意不**给旧页加 LV_OBJ_FLAG_HIDDEN：
         *   1) LVGL 只渲染"活动屏幕"，lv_screen_load() 本身就是切换，hide 是多余的；
         *   2) 对 screen 对象调用 flag API 会踩到 LVGL 9.5 的 NULL-parent 路径
         *      （screen 的 parent 是 NULL，lv_obj_mark_layout_as_dirty 不判空）→ 崩溃。
         *      见 ui_router_pop() 里的详细说明。
         */
    }

    s_stack[s_depth].scr      = scr;
    s_stack[s_depth].desc     = *page;    /* 深拷贝描述符本身 */
    s_stack[s_depth].has_desc = true;
    s_depth++;

    /* auto_del=false：旧页留在栈里，返回手势还要用它做视差背景 */
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_OVER_LEFT, UI_PAGE_ANIM_MS, 0, false);
    entry_enter(&s_stack[s_depth - 1]);
    ui_back_button_update();     /* 新页压栈：刷新全局返回键（见 ui_core.cc） */
    ESP_LOGD(TAG, "push '%s' depth=%d", page->id ? page->id : "?", s_depth);
    return ESP_OK;
}

/*
 * 退栈的公共实现。
 *
 * animate = true  ：走 LVGL 屏幕动画（OUT_RIGHT，旧页在上层向右滑出）；
 *                   auto_del=true，动画跑完由 LVGL 自己删掉被退掉的那一屏。
 * animate = false ：立即切屏 + lv_obj_delete_async —— 给返回手势用，
 *                   手势已经把顶层页 1:1 滑出屏幕了，再补一次动画会先跳回
 *                   原位再滑一遍。
 */
static esp_err_t router_pop(bool animate)
{
    if (s_depth <= 1) {
        return ESP_ERR_INVALID_STATE;   /* 首页不回退 */
    }

    route_entry_t *top = &s_stack[s_depth - 1];
    entry_exit(top);

    /* 换页 = 音量条失去焦点，立刻缩回（同 push） */
    ui_volume_hide();

    /*
     * 顺序很重要：**先把上一层切为活动屏幕，再销毁顶层**。
     * 反过来先删，等于删掉当前活动屏幕，而显示管线仍持有它的指针 → 崩溃。
     */
    s_depth--;
    route_entry_t *prev = &s_stack[s_depth - 1];

    if (animate) {
        /*
         * auto_del=true 时被退掉的那一屏由 LVGL 在动画结束回调里删除
         * （lv_display.c 里 d->prev_scr + d->del_prev），**不能再删一次**。
         * 那个回调跑在 lv_timer 上下文，不在事件派发里，所以是安全的。
         */
        top->scr = NULL;
        lv_screen_load_anim(prev->scr, LV_SCREEN_LOAD_ANIM_OUT_RIGHT, UI_PAGE_ANIM_MS, 0, true);
    } else {
        lv_screen_load(prev->scr);

        /*
         * 两段式销毁（工程规范 F4）：**绝不能在事件回调里同步删屏** ——
         * 返回手势的调用链是 on_released → ui_router_pop，此时 LVGL 事件派发器
         * 仍在遍历该对象树，同步 lv_obj_delete 会 hard fault。
         * 用 lv_obj_delete_async，等派发与切屏都结束后再释放。
         *
         * 另：不要对 prev->scr 调 lv_obj_remove_flag(HIDDEN) —— LVGL 9.5 的
         * lv_obj_remove_flag 在 HIDDEN 分支**无条件**执行
         *     lv_obj_mark_layout_as_dirty(lv_obj_get_parent(obj));
         * 而 screen 的 parent 为 NULL，该函数又不判空，直接空指针解引用。
         * （lv_obj_add_flag 只在"布局定位状态变化"时才走那行，所以 push 侥幸没崩。）
         */
        if (top->scr) {
            lv_obj_delete_async(top->scr);
            top->scr = NULL;
        }
    }

    entry_enter(prev);
    ui_back_button_update();     /* 退栈：刷新全局返回键（见 ui_core.cc） */
    ESP_LOGD(TAG, "pop depth=%d (anim=%d)", s_depth, (int)animate);
    return ESP_OK;
}

esp_err_t ui_router_pop(void)
{
    return router_pop(true);
}

esp_err_t ui_router_pop_immediate(void)
{
    return router_pop(false);
}

esp_err_t ui_router_reset_to_home(void)
{
    while (s_depth > 1) {
        ui_router_pop();
    }
    return ESP_OK;
}

/* 由 ui_core_init 调用的内部入口：把首页压栈（不触发动画） */
esp_err_t ui_router_init_home(void)
{
    s_depth = 0;
    lv_obj_t *scr = ui_home_create();
    if (!scr) {
        return ESP_FAIL;
    }
    s_stack[0].scr      = scr;
    s_stack[0].has_desc = false;   /* 首页由 ui_home_create 自己管，没有描述符 */
    s_depth = 1;
    lv_screen_load(scr);
    ui_back_button_update();       /* 首页没有上一级：全局返回键隐藏 */
    return ESP_OK;
}

/* 当前栈顶页是否需要全局返回键：栈深 > 1（有上一级可回）且该页没有自带返回钮 */
bool ui_router_top_needs_back(void)
{
    if (s_depth <= 1) {
        return false;
    }
    const route_entry_t *top = &s_stack[s_depth - 1];
    return !(top->has_desc && top->desc.no_back);
}

/* 最深一页的对象（供手势层做跟手位移） */
lv_obj_t *ui_router_top_screen(void)
{
    return (s_depth > 0) ? s_stack[s_depth - 1].scr : NULL;
}

/* 上一层对象（返回手势的视差背景）；无则返回 NULL */
lv_obj_t *ui_router_under_screen(void)
{
    return (s_depth > 1) ? s_stack[s_depth - 2].scr : NULL;
}