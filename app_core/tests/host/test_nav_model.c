#include "app_manager_nav_model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void _page_handler(app_manager_msg_type_t message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static const app_manager_app_desc_t s_apps[APP_MANAGER_MAX_RESIDENT_APPS] =
{
    {.name = "a0", .id = "a0", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a1", .id = "a1", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a2", .id = "a2", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a3", .id = "a3", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a4", .id = "a4", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a5", .id = "a5", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a6", .id = "a6", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
    {.name = "a7", .id = "a7", .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN},
};

static const app_manager_page_desc_t s_root_pages[
    APP_MANAGER_MAX_RESIDENT_APPS] =
{
    {.app_id = "a0", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a1", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a2", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a3", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a4", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a5", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a6", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
    {.app_id = "a7", .page_id = "root", .handler = _page_handler, .memory_size = 16U},
};

static const app_manager_page_desc_t s_child_page =
{
    .app_id = "a0",
    .page_id = "child",
    .handler = _page_handler,
    .memory_size = 16U,
};

static uint32_t _random_next(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void _set_page_foreground(app_manager_page_node_t *page, bool active)
{
    page->started = true;
    page->active = active;
    page->mounted = active;
    page->screen = active ? (lv_obj_t *)(uintptr_t)(page->generation + 1U) :
                   NULL;
}

static void _foreground_app(app_manager_nav_model_t *model,
                            app_manager_app_node_t *app)
{
    if (model->active_app != NULL)
    {
        model->active_app->active = false;
        _set_page_foreground(model->active_app->current_page, false);
    }
    app_manager_nav_model_remove_app(model, app);
    app_manager_nav_model_push_app(model, app);
    app->active = true;
    _set_page_foreground(app->current_page, true);
}

static app_manager_app_node_t *_create_app(app_manager_nav_model_t *model,
        size_t descriptor_index)
{
    app_manager_app_node_t *app = app_manager_nav_model_alloc_app(model);
    if (app == NULL)
    {
        return NULL;
    }
    app_manager_page_node_t *page = app_manager_nav_model_alloc_page(model,
                                    16U);
    if (page == NULL)
    {
        app_manager_nav_model_free_app(app);
        return NULL;
    }
    app->descriptor = &s_apps[descriptor_index];
    page->descriptor = &s_root_pages[descriptor_index];
    page->started = true;
    memset(page->private_memory, 0xA5, page->private_memory_size);
    app_manager_nav_model_link_page(app, page);
    if (model->active_app != NULL)
    {
        model->active_app->active = false;
        _set_page_foreground(model->active_app->current_page, false);
    }
    app_manager_nav_model_push_app(model, app);
    app->active = true;
    _set_page_foreground(page, true);
    return app;
}

static void _remove_current_app(app_manager_nav_model_t *model)
{
    app_manager_app_node_t *app = model->active_app;
    if (app == NULL)
    {
        return;
    }
    while (app->current_page != NULL)
    {
        app_manager_page_node_t *page = app->current_page;
        app_manager_nav_model_unlink_page(app, page);
        app_manager_nav_model_free_page(model, page);
    }
    app_manager_nav_model_remove_app(model, app);
    app_manager_nav_model_free_app(app);
    if (model->active_app != NULL)
    {
        model->active_app->active = true;
        _set_page_foreground(model->active_app->current_page, true);
    }
}

static void _push_child(app_manager_nav_model_t *model)
{
    app_manager_app_node_t *app = model->active_app;
    if (app == NULL || app_manager_nav_model_find_page(app, "child") != NULL)
    {
        return;
    }
    app_manager_page_node_t *page = app_manager_nav_model_alloc_page(model,
                                    16U);
    if (page == NULL)
    {
        return;
    }
    _set_page_foreground(app->current_page, false);
    page->descriptor = &s_child_page;
    page->started = true;
    app_manager_nav_model_link_page(app, page);
    _set_page_foreground(page, true);
}

static void _pop_child(app_manager_nav_model_t *model)
{
    app_manager_app_node_t *app = model->active_app;
    if (app == NULL || app->current_page == NULL ||
            app->current_page->previous == NULL)
    {
        return;
    }
    app_manager_page_node_t *page = app->current_page;
    app_manager_nav_model_unlink_page(app, page);
    app_manager_nav_model_free_page(model, page);
    _set_page_foreground(app->current_page, true);
}

static void _test_random_model_commands(void)
{
    app_manager_nav_model_t model;
    assert(app_manager_nav_model_init(
               &model, APP_MANAGER_MAX_PAGE_INSTANCES * 64U) == ESP_OK);
    assert(app_manager_nav_model_invariants(&model));

    uint32_t random_state = UINT32_C(0x51F11A7E);
    for (size_t iteration = 0; iteration < 5000U; iteration++)
    {
        const uint32_t value = _random_next(&random_state);
        switch (value % 5U)
        {
        case 0:
        {
            const size_t index = (value >> 8) % APP_MANAGER_MAX_RESIDENT_APPS;
            app_manager_app_node_t *app = app_manager_nav_model_find_app(
                                              &model, s_apps[index].id);
            if (app == NULL)
            {
                (void)_create_app(&model, index);
            }
            else
            {
                _foreground_app(&model, app);
            }
            break;
        }
        case 1:
            _push_child(&model);
            break;
        case 2:
            _pop_child(&model);
            break;
        case 3:
            _remove_current_app(&model);
            break;
        case 4:
            if (model.active_app != NULL &&
                    model.active_app->previous != NULL)
            {
                _foreground_app(&model, model.active_app->previous);
            }
            break;
        default:
            assert(false);
            break;
        }
        assert(app_manager_nav_model_running_count(&model) <=
               APP_MANAGER_MAX_RESIDENT_APPS);
        if (!app_manager_nav_model_invariants(&model))
        {
            (void)fprintf(stderr, "invalid model at iteration=%zu op=%u\n",
                          iteration, (unsigned)(value % 5U));
            for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS;
                    index++)
            {
                const app_manager_app_node_t *app = &model.apps[index];
                if (app->in_use)
                {
                    (void)fprintf(stderr,
                                  "app[%zu] id=%s active=%d current=%td\n",
                                  index, app->descriptor->id, app->active,
                                  app->current_page - model.pages);
                }
            }
            for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES;
                    index++)
            {
                const app_manager_page_node_t *page = &model.pages[index];
                if (page->in_use)
                {
                    (void)fprintf(stderr,
                                  "page[%zu] id=%s active=%d mounted=%d screen=%p app=%p\n",
                                  index, page->descriptor->page_id,
                                  page->active, page->mounted,
                                  (void *)page->screen, (void *)page->app);
                }
            }
            assert(false);
        }
    }

    while (model.active_app != NULL)
    {
        _remove_current_app(&model);
    }
    assert(app_manager_nav_model_running_count(&model) == 0U);
    assert(app_manager_nav_model_invariants(&model));
    app_manager_nav_model_deinit(&model);
}

static void _test_memory_capacity_and_scrub(void)
{
    app_manager_nav_model_t model;
    assert(app_manager_nav_model_init(
               &model, APP_MANAGER_MAX_PAGE_INSTANCES * 64U) == ESP_OK);
    assert(app_manager_nav_model_alloc_page(&model, 65U) == NULL);

    app_manager_page_node_t *pages[APP_MANAGER_MAX_PAGE_INSTANCES];
    for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES; index++)
    {
        pages[index] = app_manager_nav_model_alloc_page(&model, 64U);
        assert(pages[index] != NULL);
    }
    assert(app_manager_nav_model_alloc_page(&model, 1U) == NULL);
    memset(pages[3]->private_memory, 0xFF, 64U);
    void *memory = pages[3]->private_memory;
    app_manager_nav_model_free_page(&model, pages[3]);
    pages[3] = app_manager_nav_model_alloc_page(&model, 64U);
    assert(pages[3] != NULL);
    assert(pages[3]->private_memory == memory);
    const uint8_t *bytes = pages[3]->private_memory;
    for (size_t index = 0; index < 64U; index++)
    {
        assert(bytes[index] == 0U);
    }
    app_manager_nav_model_deinit(&model);
}

static void _test_paused_and_cleanup_owned_nodes(void)
{
    app_manager_nav_model_t model;
    assert(app_manager_nav_model_init(
               &model, APP_MANAGER_MAX_PAGE_INSTANCES * 64U) == ESP_OK);
    app_manager_app_node_t *active = _create_app(&model, 0U);
    assert(active != NULL);
    _set_page_foreground(active->current_page, false);
    active->active = false;
    assert(app_manager_nav_model_invariants(&model));
    active->current_page->mounted = true;
    active->current_page->unmount_pending = true;
    active->current_page->screen = (lv_obj_t *)(uintptr_t)1U;
    assert(app_manager_nav_model_invariants(&model));
    active->current_page->started = false;
    assert(!app_manager_nav_model_invariants(&model));
    active->current_page->started = true;
    active->current_page->mounted = false;
    active->current_page->unmount_pending = false;
    active->current_page->screen = NULL;

    app_manager_page_node_t *detached = app_manager_nav_model_alloc_page(
                                            &model, 16U);
    assert(detached != NULL);
    detached->descriptor = &s_child_page;
    detached->app = active;
    detached->started = true;
    detached->cleanup_pending = true;
    assert(app_manager_nav_model_invariants(&model));
    detached->mounted = true;
    detached->screen = (lv_obj_t *)(uintptr_t)2U;
    assert(app_manager_nav_model_invariants(&model));
    detached->started = false;
    assert(!app_manager_nav_model_invariants(&model));
    detached->started = true;

    app_manager_app_node_t *orphan = app_manager_nav_model_alloc_app(&model);
    app_manager_page_node_t *orphan_page = app_manager_nav_model_alloc_page(
            &model, 16U);
    assert(orphan != NULL && orphan_page != NULL);
    orphan->descriptor = &s_apps[1];
    orphan->cleanup_pending = true;
    orphan_page->descriptor = &s_root_pages[1];
    orphan_page->started = true;
    app_manager_nav_model_link_page(orphan, orphan_page);
    assert(app_manager_nav_model_invariants(&model));
    orphan_page->mounted = true;
    orphan_page->screen = (lv_obj_t *)(uintptr_t)3U;
    assert(app_manager_nav_model_invariants(&model));

    app_manager_nav_model_free_page(&model, detached);
    app_manager_nav_model_unlink_page(orphan, orphan_page);
    app_manager_nav_model_free_page(&model, orphan_page);
    app_manager_nav_model_free_app(orphan);
    active->active = true;
    _set_page_foreground(active->current_page, true);
    active->current_page->unmount_pending = true;
    assert(!app_manager_nav_model_invariants(&model));
    active->current_page->unmount_pending = false;
    assert(app_manager_nav_model_invariants(&model));
    _remove_current_app(&model);
    app_manager_nav_model_deinit(&model);
}

int main(void)
{
    _test_random_model_commands();
    _test_memory_capacity_and_scrub();
    _test_paused_and_cleanup_owned_nodes();
    return 0;
}
