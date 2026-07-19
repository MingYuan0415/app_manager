#define DBG_TAG "app_navigation"
#define DBG_LVL DBG_INFO

#ifndef HOST_TEST
    #include "mt_log.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "freertos/task.h"
#else
    #include "host_freertos.h"
    #define LOG_E(fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_W(fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
#endif

#include "app_manager.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

typedef struct navigation_command
{
    bool in_use;
    bool synchronous;
    bool executing;
    bool completed;
    bool cancelled;
    bool screen_access_owned;
    app_manager_nav_request_t request;
    char app_id[APP_MANAGER_ID_BYTES];
    char page_id[APP_MANAGER_ID_BYTES];
    app_manager_nav_completion_cb_t completion;
    void *completion_context;
    esp_err_t result;
    SemaphoreHandle_t done;
#ifndef HOST_TEST
    StaticSemaphore_t done_storage;
#endif
} navigation_command_t;

static navigation_command_t s_commands[APP_MANAGER_NAV_COMMAND_CAPACITY];
static SemaphoreHandle_t s_lock;
#ifndef HOST_TEST
    static StaticSemaphore_t s_lock_storage;
#endif
static atomic_bool s_initialized;
static atomic_bool s_accepting;
static atomic_uint s_api_users;
static atomic_uint s_sync_waiters;

static bool _navigation_id_copy(char destination[APP_MANAGER_ID_BYTES],
                                const char *source)
{
    bool valid = source == NULL;
    if (source != NULL)
    {
        size_t length = strnlen(source, APP_MANAGER_ID_BYTES);
        valid = length > 0 && length < APP_MANAGER_ID_BYTES;
        if (valid)
        {
            memcpy(destination, source, length + 1U);
        }
    }
    return valid;
}

static navigation_command_t *_navigation_alloc_locked(
    const app_manager_nav_request_t *request)
{
    navigation_command_t *command = NULL;
    for (size_t index = 0; index < APP_MANAGER_NAV_COMMAND_CAPACITY; index++)
    {
        if (!s_commands[index].in_use)
        {
            command = &s_commands[index];
            command->in_use = true;
            command->synchronous = false;
            command->executing = false;
            command->completed = false;
            command->cancelled = false;
            command->screen_access_owned = false;
            command->request = *request;
            command->completion = NULL;
            command->completion_context = NULL;
            command->result = ESP_OK;
            memset(command->app_id, 0, sizeof(command->app_id));
            memset(command->page_id, 0, sizeof(command->page_id));
            (void)xSemaphoreTake(command->done, 0);
            break;
        }
    }
    return command;
}

static void _navigation_free_locked(navigation_command_t *command)
{
    command->in_use = false;
    command->synchronous = false;
    command->executing = false;
    command->completed = false;
    command->cancelled = false;
    command->screen_access_owned = false;
    memset(&command->request, 0, sizeof(command->request));
    memset(command->app_id, 0, sizeof(command->app_id));
    memset(command->page_id, 0, sizeof(command->page_id));
    command->completion = NULL;
    command->completion_context = NULL;
    command->result = ESP_OK;
}

static bool _navigation_access_begin(void)
{
    bool admitted = atomic_load(&s_initialized) && atomic_load(&s_accepting);
    if (admitted)
    {
        atomic_fetch_add(&s_api_users, 1U);
        admitted = atomic_load(&s_initialized) && atomic_load(&s_accepting);
        if (!admitted)
        {
            atomic_fetch_sub(&s_api_users, 1U);
        }
    }
    return admitted;
}

static void _navigation_access_end(void)
{
    atomic_fetch_sub(&s_api_users, 1U);
}

static bool _navigation_release_screen_access_locked(
    navigation_command_t *command)
{
    const bool owned = command->screen_access_owned;
    command->screen_access_owned = false;
    return owned;
}

static void _navigation_complete(esp_err_t result, void *context)
{
    navigation_command_t *command = context;
    app_manager_nav_completion_cb_t completion = NULL;
    void *completion_context = NULL;
    bool release_screen_access = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!command->in_use || command->completed)
    {
        xSemaphoreGive(s_lock);
        goto exit;
    }
    command->result = result;
    command->executing = false;
    command->completed = true;
    release_screen_access =
        _navigation_release_screen_access_locked(command);
    if (command->synchronous)
    {
        xSemaphoreGive(command->done);
    }
    else
    {
        completion = command->completion;
        completion_context = command->completion_context;
        _navigation_free_locked(command);
    }
    xSemaphoreGive(s_lock);

    if (completion != NULL)
    {
        completion(result, completion_context);
    }
    if (release_screen_access)
    {
        app_manager_lifecycle_screen_access_end();
    }

exit:
    return;
}

static void _navigation_execute(void *context)
{
    navigation_command_t *command = context;
    bool cancelled = false;
    bool release_screen_access = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (command->cancelled)
    {
        release_screen_access =
            _navigation_release_screen_access_locked(command);
        _navigation_free_locked(command);
        cancelled = true;
    }
    else
    {
        command->executing = true;
    }
    xSemaphoreGive(s_lock);
    if (cancelled)
    {
        if (release_screen_access)
        {
            app_manager_lifecycle_screen_access_end();
        }
        goto exit;
    }

    app_manager_navigation_finish_transition();
    esp_err_t result = app_manager_lifecycle_navigate(
                           &command->request, _navigation_complete, command);
    if (result != ESP_OK)
    {
        _navigation_complete(result, command);
    }

exit:
    return;
}

static esp_err_t _navigation_submit(const app_manager_nav_request_t *request,
                                    bool synchronous,
                                    app_manager_nav_completion_cb_t completion,
                                    void *completion_context,
                                    navigation_command_t **out_command)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    bool screen_access_owned = false;
    navigation_command_t *command = NULL;
    const char *app_id = NULL;

    if (request == NULL || out_command == NULL)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }

    app_id = request->app_id;
    if (request->operation == APP_MANAGER_NAV_OP_EXIT_SELF)
    {
        if (app_id != NULL)
        {
            result = ESP_ERR_INVALID_ARG;
            goto exit;
        }
        if (!app_manager_mailbox_is_worker())
        {
            goto exit;
        }
        app_id = app_manager_lifecycle_callback_app_id();
        if (app_id == NULL)
        {
            goto exit;
        }
    }
    if (!app_manager_lifecycle_screen_access_begin())
    {
        goto exit;
    }
    screen_access_owned = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_accepting))
    {
        goto exit;
    }
    command = _navigation_alloc_locked(request);
    if (command == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    if (!_navigation_id_copy(command->app_id, app_id) ||
            !_navigation_id_copy(command->page_id, request->page_id))
    {
        _navigation_free_locked(command);
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    command->request.app_id = app_id != NULL ? command->app_id : NULL;
    command->request.page_id = request->page_id != NULL ? command->page_id : NULL;
    command->synchronous = synchronous;
    command->completion = completion;
    command->completion_context = completion_context;
    command->screen_access_owned = true;
    screen_access_owned = false;
    xSemaphoreGive(s_lock);
    lock_owned = false;

    result = app_manager_ui_post(_navigation_execute, command);
    if (result != ESP_OK)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        screen_access_owned =
            _navigation_release_screen_access_locked(command);
        _navigation_free_locked(command);
        xSemaphoreGive(s_lock);
        goto exit;
    }
    *out_command = command;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
    if (screen_access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

static esp_err_t _navigation_finish_on_ui(void *context)
{
    (void)context;
    app_manager_navigation_finish_transition();
    return ESP_OK;
}

static void _navigation_wait_for_counter(atomic_uint *counter)
{
    while (atomic_load(counter) != 0U)
    {
#ifndef HOST_TEST
        vTaskDelay(1);
#else
        usleep(1000);
#endif
    }
}

static void _navigation_release_primitives(void)
{
    for (size_t index = 0; index < APP_MANAGER_NAV_COMMAND_CAPACITY; index++)
    {
        if (s_commands[index].done != NULL)
        {
            vSemaphoreDelete(s_commands[index].done);
            s_commands[index].done = NULL;
        }
    }
    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

static esp_err_t _navigation_wait(navigation_command_t *command,
                                  uint32_t timeout_ms)
{
    TickType_t timeout = timeout_ms == UINT32_MAX ?
                         portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    BaseType_t taken = xSemaphoreTake(command->done, timeout);
    esp_err_t result;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (taken != pdTRUE && !command->executing && !command->completed)
    {
        command->cancelled = true;
        result = ESP_ERR_TIMEOUT;
        goto exit;
    }
    if (taken != pdTRUE)
    {
        if (command->executing)
        {
            xSemaphoreGive(s_lock);
            (void)xSemaphoreTake(command->done, portMAX_DELAY);
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        _navigation_free_locked(command);
        result = ESP_ERR_TIMEOUT;
        goto exit;
    }
    result = command->result;
    _navigation_free_locked(command);

exit:
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t app_manager_navigation_init(void)
{
    esp_err_t result = ESP_OK;
    if (atomic_load(&s_initialized))
    {
        goto exit;
    }
#ifndef HOST_TEST
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
#else
    s_lock = xSemaphoreCreateMutex();
#endif
    if (s_lock == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    memset(s_commands, 0, sizeof(s_commands));
    for (size_t index = 0; index < APP_MANAGER_NAV_COMMAND_CAPACITY; index++)
    {
#ifndef HOST_TEST
        s_commands[index].done =
            xSemaphoreCreateBinaryStatic(&s_commands[index].done_storage);
#else
        s_commands[index].done = xSemaphoreCreateBinary();
#endif
        if (s_commands[index].done == NULL)
        {
            result = ESP_ERR_NO_MEM;
            goto fail;
        }
    }
    atomic_store(&s_api_users, 0U);
    atomic_store(&s_sync_waiters, 0U);
    atomic_store(&s_accepting, true);
    atomic_store(&s_initialized, true);
    goto exit;

fail:
    _navigation_release_primitives();
    memset(s_commands, 0, sizeof(s_commands));
    atomic_store(&s_accepting, false);

exit:
    return result;
}

esp_err_t app_manager_navigation_deinit(void)
{
    esp_err_t result = ESP_OK;
    bool lock_owned = false;

    if (!atomic_load(&s_initialized))
    {
        goto exit;
    }
    if (app_manager_mailbox_is_worker())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    atomic_store(&s_accepting, false);
    xSemaphoreGive(s_lock);
    _navigation_wait_for_counter(&s_api_users);

    result = app_manager_ui_call(_navigation_finish_on_ui, NULL, UINT32_MAX);
    if (result != ESP_OK)
    {
        goto exit;
    }
    _navigation_wait_for_counter(&s_sync_waiters);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    for (size_t index = 0; index < APP_MANAGER_NAV_COMMAND_CAPACITY; index++)
    {
        if (s_commands[index].in_use)
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
    }
    xSemaphoreGive(s_lock);
    lock_owned = false;

    _navigation_release_primitives();
    memset(s_commands, 0, sizeof(s_commands));
    atomic_store(&s_initialized, false);

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
    return result;
}

void app_manager_navigation_finish_transition(void)
{
    if (app_manager_presentation_is_running())
    {
        app_manager_presentation_finish_now();
    }
}

esp_err_t app_manager_navigate(const app_manager_nav_request_t *request,
                               uint32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    navigation_command_t *command = NULL;
    if (app_manager_mailbox_is_worker())
    {
        goto exit;
    }
    if (!_navigation_access_begin())
    {
        goto exit;
    }
    result = _navigation_submit(request, true, NULL, NULL, &command);
    if (result == ESP_OK)
    {
        atomic_fetch_add(&s_sync_waiters, 1U);
        _navigation_access_end();
        result = _navigation_wait(command, timeout_ms);
        atomic_fetch_sub(&s_sync_waiters, 1U);
    }
    else
    {
        _navigation_access_end();
    }

exit:
    return result;
}

esp_err_t app_manager_navigate_async(
    const app_manager_nav_request_t *request,
    app_manager_nav_completion_cb_t completion, void *completion_context)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    navigation_command_t *command = NULL;
    if (!_navigation_access_begin())
    {
        goto exit;
    }
    result = _navigation_submit(request, false, completion,
                                completion_context, &command);
    _navigation_access_end();

exit:
    return result;
}
