/*
 * ao_result.cc —— 结果分发（on_result）+ 200 ms 轮询（poll_cb）
 *
 * 这是"工作线程 → 界面"的**唯一出口**，所有跨任务结果都在这里落地。
 * 本文件只做"分派 + 调各页的重建"，具体状态迁移在 ao_model.cc，具体绘制在
 * ao_page_*.cc —— 别把画界面的代码搬进来。
 *
 * ★ 取结果的顺序（见 ao_work.h）：ao_work_take() 一次性把整份负载拷到本地、并清掉
 *   槽位，之后本文件只用本地副本。**绝不能**在分派过程中再去读 s_res_*。
 */
#include "ao_result.h"

#include "online_service.h"
#include "ui_core.h"

#include "ao_common.h"
#include "ao_account.h"
#include "ao_model.h"
#include "ao_pages.h"
#include "ao_overlay.h"
#include "ao_work.h"

static lv_timer_t *s_poll;

void ao_result_poll_start(void)
{
    s_poll = lv_timer_create(ao_result_poll_cb, POLL_MS, NULL);
}

void ao_result_poll_stop(void)
{
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
}

/*
 * 结果分发：按 kind 走 12 条分支（含二维码 800/801/802/803 状态机、账号页懒加载分页）。
 * ⚠ 每个分支里的重建/Toast 顺序与拆分前逐字一致 —— 顺序变了会出现"先看到别人的歌单
 *   再被刷掉"这类一闪而过的错画面。
 */
static void on_result(void)
{
    ao_result_t res;
    ao_work_take(&res);
    if (res.kind == OREQ_NONE) {
        return;
    }
    /* 取到本地之后槽位已经空了（工作线程可以立刻开始下一轮），下面全用 res.* */
    const int kind = res.kind;
    const esp_err_t err = res.err;
    online_song_t *songs = res.songs;
    const size_t count = res.count;
    /* OREQ_ACCT_SONGS 的分页信息：和 songs/msg 一样，**发布标志一清就先取到本地**，
     * 免得后面被下一份结果的同名字段（工作线程会重写）串味 */
    const int page_off  = res.off;
    const int page_want = res.want;
    const char *msg = res.msg;

    switch (kind) {
    case OREQ_ADD:                    /* 添加 / 更新歌单走同一条路径（已存在就是刷新） */
        if (err == ESP_OK) {
            ui_toast("歌单已更新", 1500);
            /* 内存里那份（s_songs）已经过期：把身份标记清掉，下次点进这个歌单会重新读，
             * 而不是先把旧内容顶上来 */
            ao_queue_key_clear_if(msg);
        } else if (err == ESP_ERR_INVALID_STATE) {
            ui_toast("Wi-Fi 未连接", 1800);
        } else {
            /* 把服务里的具体原因带出来（HTTP 400 / 网络错误 / 写 SD 失败…） */
            const char *why = online_last_error();
            char buf[96];
            snprintf(buf, sizeof(buf), "抓取失败：%s", why ? why : "ID 不对或网络不通");
            ui_toast(buf, 2600);
        }
        ao_pl_rebuild();
        break;
    case OREQ_LOAD:
        if (err == ESP_OK && songs) {
            ao_queue_install(songs, count, msg, OWNER_PL);
            ui_toast("歌单已载入", 1200);
        } else {
            free(songs);
            /* 把服务里的具体原因带出来（"响应过大" / HTTP 4xx / 网络错误…）——
             * 只报一句"歌单读取失败"时根本没法判断是哪种（2026-10 踩过） */
            const char *why = online_last_error();
            char buf[128];
            snprintf(buf, sizeof(buf), "歌单读取失败：%.60s", why ? why : "网络不通");
            ui_toast(buf, 2600);
            ao_pl_detail_drop();      /* 读不出来就退回歌单列表，别停在空壳上 */
        }
        ao_pl_rebuild();
        break;
    case OREQ_SEARCH:
        ao_srch_set_busy(false);      /* 结果到了（或失败了）：退出"正在搜索"态 */
        if (err == ESP_OK && songs) {
            ao_queue_install(songs, count, "", OWNER_SRCH);
            ao_srch_set_has_res(true);
            ui_toast("搜索完成", 1200);
        } else {
            free(songs);
            /* 仍然占住"结果"这个状态：显示"没找到"，而不是退回历史列表 */
            ao_srch_set_has_res(true);
            ui_toast("没有找到匹配的歌曲", 1800);
        }
        ao_srch_rebuild();
        break;
    case OREQ_REMOVE:
        /* 只重画页面：**绝不能**在这里 free(s_songs) —— 播放器可能正在放它，
         * 等下一次 ao_queue_install 把它挪进 s_stale 再释放（那才是安全时机）。 */
        ao_pl_rebuild();
        break;

    /* ---------------- 网易云扫码登录（状态机：801 待扫码 / 802 待确认 / 803 成功 / 800 失效） */
    case OREQ_QR_KEY:
        if (err != ESP_OK || !res.img) {
            free(res.img);
            ao_qr_set_status("二维码获取失败，点「刷新」重试");
            break;
        }
        ao_qr_show_key(msg, res.img, res.img_len);   /* 内部 free(res.img) */
        break;
    case OREQ_QR_POLL: {
        const int code = res.qr;
        if (code == NET_QR_OK) {
            ao_qr_set_status("登录成功");
            ui_toast("网易云登录成功", 1800);
            ao_login_success_forget(lv_tick_get());
            ao_qr_close();
            ao_pl_rebuild();
            ao_acct_rebuild();
            ao_login_success_pull_account();
        } else if (code == NET_QR_SCAN) {
            ao_qr_set_status("已扫码，请在手机上确认");
        } else if (code == NET_QR_WAIT) {
            ao_qr_set_status("等待扫码…");
        } else if (code == NET_QR_EXPIRE) {
            ao_qr_set_status("二维码已失效，点「刷新」重新获取");
            ao_qr_stop_poll();
        }
        break;
    }
    case OREQ_LOGOUT:
        ao_logout_reset(lv_tick_get());
        ui_toast(err == ESP_OK ? "已退出网易云登录" : "退出失败", 1600);
        ao_pl_rebuild();
        ao_acct_rebuild();
        break;
    case OREQ_ACCOUNT:
        ao_account_result(err, msg, lv_tick_get());
        ao_pl_rebuild();
        ao_acct_rebuild();
        ao_acct_pls_after_account(err == ESP_OK, lv_tick_get());
        break;

    /* ---------------- 第 0 页「我的歌单」：账号歌单 / 每日推荐 ---------------- */
    case OREQ_ACCT_PLS:
        ao_acct_pls_result(err, res.pls, res.pls_n, lv_tick_get());
        ao_acct_rebuild();
        break;
    case OREQ_ACCT_SONGS: {
        /*
         * **懒加载的一页**：把这一页追加到 s_songs 的已载入末尾（必须连续）。
         * 注意这是"追加"而不是"安装" —— 缓冲是 ao_lazy_begin() 一次性分配好的，
         * 指针从头到尾不变，所以播放器那边的队列指针一直有效。
         */
        const ao_page_outcome_t oc = ao_lazy_handle_page(page_off, page_want, err, msg,
                                                         songs, count);
        if (oc == AO_PAGE_FAILED && ao_queue_count() == 0) {
            /* 一首都没载入时（第一次就失败）直接退回歌单列表并说明原因 */
            const char *why = online_last_error();
            char buf[128];
            snprintf(buf, sizeof(buf), "歌单读取失败：%.60s", why ? why : "网络不通");
            ui_toast(buf, 2600);
            ao_acct_detail_close();
        }
        free(songs);
        ao_acct_rebuild();
        break;
    }
    case OREQ_DAILY:
        if (err == ESP_OK && songs) {
            ao_queue_install(songs, count, DETAIL_KEY_DAILY, OWNER_ACCT);
            ao_daily_loaded();
            ui_toast("每日推荐已载入", 1200);
        } else {
            free(songs);
            /* 同上：带出服务侧的具体原因（"响应过大" / 版权 / 网络…） */
            const char *why = online_last_error();
            char buf[128];
            snprintf(buf, sizeof(buf), "每日推荐读取失败：%.60s", why ? why : "网络不通");
            ui_toast(buf, 2600);
            ao_acct_detail_close();   /* 读不出来就退回列表，别停在空壳上 */
        }
        ao_acct_rebuild();
        break;
    default:
        free(songs);
        break;
    }
}

void ao_result_poll_cb(lv_timer_t *t)
{
    (void)t;
    /* 网络状态变了就刷新一下提示（连上网/断网） */
    static bool s_net_last;
    const bool net = online_has_network();
    if (net != s_net_last) {
        s_net_last = net;
        ao_pl_rebuild();
        ao_srch_rebuild();
        ao_acct_rebuild();              /* 账号页：网络回来要重新拉歌单/昵称 */
    }
    on_result();

    /*
     * 登录态下的"补拉"：开机后 Wi-Fi 可能还没连上，那次拉歌单/昵称必然失败
     * —— 以前"问过就不再问"，用户重启后进来就是一片空。这里按 5 秒节流重试，
     * 转圈动画也跟着出现（见 ao_acct_kick_if_needed / ao_acct_nick_kick_if_needed）。
     */
    if (online_netease_logged_in()) {
        const bool busy_before = ao_acct_pls_busy();
        ao_acct_nick_kick_if_needed(lv_tick_get());
        ao_acct_kick_if_needed(lv_tick_get());
        /* 转圈要跟着"这次重试"出现：只在本页可见时重画，别的页等结果回调自己重画 */
        if (ao_pages_current() == 0 && ao_acct_pls_busy() != busy_before) {
            ao_acct_rebuild();
        }
    }
}
