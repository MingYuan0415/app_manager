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

#if CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG
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
#endif

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
    lifecycle_settings_t settings;
    app_manager_transition_t default_app_forward = {0};
    app_manager_transition_t default_app_back = {0};
    app_manager_transition_t default_page_forward = {0};
    app_manager_transition_t default_page_back = {0};

    if (s_initialized ||
            atomic_load_explicit(&s_screen_phase, memory_order_acquire) !=
            LIFECYCLE_SCREEN_CLOSED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (max_resident_apps > APP_MANAGER_MAX_RESIDENT_APPS ||
            resident_policy < APP_MANAGER_RESIDENT_REJECT ||
            resident_policy > APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND)
    {
        return ESP_ERR_INVALID_ARG;
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
        return ESP_ERR_INVALID_ARG;
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
    return ESP_OK;
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

static void _lifecycle_call_ops(const app_manager_page_ops_t *ops,
                                app_manager_msg_type_t message)
{
    const app_manager_page_context_t context =
    {
        .state = app_manager_this_page_memory(),
        .route_user_data = app_manager_this_page_userdata(),
        .screen = app_manager_this_page_screen(),
        .arguments = app_manager_this_page_arguments(),
    };
    esp_err_t cleanup_result = ESP_OK;

    switch (message)
    {
    case APP_MANAGER_MSG_ONSTART:
        if (ops->start != NULL)
        {
            ops->start(&context);
        }
        break;
    case APP_MANAGER_MSG_ONMOUNT:
        if (ops->mount != NULL)
        {
            ops->mount(&context);
        }
        break;
    case APP_MANAGER_MSG_ONRESUME:
        if (ops->resume != NULL)
        {
            ops->resume(&context);
        }
        break;
    case APP_MANAGER_MSG_ONPAUSE:
        if (ops->pause != NULL)
        {
            cleanup_result = ops->pause(&context);
        }
        break;
    case APP_MANAGER_MSG_ONUNMOUNT:
        if (ops->unmount != NULL)
        {
            ops->unmount(&context);
        }
        break;
    case APP_MANAGER_MSG_ONSTOP:
        if (ops->stop != NULL)
        {
            cleanup_result = ops->stop(&context);
        }
        break;
    case APP_MANAGER_MSG_ONNEWINTENT:
        if (ops->new_intent != NULL)
        {
            ops->new_intent(&context);
        }
        break;
    case APP_MANAGER_MSG_USER_END:
    default:
        break;
    }
    if (cleanup_result != ESP_OK && s_handler_cleanup_error == ESP_OK)
    {
        s_handler_cleanup_error = cleanup_result;
    }
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

    if (page == NULL || !page->in_use || page->route == NULL ||
            page->route->definition == NULL ||
            ((page->route->definition->handler == NULL) ==
             (page->route->definition->ops == NULL)))
    {
        return ESP_ERR_INVALID_STATE;
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
        observer(page->app->descriptor->id, page->route->page_id,
                 message, APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE);
    }
#endif
#if CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG
    LOG_I("[%s][%s] ON %s", page->app->descriptor->name,
          page->route->page_id, s_lifecycle_message_names[message]);
#endif
    const app_manager_page_definition_t *definition =
        page->route->definition;
    if (definition->ops != NULL)
    {
        _lifecycle_call_ops(definition->ops, message);
    }
    else
    {
        definition->handler(message, page->route->user_data);
    }
#ifdef HOST_TEST
    if (observer != NULL)
    {
        observer(page->app->descriptor->id, page->route->page_id,
                 message, APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    }
#endif
    cleanup_error = s_handler_cleanup_error;
    s_handler_page = previous_page;
    s_handler_message = previous_message;
    s_handler_cleanup_error = previous_cleanup_error;

    if (!page->in_use || page->generation != generation)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((message == APP_MANAGER_MSG_ONPAUSE ||
            message == APP_MANAGER_MSG_ONSTOP) && cleanup_error != ESP_OK)
    {
        return cleanup_error;
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
    return result;
}

static esp_err_t _lifecycle_start_page(app_manager_page_node_t *page)
{
    if (page->started)
    {
        return ESP_OK;
    }
    const size_t memory_size = page->route->definition->memory_size;
    if (page->private_memory != NULL && memory_size > 0U)
    {
        memset(page->private_memory, 0, memory_size);
    }
    return _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTART);
}

static esp_err_t _lifecycle_mount_page(app_manager_page_node_t *page)
{
    esp_err_t result = ESP_OK;
    bool screen_created = false;

    if (page == NULL)
    {
        return ESP_OK;
    }
    if (page->mounted)
    {
        return ESP_OK;
    }
    if (!page->started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (page->screen == NULL)
    {
        page->screen = app_manager_presentation_create_page_screen();
        if (page->screen == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        screen_created = true;
    }
    result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONMOUNT);
    if (result != ESP_OK && screen_created)
    {
        (void)app_manager_presentation_delete_page_screen(page->screen);
        page->screen = NULL;
    }
    return result;
}

static esp_err_t _lifecycle_delete_unmounted_screen(
    app_manager_page_node_t *page)
{
    if (page == NULL || page->screen == NULL)
    {
        return ESP_OK;
    }
    if (page->mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = app_manager_presentation_delete_page_screen(page->screen);
    if (result == ESP_OK)
    {
        page->screen = NULL;
    }
    return result;
}

static esp_err_t _lifecycle_unmount_page(app_manager_page_node_t *page,
        bool delete_screen)
{
    if (page == NULL || !page->mounted)
    {
        return ESP_OK;
    }
    if (page->active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONUNMOUNT);
    if (result == ESP_OK && delete_screen)
    {
        result = _lifecycle_delete_unmounted_screen(page);
    }
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
    if (page == NULL)
    {
        return ESP_OK;
    }
    esp_err_t result = _lifecycle_pause_page(page);
    if (result != ESP_OK)
    {
        return result;
    }
    if (page->mounted)
    {
        result = _lifecycle_unmount_page(page, true);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (page->started)
    {
        result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTOP);
    }
    return result;
}

static bool _lifecycle_page_is_linked(
    const app_manager_app_node_t *app,
    const app_manager_page_node_t *page)
{
    if (app == NULL || page == NULL)
    {
        return false;
    }
    for (const app_manager_page_node_t *node = app->current_page;
            node != NULL; node = node->previous)
    {
        if (node == page)
        {
            return true;
        }
    }
    return false;
}

static esp_err_t _lifecycle_free_page(app_manager_page_node_t *page)
{
    if (page == NULL || page->active || page->mounted || page->started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    app_manager_app_node_t *app = page->app;
    if (page->screen != NULL)
    {
        esp_err_t result = _lifecycle_delete_unmounted_screen(page);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (_lifecycle_page_is_linked(app, page))
    {
        app_manager_nav_model_unlink_page(app, page);
    }
    app_manager_nav_model_free_page(&s_model, page);
    return ESP_OK;
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

static esp_err_t _lifecycle_free_app(app_manager_app_node_t *app)
{
    esp_err_t result = ESP_OK;
    for (app_manager_page_node_t *page = app->current_page;
            page != NULL; page = page->previous)
    {
        if (page->active || page->mounted || page->started)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (page->screen != NULL)
        {
            result = _lifecycle_delete_unmounted_screen(page);
            if (result != ESP_OK)
            {
                return result;
            }
        }
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
    return ESP_OK;
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
    if (app == NULL || app->active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _lifecycle_stop_all_app_pages(app);
    if (result == ESP_OK)
    {
        result = _lifecycle_free_app(app);
    }
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
                return result;
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
            return result;
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
            return result;
        }
    }
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
    if (app_manager_nav_model_running_count(&s_model) <
            s_settings.max_resident_apps)
    {
        return ESP_OK;
    }
    if (s_settings.resident_policy !=
            APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND)
    {
        return ESP_ERR_NO_MEM;
    }
    app_manager_app_node_t *candidate = _lifecycle_oldest_evictable_app();
    if (candidate == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return _lifecycle_destroy_background_app(candidate);
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
                                        const app_manager_page_route_t *route,
                                        const app_manager_nav_request_t *request,
                                        bool link_page,
                                        app_manager_page_node_t **out_page)
{
    app_manager_page_node_t *page = app_manager_nav_model_alloc_page(
                                        &s_model, route);
    if (page == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    page->app = app;
    if (link_page)
    {
        app_manager_nav_model_link_page(app, page);
    }
    _lifecycle_copy_arguments(app, page, request);
    esp_err_t result = _lifecycle_start_page(page);
    if (result != ESP_OK)
    {
        (void)_lifecycle_free_page(page);
        return result;
    }
    *out_page = page;
    return ESP_OK;
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
    const app_manager_app_desc_t *descriptor =
        app_manager_builtin_find_by_id(app_id);

    if (descriptor == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    const app_manager_page_route_t *root_route =
        app_manager_builtin_find_page(descriptor, descriptor->root_page_id);
    if (root_route == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const app_manager_page_route_t *target_route = root_route;
    if (target_page_id != NULL &&
            strcmp(target_page_id, root_route->page_id) != 0)
    {
        target_route = app_manager_builtin_find_page(descriptor,
                       target_page_id);
        if (target_route == NULL)
        {
            return ESP_ERR_NOT_FOUND;
        }
    }
    result = _lifecycle_make_resident_space();
    if (result != ESP_OK)
    {
        return result;
    }
    app = app_manager_nav_model_alloc_app(&s_model);
    if (app == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    app->descriptor = descriptor;
    result = _lifecycle_create_page(app, root_route, request, true,
                                    &root);
    if (result != ESP_OK)
    {
        goto fail_app;
    }
    target = root;
    if (target_route != root_route)
    {
        result = _lifecycle_create_page(app, target_route, request, true,
                                        &target);
        if (result != ESP_OK)
        {
            goto fail_pages;
        }
    }
    *out_app = app;
    *out_page = target;
    return ESP_OK;

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
        app_manager_nav_model_free_app(app);
    }
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
            return result;
        }
        result = _lifecycle_free_page(page);
        if (result != ESP_OK)
        {
            return result;
        }
        page = previous;
    }
    if (page != target)
    {
        result = ESP_ERR_NOT_FOUND;
    }
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
            return result;
        }
        result = _lifecycle_free_page(page);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (app->current_page != target)
    {
        result = ESP_ERR_NOT_FOUND;
    }
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
                s_transition.target_page->route->page_id) !=
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
        return ESP_ERR_INVALID_STATE;
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
        return app_manager_presentation_load_immediate(
                   app_manager_presentation_neutral_screen());
    }
    result = _lifecycle_start_page(source);
    if (result == ESP_OK)
    {
        result = _lifecycle_mount_page(source);
    }
    if (result != ESP_OK)
    {
        return result;
    }
    result = app_manager_presentation_load_immediate(source->screen);
    if (result != ESP_OK)
    {
        return result;
    }
    result = _lifecycle_resume_page(source);
    if (result == ESP_OK && s_transition.source_app != NULL)
    {
        s_transition.source_app->active = true;
        s_transition.source_paused = false;
    }
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
        return ESP_OK;
    }

    app_manager_app_node_t *app = s_model.active_app;
    app_manager_page_node_t *page = app != NULL ? app->current_page : NULL;
    if (app == NULL || page == NULL || !app->in_use || !page->in_use ||
            app->descriptor == NULL || page->route == NULL)
    {
        return ESP_ERR_INVALID_STATE;
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
    return result;
}

static esp_err_t _lifecycle_retry_source_recovery(void)
{
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_recovery_pending, memory_order_acquire))
    {
        return ESP_OK;
    }
    if (s_recovery_app == NULL || s_recovery_page == NULL ||
            !s_recovery_app->in_use || !s_recovery_page->in_use)
    {
        return ESP_ERR_INVALID_STATE;
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
    return result;

rollback:
    _lifecycle_rollback_to_source();
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
    return result;

rollback:
    _lifecycle_rollback_to_source();
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
    result = _lifecycle_free_app(s_transition.source_app);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    _lifecycle_commit_target();
    return result;

rollback:
    _lifecycle_rollback_to_source();
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
        return ESP_ERR_INVALID_STATE;
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
        return primary_result;
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
            return primary_result;
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
            return result;
        }
    }
    else
    {
        target_page = target_app->current_page;
        if (target_page == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        _lifecycle_update_intent(target_app, target_page, request);
    }
    if (target_app == source_app && target_page == source_page)
    {
        completion(ESP_OK, context);
        return ESP_OK;
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
        return result;
    }
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request,
            &s_settings.app_forward);
    result = _lifecycle_begin_transition(
                 LIFECYCLE_TRANSITION_FORWARD_APP,
                 source_app, source_page, target_app, target_page,
                 app_created, false, &transition, completion, context);
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

    const app_manager_app_desc_t *target_descriptor = target_app != NULL ?
        target_app->descriptor :
        app_manager_builtin_find_by_id(request->app_id);
    if (app_manager_builtin_find_page(target_descriptor,
                                      request->page_id) == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (target_app == NULL)
    {
        result = _lifecycle_create_app(request->app_id, request->page_id,
                                       request, &target_app, &target_page);
        app_created = result == ESP_OK;
        if (result != ESP_OK)
        {
            return result;
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
                return ESP_OK;
            }
            result = target_app == source_app ?
                     _lifecycle_trim_between(target_app, source_page,
                                             target_page) :
                     _lifecycle_trim_to_background_target(target_app,
                         target_page);
            if (result != ESP_OK)
            {
                return result;
            }
        }
        else
        {
            const app_manager_page_route_t *route =
                app_manager_builtin_find_page(target_app->descriptor,
                                              request->page_id);
            if (route == NULL)
            {
                return ESP_ERR_NOT_FOUND;
            }
            result = _lifecycle_create_page(target_app, route, request,
                                            false,
                                            &target_page);
            page_created = result == ESP_OK;
            if (result != ESP_OK)
            {
                return result;
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
        return result;
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
        return result;
    }
    result = _lifecycle_prepare_target(target_page, source);
    if (result != ESP_OK)
    {
        if (target_page->mounted)
        {
            (void)_lifecycle_unmount_page(target_page, true);
        }
        return result;
    }
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request, &s_settings.page_back);
    result = _lifecycle_begin_transition(
                 LIFECYCLE_TRANSITION_BACK_PAGE, app, source, app, target_page,
                 false, false, &transition, completion, context);
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
            return result;
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
            return result;
        }
    }
    app_manager_transition_t transition =
        _lifecycle_resolve_request_transition(request, &s_settings.app_back);
    result = _lifecycle_begin_transition(
                 LIFECYCLE_TRANSITION_BACK_APP, source_app, source_page,
                 target_app, target_page, false, false, &transition,
                 completion, context);
    return result;
}

static esp_err_t _lifecycle_back(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    if (s_model.active_app == NULL || s_model.active_app->current_page == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (request->app_id != NULL &&
            strcmp(request->app_id, s_model.active_app->descriptor->id) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    app_manager_page_node_t *source = s_model.active_app->current_page;
    if (source->previous != NULL)
    {
        return _lifecycle_back_page(request, source->previous,
                                    completion, context);
    }
    return _lifecycle_back_app(request, completion, context);
}

static esp_err_t _lifecycle_back_to(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    app_manager_app_node_t *app = s_model.active_app;
    if (app == NULL || (request->app_id != NULL &&
                        strcmp(request->app_id, app->descriptor->id) != 0))
    {
        return ESP_ERR_NOT_FOUND;
    }
    app_manager_page_node_t *target = app_manager_nav_model_find_page(
                                          app, request->page_id);
    if (target == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (target == app->current_page)
    {
        completion(ESP_OK, context);
        return ESP_OK;
    }
    return _lifecycle_back_page(request, target, completion, context);
}

static esp_err_t _lifecycle_exit_app(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    const char *app_id = request->app_id;
    if (app_id == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    app_manager_app_node_t *app = app_manager_nav_model_find_app(&s_model,
                                  app_id);
    if (app == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (app == s_model.active_app)
    {
        return _lifecycle_back_app(request, completion, context);
    }
    esp_err_t result = _lifecycle_destroy_background_app(app);
    if (result == ESP_OK)
    {
        completion(ESP_OK, context);
    }
    return result;
}

static esp_err_t _lifecycle_remove_page(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    app_manager_app_node_t *app = request->app_id != NULL ?
                                  app_manager_nav_model_find_app(
                                      &s_model, request->app_id) :
                                  s_model.active_app;
    if (app == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    app_manager_page_node_t *page = app_manager_nav_model_find_page(
                                        app, request->page_id);
    if (page == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (app == s_model.active_app && page == app->current_page)
    {
        return _lifecycle_back(request, completion, context);
    }
    esp_err_t result = _lifecycle_stop_page(page);
    if (result != ESP_OK)
    {
        return result;
    }
    result = _lifecycle_free_page(page);
    if (result != ESP_OK)
    {
        return result;
    }
    if (app->current_page == NULL)
    {
        result = _lifecycle_free_app(app);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    completion(ESP_OK, context);
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
            return result;
        }
    }
    completion(ESP_OK, context);
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
        return false;
    }
    if (request->has_arguments &&
            (request->arguments.version != APP_MANAGER_TYPED_BLOB_VERSION ||
             request->arguments.size > APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES ||
             request->arguments.reserved != 0U))
    {
        return false;
    }
    if (request->app_id != NULL &&
            !app_manager_lifecycle_id_is_valid(request->app_id))
    {
        return false;
    }
    if (request->page_id != NULL &&
            !app_manager_lifecycle_id_is_valid(request->page_id))
    {
        return false;
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
    return valid;
}

esp_err_t app_manager_lifecycle_navigate(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    if (!_lifecycle_request_valid(request) || completion == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_tearing_down ||
            !app_manager_mailbox_is_worker() ||
            !app_manager_lifecycle_screen_mutation_allowed() ||
            s_handler_page != NULL ||
            atomic_load_explicit(&s_transitioning, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _lifecycle_retry_target_recovery();
    if (result != ESP_OK)
    {
        return result;
    }
    result = _lifecycle_retry_source_recovery();
    if (result != ESP_OK)
    {
        return result;
    }
    result = _lifecycle_retry_pending_cleanup();
    if (result != ESP_OK)
    {
        return result;
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
    return result;
}

esp_err_t app_manager_lifecycle_init(void)
{
    esp_err_t result = ESP_OK;
    int expected = LIFECYCLE_SCREEN_CLOSED;

    if (s_initialized || !atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_INITIALIZING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
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
    result = app_manager_nav_model_init(&s_model);
    if (result != ESP_OK)
    {
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
    return ESP_OK;

fail:
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_CLOSED,
                          memory_order_release);
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
        return ESP_ERR_INVALID_STATE;
    }
    s_tearing_down = true;
    app_manager_presentation_finish_now();
    result = _lifecycle_retry_pending_cleanup();
    if (result != ESP_OK)
    {
        return result;
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
                return result;
            }
            app->active = false;
        }
        if (app->current_page != NULL && app->current_page->mounted)
        {
            result = app_manager_presentation_load_immediate(
                         app_manager_presentation_neutral_screen());
            if (result != ESP_OK)
            {
                return result;
            }
        }
        result = _lifecycle_destroy_background_app(app);
        if (result != ESP_OK)
        {
            return result;
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
    return ESP_OK;
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
    if (app_manager_mailbox_is_worker() ||
            atomic_load(&s_screen_phase) != LIFECYCLE_SCREEN_PAUSED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    while (atomic_load(&s_screen_accesses) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        vTaskDelay(1);
#endif
    }
    return atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_PAUSED ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t app_manager_lifecycle_shutdown_begin_and_wait(void)
{
    if (app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
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
    return ESP_OK;
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
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_PAUSED)
    {
        return ESP_OK;
    }
    app_manager_presentation_finish_now();
    if (atomic_load_explicit(&s_transitioning, memory_order_acquire) ||
            app_manager_presentation_is_running())
    {
        return ESP_ERR_INVALID_STATE;
    }
    result = _lifecycle_retry_pending_cleanup();
    if (result != ESP_OK)
    {
        return result;
    }
    app = s_model.active_app;
    page = app != NULL ? app->current_page : NULL;
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_PAUSING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return result;
    }
    result = _lifecycle_pause_page(page);
    if (result != ESP_OK)
    {
        atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE);
        return result;
    }
    result = app_manager_presentation_load_immediate(
                 app_manager_presentation_neutral_screen());
    if (result != ESP_OK)
    {
        (void)_lifecycle_resume_page(page);
        atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE);
        return result;
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
                return result;
            }
        }
        if (app != NULL)
        {
            app->active = true;
        }
        atomic_store(&s_screen_phase, LIFECYCLE_SCREEN_ACTIVE);
        return result;
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
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_ACTIVE)
    {
        return ESP_OK;
    }
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_RESUMING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
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

app_manager_lifecycle_back_target_t app_manager_lifecycle_back_target(
    const char **source_app_id)
{
    app_manager_lifecycle_back_target_t target =
        APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE;
    app_manager_app_node_t *source_app = s_model.active_app;
    app_manager_page_node_t *source_page = source_app != NULL ?
                                           source_app->current_page : NULL;

    if (source_app_id != NULL)
    {
        *source_app_id = NULL;
    }
    if (source_app_id == NULL || !s_initialized || s_tearing_down ||
            !app_manager_mailbox_is_worker() ||
            !_lifecycle_topology_query_allowed() || s_handler_page != NULL ||
            !app_manager_nav_model_invariants(&s_model) || source_app == NULL ||
            !source_app->in_use || source_app->cleanup_pending ||
            !source_app->active || source_app->descriptor == NULL ||
            source_page == NULL || !source_page->in_use ||
            source_page->cleanup_pending || source_page->unmount_pending ||
            !source_page->started || !source_page->mounted ||
            !source_page->active || source_page->app != source_app)
    {
        return APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE;
    }

    app_manager_page_node_t *target_page = source_page->previous;
    if (target_page != NULL)
    {
        if (!target_page->in_use || target_page->cleanup_pending ||
                target_page->unmount_pending || !target_page->started ||
                target_page->active || target_page->app != source_app)
        {
            return APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE;
        }
        target = APP_MANAGER_LIFECYCLE_BACK_TARGET_PAGE;
    }
    else
    {
        app_manager_app_node_t *target_app = source_app->previous;
        target_page = target_app != NULL ? target_app->current_page : NULL;
        if (target_app == NULL || !target_app->in_use ||
                target_app->cleanup_pending || target_app->active ||
                target_app->descriptor == NULL || target_page == NULL ||
                !target_page->in_use || target_page->cleanup_pending ||
                target_page->unmount_pending || !target_page->started ||
                target_page->active || target_page->app != target_app)
        {
            return APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE;
        }
        target = APP_MANAGER_LIFECYCLE_BACK_TARGET_APP;
    }
    *source_app_id = source_app->descriptor->id;
    return target;
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
    return s_handler_page != NULL && s_handler_page->route != NULL ?
           s_handler_page->route->user_data : NULL;
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
