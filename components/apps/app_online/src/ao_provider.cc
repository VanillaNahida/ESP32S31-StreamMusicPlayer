/*
 * ao_provider.cc —— 音源选择的实现（只有一份 static 状态 + 几组文案）
 */
#include "ao_provider.h"

#include <stddef.h>

#include "online_service.h"

static int s_provider = AO_PROV_NETEASE;

void ao_provider_set(int p)
{
    s_provider = (p == AO_PROV_QQ) ? AO_PROV_QQ : AO_PROV_NETEASE;
}

int ao_provider(void)
{
    return s_provider;
}

bool ao_is_qq(void)
{
    return s_provider == AO_PROV_QQ;
}

const char *ao_provider_title(void)
{
    return ao_is_qq() ? "QQ 音乐" : "网易云音乐";
}

const char *ao_provider_account_name(void)
{
    return ao_is_qq() ? "QQ 音乐" : "网易云";
}

/* 二维码浮层下方那句话（ao_overlay.cc 用） */
const char *ao_provider_scan_hint(void)
{
    return ao_is_qq() ? "用手机 QQ 扫描二维码，即可获取你的歌单与「我喜欢」"
                      : "用网易云音乐 App 扫码，即可获取你的账号歌单与每日推荐";
}

/* 二维码出来后状态行的第一句 */
const char *ao_provider_scan_status(void)
{
    return ao_is_qq() ? "请用手机 QQ 扫码" : "请用网易云音乐 App 扫码";
}

bool ao_logged_in(void)
{
    return ao_is_qq() ? online_qq_logged_in() : online_netease_logged_in();
}

const char *ao_nickname(void)
{
    return ao_is_qq() ? online_qq_nickname() : online_netease_nickname();
}

void ao_nickname_clear(void)
{
    if (ao_is_qq()) {
        online_qq_nickname_set(NULL);
    } else {
        online_netease_nickname_set(NULL);
    }
}

void ao_login_state_reload(void)
{
    /* 网易云：开机后 SD 可能还没挂上，那次读不到不代表没登录过；
     * QQ：凭证在 NVS 里，没有这个时序问题，但接口保留对称。 */
    if (ao_is_qq()) {
        online_qq_cred_reload();
    } else {
        online_netease_cookie_reload();
    }
}
