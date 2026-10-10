/*
 * os_state.cc —— 共享状态（歌单 ID 表 / init 标志 / last_err）的实现
 *
 * 这里只是把原 online_service.cc 顶层的 static 变量搬出来，逻辑逐字未改
 * （原来是 135–141 行的 s_ready / s_ids / s_names / s_id_count / s_last_err）。
 * 区别只有一点：**读写都经访问器**，其它 .cc 不再直接摸这些 static
 * （拆分蓝图 §二.4"共享状态收敛：不共享裸 static"）。
 */
#include "internal/os_state.h"

#include <stdio.h>
#include <string.h>

#include "internal/os_util.h"   /* os_str_copy */

static bool   s_ready;
static char   s_ids[ONLINE_MAX_PLAYLISTS][ONLINE_ID_MAX];
/* 显示名（可与 ID 一一对应；空串表示"没起过名"，UI 自己决定显示 ID 还是标题） */
static char   s_names[ONLINE_MAX_PLAYLISTS][ONLINE_NAME_MAX];
static size_t s_id_count;
/* 最近一次失败的可读原因（给 UI 的 Toast 用） */
static char   s_last_err[64];

bool os_state_ready(void)
{
    return s_ready;
}

void os_state_set_ready(void)
{
    s_ready = true;
}

size_t os_state_playlist_count(void)
{
    return s_id_count;
}

const char *os_state_playlist_id(size_t idx)
{
    return (idx < s_id_count) ? s_ids[idx] : NULL;
}

const char *os_state_playlist_name(size_t idx)
{
    if (idx >= s_id_count) {
        return NULL;
    }
    /* 没起过名就空串 —— 让 UI 自己决定退回显示 ID 还是别的，服务不做展示决策 */
    return s_names[idx];
}

void os_state_playlist_rename(size_t idx, const char *name)
{
    if (idx >= s_id_count) {
        return;
    }
    /* name == NULL 或空串 = 清掉自定义名（退回显示 ID） */
    os_str_copy(s_names[idx], sizeof(s_names[0]), name ? name : "");
}

int os_state_playlist_index(const char *id)
{
    for (size_t i = 0; i < s_id_count; i++) {
        if (strcmp(s_ids[i], id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool os_state_playlist_append(const char *id)
{
    if (s_id_count >= ONLINE_MAX_PLAYLISTS) {
        return false;
    }
    os_str_copy(s_ids[s_id_count], sizeof(s_ids[0]), id);
    s_id_count++;
    return true;
}

void os_state_playlist_remove_at(size_t idx)
{
    if (idx >= s_id_count) {
        return;
    }
    for (size_t i = idx + 1; i < s_id_count; i++) {
        os_str_copy(s_ids[i - 1], sizeof(s_ids[0]), s_ids[i]);
        os_str_copy(s_names[i - 1], sizeof(s_names[0]), s_names[i]);   /* 名字跟着挪 */
    }
    s_id_count--;
    s_names[s_id_count][0] = '\0';
}

void os_state_playlist_reset(void)
{
    s_id_count = 0;
}

bool os_state_playlist_load_one(const char *id, const char *name)
{
    if (s_id_count >= ONLINE_MAX_PLAYLISTS) {
        return false;
    }
    const size_t i = s_id_count;
    os_str_copy(s_ids[i], sizeof(s_ids[0]), id);
    /* 名字是可选的：没有就是空串（UI 退回显示 ID） */
    os_str_copy(s_names[i], sizeof(s_names[0]), name ? name : "");
    s_id_count++;
    return true;
}

void os_state_set_last_error(const char *msg)
{
    snprintf(s_last_err, sizeof(s_last_err), "%s", msg ? msg : "");
}

const char *os_state_last_error(void)
{
    return s_last_err[0] ? s_last_err : NULL;
}
