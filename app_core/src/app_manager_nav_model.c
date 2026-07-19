#ifndef HOST_TEST
    #include "esp_heap_caps.h"
#else
    #include <stdlib.h>
#endif

#include "app_manager_nav_model.h"

#include <string.h>

#define APP_MANAGER_DEFAULT_ARENA_BYTES (32U * 1024U)

esp_err_t app_manager_nav_model_init(app_manager_nav_model_t *model,
                                     size_t arena_bytes)
{
    if (model == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(model, 0, sizeof(*model));
    if (arena_bytes == 0)
    {
        arena_bytes = APP_MANAGER_DEFAULT_ARENA_BYTES;
    }
    model->page_stride =
        (arena_bytes / APP_MANAGER_MAX_PAGE_INSTANCES) & ~(size_t)7U;
    if (model->page_stride == 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }
#ifndef HOST_TEST
    model->page_arena = heap_caps_aligned_calloc(
                            8, 1,
                            model->page_stride * APP_MANAGER_MAX_PAGE_INSTANCES,
                            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (model->page_arena == NULL)
    {
        model->page_arena = heap_caps_aligned_calloc(
                                8, 1,
                                model->page_stride * APP_MANAGER_MAX_PAGE_INSTANCES,
                                MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
#else
    model->page_arena = calloc(1,
                               model->page_stride * APP_MANAGER_MAX_PAGE_INSTANCES);
#endif
    return model->page_arena != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_manager_nav_model_deinit(app_manager_nav_model_t *model)
{
    if (model == NULL)
    {
        return;
    }
#ifndef HOST_TEST
    heap_caps_free(model->page_arena);
#else
    free(model->page_arena);
#endif
    memset(model, 0, sizeof(*model));
}

app_manager_app_node_t *app_manager_nav_model_find_app(
    app_manager_nav_model_t *model, const char *app_id)
{
    if (model == NULL || app_id == NULL)
    {
        return NULL;
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        app_manager_app_node_t *app = &model->apps[index];
        if (app->in_use && !app->cleanup_pending && app->descriptor != NULL &&
                strcmp(app->descriptor->id, app_id) == 0)
        {
            return app;
        }
    }
    return NULL;
}

app_manager_page_node_t *app_manager_nav_model_find_page(
    app_manager_app_node_t *app, const char *page_id)
{
    if (app == NULL || page_id == NULL)
    {
        return NULL;
    }
    for (app_manager_page_node_t *page = app->current_page;
            page != NULL; page = page->previous)
    {
        if (!page->cleanup_pending && page->descriptor != NULL &&
                strcmp(page->descriptor->page_id, page_id) == 0)
        {
            return page;
        }
    }
    return NULL;
}

app_manager_app_node_t *app_manager_nav_model_alloc_app(
    app_manager_nav_model_t *model)
{
    if (model == NULL)
    {
        return NULL;
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        if (!model->apps[index].in_use)
        {
            memset(&model->apps[index], 0, sizeof(model->apps[index]));
            model->apps[index].in_use = true;
            return &model->apps[index];
        }
    }
    return NULL;
}

app_manager_page_node_t *app_manager_nav_model_alloc_page(
    app_manager_nav_model_t *model, size_t memory_size)
{
    if (model == NULL || memory_size > model->page_stride)
    {
        return NULL;
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES; index++)
    {
        if (!model->pages[index].in_use)
        {
            uint32_t generation = model->pages[index].generation + 1U;
            memset(&model->pages[index], 0, sizeof(model->pages[index]));
            model->pages[index].in_use = true;
            model->pages[index].generation = generation;
            model->pages[index].private_memory_size = memory_size;
            if (memory_size > 0)
            {
                model->pages[index].private_memory =
                    &model->page_arena[index * model->page_stride];
                memset(model->pages[index].private_memory, 0, memory_size);
            }
            return &model->pages[index];
        }
    }
    return NULL;
}

void app_manager_nav_model_free_app(app_manager_app_node_t *app)
{
    if (app != NULL)
    {
        memset(app, 0, sizeof(*app));
    }
}

void app_manager_nav_model_free_page(app_manager_nav_model_t *model,
                                     app_manager_page_node_t *page)
{
    if (model == NULL || page == NULL)
    {
        return;
    }
    size_t index = (size_t)(page - model->pages);
    uint32_t generation = page->generation;
    if (page->private_memory != NULL && page->private_memory_size > 0)
    {
        memset(&model->page_arena[index * model->page_stride], 0,
               page->private_memory_size);
    }
    memset(page, 0, sizeof(*page));
    page->generation = generation;
}

void app_manager_nav_model_push_app(app_manager_nav_model_t *model,
                                    app_manager_app_node_t *app)
{
    if (model == NULL || app == NULL)
    {
        return;
    }
    if (model->active_app != NULL)
    {
        model->active_app->next = app;
        app->previous = model->active_app;
    }
    app->next = NULL;
    model->active_app = app;
}

void app_manager_nav_model_remove_app(app_manager_nav_model_t *model,
                                      app_manager_app_node_t *app)
{
    if (model == NULL || app == NULL)
    {
        return;
    }
    if (app->previous != NULL)
    {
        app->previous->next = app->next;
    }
    if (app->next != NULL)
    {
        app->next->previous = app->previous;
    }
    if (model->active_app == app)
    {
        model->active_app = app->previous;
    }
    app->previous = NULL;
    app->next = NULL;
}

void app_manager_nav_model_link_page(app_manager_app_node_t *app,
                                     app_manager_page_node_t *page)
{
    if (app == NULL || page == NULL)
    {
        return;
    }
    page->app = app;
    page->previous = app->current_page;
    if (app->current_page != NULL)
    {
        app->current_page->next = page;
    }
    page->next = NULL;
    app->current_page = page;
}

void app_manager_nav_model_unlink_page(app_manager_app_node_t *app,
                                       app_manager_page_node_t *page)
{
    if (app == NULL || page == NULL)
    {
        return;
    }
    if (page->previous != NULL)
    {
        page->previous->next = page->next;
    }
    if (page->next != NULL)
    {
        page->next->previous = page->previous;
    }
    if (app->current_page == page)
    {
        app->current_page = page->previous;
    }
    page->previous = NULL;
    page->next = NULL;
}

uint32_t app_manager_nav_model_running_count(
    const app_manager_nav_model_t *model)
{
    uint32_t count = 0;
    if (model != NULL)
    {
        for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
        {
            count += model->apps[index].in_use ? 1U : 0U;
        }
    }
    return count;
}

bool app_manager_nav_model_invariants(const app_manager_nav_model_t *model)
{
    if (model == NULL)
    {
        return false;
    }

    bool valid = true;
    bool visited_apps[APP_MANAGER_MAX_RESIDENT_APPS] = {0};
    bool visited_pages[APP_MANAGER_MAX_PAGE_INSTANCES] = {0};
    size_t resident_count = 0;
    size_t linked_app_count = 0;
    size_t retained_page_count = 0;
    size_t linked_page_count = 0;
    size_t active_app_count = 0;
    size_t active_page_count = 0;
    size_t mounted_page_count = 0;
    size_t pending_mounted_page_count = 0;
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        const app_manager_app_node_t *app = &model->apps[index];
        if (!app->in_use)
        {
            continue;
        }
        resident_count++;
        valid = app->descriptor != NULL &&
                (app->current_page != NULL || app->cleanup_pending);
        if (!valid)
        {
            return false;
        }
        active_app_count += app->active ? 1U : 0U;
    }

    const app_manager_app_node_t *newer = NULL;
    for (const app_manager_app_node_t *app = model->active_app;
            app != NULL; app = app->previous)
    {
        ptrdiff_t index = app - model->apps;
        if (index < 0 || index >= APP_MANAGER_MAX_RESIDENT_APPS ||
                !app->in_use || visited_apps[index] || app->next != newer)
        {
            return false;
        }
        visited_apps[index] = true;
        linked_app_count++;
        newer = app;
    }
    if ((model->active_app == NULL && linked_app_count != 0U) ||
            (model->active_app != NULL && model->active_app->next != NULL))
    {
        return false;
    }

    for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES; index++)
    {
        const app_manager_page_node_t *page = &model->pages[index];
        if (!page->in_use)
        {
            continue;
        }
        retained_page_count++;
        valid = page->descriptor != NULL && page->app != NULL &&
                page->app >= model->apps &&
                page->app < model->apps + APP_MANAGER_MAX_RESIDENT_APPS &&
                page->app->in_use && (!page->mounted || page->screen != NULL) &&
                (!page->active || (page->started && page->mounted)) &&
                (!page->unmount_pending ||
                 (!page->active && page->started && page->screen != NULL)) &&
                (!page->cleanup_pending ||
                 (!page->active && (!page->mounted || page->started))) &&
                (!page->app->cleanup_pending ||
                 (!page->active && (!page->mounted || page->started)));
        if (!valid)
        {
            return false;
        }
        active_page_count += page->active ? 1U : 0U;
        mounted_page_count += page->mounted ? 1U : 0U;
        pending_mounted_page_count +=
            page->mounted &&
            (page->unmount_pending || page->cleanup_pending ||
             page->app->cleanup_pending) ? 1U : 0U;
    }

    for (size_t app_index = 0;
            app_index < APP_MANAGER_MAX_RESIDENT_APPS; app_index++)
    {
        const app_manager_app_node_t *app = &model->apps[app_index];
        if (!app->in_use)
        {
            continue;
        }
        const app_manager_page_node_t *newer_page = NULL;
        for (const app_manager_page_node_t *page = app->current_page;
                page != NULL; page = page->previous)
        {
            ptrdiff_t page_index = page - model->pages;
            if (page_index < 0 ||
                    page_index >= APP_MANAGER_MAX_PAGE_INSTANCES ||
                    !page->in_use || visited_pages[page_index] ||
                    page->app != app || page->next != newer_page)
            {
                return false;
            }
            visited_pages[page_index] = true;
            linked_page_count++;
            newer_page = page;
        }
        if (app->current_page != NULL && app->current_page->next != NULL)
        {
            return false;
        }
    }

    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        const app_manager_app_node_t *app = &model->apps[index];
        if (app->in_use && !visited_apps[index] &&
                (!app->cleanup_pending || app->active ||
                 app->previous != NULL || app->next != NULL))
        {
            return false;
        }
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES; index++)
    {
        const app_manager_page_node_t *page = &model->pages[index];
        if (page->in_use && !visited_pages[index] &&
                (!page->cleanup_pending || page->active ||
                 page->previous != NULL || page->next != NULL))
        {
            return false;
        }
    }

    valid = linked_app_count <= resident_count &&
            linked_page_count <= retained_page_count &&
            active_app_count <= 1U && active_page_count == active_app_count &&
            mounted_page_count ==
            active_page_count + pending_mounted_page_count;
    if (valid && active_app_count == 1U)
    {
        valid = model->active_app != NULL && model->active_app->active &&
                model->active_app->current_page != NULL &&
                model->active_app->current_page->active &&
                model->active_app->current_page->mounted;
    }
    else if (valid && active_app_count == 0U)
    {
        valid = active_page_count == 0U &&
                mounted_page_count == pending_mounted_page_count;
    }
    return valid;
}
