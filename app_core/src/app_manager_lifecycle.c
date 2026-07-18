#define DBG_TAG "app_lifecycle"
#define DBG_LVL DBG_INFO

#ifndef HOST_TEST
    #include "mt_log.h"
    #include "esp_heap_caps.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
#else
    #include <stdio.h>
    #include <stdlib.h>
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
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define APP_MANAGER_MAX_APPS             8
#define APP_MANAGER_MAX_PAGES            12
#define APP_MANAGER_PAGE_ID_BYTES        32
#define APP_MANAGER_DEFERRED_COMMANDS     8
#define APP_MANAGER_DEFAULT_ARENA_BYTES   (32U * 1024U)

typedef struct app_node app_node_t;
typedef struct page_node page_node_t;

struct page_node
{
    bool in_use;
    bool started;
    bool active;
    uint32_t generation;
    char id[APP_MANAGER_PAGE_ID_BYTES];
    app_page_msg_cb_t handler;
    void *user_data;
    void *private_memory;
    size_t private_memory_size;
    page_node_t *previous;
    page_node_t *next;
    app_node_t *app;
};

struct app_node
{
    bool in_use;
    bool active;
    const app_builtin_app_desc_t *descriptor;
    page_node_t *current_page;
    app_node_t *previous;
    app_node_t *next;
};

typedef enum
{
    DEFERRED_RUN = 0,
    DEFERRED_EXIT,
    DEFERRED_GOBACK,
    DEFERRED_GOBACK_TO,
    DEFERRED_CREATE_PAGE,
    DEFERRED_REMOVE_PAGE,
} deferred_type_t;

typedef struct deferred_command
{
    deferred_type_t type;
    char app_id[APP_MANAGER_PAGE_ID_BYTES];
    char page_id[APP_MANAGER_PAGE_ID_BYTES];
    app_page_msg_cb_t handler;
    void *user_data;
    uint32_t memory_size;
} deferred_command_t;

typedef enum lifecycle_screen_phase
{
    LIFECYCLE_SCREEN_CLOSED = 0,
    LIFECYCLE_SCREEN_INITIALIZING,
    LIFECYCLE_SCREEN_ACTIVE,
    LIFECYCLE_SCREEN_PAUSING,
    LIFECYCLE_SCREEN_PAUSED,
} lifecycle_screen_phase_t;

static app_node_t s_apps[APP_MANAGER_MAX_APPS];
static page_node_t s_pages[APP_MANAGER_MAX_PAGES];
static deferred_command_t s_deferred[APP_MANAGER_DEFERRED_COMMANDS];
static size_t s_deferred_head;
static size_t s_deferred_count;
static app_node_t *s_active_app;
static app_node_t *s_entry_app;
static page_node_t *s_handler_page;
static esp_err_t s_handler_cleanup_error;
static uint32_t s_callback_depth;
static uint32_t s_operation_depth;
static uint8_t *s_page_arena;
static size_t s_page_stride;
static bool s_initialized;
static bool s_tearing_down;
static atomic_int s_screen_phase = ATOMIC_VAR_INIT(LIFECYCLE_SCREEN_CLOSED);
static atomic_uint s_screen_accesses = ATOMIC_VAR_INIT(0U);
static atomic_uint s_paused_running_apps;

#ifdef HOST_TEST
    static _Atomic(app_manager_lifecycle_observer_t) s_host_observer;
#endif

static const char *const s_lifecycle_message_names[APP_MANAGER_MSG_USER_END] =
{
    [APP_MANAGER_MSG_ONSTART] = "START",
    [APP_MANAGER_MSG_ONRESUME] = "RESUME",
    [APP_MANAGER_MSG_ONPAUSE] = "PAUSE",
    [APP_MANAGER_MSG_ONSTOP] = "STOP",
};

static void _lifecycle_copy_id(char *destination, const char *source)
{
    strncpy(destination, source, APP_MANAGER_PAGE_ID_BYTES - 1U);
    destination[APP_MANAGER_PAGE_ID_BYTES - 1U] = '\0';
}

bool app_manager_lifecycle_id_is_valid(const char *id)
{
    return id != NULL && id[0] != '\0' &&
           strnlen(id, APP_MANAGER_PAGE_ID_BYTES) < APP_MANAGER_PAGE_ID_BYTES;
}

static bool _lifecycle_should_defer(void)
{
    return s_callback_depth > 0 || s_operation_depth > 0;
}

static const char *_lifecycle_request_app_id(void)
{
    const char *app_id = NULL;
    if (s_handler_page != NULL && s_handler_page->app != NULL)
    {
        app_id = s_handler_page->app->descriptor->id;
    }
    else if (s_entry_app != NULL)
    {
        app_id = s_entry_app->descriptor->id;
    }
    else if (s_active_app != NULL)
    {
        app_id = s_active_app->descriptor->id;
    }
    return app_id;
}

static app_node_t *_lifecycle_find_app(const char *app_id)
{
    app_node_t *match = NULL;
    for (size_t i = 0; i < APP_MANAGER_MAX_APPS; i++)
    {
        if (s_apps[i].in_use && strcmp(s_apps[i].descriptor->id, app_id) == 0)
        {
            match = &s_apps[i];
            break;
        }
    }
    return match;
}

static page_node_t *_lifecycle_find_page(app_node_t *app, const char *page_id)
{
    page_node_t *match = NULL;
    if (app == NULL)
    {
        goto exit;
    }
    for (size_t i = 0; i < APP_MANAGER_MAX_PAGES; i++)
    {
        if (s_pages[i].in_use && s_pages[i].app == app &&
                strcmp(s_pages[i].id, page_id) == 0)
        {
            match = &s_pages[i];
            break;
        }
    }

exit:
    return match;
}

static app_node_t *_lifecycle_alloc_app(void)
{
    app_node_t *app = NULL;
    for (size_t i = 0; i < APP_MANAGER_MAX_APPS; i++)
    {
        if (!s_apps[i].in_use)
        {
            memset(&s_apps[i], 0, sizeof(s_apps[i]));
            s_apps[i].in_use = true;
            app = &s_apps[i];
            break;
        }
    }
    return app;
}

static page_node_t *_lifecycle_alloc_page(size_t memory_size)
{
    page_node_t *page = NULL;
    if (memory_size > s_page_stride)
    {
        goto exit;
    }
    for (size_t i = 0; i < APP_MANAGER_MAX_PAGES; i++)
    {
        if (!s_pages[i].in_use)
        {
            uint32_t generation = s_pages[i].generation + 1U;
            memset(&s_pages[i], 0, sizeof(s_pages[i]));
            s_pages[i].in_use = true;
            s_pages[i].generation = generation;
            s_pages[i].private_memory = memory_size > 0 ?
                                        &s_page_arena[i * s_page_stride] : NULL;
            s_pages[i].private_memory_size = memory_size;
            if (memory_size > 0)
            {
                memset(s_pages[i].private_memory, 0, memory_size);
            }
            page = &s_pages[i];
            break;
        }
    }

exit:
    return page;
}

static void _lifecycle_free_page(page_node_t *page)
{
    uint32_t generation = page->generation;
    size_t index = (size_t)(page - s_pages);
    if (page->private_memory_size > 0)
    {
        memset(&s_page_arena[index * s_page_stride], 0,
               page->private_memory_size);
    }
    memset(page, 0, sizeof(*page));
    page->generation = generation;
}

static esp_err_t _lifecycle_call_handler(page_node_t *page,
        app_manager_msg_type_t message)
{
    esp_err_t result = ESP_OK;
    if (page == NULL || page->handler == NULL || !page->in_use)
    {
        goto exit;
    }

    page_node_t *previous_context = s_handler_page;
    esp_err_t previous_cleanup_error = s_handler_cleanup_error;
    uint32_t generation = page->generation;
    s_handler_cleanup_error = ESP_OK;
    s_handler_page = page;
    s_callback_depth++;
#ifdef HOST_TEST
    app_manager_lifecycle_observer_t observer = atomic_load(&s_host_observer);
    if (observer != NULL)
    {
        observer(page->app->descriptor->id, page->id, message,
                 APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE);
    }
#endif
    LOG_I("[%s][%s] ON %s", page->app->descriptor->name, page->id,
          s_lifecycle_message_names[message]);
    page->handler(message, page->user_data);
#ifdef HOST_TEST
    if (observer != NULL)
    {
        observer(page->app->descriptor->id, page->id, message,
                 APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    }
#endif
    s_callback_depth--;
    s_handler_page = previous_context;
    esp_err_t cleanup_error = s_handler_cleanup_error;
    s_handler_cleanup_error = previous_cleanup_error;

    if (!page->in_use || page->generation != generation)
    {
        goto exit;
    }
    if (message == APP_MANAGER_MSG_ONPAUSE ||
            message == APP_MANAGER_MSG_ONSTOP)
    {
        result = cleanup_error;
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (message == APP_MANAGER_MSG_ONSTART)
    {
        page->started = true;
    }
    else if (message == APP_MANAGER_MSG_ONRESUME)
    {
        page->active = true;
    }
    else if (message == APP_MANAGER_MSG_ONPAUSE)
    {
        page->active = false;
    }
    else if (message == APP_MANAGER_MSG_ONSTOP)
    {
        page->started = false;
    }

exit:
    return result;
}

static esp_err_t _lifecycle_pause_app(app_node_t *app)
{
    esp_err_t result = ESP_OK;
    if (app != NULL && app->active)
    {
        result = _lifecycle_call_handler(app->current_page,
                                         APP_MANAGER_MSG_ONPAUSE);
        if (result == ESP_OK)
        {
            app->active = false;
        }
    }
    return result;
}

static void _lifecycle_resume_app(app_node_t *app)
{
    if (app != NULL)
    {
        app->active = true;
        s_active_app = app;
        _lifecycle_call_handler(app->current_page, APP_MANAGER_MSG_ONRESUME);
    }
}

static void _lifecycle_stack_remove(app_node_t *app)
{
    if (app->previous != NULL)
    {
        app->previous->next = app->next;
    }
    if (app->next != NULL)
    {
        app->next->previous = app->previous;
    }
    app->previous = NULL;
    app->next = NULL;
}

static void _lifecycle_stack_push(app_node_t *app)
{
    if (s_active_app != NULL)
    {
        s_active_app->next = app;
        app->previous = s_active_app;
    }
    app->next = NULL;
    s_active_app = app;
}

static void _lifecycle_unlink_page(page_node_t *page)
{
    if (page->previous != NULL)
    {
        page->previous->next = page->next;
    }
    if (page->next != NULL)
    {
        page->next->previous = page->previous;
    }
    if (page->app != NULL && page->app->current_page == page)
    {
        page->app->current_page = page->previous;
    }
    page->previous = NULL;
    page->next = NULL;
}

static esp_err_t _lifecycle_stop_page(page_node_t *page)
{
    esp_err_t result = ESP_OK;
    if (page->active)
    {
        result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONPAUSE);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (page->in_use && page->started)
    {
        result = _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTOP);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    if (page->in_use)
    {
        _lifecycle_unlink_page(page);
        _lifecycle_free_page(page);
    }

exit:
    return result;
}

static esp_err_t _lifecycle_destroy_app(app_node_t *app)
{
    esp_err_t result = ESP_OK;
    page_node_t *page = app->current_page;
    while (page != NULL)
    {
        page_node_t *previous = page->previous;
        result = _lifecycle_stop_page(page);
        if (result != ESP_OK)
        {
            goto exit;
        }
        page = previous;
    }
    _lifecycle_stack_remove(app);
    memset(app, 0, sizeof(*app));

exit:
    return result;
}

static esp_err_t _lifecycle_enqueue(deferred_type_t type, const char *app_id,
                                    const char *page_id,
                                    app_page_msg_cb_t handler, void *user_data,
                                    uint32_t memory_size)
{
    esp_err_t result = ESP_OK;
    if (s_tearing_down)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_deferred_count == APP_MANAGER_DEFERRED_COMMANDS)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    size_t index = (s_deferred_head + s_deferred_count) % APP_MANAGER_DEFERRED_COMMANDS;
    deferred_command_t *command = &s_deferred[index];
    memset(command, 0, sizeof(*command));
    command->type = type;
    if (app_id != NULL)
    {
        _lifecycle_copy_id(command->app_id, app_id);
    }
    if (page_id != NULL)
    {
        _lifecycle_copy_id(command->page_id, page_id);
    }
    command->handler = handler;
    command->user_data = user_data;
    command->memory_size = memory_size;
    s_deferred_count++;

exit:
    return result;
}

static esp_err_t _lifecycle_run_now(const char *app_id);
static esp_err_t _lifecycle_exit_now(const char *app_id);
static esp_err_t _lifecycle_goback_now(const char *app_id);
static esp_err_t _lifecycle_goback_to_now(const char *page_id,
        const char *app_id);
static esp_err_t _lifecycle_create_page_now(const char *page_id,
        const char *app_id,
        app_page_msg_cb_t handler,
        void *user_data,
        uint32_t memory_size);
static esp_err_t _lifecycle_remove_page_now(const char *page_id,
        const char *app_id);

static void _lifecycle_drain_deferred(void)
{
    while (s_deferred_count > 0 && s_callback_depth == 0)
    {
        deferred_command_t command = s_deferred[s_deferred_head];
        s_deferred_head = (s_deferred_head + 1U) % APP_MANAGER_DEFERRED_COMMANDS;
        s_deferred_count--;
        s_operation_depth++;
        switch (command.type)
        {
        case DEFERRED_RUN:
            (void)_lifecycle_run_now(command.app_id);
            break;
        case DEFERRED_EXIT:
            (void)_lifecycle_exit_now(command.app_id);
            break;
        case DEFERRED_GOBACK:
            (void)_lifecycle_goback_now(command.app_id);
            break;
        case DEFERRED_GOBACK_TO:
            (void)_lifecycle_goback_to_now(command.page_id, command.app_id);
            break;
        case DEFERRED_CREATE_PAGE:
            (void)_lifecycle_create_page_now(command.page_id,
                                             command.app_id[0] != '\0' ?
                                             command.app_id : NULL,
                                             command.handler,
                                             command.user_data,
                                             command.memory_size);
            break;
        case DEFERRED_REMOVE_PAGE:
            (void)_lifecycle_remove_page_now(command.page_id, command.app_id);
            break;
        }
        s_operation_depth--;
    }
}

static void _lifecycle_operation_end(void)
{
    s_operation_depth--;
    if (s_operation_depth == 0 && s_callback_depth == 0)
    {
        _lifecycle_drain_deferred();
    }
}

static esp_err_t _lifecycle_run_now(const char *app_id)
{
    esp_err_t result = ESP_OK;
    app_node_t *existing = _lifecycle_find_app(app_id);
    if (existing != NULL)
    {
        if (existing != s_active_app)
        {
            result = _lifecycle_pause_app(s_active_app);
            if (result != ESP_OK)
            {
                goto exit;
            }
            _lifecycle_stack_remove(existing);
            _lifecycle_stack_push(existing);
            _lifecycle_resume_app(existing);
        }
        goto exit;
    }

    const app_builtin_app_desc_t *descriptor =
        app_manager_builtin_find_by_id(app_id);
    if (descriptor == NULL || descriptor->entry == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    app_node_t *previous = s_active_app;
    app_node_t *app = _lifecycle_alloc_app();
    if (app == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    result = _lifecycle_pause_app(previous);
    if (result != ESP_OK)
    {
        memset(app, 0, sizeof(*app));
        goto exit;
    }
    app->descriptor = descriptor;
    app->active = true;
    _lifecycle_stack_push(app);

    size_t deferred_count_before_entry = s_deferred_count;
    s_entry_app = app;
    result = descriptor->entry();
    s_entry_app = NULL;
    if (result != ESP_OK || app->current_page == NULL)
    {
        result = result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
        s_deferred_count = deferred_count_before_entry;
        esp_err_t cleanup_result = _lifecycle_destroy_app(app);
        s_deferred_count = deferred_count_before_entry;
        if (cleanup_result == ESP_OK)
        {
            s_active_app = previous;
            _lifecycle_resume_app(previous);
        }
    }

exit:
    return result;
}

static esp_err_t _lifecycle_exit_now(const char *app_id)
{
    esp_err_t result = ESP_OK;
    app_node_t *app = _lifecycle_find_app(app_id);
    if (app == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    bool was_active = app == s_active_app;
    app_node_t *resume = app->previous;
    result = _lifecycle_destroy_app(app);
    if (result == ESP_OK && was_active)
    {
        s_active_app = resume;
        _lifecycle_resume_app(resume);
    }

exit:
    return result;
}

static esp_err_t _lifecycle_create_page_now(const char *page_id,
        const char *app_id,
        app_page_msg_cb_t handler,
        void *user_data,
        uint32_t memory_size)
{
    esp_err_t result = ESP_OK;
    if (s_active_app == NULL ||
            (app_id != NULL && strcmp(s_active_app->descriptor->id, app_id) != 0))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    page_node_t *existing = _lifecycle_find_page(s_active_app, page_id);
    if (existing != NULL)
    {
        result = _lifecycle_goback_to_now(page_id, app_id);
        goto exit;
    }

    page_node_t *page = _lifecycle_alloc_page(memory_size);
    if (page == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    _lifecycle_copy_id(page->id, page_id);
    page->handler = handler;
    page->user_data = user_data;
    page->app = s_active_app;

    page_node_t *old = s_active_app->current_page;
    result = _lifecycle_call_handler(old, APP_MANAGER_MSG_ONPAUSE);
    if (result != ESP_OK)
    {
        _lifecycle_free_page(page);
        goto exit;
    }
    page->previous = old;
    if (old != NULL)
    {
        old->next = page;
    }
    s_active_app->current_page = page;
    _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTART);
    _lifecycle_call_handler(page, APP_MANAGER_MSG_ONRESUME);

exit:
    return result;
}

static esp_err_t _lifecycle_goback_now(const char *app_id)
{
    esp_err_t result = ESP_OK;
    if (s_active_app == NULL || s_active_app->current_page == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (app_id != NULL && strcmp(s_active_app->descriptor->id, app_id) != 0)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    page_node_t *current = s_active_app->current_page;
    if (current->previous == NULL)
    {
        result = _lifecycle_exit_now(s_active_app->descriptor->id);
        goto exit;
    }
    page_node_t *previous = current->previous;
    result = _lifecycle_stop_page(current);
    if (result != ESP_OK)
    {
        goto exit;
    }
    _lifecycle_call_handler(previous, APP_MANAGER_MSG_ONRESUME);

exit:
    return result;
}

static esp_err_t _lifecycle_goback_to_now(const char *page_id,
        const char *app_id)
{
    esp_err_t result = ESP_OK;
    if (s_active_app == NULL ||
            (app_id != NULL &&
             strcmp(s_active_app->descriptor->id, app_id) != 0) ||
            _lifecycle_find_page(s_active_app, page_id) == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    while (s_active_app != NULL && s_active_app->current_page != NULL &&
            strcmp(s_active_app->current_page->id, page_id) != 0)
    {
        result = _lifecycle_goback_now(app_id);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }

exit:
    return result;
}

static esp_err_t _lifecycle_remove_page_now(const char *page_id,
        const char *app_id)
{
    esp_err_t result = ESP_OK;
    if (s_active_app == NULL ||
            (app_id != NULL && strcmp(s_active_app->descriptor->id, app_id) != 0))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    page_node_t *page = _lifecycle_find_page(s_active_app, page_id);
    if (page == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    if (page == s_active_app->current_page)
    {
        result = _lifecycle_goback_now(app_id);
        goto exit;
    }
    result = _lifecycle_stop_page(page);

exit:
    return result;
}

esp_err_t app_manager_lifecycle_init(size_t arena_bytes)
{
    esp_err_t result = ESP_OK;
    bool phase_owned = false;
    if (s_initialized)
    {
        if (atomic_load(&s_screen_phase) == LIFECYCLE_SCREEN_CLOSED)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        goto exit;
    }
    int expected = LIFECYCLE_SCREEN_CLOSED;
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_INITIALIZING,
                memory_order_acq_rel, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    phase_owned = true;
    while (atomic_load(&s_screen_accesses) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        vTaskDelay(1);
#endif
    }
    if (arena_bytes == 0)
    {
        arena_bytes = APP_MANAGER_DEFAULT_ARENA_BYTES;
    }
    s_page_stride = (arena_bytes / APP_MANAGER_MAX_PAGES) & ~(size_t)7U;
    if (s_page_stride == 0)
    {
        result = ESP_ERR_INVALID_SIZE;
        goto exit;
    }
#ifndef HOST_TEST
    s_page_arena = heap_caps_aligned_calloc(
                       8, 1, s_page_stride * APP_MANAGER_MAX_PAGES,
                       MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (s_page_arena == NULL)
    {
        s_page_arena = heap_caps_aligned_calloc(
                           8, 1, s_page_stride * APP_MANAGER_MAX_PAGES,
                           MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
#else
    s_page_arena = calloc(1, s_page_stride * APP_MANAGER_MAX_PAGES);
#endif
    if (s_page_arena == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    memset(s_apps, 0, sizeof(s_apps));
    memset(s_pages, 0, sizeof(s_pages));
    memset(s_deferred, 0, sizeof(s_deferred));
    s_active_app = NULL;
    s_entry_app = NULL;
    s_handler_page = NULL;
    s_handler_cleanup_error = ESP_OK;
    s_deferred_head = 0;
    s_deferred_count = 0;
    s_callback_depth = 0;
    s_operation_depth = 0;
    s_tearing_down = false;
    atomic_store(&s_paused_running_apps, 0U);
    s_initialized = true;
    expected = LIFECYCLE_SCREEN_INITIALIZING;
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_ACTIVE,
                memory_order_release, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        app_manager_lifecycle_force_deinit();
        goto exit;
    }

exit:
    if (phase_owned && !s_initialized)
    {
        atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_CLOSED,
                              memory_order_release);
    }
    return result;
}

void app_manager_lifecycle_force_deinit(void)
{
    atomic_store_explicit(&s_screen_phase, LIFECYCLE_SCREEN_CLOSED,
                          memory_order_release);
    if (s_initialized)
    {
#ifndef HOST_TEST
        heap_caps_free(s_page_arena);
#else
        free(s_page_arena);
#endif
        s_page_arena = NULL;
        s_page_stride = 0;
        s_active_app = NULL;
        s_entry_app = NULL;
        s_handler_page = NULL;
        s_handler_cleanup_error = ESP_OK;
        s_deferred_head = 0;
        s_deferred_count = 0;
        s_callback_depth = 0;
        s_operation_depth = 0;
        s_tearing_down = false;
        s_initialized = false;
    }
    atomic_store(&s_paused_running_apps, 0U);
}

esp_err_t app_manager_lifecycle_deinit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_initialized)
    {
        if (atomic_load(&s_screen_phase) != LIFECYCLE_SCREEN_CLOSED ||
                atomic_load(&s_screen_accesses) != 0U)
        {
            goto exit;
        }
        s_tearing_down = true;
        memset(s_deferred, 0, sizeof(s_deferred));
        s_deferred_head = 0;
        s_deferred_count = 0;

        while (s_active_app != NULL)
        {
            app_node_t *app = s_active_app;
            app_node_t *previous = app->previous;
            result = _lifecycle_destroy_app(app);
            if (result != ESP_OK)
            {
                goto exit;
            }
            s_active_app = previous;
        }

        memset(s_deferred, 0, sizeof(s_deferred));
        s_deferred_head = 0;
        s_deferred_count = 0;
        app_manager_lifecycle_force_deinit();
    }
    result = ESP_OK;

exit:
    return result;
}

bool app_manager_lifecycle_screen_mutation_allowed(void)
{
    return atomic_load_explicit(&s_screen_phase, memory_order_acquire) ==
           LIFECYCLE_SCREEN_ACTIVE;
}

static uint32_t _lifecycle_running_app_count(void)
{
    uint32_t count = 0;
    for (size_t i = 0; i < APP_MANAGER_MAX_APPS; i++)
    {
        count += s_apps[i].in_use ? 1U : 0U;
    }
    return count;
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
    if (atomic_load(&s_screen_phase) != LIFECYCLE_SCREEN_PAUSED)
    {
        goto exit;
    }
    result = ESP_OK;

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
    bool available = false;
    uint32_t snapshot = 0;
    if (count == NULL ||
            atomic_load_explicit(&s_screen_phase, memory_order_acquire) !=
            LIFECYCLE_SCREEN_PAUSED)
    {
        goto exit;
    }
    snapshot = atomic_load(&s_paused_running_apps);
    available = atomic_load_explicit(&s_screen_phase, memory_order_acquire) ==
                LIFECYCLE_SCREEN_PAUSED;
    if (available)
    {
        *count = snapshot;
    }

exit:
    return available;
}

esp_err_t app_manager_lifecycle_screen_pause(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    lifecycle_screen_phase_t phase = atomic_load(&s_screen_phase);
    if (!s_initialized || s_tearing_down || !app_manager_mailbox_is_worker())
    {
        goto exit;
    }
    if (phase == LIFECYCLE_SCREEN_PAUSED)
    {
        result = ESP_OK;
        goto exit;
    }
    if (phase != LIFECYCLE_SCREEN_ACTIVE || _lifecycle_should_defer())
    {
        goto exit;
    }

    int pause_expected = LIFECYCLE_SCREEN_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &pause_expected, LIFECYCLE_SCREEN_PAUSING,
                memory_order_acq_rel, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    s_operation_depth++;
    result = _lifecycle_pause_app(s_active_app);
    _lifecycle_operation_end();
    lifecycle_screen_phase_t next_phase = result == ESP_OK ?
                                          LIFECYCLE_SCREEN_PAUSED :
                                          LIFECYCLE_SCREEN_ACTIVE;
    if (result == ESP_OK)
    {
        atomic_store(&s_paused_running_apps, _lifecycle_running_app_count());
    }
    int expected = LIFECYCLE_SCREEN_PAUSING;
    (void)atomic_compare_exchange_strong_explicit(
        &s_screen_phase, &expected, next_phase,
        memory_order_release, memory_order_relaxed);

exit:
    return result;
}

esp_err_t app_manager_lifecycle_screen_resume(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    lifecycle_screen_phase_t phase = atomic_load(&s_screen_phase);
    if (!s_initialized || s_tearing_down || !app_manager_mailbox_is_worker())
    {
        goto exit;
    }
    if (phase == LIFECYCLE_SCREEN_ACTIVE)
    {
        result = ESP_OK;
        goto exit;
    }
    if (phase != LIFECYCLE_SCREEN_PAUSED || _lifecycle_should_defer())
    {
        goto exit;
    }

    int expected = LIFECYCLE_SCREEN_PAUSED;
    if (!atomic_compare_exchange_strong_explicit(
                &s_screen_phase, &expected, LIFECYCLE_SCREEN_ACTIVE,
                memory_order_acq_rel, memory_order_acquire))
    {
        goto exit;
    }
    s_operation_depth++;
    _lifecycle_resume_app(s_active_app);
    _lifecycle_operation_end();
    result = ESP_OK;

exit:
    return result;
}

esp_err_t app_manager_lifecycle_run_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_RUN, request->app_id,
                                    NULL, NULL, NULL, 0);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_run_now(request->app_id);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_exit_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_EXIT, request->app_id,
                                    NULL, NULL, NULL, 0);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_exit_now(request->app_id);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_self_exit_call(void *opaque)
{
    (void)opaque;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    const char *app_id = _lifecycle_request_app_id();
    if (app_id == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_EXIT, app_id, NULL, NULL, NULL, 0);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_exit_now(app_id);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_create_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    const char *app_id = request->app_id;
    if (app_id == NULL)
    {
        app_id = _lifecycle_request_app_id();
    }
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_CREATE_PAGE, app_id,
                                    request->page_id, request->handler,
                                    request->user_data, request->memory_size);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_create_page_now(
                     request->page_id, app_id, request->handler,
                     request->user_data, request->memory_size);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_goback_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    const char *app_id = request->app_id != NULL ?
                         request->app_id : _lifecycle_request_app_id();
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_GOBACK, app_id,
                                    NULL, NULL, NULL, 0);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_goback_now(app_id);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_goback_to_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    const char *app_id = request->app_id != NULL ?
                         request->app_id : _lifecycle_request_app_id();
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_GOBACK_TO, app_id,
                                    request->page_id, NULL, NULL, 0);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_goback_to_now(request->page_id, app_id);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_remove_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    const char *app_id = request->app_id != NULL ?
                         request->app_id : _lifecycle_request_app_id();
    if (_lifecycle_should_defer())
    {
        result = _lifecycle_enqueue(DEFERRED_REMOVE_PAGE, app_id,
                                    request->page_id, NULL, NULL, 0);
    }
    else
    {
        s_operation_depth++;
        result = _lifecycle_remove_page_now(request->page_id, app_id);
        _lifecycle_operation_end();
    }

exit:
    return result;
}

esp_err_t app_manager_lifecycle_get_userdata_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    page_node_t *page = _lifecycle_find_page(s_active_app, request->page_id);
    request->value = page != NULL ? page->user_data : NULL;
    return page != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t app_manager_lifecycle_set_userdata_call(void *opaque)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    app_manager_lifecycle_request_t *request = opaque;
    if (!s_initialized || !app_manager_lifecycle_screen_mutation_allowed())
    {
        goto exit;
    }
    page_node_t *page = _lifecycle_find_page(s_active_app, request->page_id);
    if (page == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    page->user_data = request->value;
    result = ESP_OK;

exit:
    return result;
}

esp_err_t app_manager_lifecycle_is_active_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->condition = app_manager_lifecycle_screen_mutation_allowed() &&
                         s_active_app != NULL &&
                         strcmp(s_active_app->descriptor->id,
                                request->app_id) == 0;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_get_active_id_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->value = app_manager_lifecycle_screen_mutation_allowed() &&
                     s_active_app != NULL ?
                     (void *)s_active_app->descriptor->id : NULL;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_page_active_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    app_node_t *app = _lifecycle_find_app(request->app_id);
    page_node_t *page = _lifecycle_find_page(app, request->page_id);
    request->condition = app_manager_lifecycle_screen_mutation_allowed() &&
                         page != NULL && page->active;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_page_present_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    app_node_t *app = _lifecycle_find_app(request->app_id);
    request->condition = _lifecycle_find_page(app, request->page_id) != NULL;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_all_closed_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->condition = s_active_app == NULL;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_running_count_call(void *opaque)
{
    app_manager_lifecycle_request_t *request = opaque;
    request->count = _lifecycle_running_app_count();
    return ESP_OK;
}

esp_err_t app_manager_regist_msg_handler_ext(const char *id,
        app_page_msg_cb_t handler, void *user_data, uint32_t memory_size)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (!app_manager_lifecycle_id_is_valid(id) || handler == NULL ||
            s_entry_app == NULL)
    {
        goto exit;
    }
    if (s_entry_app->current_page != NULL ||
            _lifecycle_find_page(s_entry_app, id) != NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    page_node_t *page = _lifecycle_alloc_page(memory_size);
    if (page == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    _lifecycle_copy_id(page->id, id);
    page->handler = handler;
    page->user_data = user_data;
    page->app = s_entry_app;
    s_entry_app->current_page = page;
    _lifecycle_call_handler(page, APP_MANAGER_MSG_ONSTART);
    _lifecycle_call_handler(page, APP_MANAGER_MSG_ONRESUME);
    result = ESP_OK;

exit:
    return result;
}

void *app_manager_this_page_memory(void)
{
    return s_handler_page != NULL ? s_handler_page->private_memory : NULL;
}

void *app_manager_this_page_userdata(void)
{
    return s_handler_page != NULL ? s_handler_page->user_data : NULL;
}

void app_manager_this_page_report_cleanup_error(esp_err_t error)
{
    if (s_handler_page != NULL && error != ESP_OK &&
            s_handler_cleanup_error == ESP_OK)
    {
        s_handler_cleanup_error = error;
    }
}

#ifdef HOST_TEST
void app_manager_lifecycle_host_set_observer(
    app_manager_lifecycle_observer_t observer)
{
    atomic_store(&s_host_observer, observer);
}
#endif
