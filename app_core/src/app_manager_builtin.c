#include "app_manager.h"
#include "app_manager_builtin.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef HOST_TEST
    extern const app_manager_app_desc_t _app_manager_apps_start[];
    extern const app_manager_app_desc_t _app_manager_apps_end[];
#endif

static const app_manager_app_desc_t *s_fallback_apps;
static size_t s_fallback_app_count;
static const app_manager_app_desc_t *s_registry_apps;
static size_t s_registry_app_count;
static bool s_registry_validated;

static bool _builtin_id_is_valid(const char *id)
{
    size_t length = 0;
    if (id != NULL)
    {
        while (length < APP_MANAGER_ID_BYTES && id[length] != '\0')
        {
            ++length;
        }
    }
    return length > 0U && length < APP_MANAGER_ID_BYTES;
}

#ifndef HOST_TEST
static bool _builtin_linker_registry_present(void)
{
    return &_app_manager_apps_end[0] > &_app_manager_apps_start[0];
}
#endif

static const app_manager_app_desc_t *_builtin_source_apps(size_t *count)
{
    const app_manager_app_desc_t *apps = s_fallback_apps;
    *count = s_fallback_app_count;
#ifndef HOST_TEST
    if (_builtin_linker_registry_present())
    {
        apps = _app_manager_apps_start;
        *count = (size_t)(_app_manager_apps_end - _app_manager_apps_start);
    }
#endif
    return apps;
}

static const app_manager_app_desc_t *_builtin_find_app_in(
    const app_manager_app_desc_t *apps, size_t count, const char *app_id)
{
    if (app_id == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (apps[i].id != NULL && strcmp(apps[i].id, app_id) == 0)
        {
            return &apps[i];
        }
    }
    return NULL;
}

static const app_manager_page_route_t *_builtin_find_page_in(
    const app_manager_app_desc_t *app, const char *page_id)
{
    if (app == NULL || page_id == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < app->page_route_count; ++i)
    {
        const app_manager_page_route_t *route = &app->page_routes[i];
        if (route->page_id != NULL && strcmp(route->page_id, page_id) == 0)
        {
            return route;
        }
    }
    return NULL;
}

static esp_err_t _builtin_validate_routes(const app_manager_app_desc_t *app)
{
    if (app->page_routes == NULL || app->page_route_count == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < app->page_route_count; ++i)
    {
        const app_manager_page_route_t *route = &app->page_routes[i];
        if (!_builtin_id_is_valid(route->page_id) ||
                route->definition == NULL ||
                ((route->definition->handler == NULL) ==
                 (route->definition->ops == NULL)) ||
                route->definition->memory_size >
                APP_MANAGER_PAGE_STATE_BYTES)
        {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (strcmp(app->page_routes[j].page_id, route->page_id) == 0)
            {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return _builtin_find_page_in(app, app->root_page_id) != NULL ?
           ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t _builtin_validate_apps(const app_manager_app_desc_t *apps,
                                        size_t app_count)
{
    const uint32_t known_flags = APP_MANAGER_APP_FLAG_PINNED |
                                 APP_MANAGER_APP_FLAG_HIDDEN;
    for (size_t i = 0; i < app_count; ++i)
    {
        const app_manager_app_desc_t *app = &apps[i];
        if (app->name == NULL || app->name[0] == '\0' ||
                !_builtin_id_is_valid(app->id) ||
                !_builtin_id_is_valid(app->root_page_id) ||
                (app->flags & ~known_flags) != 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (strcmp(apps[j].id, app->id) == 0)
            {
                return ESP_ERR_INVALID_ARG;
            }
        }
        const esp_err_t result = _builtin_validate_routes(app);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    return ESP_OK;
}

esp_err_t app_manager_register_builtin_descriptors(
    const app_manager_app_desc_t *apps, size_t app_count)
{
    if ((apps == NULL) != (app_count == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registry_validated)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_fallback_apps = apps;
    s_fallback_app_count = app_count;
    s_registry_apps = NULL;
    s_registry_app_count = 0U;
    return ESP_OK;
}

esp_err_t app_manager_builtin_registry_validate(void)
{
    size_t app_count = 0U;
    const app_manager_app_desc_t *apps = _builtin_source_apps(&app_count);

    s_registry_validated = false;
    s_registry_apps = NULL;
    s_registry_app_count = 0U;
    if ((apps == NULL) != (app_count == 0U) || app_count == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = _builtin_validate_apps(apps, app_count);
    if (result != ESP_OK)
    {
        return result;
    }
    s_registry_apps = apps;
    s_registry_app_count = app_count;
    s_registry_validated = true;
    return ESP_OK;
}

void app_manager_builtin_registry_reset(void)
{
    s_registry_apps = NULL;
    s_registry_app_count = 0U;
    s_registry_validated = false;
}

size_t app_manager_builtin_app_count(void)
{
    return s_registry_validated ? s_registry_app_count : 0U;
}

const app_manager_app_desc_t *app_manager_builtin_enumerate(size_t index)
{
    return s_registry_validated && index < s_registry_app_count ?
           &s_registry_apps[index] : NULL;
}

const app_manager_app_desc_t *app_manager_builtin_find_by_id(const char *app_id)
{
    return s_registry_validated ?
           _builtin_find_app_in(s_registry_apps, s_registry_app_count, app_id) :
           NULL;
}

const app_manager_page_route_t *app_manager_builtin_find_page(
    const app_manager_app_desc_t *app, const char *page_id)
{
    if (!s_registry_validated || app == NULL)
    {
        return NULL;
    }
    bool known_app = false;
    for (size_t i = 0; i < s_registry_app_count; ++i)
    {
        if (&s_registry_apps[i] == app)
        {
            known_app = true;
            break;
        }
    }
    return known_app ? _builtin_find_page_in(app, page_id) : NULL;
}

int app_manager_builtin_discover(void)
{
    int count = 0;
    esp_err_t result = ESP_OK;
    if (!s_registry_validated)
    {
        result = app_manager_builtin_registry_validate();
    }
    if (result == ESP_OK && s_registry_app_count <= INT_MAX)
    {
        count = (int)s_registry_app_count;
    }
    return count;
}

const app_manager_app_desc_t *app_manager_builtin_list_open(void)
{
    return app_manager_builtin_enumerate(0U);
}

const app_manager_app_desc_t *app_manager_builtin_list_get_next(
    const app_manager_app_desc_t *previous)
{
    if (!s_registry_validated || previous == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < s_registry_app_count; ++i)
    {
        if (&s_registry_apps[i] == previous)
        {
            return app_manager_builtin_enumerate(i + 1U);
        }
    }
    return NULL;
}
