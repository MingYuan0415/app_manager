#define DBG_TAG "app_builtin"
#define DBG_LVL DBG_INFO

#ifndef HOST_TEST
    #include "mt_log.h"
#endif
#include "app_manager.h"
#include "app_manager_builtin.h"
#include <stdint.h>
#include <string.h>

#ifndef HOST_TEST
    extern const app_builtin_app_desc_t _app_builtin_apps_start[];
    extern const app_builtin_app_desc_t _app_builtin_apps_end[];
#endif

static const app_builtin_app_desc_t *s_fallback_apps;
static size_t s_fallback_count;

esp_err_t app_manager_register_builtin_apps(const app_builtin_app_desc_t *apps,
        size_t count)
{
    esp_err_t result = ESP_OK;
    if ((apps == NULL) != (count == 0))
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    s_fallback_apps = apps;
    s_fallback_count = count;

exit:
    return result;
}

static const app_builtin_app_desc_t *_builtin_begin(void)
{
    const app_builtin_app_desc_t *begin =
        s_fallback_count > 0 ? s_fallback_apps : NULL;
#ifndef HOST_TEST
    if (&_app_builtin_apps_end[0] > &_app_builtin_apps_start[0])
    {
        begin = _app_builtin_apps_start;
    }
#endif
    return begin;
}

static const app_builtin_app_desc_t *_builtin_limit(void)
{
    const app_builtin_app_desc_t *limit =
        s_fallback_count > 0 ? s_fallback_apps + s_fallback_count : NULL;
#ifndef HOST_TEST
    if (&_app_builtin_apps_end[0] > &_app_builtin_apps_start[0])
    {
        limit = _app_builtin_apps_end;
    }
#endif
    return limit;
}

int app_manager_builtin_discover(void)
{
    const app_builtin_app_desc_t *begin = _builtin_begin();
    const app_builtin_app_desc_t *end = _builtin_limit();
    return begin != NULL && end != NULL ? (int)(end - begin) : 0;
}

const app_builtin_app_desc_t *app_manager_builtin_list_open(void)
{
    return _builtin_begin();
}

const app_builtin_app_desc_t *app_manager_builtin_list_get_next(
    const app_builtin_app_desc_t *previous)
{
    const app_builtin_app_desc_t *next = NULL;
    const app_builtin_app_desc_t *begin = _builtin_begin();
    const app_builtin_app_desc_t *end = _builtin_limit();
    if (begin == NULL || end == NULL)
    {
        goto exit;
    }
    if (previous == NULL)
    {
        next = begin;
        goto exit;
    }
    for (const app_builtin_app_desc_t *cursor = begin;
            cursor < end; cursor++)
    {
        if (cursor == previous)
        {
            next = cursor + 1 < end ? cursor + 1 : NULL;
            break;
        }
    }

exit:
    return next;
}

const app_builtin_app_desc_t *app_manager_builtin_find_by_id(const char *app_id)
{
    const app_builtin_app_desc_t *match = NULL;
    if (app_id == NULL)
    {
        goto exit;
    }
    const app_builtin_app_desc_t *app = _builtin_begin();
    const app_builtin_app_desc_t *end = _builtin_limit();
    while (app != NULL && app < end)
    {
        if (app->id != NULL && strcmp(app->id, app_id) == 0)
        {
            match = app;
            break;
        }
        app++;
    }

exit:
    return match;
}
