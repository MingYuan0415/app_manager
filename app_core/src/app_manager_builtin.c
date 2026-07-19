#include "app_manager.h"
#include "app_manager_builtin.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define APP_MANAGER_BUILTIN_LEASE_TAG 0xB17EU

typedef struct app_manager_builtin_lease_slot
{
    const app_manager_app_desc_t *app;
    uint32_t generation;
    bool in_use;
} app_manager_builtin_lease_slot_t;

#ifndef HOST_TEST
    extern const app_manager_app_desc_t _app_manager_apps_start[];
    extern const app_manager_app_desc_t _app_manager_apps_end[];
    extern const app_manager_page_desc_t _app_manager_pages_start[];
    extern const app_manager_page_desc_t _app_manager_pages_end[];
#endif

static const app_manager_app_desc_t *s_fallback_apps;
static size_t s_fallback_app_count;
static const app_manager_page_desc_t *s_fallback_pages;
static size_t s_fallback_page_count;
static const app_manager_app_desc_t *s_registry_apps;
static size_t s_registry_app_count;
static const app_manager_page_desc_t *s_registry_pages;
static size_t s_registry_page_count;
static app_manager_builtin_lease_slot_t
s_lease_slots[APP_MANAGER_MAX_RESIDENT_APPS];
static bool s_registry_validated;
#ifdef HOST_TEST
    static size_t s_release_failures;
#endif

static bool _builtin_id_is_valid(const char *id)
{
    size_t length = 0;
    if (id != NULL)
    {
        while (length < APP_MANAGER_ID_BYTES && id[length] != '\0')
        {
            length++;
        }
    }
    return length > 0 && length < APP_MANAGER_ID_BYTES;
}

#ifndef HOST_TEST
static bool _builtin_linker_registry_present(void)
{
    return &_app_manager_apps_end[0] > &_app_manager_apps_start[0] ||
           &_app_manager_pages_end[0] > &_app_manager_pages_start[0];
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

static const app_manager_page_desc_t *_builtin_source_pages(size_t *count)
{
    const app_manager_page_desc_t *pages = s_fallback_pages;
    *count = s_fallback_page_count;
#ifndef HOST_TEST
    if (_builtin_linker_registry_present())
    {
        pages = _app_manager_pages_start;
        *count = (size_t)(_app_manager_pages_end - _app_manager_pages_start);
    }
#endif
    return pages;
}

static bool _builtin_has_leases(void)
{
    bool has_leases = false;
    for (size_t i = 0; i < APP_MANAGER_MAX_RESIDENT_APPS; i++)
    {
        if (s_lease_slots[i].in_use)
        {
            has_leases = true;
            break;
        }
    }
    return has_leases;
}

static const app_manager_app_desc_t *_builtin_find_app_in(
    const app_manager_app_desc_t *apps, size_t count, const char *app_id)
{
    const app_manager_app_desc_t *match = NULL;
    if (app_id == NULL)
    {
        goto exit;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (apps[i].id != NULL && strcmp(apps[i].id, app_id) == 0)
        {
            match = &apps[i];
            break;
        }
    }

exit:
    return match;
}

static const app_manager_page_desc_t *_builtin_find_page_in(
    const app_manager_page_desc_t *pages, size_t count, const char *app_id,
    const char *page_id)
{
    const app_manager_page_desc_t *match = NULL;
    if (app_id == NULL || page_id == NULL)
    {
        goto exit;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (pages[i].app_id != NULL && pages[i].page_id != NULL &&
                strcmp(pages[i].app_id, app_id) == 0 &&
                strcmp(pages[i].page_id, page_id) == 0)
        {
            match = &pages[i];
            break;
        }
    }

exit:
    return match;
}

static esp_err_t _builtin_validate_apps(const app_manager_app_desc_t *apps,
                                        size_t app_count)
{
    esp_err_t result = ESP_OK;
    const uint32_t known_flags = APP_MANAGER_APP_FLAG_PINNED |
                                 APP_MANAGER_APP_FLAG_HIDDEN;
    for (size_t i = 0; i < app_count; i++)
    {
        const app_manager_app_desc_t *app = &apps[i];
        if (app->name == NULL || app->name[0] == '\0' ||
                !_builtin_id_is_valid(app->id) ||
                !_builtin_id_is_valid(app->root_page_id) ||
                app->type != APP_MANAGER_APP_BUILTIN ||
                (app->flags & ~known_flags) != 0)
        {
            result = ESP_ERR_INVALID_ARG;
            goto exit;
        }
        for (size_t j = 0; j < i; j++)
        {
            if (strcmp(apps[j].id, app->id) == 0)
            {
                result = ESP_ERR_INVALID_ARG;
                goto exit;
            }
        }
    }

exit:
    return result;
}

static esp_err_t _builtin_validate_pages(
    const app_manager_app_desc_t *apps, size_t app_count,
    const app_manager_page_desc_t *pages, size_t page_count,
    size_t max_page_memory_size)
{
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < page_count; i++)
    {
        const app_manager_page_desc_t *page = &pages[i];
        if (!_builtin_id_is_valid(page->app_id) ||
                !_builtin_id_is_valid(page->page_id) || page->handler == NULL ||
                page->memory_size > max_page_memory_size ||
                _builtin_find_app_in(apps, app_count, page->app_id) == NULL)
        {
            result = ESP_ERR_INVALID_ARG;
            goto exit;
        }
        for (size_t j = 0; j < i; j++)
        {
            if (strcmp(pages[j].app_id, page->app_id) == 0 &&
                    strcmp(pages[j].page_id, page->page_id) == 0)
            {
                result = ESP_ERR_INVALID_ARG;
                goto exit;
            }
        }
    }
    for (size_t i = 0; i < app_count; i++)
    {
        if (_builtin_find_page_in(pages, page_count, apps[i].id,
                                  apps[i].root_page_id) == NULL)
        {
            result = ESP_ERR_INVALID_ARG;
            goto exit;
        }
    }

exit:
    return result;
}

static app_manager_builtin_lease_slot_t *_builtin_lease_slot(
    const app_manager_builtin_lease_t *lease)
{
    app_manager_builtin_lease_slot_t *slot = NULL;
    if (lease == NULL || lease->slot >= APP_MANAGER_MAX_RESIDENT_APPS ||
            lease->reserved != APP_MANAGER_BUILTIN_LEASE_TAG)
    {
        goto exit;
    }
    app_manager_builtin_lease_slot_t *candidate = &s_lease_slots[lease->slot];
    if (candidate->in_use && candidate->generation == lease->generation)
    {
        slot = candidate;
    }

exit:
    return slot;
}

static void _builtin_invalidate_lease(app_manager_builtin_lease_t *lease)
{
    if (lease != NULL)
    {
        lease->slot = UINT16_MAX;
        lease->reserved = 0;
        lease->generation = 0;
    }
}

esp_err_t app_manager_register_builtin_descriptors(
    const app_manager_app_desc_t *apps, size_t app_count,
    const app_manager_page_desc_t *pages, size_t page_count)
{
    esp_err_t result = ESP_OK;
    if ((apps == NULL) != (app_count == 0) ||
            (pages == NULL) != (page_count == 0))
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    if (_builtin_has_leases())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    s_fallback_apps = apps;
    s_fallback_app_count = app_count;
    s_fallback_pages = pages;
    s_fallback_page_count = page_count;
    s_registry_apps = NULL;
    s_registry_app_count = 0;
    s_registry_pages = NULL;
    s_registry_page_count = 0;
    s_registry_validated = false;

exit:
    return result;
}

esp_err_t app_manager_builtin_registry_validate(size_t max_page_memory_size)
{
    esp_err_t result = ESP_OK;
    size_t app_count = 0;
    size_t page_count = 0;
    const app_manager_app_desc_t *apps = _builtin_source_apps(&app_count);
    const app_manager_page_desc_t *pages = _builtin_source_pages(&page_count);
    if (_builtin_has_leases())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    s_registry_validated = false;
    if ((apps == NULL) != (app_count == 0) ||
            (pages == NULL) != (page_count == 0) || app_count == 0)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    result = _builtin_validate_apps(apps, app_count);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _builtin_validate_pages(apps, app_count, pages, page_count,
                                     max_page_memory_size);
    if (result != ESP_OK)
    {
        goto exit;
    }
    s_registry_apps = apps;
    s_registry_app_count = app_count;
    s_registry_pages = pages;
    s_registry_page_count = page_count;
    s_registry_validated = true;

exit:
    if (result != ESP_OK)
    {
        s_registry_apps = NULL;
        s_registry_app_count = 0;
        s_registry_pages = NULL;
        s_registry_page_count = 0;
    }
    return result;
}

esp_err_t app_manager_builtin_registry_reset(void)
{
    esp_err_t result = ESP_OK;
    if (_builtin_has_leases())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    s_registry_apps = NULL;
    s_registry_app_count = 0;
    s_registry_pages = NULL;
    s_registry_page_count = 0;
    s_registry_validated = false;

exit:
    return result;
}

size_t app_manager_builtin_app_count(void)
{
    return s_registry_validated ? s_registry_app_count : 0;
}

size_t app_manager_builtin_page_count(void)
{
    return s_registry_validated ? s_registry_page_count : 0;
}

const app_manager_app_desc_t *app_manager_builtin_enumerate(size_t index)
{
    const app_manager_app_desc_t *app = NULL;
    if (s_registry_validated && index < s_registry_app_count)
    {
        app = &s_registry_apps[index];
    }
    return app;
}

const app_manager_page_desc_t *app_manager_builtin_enumerate_page(size_t index)
{
    const app_manager_page_desc_t *page = NULL;
    if (s_registry_validated && index < s_registry_page_count)
    {
        page = &s_registry_pages[index];
    }
    return page;
}

const app_manager_app_desc_t *app_manager_builtin_find_by_id(const char *app_id)
{
    const app_manager_app_desc_t *app = NULL;
    if (s_registry_validated)
    {
        app = _builtin_find_app_in(s_registry_apps, s_registry_app_count, app_id);
    }
    return app;
}

const app_manager_page_desc_t *app_manager_builtin_find_page(
    const char *app_id, const char *page_id)
{
    const app_manager_page_desc_t *page = NULL;
    if (s_registry_validated)
    {
        page = _builtin_find_page_in(s_registry_pages, s_registry_page_count,
                                     app_id, page_id);
    }
    return page;
}

esp_err_t app_manager_builtin_acquire(const char *app_id,
                                      app_manager_builtin_lease_t *out_lease)
{
    esp_err_t result = ESP_OK;
    if (out_lease == NULL)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    _builtin_invalidate_lease(out_lease);
    const app_manager_app_desc_t *app = app_manager_builtin_find_by_id(app_id);
    if (app == NULL)
    {
        result = s_registry_validated ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
        goto exit;
    }
    size_t index = APP_MANAGER_MAX_RESIDENT_APPS;
    for (size_t i = 0; i < APP_MANAGER_MAX_RESIDENT_APPS; i++)
    {
        if (!s_lease_slots[i].in_use)
        {
            index = i;
            break;
        }
    }
    if (index == APP_MANAGER_MAX_RESIDENT_APPS)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    app_manager_builtin_lease_slot_t *slot = &s_lease_slots[index];
    slot->generation++;
    if (slot->generation == 0)
    {
        slot->generation++;
    }
    slot->app = app;
    slot->in_use = true;
    out_lease->slot = (uint16_t)index;
    out_lease->reserved = APP_MANAGER_BUILTIN_LEASE_TAG;
    out_lease->generation = slot->generation;

exit:
    return result;
}

const app_manager_app_desc_t *app_manager_builtin_lease_app(
    const app_manager_builtin_lease_t *lease)
{
    app_manager_builtin_lease_slot_t *slot = _builtin_lease_slot(lease);
    return slot != NULL ? slot->app : NULL;
}

const app_manager_page_desc_t *app_manager_builtin_lease_find_page(
    const app_manager_builtin_lease_t *lease, const char *page_id)
{
    const app_manager_page_desc_t *page = NULL;
    const app_manager_app_desc_t *app = app_manager_builtin_lease_app(lease);
    if (app != NULL)
    {
        page = app_manager_builtin_find_page(app->id, page_id);
    }
    return page;
}

esp_err_t app_manager_builtin_release(app_manager_builtin_lease_t *lease)
{
    esp_err_t result = ESP_OK;
    app_manager_builtin_lease_slot_t *slot = _builtin_lease_slot(lease);
    if (slot == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
#ifdef HOST_TEST
    if (s_release_failures > 0U)
    {
        s_release_failures--;
        result = ESP_FAIL;
        goto exit;
    }
#endif
    slot->app = NULL;
    slot->in_use = false;
    _builtin_invalidate_lease(lease);

exit:
    return result;
}

#ifdef HOST_TEST
void app_manager_builtin_host_fail_releases(size_t count)
{
    s_release_failures = count;
}
#endif

int app_manager_builtin_discover(void)
{
    int count = 0;
    esp_err_t result = ESP_OK;
    if (!s_registry_validated)
    {
        result = app_manager_builtin_registry_validate(SIZE_MAX);
    }
    if (result == ESP_OK && s_registry_app_count <= INT_MAX)
    {
        count = (int)s_registry_app_count;
    }
    return count;
}

const app_manager_app_desc_t *app_manager_builtin_list_open(void)
{
    return app_manager_builtin_enumerate(0);
}

const app_manager_app_desc_t *app_manager_builtin_list_get_next(
    const app_manager_app_desc_t *previous)
{
    const app_manager_app_desc_t *next = NULL;
    if (!s_registry_validated || previous == NULL)
    {
        goto exit;
    }
    for (size_t i = 0; i < s_registry_app_count; i++)
    {
        if (&s_registry_apps[i] == previous)
        {
            next = app_manager_builtin_enumerate(i + 1U);
            break;
        }
    }

exit:
    return next;
}
