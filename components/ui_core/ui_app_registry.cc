/*
 * ui_app_registry.cc —— 桌面 APP 注册表（描述符数组，链接期收集）
 */
#include "ui_app_registry.h"

#include <string.h>

#define UI_APP_MAX 16

static const app_desc_t *s_apps[UI_APP_MAX];
static size_t s_count;

void ui_app_register(const app_desc_t *desc)
{
    if (!desc || !desc->id || s_count >= UI_APP_MAX) {
        return;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i]->id, desc->id) == 0) {
            s_apps[i] = desc;   /* 同 id 覆盖，便于开发期热替换 */
            return;
        }
    }
    s_apps[s_count++] = desc;
}

void ui_app_reset(void)
{
    s_count = 0;
}

const app_desc_t *ui_app_at(size_t idx, bool exclude_hidden)
{
    size_t seen = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (exclude_hidden && (s_apps[i]->flags & APP_FLAG_HIDDEN)) {
            continue;
        }
        if (seen == idx) {
            return s_apps[i];
        }
        seen++;
    }
    return NULL;
}

size_t ui_app_count(bool exclude_hidden)
{
    if (!exclude_hidden) {
        return s_count;
    }
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (!(s_apps[i]->flags & APP_FLAG_HIDDEN)) {
            n++;
        }
    }
    return n;
}

const app_desc_t *ui_app_find(const char *id)
{
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i]->id, id) == 0) {
            return s_apps[i];
        }
    }
    return NULL;
}