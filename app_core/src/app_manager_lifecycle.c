#define DBG_TAG "app_lifecycle"
#define DBG_LVL DBG_INFO

#ifndef HOST_TEST
    #include "mt_log.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
#else
    #include <stdio.h>
    #include <unistd.h>
    #define LOG_E(fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_W(fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_I(fmt, ...) fprintf(stderr, "[I][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
#endif

#include "app_manager.h"
#include "app_manager_builtin.h"
#include "app_manager_lifecycle.h"
#include "app_manager_lifecycle_internal.h"
#include "app_manager_mailbox.h"
#include "app_manager_nav_model.h"
#include "app_manager_presentation.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum lifecycle_screen_phase
{
    LIFECYCLE_SCREEN_CLOSED = 0,
    LIFECYCLE_SCREEN_INITIALIZING,
    LIFECYCLE_SCREEN_ACTIVE,
    LIFECYCLE_SCREEN_PAUSING,
    LIFECYCLE_SCREEN_PAUSED,
    LIFECYCLE_SCREEN_RESUMING,
} lifecycle_screen_phase_t;

typedef struct lifecycle_settings
{
    size_t max_resident_apps;
    app_manager_resident_policy_t resident_policy;
    app_manager_transition_t app_forward;
    app_manager_transition_t app_back;
    app_manager_transition_t page_forward;
    app_manager_transition_t page_back;
} lifecycle_settings_t;

typedef enum lifecycle_transition_kind
{
    LIFECYCLE_TRANSITION_FORWARD_PAGE = 0,
    LIFECYCLE_TRANSITION_FORWARD_APP,
    LIFECYCLE_TRANSITION_BACK_PAGE,
    LIFECYCLE_TRANSITION_BACK_APP,
} lifecycle_transition_kind_t;

typedef enum lifecycle_transition_phase
{
    LIFECYCLE_TRANSITION_PREPARING = 0,
    LIFECYCLE_TRANSITION_HANDOFF,
    LIFECYCLE_TRANSITION_ANIMATING,
    LIFECYCLE_TRANSITION_FINALIZING,
    LIFECYCLE_TRANSITION_ROLLING_BACK,
} lifecycle_transition_phase_t;

typedef struct lifecycle_transition_state
{
    bool in_use;
    bool target_app_created;
    bool target_page_created;
    bool source_paused;
    bool target_resumed;
    bool source_unmounted;
    bool source_stopped;
    bool provider_released;
    lifecycle_transition_kind_t kind;
    lifecycle_transition_phase_t phase;
    app_manager_app_node_t *source_app;
    app_manager_page_node_t *source_page;
    app_manager_app_node_t *target_app;
    app_manager_page_node_t *target_page;
    app_manager_lifecycle_nav_complete_t completion;
    void *completion_context;
} lifecycle_transition_state_t;

static app_manager_nav_model_t s_model;
static lifecycle_settings_t s_settings;
static lifecycle_transition_state_t s_transition;
static app_manager_page_node_t *s_handler_page;
static app_manager_msg_type_t s_handler_message;
static esp_err_t s_handler_cleanup_error;
static bool s_initialized;
static bool s_tearing_down;
static bool s_settings_configured;
static atomic_bool s_recovery_pending;
static app_manager_app_node_t *s_recovery_app;
static app_manager_page_node_t *s_recovery_page;
static atomic_bool s_target_recovery_pending;
static atomic_bool s_transitioning;
static atomic_int s_screen_phase = ATOMIC_VAR_INIT(LIFECYCLE_SCREEN_CLOSED);
static atomic_uint s_screen_accesses;
static atomic_uint s_paused_running_apps;

#ifdef HOST_TEST
    static _Atomic(app_manager_lifecycle_observer_t) s_host_observer;
#endif

static const char *const s_lifecycle_message_names[APP_MANAGER_MSG_USER_END] =
{
    [APP_MANAGER_MSG_ONSTART] = "START",
    [APP_MANAGER_MSG_ONMOUNT] = "MOUNT",
    [APP_MANAGER_MSG_ONRESUME] = "RESUME",
    [APP_MANAGER_MSG_ONPAUSE] = "PAUSE",
    [APP_MANAGER_MSG_ONUNMOUNT] = "UNMOUNT",
    [APP_MANAGER_MSG_ONSTOP] = "STOP",
    [APP_MANAGER_MSG_ONNEWINTENT] = "NEW_INTENT",
};

static bool _lifecycle_topology_query_allowed(void);

static app_manager_transition_t _lifecycle_default_transition(
    app_manager_transition_effect_t effect)
{
    app_manager_transition_t transition =
    {
        .effect = effect,
        .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        .reserved = 0,
    };
    return transition;
}

static void _lifecycle_settings_defaults(lifecycle_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->max_resident_apps = APP_MANAGER_MAX_RESIDENT_APPS;
    settings->resident_policy = APP_MANAGER_RESIDENT_REJECT;
    settings->app_forward = _lifecycle_default_transition(
                                APP_MANAGER_TRANSITION_FADE);
    settings->app_back = _lifecycle_default_transition(
                             APP_MANAGER_TRANSITION_FADE);
    settings->page_forward = _lifecycle_default_transition(
                                 APP_MANAGER_TRANSITION_PUSH_LEFT);
    settings->page_back = _lifecycle_default_transition(
                              APP_MANAGER_TRANSITION_PUSH_RIGHT);
}

static bool _lifecycle_transition_value_valid(
    const app_manager_transition_t *transition)
{
    return transition != NULL && transition->reserved == 0U &&
           transition->effect >= APP_MANAGER_TRANSITION_DEFAULT &&
           transition->effect < APP_MANAGER_TRANSITION_END &&
           transition->duration_ms <= APP_MANAGER_TRANSITION_MAX_DURATION_MS;
}

static app_manager_transition_t _lifecycle_resolve_config_transition(
    const app_manager_transition_t *configured,
    app_manager_transition_effect_t fallback_effect)
{
    app_manager_transition_t resolved = *configured;
    if (resolved.effect == APP_MANAGER_TRANSITION_DEFAULT)
    {
        resolved.effect = fallback_effect;
    }
    if (resolved.duration_ms == 0U)
    {
        resolved.duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS;
    }
    if (resolved.effect == APP_MANAGER_TRANSITION_NONE)
    {
        resolved.duration_ms = 0U;
    }
    return resolved;
}

esp_err_t app_manager_lifecycle_configure(
    size_t max_resident_apps,
    app_manager_resident_policy_t resident_policy,
    const app_manager_transition_t *app_forward,
    const app_manager_transition_t *app_back,
    const app_manager_transition_t *page_forward,
    const app_manager_transition_t *page_back)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    lifecycle_settings_t settings;
    app_manager_transition_t default_app_forward = {0};
    app_manager_transition_t default_app_back = {0};
    app_manager_transition_t default_page_forward = {0};
    app_manager_transition_t default_page_back = {0};

    if (s_initialized ||
            atomic_load_explicit(&s_screen_phase, memory_order_acquire) !=
            LIFECYCLE_SCREEN_CLOSED)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (max_resident_apps > APP_MANAGER_MAX_RESIDENT_APPS ||
            resident_policy < APP_MANAGER_RESIDENT_REJECT ||
            resident_policy > APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND)
    {
        goto exit;
    }
    app_forward = app_forward != NULL ? app_forward : &default_app_forward;
    app_back = app_back != NULL ? app_back : &default_app_back;
    page_forward = page_forward != NULL ?
                   page_forward : &default_page_forward;
    page_back = page_back != NULL ? page_back : &default_page_back;
    if (!_lifecycle_transition_value_valid(app_forward) ||
            !_lifecycle_transition_value_valid(app_back) ||
            !_lifecycle_transition_value_valid(page_forward) ||
            !_lifecycle_transition_value_valid(page_back))
    {
        goto exit;
    }

    _lifecycle_settings_defaults(&settings);
    settings.max_resident_apps = max_resident_apps == 0U ?
                                 APP_MANAGER_MAX_RESIDENT_APPS :
                                 max_resident_apps;
    settings.resident_policy = resident_policy;
    settings.app_forward = _lifecycle_resolve_config_transition(
                               app_forward, APP_MANAGER_TRANSITION_FADE);
    settings.app_back = _lifecycle_resolve_config_transition(
                            app_back, APP_MANAGER_TRANSITION_FADE);
    settings.page_forward = _lifecycle_resolve_config_transition(
                                page_forward,
                                APP_MANAGER_TRANSITION_PUSH_LEFT);
    settings.page_back = _lifecycle_resolve_config_transition(
                             page_back,
                             APP_MANAGER_TRANSITION_PUSH_RIGHT);
    s_settings = settings;
    s_settings_configured = true;
    result = ESP_OK;

exit:
    return result;
}

bool app_manager_lifecycle_id_is_valid(const char *id)
{
    return id != NULL && id[0] != '\0' &&
           strnlen(id, APP_MANAGER_ID_BYTES) < APP_MANAGER_ID_BYTES;
}

static const char *_lifecycle_callback_app_id(void)
{
    const char *app_id = NULL;
    if (s_handler_page != NULL && s_handler_page->app != NULL &&
            s_handler_page->app->descriptor != NULL)
    {
        app_id = s_handler_page->app->descriptor->id;
    }
    return app_id;
}

const char *app_manager_lifecycle_callback_app_id(void)
{
    return _lifecycle_callback_app_id();
}

static esp_err_t _lifecycle_call_handler(app_manager_page_node_t *page,
        app_manager_msg_type_t message)
{
    esp_err_t result = ESP_OK;
    app_manager_page_node_t *previous_page;
    app_manager_msg_type_t previous_message;
    esp_err_t previous_cleanup_error;
    esp_err_t cleanup_error;
    uint32_t generation;

    if (page == NULL || !page->in_use || page->descriptor == NULL ||
            page->descriptor->handler == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    previous_page = s_handler_page;
    previous_message = s_handler_message;
    previous_cleanup_error = s_handler_cleanup_error;
    generation = page->generation;
    s_handler_page = page;
    s_handler_message = message;
    s_handler_cleanup_error = ESP_OK;
#ifdef HOST_TEST
    app_manager_lifecycle_observer_t observer = atomic_load(&s_host_observer);
    if (observer != NULL)
    {
        observer(page->app->descriptor->id, page->descriptor->page_id,
                 message, APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE);
    }
#endif
    LOG_I("[%s][%s] ON %s", page->app->descriptor->name,
          page->descriptor->page_id, s_lifecycle_message_names[message]);
    page->descriptor->handler(message, page->user_data);
#ifdef HOST_TEST
    if (observer != NULL)
    {
        observer(page->app->descriptor->id, page->descriptor->page_id,
                 message, APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    }
#endif
    cleanup_error = s_handler_cleanup_error;
    s_handler_page = previous_page;
    s_handler_message = previous_message;
    s_handler_cleanup_error = previous_cleanup_error;

    if (!page->in_use || page->generation != generation)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if ((message == APP_MANAGER_MSG_ONPAUSE ||
            message == APP_MANAGER_MSG_ONSTOP) && cleanup_error != ESP_OK)
    {
        result = cleanup_error;
        goto exit;
    }
    switch (message)
    {
    case APP_MANAGER_MSG_ONSTART:
        page->started = true;
        break;
    case APP_MANAGER_MSG_ONMOUNT:
        page->mounted = true;
        break;
    case APP_MANAGER_MSG_ONRESUME:
        page->active = true;
        break;
    case APP_MANAGER_MSG_ONPAUSE:
        page->active = false;
        break;
    case APP_MANAGER_MSG_ONUNMOUNT:
        page->mounted = false;
        break;
    case APP_MANAGER_MSG_ONSTOP:
        page->started = false;
        break;
    case APP_MANAGER_MSG_ONNEWINTENT:
    case APP_MANAGER_MSG_USER_END:
    default:
        break;
    }

exit:
    return result;
}

static esp_err_t _lifecycle_start_page(app_manager_page_node_t *page)
{
    esp_err_t result = ESP_OK;
    if (page->started)
    {
        goto exit;
    }
    if (page->private_memory != NULL && page->private_memory_size > 0U)
    {
        memset(page->private_memory, 0, page->private_memory_size);
    }
    result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTART);

exit:
    return result;
}

static esp_err_t _lifecycle_mount_page(app_manager_page_node_t *page)
{
    esp_err_t result = ESP_OK;
    bool screen_created = false;

    if (page == NULL)
    {
        goto exit;
    }
    if (page->mounted)
    {
        goto exit;
    }
    if (!page->started)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (page->screen == NULL)
    {
        page->screen = app_manager_presentation_create_page_screen();
        if (page->screen == NULL)
        {
            result = ESP_ERR_NO_MEM;
            goto exit;
        }
        screen_created = true;
    }
    result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONMOUNT);
    if (result != ESP_OK && screen_created)
    {
        (void)app_manager_presentation_delete_page_screen(page->screen);
        page->screen = NULL;
    }

exit:
    return result;
}

static esp_err_t _lifecycle_delete_unmounted_screen(
    app_manager_page_node_t *page)
{
    esp_err_t result = ESP_OK;
    if (page == NULL || page->screen == NULL)
    {
        goto exit;
    }
    if (page->mounted)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = app_manager_presentation_delete_page_screen(page->screen);
    if (result == ESP_OK)
    {
        page->screen = NULL;
    }

exit:
    return result;
}

static esp_err_t _lifecycle_unmount_page(app_manager_page_node_t *page,
        bool delete_screen)
{
    esp_err_t result = ESP_OK;
    if (page == NULL || !page->mounted)
    {
        goto exit;
    }
    if (page->active)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONUNMOUNT);
    if (result == ESP_OK && delete_screen)
    {
        result = _lifecycle_delete_unmounted_screen(page);
    }

exit:
    return result;
}

static esp_err_t _lifecycle_pause_page(app_manager_page_node_t *page)
{
    return page == NULL || !page->active ? ESP_OK :
           _lifecycle_call_handler(page, APP_MANAGER_MSG_ONPAUSE);
}

static esp_err_t _lifecycle_resume_page(app_manager_page_node_t *page)
{
    return page == NULL ? ESP_OK :
           _lifecycle_call_handler(page, APP_MANAGER_MSG_ONRESUME);
}

static esp_err_t _lifecycle_stop_page(app_manager_page_node_t *page)
{
    esp_err_t result = ESP_OK;
    if (page == NULL)
    {
        goto exit;
    }
    result = _lifecycle_pause_page(page);
    if (result != ESP_OK)
    {
        goto exit;
    }
    if (page->mounted)
    {
        result = _lifecycle_unmount_page(page, true);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (page->started)
    {
        result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTOP);
    }

exit:
    return result;
}

static bool _lifecycle_page_is_linked(
    const app_manager_app_node_t *app,
    const app_manager_page_node_t *page)
{
    bool linked = false;
    if (app == NULL || page == NULL)
    {
        goto exit;
    }
    for (const app_manager_page_node_t *node = app->current_page;
            node != NULL; node = node->previous)
    {
        if (node == page)
        {
            linked = true;
            break;
        }
    }

exit:
    return linked;
}

static esp_err_t _lifecycle_free_page(app_manager_page_node_t *page)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (page == NULL || page->active || page->mounted || page->started)
    {
        goto exit;
    }
    app_manager_app_node_t *app = page->app;
    if (page->screen != NULL)
    {
        result = _lifecycle_delete_unmounted_screen(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (_lifecycle_page_is_linked(app, page))
    {
        app_manager_nav_model_unlink_page(app, page);
    }
    app_manager_nav_model_free_page(&s_model, page);
    result = ESP_OK;

exit:
    return result;
}

static bool _lifecycle_app_is_linked(const app_manager_app_node_t *app)
{
    bool linked = false;
    for (app_manager_app_node_t *node = s_model.active_app;
            node != NULL; node = node->previous)
    {
        if (node == app)
        {
            linked = true;
            break;
        }
    }
    return linked;
}

static void _lifecycle_activate_app(app_manager_app_node_t *app)
{
    app_manager_app_node_t *old_active = s_model.active_app;
    if (old_active != NULL && old_active != app)
    {
        old_active->active = false;
    }
    if (_lifecycle_app_is_linked(app) && s_model.active_app != app)
    {
        app_manager_nav_model_remove_app(&s_model, app);
    }
    if (s_model.active_app != app)
    {
        app_manager_nav_model_push_app(&s_model, app);
    }
    app->active = true;
}

static esp_err_t _lifecycle_release_and_free_app(app_manager_app_node_t *app)
{
    esp_err_t result = ESP_OK;
    for (app_manager_page_node_t *page = app->current_page;
            page != NULL; page = page->previous)
    {
        if (page->active || page->mounted || page->started)
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
        if (page->screen != NULL)
        {
            result = _lifecycle_delete_unmounted_screen(page);
            if (result != ESP_OK)
            {
                goto exit;
            }
        }
    }
    result = app_manager_builtin_release(&app->lease);
    if (result != ESP_OK)
    {
        goto exit;
    }
    while (app->current_page != NULL)
    {
        app_manager_page_node_t *page = app->current_page;
        app_manager_nav_model_unlink_page(app, page);
        app_manager_nav_model_free_page(&s_model, page);
    }
    if (_lifecycle_app_is_linked(app))
    {
        app_manager_nav_model_remove_app(&s_model, app);
    }
    app_manager_nav_model_free_app(app);

exit:
    return result;
}

static esp_err_t _lifecycle_stop_all_app_pages(app_manager_app_node_t *app)
{
    esp_err_t result = ESP_OK;
    for (app_manager_page_node_t *page = app->current_page;
            page != NULL; page = page->previous)
    {
        result = _lifecycle_stop_page(page);
        if (result != ESP_OK)
        {
            break;
        }
    }
    return result;
}

static esp_err_t _lifecycle_destroy_background_app(app_manager_app_node_t *app)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (app == NULL || app->active)
    {
        goto exit;
    }
    result = _lifecycle_stop_all_app_pages(app);
    if (result == ESP_OK)
    {
        result = _lifecycle_release_and_free_app(app);
    }

exit:
    return result;
}

static esp_err_t _lifecycle_retry_pending_cleanup(void)
{
    esp_err_t result = ESP_OK;
    for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES; index++)
    {
        app_manager_page_node_t *page = &s_model.pages[index];
        if (page->in_use && page->unmount_pending)
        {
            result = page->mounted ?
                     _lifecycle_unmount_page(page, true) :
                     _lifecycle_delete_unmounted_screen(page);
            if (result != ESP_OK)
            {
                goto exit;
            }
            page->unmount_pending = false;
        }
        if (!page->in_use || !page->cleanup_pending)
        {
            continue;
        }
        result = _lifecycle_stop_page(page);
        if (result == ESP_OK)
        {
            result = _lifecycle_free_page(page);
        }
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        app_manager_app_node_t *app = &s_model.apps[index];
        if (!app->in_use || !app->cleanup_pending)
        {
            continue;
        }
        result = _lifecycle_destroy_background_app(app);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
exit:
    return result;
}

static app_manager_app_node_t *_lifecycle_oldest_evictable_app(void)
{
    app_manager_app_node_t *oldest = s_model.active_app;
    while (oldest != NULL && oldest->previous != NULL)
    {
        oldest = oldest->previous;
    }
    for (app_manager_app_node_t *app = oldest;
            app != NULL; app = app->next)
    {
        if (!app->active && app->descriptor != NULL &&
                (app->descriptor->flags & APP_MANAGER_APP_FLAG_PINNED) == 0U)
        {
            return app;
        }
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        app_manager_app_node_t *app = &s_model.apps[index];
        if (app->in_use && !app->active && !_lifecycle_app_is_linked(app) &&
                app->descriptor != NULL &&
                (app->descriptor->flags & APP_MANAGER_APP_FLAG_PINNED) == 0U)
        {
            return app;
        }
    }
    return NULL;
}

static esp_err_t _lifecycle_make_resident_space(void)
{
    esp_err_t result = ESP_OK;
    if (app_manager_nav_model_running_count(&s_model) <
            s_settings.max_resident_apps)
    {
        goto exit;
    }
    if (s_settings.resident_policy !=
            APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    app_manager_app_node_t *candidate = _lifecycle_oldest_evictable_app();
    if (candidate == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    result = _lifecycle_destroy_background_app(candidate);

exit:
    return result;
}

static void _lifecycle_copy_arguments(app_manager_app_node_t *app,
                                      app_manager_page_node_t *page,
                                      const app_manager_nav_request_t *request)
{
    app->has_arguments = request->has_arguments;
    page->has_arguments = request->has_arguments;
    if (request->has_arguments)
    {
        app->arguments = request->arguments;
        page->arguments = request->arguments;
    }
    else
    {
        memset(&app->arguments, 0, sizeof(app->arguments));
        memset(&page->arguments, 0, sizeof(page->arguments));
    }
}

static esp_err_t _lifecycle_create_page(app_manager_app_node_t *app,
                                        const app_manager_page_desc_t *descriptor,
                                        const app_manager_nav_request_t *request,
                                        bool link_page,
                                        app_manager_page_node_t **out_page)
{
    esp_err_t result = ESP_ERR_NO_MEM;
    app_manager_page_node_t *page = app_manager_nav_model_alloc_page(
                                        &s_model, descriptor->memory_size);
    if (page == NULL)
    {
        goto exit;
    }
    page->descriptor = descriptor;
    page->user_data = descriptor->user_data;
    page->app = app;
    if (link_page)
    {
        app_manager_nav_model_link_page(app, page);
    }
    _lifecycle_copy_arguments(app, page, request);
    result = _lifecycle_start_page(page);
    if (result != ESP_OK)
    {
        (void)_lifecycle_free_page(page);
        goto exit;
    }
    *out_page = page;

exit:
    return result;
}

static esp_err_t _lifecycle_create_app(const char *app_id,
                                       const char *target_page_id, const app_manager_nav_request_t *request,
                                       app_manager_app_node_t **out_app,
                                       app_manager_page_node_t **out_page)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;
    app_manager_app_node_t *app = NULL;
    app_manager_page_node_t *root = NULL;
    app_manager_page_node_t *target = NULL;

    if (app_manager_builtin_find_by_id(app_id) == NULL)
    {
        goto exit;
    }
    result = _lifecycle_make_resident_space();
    if (result != ESP_OK)
    {
        goto exit;
    }
    app = app_manager_nav_model_alloc_app(&s_model);
    if (app == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    result = app_manager_builtin_acquire(app_id, &app->lease);
    if (result != ESP_OK)
    {
        app_manager_nav_model_free_app(app);
        goto exit;
    }
    app->descriptor = app_manager_builtin_lease_app(&app->lease);
    if (app->descriptor == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto fail_app;
    }
    const app_manager_page_desc_t *root_descriptor =
        app_manager_builtin_lease_find_page(&app->lease,
                                            app->descriptor->root_page_id);
    if (root_descriptor == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto fail_app;
    }
    result = _lifecycle_create_page(app, root_descriptor, request, true,
                                    &root);
    if (result != ESP_OK)
    {
        goto fail_app;
    }
    target = root;
    if (target_page_id != NULL &&
            strcmp(target_page_id, root_descriptor->page_id) != 0)
    {
        const app_manager_page_desc_t *target_descriptor =
            app_manager_builtin_lease_find_page(&app->lease, target_page_id);
        if (target_descriptor == NULL)
        {
            result = ESP_ERR_NOT_FOUND;
            goto fail_pages;
        }
        result = _lifecycle_create_page(app, target_descriptor, request, true,
                                        &target);
        if (result != ESP_OK)
        {
            goto fail_pages;
        }
    }
    *out_app = app;
    *out_page = target;
    goto exit;

fail_pages:
    (void)_lifecycle_stop_all_app_pages(app);
    while (app->current_page != NULL && !app->current_page->started)
    {
        if (_lifecycle_free_page(app->current_page) != ESP_OK)
        {
            break;
        }
    }
    if (app->current_page != NULL)
    {
        app->cleanup_pending = true;
    }
fail_app:
    if (app->current_page == NULL)
    {
        const esp_err_t release_result = app_manager_builtin_release(
                                             &app->lease);
        if (release_result == ESP_OK)
        {
            app_manager_nav_model_free_app(app);
        }
        else
        {
            app->cleanup_pending = true;
        }
    }
    goto exit;

exit:
    return result;
}

static void _lifecycle_update_intent(app_manager_app_node_t *app,
                                     app_manager_page_node_t *page,
                                     const app_manager_nav_request_t *request)
{
    _lifecycle_copy_arguments(app, page, request);
    (void)_lifecycle_call_handler(page, APP_MANAGER_MSG_ONNEWINTENT);
}

static esp_err_t _lifecycle_trim_between(app_manager_app_node_t *app,
        app_manager_page_node_t *source, app_manager_page_node_t *target)
{
    esp_err_t result = ESP_OK;
    app_manager_page_node_t *page = source != NULL ?
                                    source->previous : app->current_page;
    while (page != NULL && page != target)
    {
        app_manager_page_node_t *previous = page->previous;
        result = _lifecycle_stop_page(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
        result = _lifecycle_free_page(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
        page = previous;
    }
    if (page != target)
    {
        result = ESP_ERR_NOT_FOUND;
    }

exit:
    return result;
}

static esp_err_t _lifecycle_trim_to_background_target(
    app_manager_app_node_t *app, app_manager_page_node_t *target)
{
    esp_err_t result = ESP_OK;
    while (app->current_page != NULL && app->current_page != target)
    {
        app_manager_page_node_t *page = app->current_page;
        result = _lifecycle_stop_page(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
        result = _lifecycle_free_page(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (app->current_page != target)
    {
        result = ESP_ERR_NOT_FOUND;
    }

exit:
    return result;
}

static app_manager_transition_t _lifecycle_resolve_request_transition(
    const app_manager_nav_request_t *request,
    const app_manager_transition_t *configured)
{
    app_manager_transition_t resolved = request->transition;
    if (resolved.effect == APP_MANAGER_TRANSITION_DEFAULT)
    {
        resolved.effect = configured->effect;
    }
    if (resolved.duration_ms == 0U)
    {
        resolved.duration_ms = configured->duration_ms;
    }
    if (resolved.effect == APP_MANAGER_TRANSITION_NONE)
    {
        resolved.duration_ms = 0U;
    }
    return resolved;
}

static void _lifecycle_complete_transition(esp_err_t result)
{
    app_manager_lifecycle_nav_complete_t completion = s_transition.completion;
    void *context = s_transition.completion_context;
    memset(&s_transition, 0, sizeof(s_transition));
    atomic_store_explicit(&s_transitioning, false, memory_order_release);
    if (completion != NULL)
    {
        completion(result, context);
    }
}

static esp_err_t _lifecycle_rollback_target(void)
{
    esp_err_t result = ESP_OK;
    app_manager_page_node_t *target_page = s_transition.target_page;
    app_manager_app_node_t *target_app = s_transition.target_app;

    if (target_page != NULL && target_page->mounted)
    {
        result = _lifecycle_unmount_page(target_page, true);
    }
    if (result != ESP_OK && target_page != NULL && target_page->in_use &&
            !s_transition.target_app_created &&
            !s_transition.target_page_created)
    {
        target_page->unmount_pending = true;
    }
    if (s_transition.target_app_created && target_app != NULL)
    {
        target_app->active = false;
        esp_err_t cleanup_result = _lifecycle_destroy_background_app(target_app);
        if (cleanup_result != ESP_OK && target_app->in_use)
        {
            target_app->cleanup_pending = true;
        }
        if (result == ESP_OK)
        {
            result = cleanup_result;
        }
    }
    else if (s_transition.target_page_created && target_page != NULL &&
             target_page->in_use)
    {
        esp_err_t cleanup_result = _lifecycle_stop_page(target_page);
        if (cleanup_result == ESP_OK)
        {
            cleanup_result = _lifecycle_free_page(target_page);
        }
        if (cleanup_result != ESP_OK && target_page->in_use)
        {
            target_page->cleanup_pending = true;
        }
        if (result == ESP_OK)
        {
            result = cleanup_result;
        }
    }
    return result;
}

static void _lifecycle_commit_target(void)
{
    if ((s_transition.kind == LIFECYCLE_TRANSITION_FORWARD_PAGE ||
            s_transition.kind == LIFECYCLE_TRANSITION_FORWARD_APP) &&
            s_transition.target_page_created &&
            !s_transition.target_app_created &&
            app_manager_nav_model_find_page(
                s_transition.target_app,
                s_transition.target_page->descriptor->page_id) !=
            s_transition.target_page)
    {
        s_transition.target_page->cleanup_pending = false;
        app_manager_nav_model_link_page(s_transition.target_app,
                                        s_transition.target_page);
    }

    if (s_transition.target_app != NULL)
    {
        _lifecycle_activate_app(s_transition.target_app);
    }
    else
    {
        s_model.active_app = NULL;
    }
}

static void _lifecycle_defer_source_cleanup(void)
{
    app_manager_page_node_t *source_page = s_transition.source_page;
    app_manager_app_node_t *source_app = s_transition.source_app;

    switch (s_transition.kind)
    {
    case LIFECYCLE_TRANSITION_FORWARD_PAGE:
    case LIFECYCLE_TRANSITION_FORWARD_APP:
        if (source_page != NULL && source_page->in_use)
        {
            source_page->active = false;
            source_page->unmount_pending = true;
        }
        if (source_app != NULL && source_app != s_transition.target_app)
        {
            source_app->active = false;
        }
        break;
    case LIFECYCLE_TRANSITION_BACK_PAGE:
        if (source_page != NULL && source_page->in_use)
        {
            source_page->active = false;
            if (_lifecycle_page_is_linked(source_app, source_page))
            {
                app_manager_nav_model_unlink_page(source_app, source_page);
            }
            source_page->cleanup_pending = true;
        }
        break;
    case LIFECYCLE_TRANSITION_BACK_APP:
        if (source_app != NULL && source_app->in_use)
        {
            source_app->active = false;
            if (_lifecycle_app_is_linked(source_app))
            {
                app_manager_nav_model_remove_app(&s_model, source_app);
            }
            source_app->cleanup_pending = true;
        }
        break;
    default:
        break;
    }
}

static esp_err_t _lifecycle_pause_transition_target(void)
{
    esp_err_t result = ESP_OK;
    app_manager_page_node_t *target = s_transition.target_page;

    if (target != NULL && target->active)
    {
        result = _lifecycle_pause_page(target);
    }
    if (result == ESP_OK)
    {
        s_transition.target_resumed = false;
    }
    return result;
}

static esp_err_t _lifecycle_stabilize_target(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_page_node_t *target = s_transition.target_page;

    if (target == NULL || !target->in_use)
    {
        goto exit;
    }
    result = _lifecycle_start_page(target);
    if (result == ESP_OK)
    {
        result = _lifecycle_mount_page(target);
    }
    if (result == ESP_OK && !target->active)
    {
        result = _lifecycle_resume_page(target);
    }
    if (result == ESP_OK)
    {
        s_transition.target_resumed = true;
        result = app_manager_presentation_load_immediate(target->screen);
    }

exit:
    return result;
}

static void _lifecycle_keep_target_foreground(void)
{
    esp_err_t result = _lifecycle_stabilize_target();

    _lifecycle_defer_source_cleanup();
    _lifecycle_commit_target();
    if (result != ESP_OK)
    {
        (void)app_manager_presentation_load_immediate(
            app_manager_presentation_neutral_screen());
        atomic_store_explicit(&s_target_recovery_pending, true,
                              memory_order_release);
    }
}

static esp_err_t _lifecycle_restore_source(void)
{
    esp_err_t result = ESP_OK;
    app_manager_page_node_t *source = s_transition.source_page;
    if (source == NULL)
    {
        result = app_manager_presentation_load_immediate(
                     app_manager_presentation_neutral_screen());
        goto exit;
    }
    result = _lifecycle_start_page(source);
    if (result == ESP_OK)
    {
        result = _lifecycle_mount_page(source);
    }
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = app_manager_presentation_load_immediate(source->screen);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _lifecycle_resume_page(source);
    if (result == ESP_OK && s_transition.source_app != NULL)
    {
        s_transition.source_app->active = true;
        s_transition.source_paused = false;
    }

exit:
    return result;
}

static void _lifecycle_mark_source_recovery(void)
{
    app_manager_page_node_t *source = s_transition.source_page;
    (void)app_manager_presentation_load_immediate(
        app_manager_presentation_neutral_screen());
    if (source != NULL)
    {
        source->active = false;
        if (source->mounted)
        {
            (void)_lifecycle_unmount_page(source, true);
        }
    }
    if (s_transition.source_app != NULL)
    {
        s_transition.source_app->active = false;
    }
    (void)_lifecycle_rollback_target();
    s_recovery_pending = source != NULL;
    s_recovery_app = s_transition.source_app;
    s_recovery_page = source;
}

static void _lifecycle_rollback_to_source(void)
{
    const bool target_was_resumed = s_transition.target_resumed ||
                                    (s_transition.target_page != NULL &&
                                     s_transition.target_page->active);

    s_transition.phase = LIFECYCLE_TRANSITION_ROLLING_BACK;
    if (_lifecycle_pause_transition_target() != ESP_OK)
    {
        _lifecycle_keep_target_foreground();
    }
    else if (_lifecycle_restore_source() == ESP_OK)
    {
        (void)_lifecycle_rollback_target();
    }
    else if (target_was_resumed)
    {
        _lifecycle_keep_target_foreground();
    }
    else
    {
        _lifecycle_mark_source_recovery();
    }
}

static esp_err_t _lifecycle_retry_target_recovery(void)
{
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_target_recovery_pending,
                              memory_order_acquire))
    {
        goto exit;
    }

    app_manager_app_node_t *app = s_model.active_app;
    app_manager_page_node_t *page = app != NULL ? app->current_page : NULL;
    if (app == NULL || page == NULL || !app->in_use || !page->in_use ||
            app->descriptor == NULL || page->descriptor == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = _lifecycle_start_page(page);
    if (result == ESP_OK)
    {
        result = _lifecycle_mount_page(page);
    }
    if (result == ESP_OK && !page->active)
    {
        result = _lifecycle_resume_page(page);
    }
    if (result == ESP_OK)
    {
        app->active = true;
        result = app_manager_presentation_load_immediate(page->screen);
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_target_recovery_pending, false,
                              memory_order_release);
    }

exit:
    return result;
}

static esp_err_t _lifecycle_retry_source_recovery(void)
{
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_recovery_pending, memory_order_acquire))
    {
        goto exit;
    }
    if (s_recovery_app == NULL || s_recovery_page == NULL ||
            !s_recovery_app->in_use || !s_recovery_page->in_use)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = _lifecycle_start_page(s_recovery_page);
    if (result == ESP_OK)
    {
        result = _lifecycle_mount_page(s_recovery_page);
    }
    if (result == ESP_OK)
    {
        result = app_manager_presentation_load_immediate(
                     s_recovery_page->screen);
    }
    if (result == ESP_OK)
    {
        result = _lifecycle_resume_page(s_recovery_page);
    }
    if (result == ESP_OK)
    {
        s_recovery_app->active = true;
        s_recovery_pending = false;
        s_recovery_app = NULL;
        s_recovery_page = NULL;
    }
    else
    {
        (void)app_manager_presentation_load_immediate(
            app_manager_presentation_neutral_screen());
        if (s_recovery_page->mounted)
        {
            (void)_lifecycle_unmount_page(s_recovery_page, true);
        }
    }

exit:
    return result;
}

static esp_err_t _lifecycle_finalize_forward(void)
{
    esp_err_t result = ESP_OK;
    s_transition.phase = LIFECYCLE_TRANSITION_FINALIZING;
    if (s_transition.source_page != NULL)
    {
        result = _lifecycle_unmount_page(s_transition.source_page, true);
        if (result != ESP_OK)
        {
            goto rollback;
        }
        s_transition.source_unmounted = true;
    }
    _lifecycle_commit_target();
    goto exit;

rollback:
    _lifecycle_rollback_to_source();

exit:
    return result;
}

static esp_err_t _lifecycle_finalize_back_page(void)
{
    s_transition.phase = LIFECYCLE_TRANSITION_FINALIZING;
    esp_err_t result = _lifecycle_unmount_page(s_transition.source_page, true);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    s_transition.source_unmounted = true;
    result = _lifecycle_call_handler(s_transition.source_page,
                                     APP_MANAGER_MSG_ONSTOP);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    s_transition.source_stopped = true;
    result = _lifecycle_free_page(s_transition.source_page);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    _lifecycle_commit_target();
    goto exit;

rollback:
    _lifecycle_rollback_to_source();

exit:
    return result;
}

static esp_err_t _lifecycle_finalize_back_app(void)
{
    s_transition.phase = LIFECYCLE_TRANSITION_FINALIZING;
    esp_err_t result = _lifecycle_unmount_page(s_transition.source_page, true);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    s_transition.source_unmounted = true;
    result = _lifecycle_call_handler(s_transition.source_page,
                                     APP_MANAGER_MSG_ONSTOP);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    s_transition.source_stopped = true;
    result = _lifecycle_release_and_free_app(s_transition.source_app);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    s_transition.provider_released = true;
    _lifecycle_commit_target();
    goto exit;

rollback:
    _lifecycle_rollback_to_source();

exit:
    return result;
}

static void _lifecycle_visual_complete(void *context)
{
    (void)context;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!s_transition.in_use)
    {
        return;
    }
    switch (s_transition.kind)
    {
    case LIFECYCLE_TRANSITION_FORWARD_PAGE:
    case LIFECYCLE_TRANSITION_FORWARD_APP:
        result = _lifecycle_finalize_forward();
        break;
    case LIFECYCLE_TRANSITION_BACK_PAGE:
        result = _lifecycle_finalize_back_page();
        break;
    case LIFECYCLE_TRANSITION_BACK_APP:
        result = _lifecycle_finalize_back_app();
        break;
    default:
        break;
    }
    _lifecycle_complete_transition(result);
}

static esp_err_t _lifecycle_begin_transition(
    lifecycle_transition_kind_t kind,
    app_manager_app_node_t *source_app,
    app_manager_page_node_t *source_page,
    app_manager_app_node_t *target_app,
    app_manager_page_node_t *target_page,
    bool target_app_created, bool target_page_created,
    const app_manager_transition_t *transition,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    lv_obj_t *from_screen = source_page != NULL ? source_page->screen :
                            app_manager_presentation_neutral_screen();
    lv_obj_t *to_screen = target_page != NULL ? target_page->screen :
                          app_manager_presentation_neutral_screen();

    if (s_transition.in_use || from_screen == NULL || to_screen == NULL)
    {
        goto exit;
    }
    s_transition = (lifecycle_transition_state_t)
    {
        .in_use = true,
        .target_app_created = target_app_created,
        .target_page_created = target_page_created,
        .kind = kind,
        .phase = LIFECYCLE_TRANSITION_PREPARING,
        .source_app = source_app,
        .source_page = source_page,
        .target_app = target_app,
        .target_page = target_page,
        .completion = completion,
        .completion_context = context,
    };
    atomic_store_explicit(&s_transitioning, true, memory_order_release);
    result = _lifecycle_pause_page(source_page);
    if (result != ESP_OK)
    {
        esp_err_t primary_result = result;
        (void)_lifecycle_rollback_target();
        memset(&s_transition, 0, sizeof(s_transition));
        atomic_store_explicit(&s_transitioning, false,
                              memory_order_release);
        result = primary_result;
        goto exit;
    }
    s_transition.source_paused = source_page != NULL;
    s_transition.phase = LIFECYCLE_TRANSITION_HANDOFF;
    if (target_page != NULL)
    {
        result = _lifecycle_resume_page(target_page);
        if (result != ESP_OK)
        {
            esp_err_t primary_result = result;
            _lifecycle_rollback_to_source();
            memset(&s_transition, 0, sizeof(s_transition));
            atomic_store_explicit(&s_transitioning, false,
                                  memory_order_release);
            result = primary_result;
            goto exit;
        }
        s_transition.target_resumed = true;
    }
    s_transition.phase = LIFECYCLE_TRANSITION_ANIMATING;
    result = app_manager_presentation_start(
                 from_screen, to_screen, transition->effect,
                 transition->duration_ms, _lifecycle_visual_complete, NULL);
    if (result != ESP_OK && s_transition.in_use)
    {
        esp_err_t primary_result = result;
        _lifecycle_rollback_to_source();
        memset(&s_transition, 0, sizeof(s_transition));
        atomic_store_explicit(&s_transitioning, false,
                              memory_order_release);
        result = primary_result;
    }

exit:
    return result;
}

static esp_err_t _lifecycle_prepare_target(app_manager_page_node_t *target,
        app_manager_page_node_t *source)
{
    (void)source;
    esp_err_t result = _lifecycle_start_page(target);
    if (result == ESP_OK)
    {
        result = _lifecycle_mount_page(target);
    }
    return result;
}

static esp_err_t _lifecycle_run_app(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_OK;
    bool app_created = false;
    app_manager_app_node_t *source_app = s_model.active_app;
    app_manager_page_node_t *source_page = source_app != NULL ?
                                           source_app->current_page : NULL;
    app_manager_app_node_t *target_app = app_manager_nav_model_find_app(
            &s_model, request->app_id);
    app_manager_page_node_t *target_page = NULL;

    if (target_app == NULL)
    {
        result = _lifecycle_create_app(request->app_id, NULL, request,
                                       &target_app, &target_page);
        app_created = result == ESP_OK;
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    else
    {
        target_page = target_app->current_page;
        if (target_page == NULL)
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
        _lifecycle_update_intent(target_app, target_page, request);
    }
    if (target_app == source_app && target_page == source_page)
    {
        completion(ESP_OK, context);
        goto exit;
    }
    result = _lifecycle_prepare_target(target_page, source_page);
    if (result != ESP_OK)
    {
        lifecycle_transition_state_t cleanup =
        {
            .target_app = target_app,
            .target_page = target_page,
            .target_app_created = app_created,
        };
        s_transition = cleanup;
        (void)_lifecycle_rollback_target();
        memset(&s_transition, 0, sizeof(s_transition));
        goto exit;
    }
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request,
            &s_settings.app_forward);
    result = _lifecycle_begin_transition(
                 LIFECYCLE_TRANSITION_FORWARD_APP,
                 source_app, source_page, target_app, target_page,
                 app_created, false, &transition, completion, context);

exit:
    return result;
}

static esp_err_t _lifecycle_open_page(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_OK;
    bool app_created = false;
    bool page_created = false;
    app_manager_app_node_t *source_app = s_model.active_app;
    app_manager_page_node_t *source_page = source_app != NULL ?
                                           source_app->current_page : NULL;
    app_manager_app_node_t *target_app = app_manager_nav_model_find_app(
            &s_model, request->app_id);
    app_manager_page_node_t *target_page = NULL;

    if (app_manager_builtin_find_page(request->app_id,
                                      request->page_id) == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    if (target_app == NULL)
    {
        result = _lifecycle_create_app(request->app_id, request->page_id,
                                       request, &target_app, &target_page);
        app_created = result == ESP_OK;
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    else
    {
        target_page = app_manager_nav_model_find_page(target_app,
                      request->page_id);
        if (target_page != NULL)
        {
            _lifecycle_update_intent(target_app, target_page, request);
            if (target_app == source_app && target_page == source_page)
            {
                completion(ESP_OK, context);
                goto exit;
            }
            result = target_app == source_app ?
                     _lifecycle_trim_between(target_app, source_page,
                                             target_page) :
                     _lifecycle_trim_to_background_target(target_app,
                         target_page);
            if (result != ESP_OK)
            {
                goto exit;
            }
        }
        else
        {
            const app_manager_page_desc_t *descriptor =
                app_manager_builtin_lease_find_page(&target_app->lease,
                                                    request->page_id);
            if (descriptor == NULL)
            {
                result = ESP_ERR_NOT_FOUND;
                goto exit;
            }
            result = _lifecycle_create_page(target_app, descriptor, request,
                                            false,
                                            &target_page);
            page_created = result == ESP_OK;
            if (result != ESP_OK)
            {
                goto exit;
            }
        }
    }

    result = _lifecycle_prepare_target(target_page, source_page);
    if (result != ESP_OK)
    {
        s_transition = (lifecycle_transition_state_t)
        {
            .target_app = target_app,
            .target_page = target_page,
            .target_app_created = app_created,
            .target_page_created = page_created,
        };
        (void)_lifecycle_rollback_target();
        memset(&s_transition, 0, sizeof(s_transition));
        goto exit;
    }

    const bool app_transition = target_app != source_app;
    const bool page_back = !page_created && !app_transition;
    const app_manager_transition_t *configured = app_transition ?
        &s_settings.app_forward :
        (page_back ? &s_settings.page_back : &s_settings.page_forward);
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request, configured);
    result = _lifecycle_begin_transition(
                 app_transition ? LIFECYCLE_TRANSITION_FORWARD_APP :
                 (page_back ? LIFECYCLE_TRANSITION_BACK_PAGE :
                  LIFECYCLE_TRANSITION_FORWARD_PAGE),
                 source_app, source_page, target_app, target_page,
                 app_created, page_created, &transition, completion, context);

exit:
    return result;
}

static esp_err_t _lifecycle_back_page(
    const app_manager_nav_request_t *request,
    app_manager_page_node_t *target_page,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    app_manager_app_node_t *app = s_model.active_app;
    app_manager_page_node_t *source = app->current_page;
    esp_err_t result = _lifecycle_trim_between(app, source, target_page);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _lifecycle_prepare_target(target_page, source);
    if (result != ESP_OK)
    {
        if (target_page->mounted)
        {
            (void)_lifecycle_unmount_page(target_page, true);
        }
        goto exit;
    }
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request, &s_settings.page_back);
    result = _lifecycle_begin_transition(
                 LIFECYCLE_TRANSITION_BACK_PAGE, app, source, app, target_page,
                 false, false, &transition, completion, context);

exit:
    return result;
}

static esp_err_t _lifecycle_back_app(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    app_manager_app_node_t *source_app = s_model.active_app;
    app_manager_page_node_t *source_page = source_app->current_page;
    app_manager_app_node_t *target_app = source_app->previous;
    app_manager_page_node_t *target_page = target_app != NULL ?
                                           target_app->current_page : NULL;
    esp_err_t result = ESP_OK;

    for (app_manager_page_node_t *page = source_page->previous;
            page != NULL; page = page->previous)
    {
        result = _lifecycle_stop_page(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (target_page != NULL)
    {
        result = _lifecycle_start_page(target_page);
        if (result == ESP_OK)
        {
            result = _lifecycle_mount_page(target_page);
        }
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request, &s_settings.app_back);
    result = _lifecycle_begin_transition(
                 LIFECYCLE_TRANSITION_BACK_APP, source_app, source_page,
                 target_app, target_page, false, false, &transition,
                 completion, context);

exit:
    return result;
}

static esp_err_t _lifecycle_back(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_model.active_app == NULL || s_model.active_app->current_page == NULL)
    {
        goto exit;
    }
    if (request->app_id != NULL &&
            strcmp(request->app_id, s_model.active_app->descriptor->id) != 0)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    app_manager_page_node_t *source = s_model.active_app->current_page;
    if (source->previous != NULL)
    {
        result = _lifecycle_back_page(request, source->previous,
                                      completion, context);
    }
    else
    {
        result = _lifecycle_back_app(request, completion, context);
    }

exit:
    return result;
}

static esp_err_t _lifecycle_back_to(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;
    app_manager_app_node_t *app = s_model.active_app;
    if (app == NULL || (request->app_id != NULL &&
                        strcmp(request->app_id, app->descriptor->id) != 0))
    {
        goto exit;
    }
    app_manager_page_node_t *target = app_manager_nav_model_find_page(
                                          app, request->page_id);
    if (target == NULL)
    {
        goto exit;
    }
    if (target == app->current_page)
    {
        completion(ESP_OK, context);
        result = ESP_OK;
        goto exit;
    }
    result = _lifecycle_back_page(request, target, completion, context);

exit:
    return result;
}

static esp_err_t _lifecycle_exit_app(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;
    const char *app_id = request->app_id;
    if (app_id == NULL)
    {
        goto exit;
    }
    app_manager_app_node_t *app = app_manager_nav_model_find_app(&s_model,
                                  app_id);
    if (app == NULL)
    {
        goto exit;
    }
    if (app == s_model.active_app)
    {
        result = _lifecycle_back_app(request, completion, context);
    }
    else
    {
        result = _lifecycle_destroy_background_app(app);
        if (result == ESP_OK)
        {
            completion(ESP_OK, context);
        }
    }

exit:
    return result;
}

static esp_err_t _lifecycle_remove_page(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;
    app_manager_app_node_t *app = request->app_id != NULL ?
                                  app_manager_nav_model_find_app(
                                      &s_model, request->app_id) :
                                  s_model.active_app;
    if (app == NULL)
    {
        goto exit;
    }
    app_manager_page_node_t *page = app_manager_nav_model_find_page(
                                        app, request->page_id);
    if (page == NULL)
    {
        goto exit;
    }
    if (app == s_model.active_app && page == app->current_page)
    {
        result = _lifecycle_back(request, completion, context);
        goto exit;
    }
    result = _lifecycle_stop_page(page);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _lifecycle_free_page(page);
    if (result != ESP_OK)
    {
        goto exit;
    }
    if (app->current_page == NULL)
    {
        result = _lifecycle_release_and_free_app(app);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    completion(ESP_OK, context);

exit:
    return result;
}

static esp_err_t _lifecycle_cleanup_background(
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_OK;
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        app_manager_app_node_t *app = &s_model.apps[index];
        if (!app->in_use || app == s_model.active_app ||
                (app->descriptor->flags & APP_MANAGER_APP_FLAG_PINNED) != 0U)
        {
            continue;
        }
        result = _lifecycle_destroy_background_app(app);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    completion(ESP_OK, context);

exit:
    return result;
}

static bool _lifecycle_request_valid(const app_manager_nav_request_t *request)
{
    bool valid = request != NULL &&
                 request->operation >= APP_MANAGER_NAV_OP_RUN &&
                 request->operation < APP_MANAGER_NAV_OP_END &&
                 _lifecycle_transition_value_valid(&request->transition);
    if (!valid)
    {
        goto exit;
    }
    if (request->has_arguments &&
            (request->arguments.version != APP_MANAGER_TYPED_BLOB_VERSION ||
             request->arguments.size > APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES ||
             request->arguments.reserved != 0U))
    {
        valid = false;
        goto exit;
    }
    if (request->app_id != NULL &&
            !app_manager_lifecycle_id_is_valid(request->app_id))
    {
        valid = false;
        goto exit;
    }
    if (request->page_id != NULL &&
            !app_manager_lifecycle_id_is_valid(request->page_id))
    {
        valid = false;
        goto exit;
    }
    switch (request->operation)
    {
    case APP_MANAGER_NAV_OP_RUN:
    case APP_MANAGER_NAV_OP_EXIT:
        valid = request->app_id != NULL;
        break;
    case APP_MANAGER_NAV_OP_OPEN_PAGE:
        valid = request->app_id != NULL && request->page_id != NULL;
        break;
    case APP_MANAGER_NAV_OP_BACK_TO:
    case APP_MANAGER_NAV_OP_REMOVE_PAGE:
        valid = request->page_id != NULL;
        break;
    case APP_MANAGER_NAV_OP_EXIT_SELF:
        valid = request->app_id != NULL;
        break;
    case APP_MANAGER_NAV_OP_BACK:
    case APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND:
        break;
    case APP_MANAGER_NAV_OP_END:
    default:
        valid = false;
        break;
    }

exit:
    return valid;
}

esp_err_t app_manager_lifecycle_navigate(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (!_lifecycle_request_valid(request) || completion == NULL)
    {
        goto exit;
    }
    if (!s_initialized || s_tearing_down ||
            !app_manager_mailbox_is_worker() ||
            !app_manager_lifecycle_screen_mutation_allowed() ||
            s_handler_page != NULL ||
            atomic_load_explicit(&s_transitioning, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = _lifecycle_retry_target_recovery();
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _lifecycle_retry_source_recovery();
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _lifecycle_retry_pending_cleanup();
    if (result != ESP_OK)
    {
        goto exit;
    }
    switch (request->operation)
    {
    case APP_MANAGER_NAV_OP_RUN:
        result = _lifecycle_run_app(request, completion, context);
        break;
    case APP_MANAGER_NAV_OP_EXIT:
    case APP_MANAGER_NAV_OP_EXIT_SELF:
        result = _lifecycle_exit_app(request, completion, context);
        break;
    case APP_MANAGER_NAV_OP_OPEN_PAGE:
        result = _lifecycle_open_page(request, completion, context);
        break;
    case APP_MANAGER_NAV_OP_BACK:
        result = _lifecycle_back(request, completion, context);
        break;
    case APP_MANAGER_NAV_OP_BACK_TO:
        result = _lifecycle_back_to(request, completion, context);
        break;
    case APP_MANAGER_NAV_OP_REMOVE_PAGE:
        result = _lifecycle_remove_page(request, completion, context);
        break;
    case APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND:
        result = _lifecycle_cleanup_background(completion, context);
        break;
    case APP_MANAGER_NAV_OP_END:
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_init(size_t arena_bytes)
{
    esp_err_t result = ESP_OK;
    int expected = LIFECYCLE_SCREEN_CLOSED;

    if (s_initialized || !atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_INITIALIZING,
                memory_order_acq_rel, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!s_settings_configured)
    {
        _lifecycle_settings_defaults(&s_settings);
    }
    while (atomic_load(&s_screen_accesses) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        vTaskDelay(1);
#endif
    }
    result = app_manager_nav_model_init(&s_model, arena_bytes);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = app_manager_builtin_registry_validate(s_model.page_stride);
    if (result != ESP_OK)
    {
        app_manager_nav_model_deinit(&s_model);
        goto fail;
    }
    if (app_manager_presentation_neutral_screen() == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        app_manager_nav_model_deinit(&s_model);
        goto fail;
    }
    memset(&s_transition, 0, sizeof(s_transition));
    s_handler_page = NULL;
    s_handler_cleanup_error = ESP_OK;
    s_handler_message = APP_MANAGER_MSG_USER_END;
    s_tearing_down = false;
    s_recovery_pending = false;
    s_recovery_app = NULL;
    s_recovery_page = NULL;
    s_target_recovery_pending = false;
    atomic_store(&s_paused_running_apps, 0U);
    atomic_store(&s_transitioning, false);
    s_initialized = true;
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE,
                          memory_order_release);
    goto exit;

fail:
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_CLOSED,
                          memory_order_release);

exit:
    return result;
}

static void _lifecycle_force_release_nodes(void)
{
    (void)app_manager_presentation_load_immediate(
        app_manager_presentation_neutral_screen());
    for (size_t index = 0; index < APP_MANAGER_MAX_PAGE_INSTANCES; index++)
    {
        app_manager_page_node_t *page = &s_model.pages[index];
        if (page->in_use && page->screen != NULL)
        {
            page->mounted = false;
            page->active = false;
            (void)_lifecycle_delete_unmounted_screen(page);
        }
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        if (s_model.apps[index].in_use)
        {
            (void)app_manager_builtin_release(&s_model.apps[index].lease);
        }
    }
}

void app_manager_lifecycle_force_deinit(void)
{
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_CLOSED,
                          memory_order_release);
    if (s_initialized)
    {
        app_manager_presentation_finish_now();
        _lifecycle_force_release_nodes();
        app_manager_nav_model_deinit(&s_model);
    }
    memset(&s_transition, 0, sizeof(s_transition));
    s_handler_page = NULL;
    s_handler_cleanup_error = ESP_OK;
    s_handler_message = APP_MANAGER_MSG_USER_END;
    s_tearing_down = false;
    s_recovery_pending = false;
    s_recovery_app = NULL;
    s_recovery_page = NULL;
    s_target_recovery_pending = false;
    s_initialized = false;
    atomic_store(&s_transitioning, false);
    atomic_store(&s_paused_running_apps, 0U);
}

esp_err_t app_manager_lifecycle_deinit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!s_initialized || !app_manager_mailbox_is_worker() ||
            atomic_load(&s_screen_phase) != LIFECYCLE_SCREEN_CLOSED ||
            atomic_load(&s_screen_accesses) != 0U)
    {
        goto exit;
    }
    s_tearing_down = true;
    app_manager_presentation_finish_now();
    result = _lifecycle_retry_pending_cleanup();
    if (result != ESP_OK)
    {
        goto exit;
    }
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        app_manager_app_node_t *app = &s_model.apps[index];
        if (!app->in_use)
        {
            continue;
        }
        if (app->active && app->current_page != NULL)
        {
            result = _lifecycle_pause_page(app->current_page);
            if (result != ESP_OK)
            {
                goto exit;
            }
            app->active = false;
        }
        if (app->current_page != NULL && app->current_page->mounted)
        {
            result = app_manager_presentation_load_immediate(
                         app_manager_presentation_neutral_screen());
            if (result != ESP_OK)
            {
                goto exit;
            }
        }
        result = _lifecycle_destroy_background_app(app);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    app_manager_nav_model_deinit(&s_model);
    s_initialized = false;
    s_tearing_down = false;
    s_recovery_pending = false;
    s_recovery_app = NULL;
    s_recovery_page = NULL;
    s_target_recovery_pending = false;
    atomic_store(&s_paused_running_apps, 0U);
    result = ESP_OK;

exit:
    return result;
}

bool app_manager_lifecycle_screen_mutation_allowed(void)
{
    return atomic_load_explicit(&s_screen_phase, memory_order_acquire) ==
           LIFECYCLE_SCREEN_ACTIVE;
}

bool app_manager_lifecycle_screen_access_begin(void)
{
    atomic_fetch_add(&s_screen_accesses, 1U);
    bool admitted = app_manager_lifecycle_screen_mutation_allowed();
    if (!admitted)
    {
        atomic_fetch_sub(&s_screen_accesses, 1U);
    }
    return admitted;
}

void app_manager_lifecycle_screen_access_end(void)
{
    atomic_fetch_sub(&s_screen_accesses, 1U);
}

esp_err_t app_manager_lifecycle_screen_access_wait(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (app_manager_mailbox_is_worker() ||
            atomic_load(&s_screen_phase) != LIFECYCLE_SCREEN_PAUSED)
    {
        goto exit;
    }
    while (atomic_load(&s_screen_accesses) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        vTaskDelay(1);
#endif
    }
    if (atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_PAUSED)
    {
        result = ESP_OK;
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_shutdown_begin_and_wait(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (app_manager_mailbox_is_worker())
    {
        goto exit;
    }
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_CLOSED,
                          memory_order_release);
    while (atomic_load(&s_screen_accesses) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        vTaskDelay(1);
#endif
    }
    result = ESP_OK;

exit:
    return result;
}

bool app_manager_lifecycle_screen_running_snapshot(uint32_t *count)
{
    bool available = count != NULL &&
                     atomic_load_explicit(&s_screen_phase,
                                          memory_order_acquire) == LIFECYCLE_SCREEN_PAUSED;
    if (available)
    {
        *count = atomic_load(&s_paused_running_apps);
    }
    return available;
}

esp_err_t app_manager_lifecycle_screen_pause(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    int expected = LIFECYCLE_SCREEN_ACTIVE;
    app_manager_app_node_t *app = NULL;
    app_manager_page_node_t *page = NULL;

    if (!s_initialized || s_tearing_down ||
            !app_manager_mailbox_is_worker())
    {
        goto exit;
    }
    if (atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_PAUSED)
    {
        result = ESP_OK;
        goto exit;
    }
    app_manager_presentation_finish_now();
    if (atomic_load_explicit(&s_transitioning, memory_order_acquire) ||
            app_manager_presentation_is_running())
    {
        goto exit;
    }
    result = _lifecycle_retry_pending_cleanup();
    if (result != ESP_OK)
    {
        goto exit;
    }
    app = s_model.active_app;
    page = app != NULL ? app->current_page : NULL;
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_PAUSING,
                memory_order_acq_rel, memory_order_acquire))
    {
        goto exit;
    }
    result = _lifecycle_pause_page(page);
    if (result != ESP_OK)
    {
        atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE);
        goto exit;
    }
    result = app_manager_presentation_load_immediate(
                 app_manager_presentation_neutral_screen());
    if (result != ESP_OK)
    {
        (void)_lifecycle_resume_page(page);
        atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE);
        goto exit;
    }
    result = _lifecycle_unmount_page(page, true);
    if (result != ESP_OK)
    {
        if (page != NULL)
        {
            esp_err_t restore_result = _lifecycle_start_page(page);
            if (restore_result == ESP_OK)
            {
                restore_result = _lifecycle_mount_page(page);
            }
            if (restore_result == ESP_OK)
            {
                restore_result = app_manager_presentation_load_immediate(
                                     page->screen);
            }
            if (restore_result == ESP_OK)
            {
                restore_result = _lifecycle_resume_page(page);
            }
            if (restore_result != ESP_OK)
            {
                (void)app_manager_presentation_load_immediate(
                    app_manager_presentation_neutral_screen());
                if (page->mounted)
                {
                    (void)_lifecycle_unmount_page(page, true);
                }
                page->active = false;
                if (app != NULL)
                {
                    app->active = false;
                }
                atomic_store(&s_paused_running_apps,
                             app_manager_nav_model_running_count(&s_model));
                atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_PAUSED);
                goto exit;
            }
        }
        if (app != NULL)
        {
            app->active = true;
        }
        atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE);
        goto exit;
    }
    if (app != NULL)
    {
        app->active = false;
    }
    atomic_store(&s_paused_running_apps,
                 app_manager_nav_model_running_count(&s_model));
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_PAUSED,
                          memory_order_release);
    atomic_store_explicit(&s_target_recovery_pending, false,
                          memory_order_release);

exit:
    return result;
}

esp_err_t app_manager_lifecycle_screen_resume(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    int expected = LIFECYCLE_SCREEN_PAUSED;
    app_manager_app_node_t *app = s_model.active_app;
    app_manager_page_node_t *page = app != NULL ? app->current_page : NULL;

    if (!s_initialized || s_tearing_down ||
            !app_manager_mailbox_is_worker())
    {
        goto exit;
    }
    if (atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_ACTIVE)
    {
        result = ESP_OK;
        goto exit;
    }
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_RESUMING,
                memory_order_acq_rel, memory_order_acquire))
    {
        goto exit;
    }
    if (page != NULL)
    {
        result = _lifecycle_start_page(page);
        if (result == ESP_OK)
        {
            result = _lifecycle_mount_page(page);
        }
        if (result == ESP_OK)
        {
            result = app_manager_presentation_load_immediate(page->screen);
        }
        if (result == ESP_OK)
        {
            result = _lifecycle_resume_page(page);
        }
        if (result == ESP_OK)
        {
            app->active = true;
            atomic_store_explicit(&s_target_recovery_pending, false,
                                  memory_order_release);
            if (atomic_load(&s_recovery_pending) && s_recovery_app == app &&
                    s_recovery_page == page)
            {
                s_recovery_pending = false;
                s_recovery_app = NULL;
                s_recovery_page = NULL;
            }
        }
    }
    else
    {
        result = app_manager_presentation_load_immediate(
                     app_manager_presentation_neutral_screen());
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE,
                              memory_order_release);
    }
    else
    {
        if (page != NULL && page->mounted)
        {
            (void)app_manager_presentation_load_immediate(
                app_manager_presentation_neutral_screen());
            (void)_lifecycle_unmount_page(page, true);
        }
        if (app != NULL)
        {
            app->active = false;
        }
        atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_PAUSED,
                              memory_order_release);
    }

exit:
    return result;
}

static bool _lifecycle_topology_query_allowed(void)
{
    return app_manager_lifecycle_screen_mutation_allowed() &&
           !atomic_load_explicit(&s_transitioning, memory_order_acquire) &&
           !atomic_load_explicit(&s_recovery_pending, memory_order_acquire) &&
           !atomic_load_explicit(&s_target_recovery_pending,
                                 memory_order_acquire);
}

esp_err_t app_manager_lifecycle_is_active_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->condition = _lifecycle_topology_query_allowed() &&
                         s_model.active_app != NULL &&
                         strcmp(s_model.active_app->descriptor->id,
                                request->app_id) == 0;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_get_active_id_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->value = _lifecycle_topology_query_allowed() &&
                     s_model.active_app != NULL ?
                     (void *)s_model.active_app->descriptor->id : NULL;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_page_active_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    app_manager_app_node_t *app = app_manager_nav_model_find_app(
                                      &s_model, request->app_id);
    app_manager_page_node_t *page = app_manager_nav_model_find_page(
                                        app, request->page_id);
    request->condition = _lifecycle_topology_query_allowed() &&
                         page != NULL && page->active;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_page_present_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    app_manager_app_node_t *app = app_manager_nav_model_find_app(
                                      &s_model, request->app_id);
    request->condition = _lifecycle_topology_query_allowed() &&
                         app_manager_nav_model_find_page(
                             app, request->page_id) != NULL;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_all_closed_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->condition = _lifecycle_topology_query_allowed() &&
                         app_manager_nav_model_running_count(&s_model) == 0U;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_running_count_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->count = _lifecycle_topology_query_allowed() ?
                     app_manager_nav_model_running_count(&s_model) : 0U;
    return ESP_OK;
}

void *app_manager_this_page_memory(void)
{
    return s_handler_page != NULL ? s_handler_page->private_memory : NULL;
}

void *app_manager_this_page_userdata(void)
{
    return s_handler_page != NULL ? s_handler_page->user_data : NULL;
}

lv_obj_t *app_manager_this_page_screen(void)
{
    return s_handler_page != NULL &&
           (s_handler_message == APP_MANAGER_MSG_ONMOUNT ||
            s_handler_page->mounted) ? s_handler_page->screen : NULL;
}

const app_manager_typed_blob_t *app_manager_this_page_arguments(void)
{
    return s_handler_page != NULL && s_handler_page->has_arguments ?
           &s_handler_page->arguments : NULL;
}

void app_manager_this_page_report_cleanup_error(esp_err_t error)
{
    if (s_handler_page != NULL && error != ESP_OK &&
            (s_handler_message == APP_MANAGER_MSG_ONPAUSE ||
             s_handler_message == APP_MANAGER_MSG_ONSTOP) &&
            s_handler_cleanup_error == ESP_OK)
    {
        s_handler_cleanup_error = error;
    }
}

bool app_manager_is_transitioning(void)
{
    return atomic_load_explicit(&s_transitioning, memory_order_acquire) ||
           atomic_load_explicit(&s_recovery_pending, memory_order_acquire) ||
           atomic_load_explicit(&s_target_recovery_pending,
                                memory_order_acquire);
}

#ifdef HOST_TEST
void app_manager_lifecycle_host_set_observer(
    app_manager_lifecycle_observer_t observer)
{
    atomic_store(&s_host_observer, observer);
}
#endif
