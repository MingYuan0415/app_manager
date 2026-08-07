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
#include "app_manager_back_gesture.h"
#include "app_manager_lifecycle.h"
#include "app_manager_lifecycle_internal.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"
#include "app_manager_task_switcher.h"

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
    bool navigation_started;
    bool keep_switcher_alive;
    app_manager_nav_system_op_t system_op;
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
static uint32_t s_epoch;
static uint32_t s_pending_count;

static void _navigation_execute(void *context);

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
            command->navigation_started = false;
            command->keep_switcher_alive = false;
            command->system_op = APP_MANAGER_NAV_SYSTEM_NONE;
            if (request != NULL)
            {
                command->request = *request;
            }
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
    command->navigation_started = false;
    command->keep_switcher_alive = false;
    command->system_op = APP_MANAGER_NAV_SYSTEM_NONE;
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

static void _navigation_epoch_advance_locked(void)
{
    s_epoch++;
    if (s_epoch == 0U)
    {
        s_epoch = 1U;
    }
}

static bool _navigation_token_current_locked(
    const app_manager_navigation_gesture_token_t *token)
{
    return token != NULL && token->epoch != 0U &&
           atomic_load(&s_accepting) && s_pending_count == 0U &&
           token->epoch == s_epoch;
}

static esp_err_t _navigation_post_locked(
    const app_manager_nav_request_t *request, const char *app_id,
    bool synchronous, app_manager_nav_completion_cb_t completion,
    void *completion_context, navigation_command_t **out_command)
{
    esp_err_t result = ESP_ERR_NO_MEM;
    navigation_command_t *command = _navigation_alloc_locked(request);
    if (command == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    if (!_navigation_id_copy(command->app_id, app_id) ||
            !_navigation_id_copy(command->page_id, request->page_id))
    {
        _navigation_free_locked(command);
        return ESP_ERR_INVALID_ARG;
    }
    command->request.app_id = app_id != NULL ? command->app_id : NULL;
    command->request.page_id = request->page_id != NULL ?
                               command->page_id : NULL;
    command->synchronous = synchronous;
    command->completion = completion;
    command->completion_context = completion_context;

    result = app_manager_ui_post(_navigation_execute, command);
    if (result != ESP_OK)
    {
        _navigation_free_locked(command);
        return result;
    }
    command->screen_access_owned = true;
    s_pending_count++;
    _navigation_epoch_advance_locked();
    if (out_command != NULL)
    {
        *out_command = command;
    }
    return result;
}

static void _navigation_complete(esp_err_t result, void *context)
{
    navigation_command_t *command = context;
    app_manager_nav_completion_cb_t completion = NULL;
    void *completion_context = NULL;
    bool release_screen_access = false;
    bool navigation_started = false;
    bool synchronous = false;
    SemaphoreHandle_t done = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!command->in_use || command->completed)
    {
        xSemaphoreGive(s_lock);
        return;
    }
    command->result = result;
    command->executing = false;
    command->completed = true;
    release_screen_access =
        _navigation_release_screen_access_locked(command);
    navigation_started = command->navigation_started;
    synchronous = command->synchronous;
    done = command->done;
    if (!synchronous)
    {
        completion = command->completion;
        completion_context = command->completion_context;
        _navigation_free_locked(command);
    }
    xSemaphoreGive(s_lock);

    if (release_screen_access)
    {
        app_manager_lifecycle_screen_access_end();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending_count > 0U)
    {
        s_pending_count--;
    }
    xSemaphoreGive(s_lock);

    if (navigation_started)
    {
        app_manager_back_gesture_navigation_settled();
    }

    if (synchronous)
    {
        xSemaphoreGive(done);
    }
    else if (completion != NULL)
    {
        completion(result, completion_context);
    }
}

static void _navigation_clear_all_home_complete(esp_err_t result,
        void *context)
{
    navigation_command_t *command = context;
    if (result == ESP_OK)
    {
        result = app_manager_lifecycle_clear_background_tasks();
    }
    app_manager_task_switcher_clear_all_finished(result);
    _navigation_complete(result, command);
}

static void _navigation_execute_clear_all(navigation_command_t *command)
{
    /* Foreground Home, then stop every eligible task inside the same
     * gateway command so no queued navigation can interleave. */
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = APP_MANAGER_ID_HOME,
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_DEFAULT,
        },
    };
    esp_err_t result = app_manager_lifecycle_navigate(
                           &request, _navigation_clear_all_home_complete,
                           command);
    if (result != ESP_OK)
    {
        app_manager_task_switcher_clear_all_finished(result);
        _navigation_complete(result, command);
    }
}

static void _navigation_execute(void *context)
{
    navigation_command_t *command = context;
    bool cancelled = false;
    bool release_screen_access = false;
    esp_err_t system_result = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (command->cancelled)
    {
        release_screen_access =
            _navigation_release_screen_access_locked(command);
        if (s_pending_count > 0U)
        {
            s_pending_count--;
        }
        _navigation_free_locked(command);
        cancelled = true;
    }
    else
    {
        command->executing = true;
        command->navigation_started = true;
    }
    xSemaphoreGive(s_lock);
    if (cancelled)
    {
        if (release_screen_access)
        {
            app_manager_lifecycle_screen_access_end();
        }
        app_manager_back_gesture_navigation_settled();
        return;
    }

    app_manager_back_gesture_navigation_begin();
    app_manager_navigation_finish_transition();

    if (command->system_op == APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW)
    {
        system_result = app_manager_task_switcher_show();
        _navigation_complete(system_result, command);
        return;
    }
    if (command->system_op == APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_CLEAR_ALL)
    {
        _navigation_execute_clear_all(command);
        return;
    }
    if (!command->keep_switcher_alive)
    {
        app_manager_task_switcher_navigation_begin();
    }

    esp_err_t result = app_manager_lifecycle_navigate(
                           &command->request, _navigation_complete, command);
    if (result != ESP_OK)
    {
        _navigation_complete(result, command);
    }
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
    const char *app_id = NULL;

    if (request == NULL || out_command == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_id = request->app_id;
    if (request->operation == APP_MANAGER_NAV_OP_EXIT_SELF)
    {
        if (app_id != NULL)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (!app_manager_mailbox_is_worker())
        {
            return ESP_ERR_INVALID_STATE;
        }
        app_id = app_manager_lifecycle_callback_app_id();
        if (app_id == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (!app_manager_lifecycle_screen_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    screen_access_owned = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_accepting))
    {
        goto exit;
    }
    result = _navigation_post_locked(request, app_id, synchronous, completion,
                                     completion_context, out_command);
    if (result == ESP_OK)
    {
        screen_access_owned = false;
    }

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
        return ESP_OK;
    }
#ifndef HOST_TEST
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
#else
    s_lock = xSemaphoreCreateMutex();
#endif
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
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
    s_pending_count = 0U;
    if (s_epoch == 0U)
    {
        s_epoch = 1U;
    }
    else
    {
        _navigation_epoch_advance_locked();
    }
    atomic_store(&s_accepting, true);
    atomic_store(&s_initialized, true);
    return ESP_OK;

fail:
    _navigation_release_primitives();
    memset(s_commands, 0, sizeof(s_commands));
    atomic_store(&s_accepting, false);
    return result;
}

esp_err_t app_manager_navigation_deinit(void)
{
    esp_err_t result = ESP_OK;
    bool lock_owned = false;

    if (!atomic_load(&s_initialized))
    {
        return ESP_OK;
    }
    if (app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    atomic_store(&s_accepting, false);
    xSemaphoreGive(s_lock);
    _navigation_wait_for_counter(&s_api_users);

    result = app_manager_ui_call(_navigation_finish_on_ui, NULL, UINT32_MAX);
    if (result != ESP_OK)
    {
        return result;
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
    if (s_pending_count != 0U)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
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

bool app_manager_navigation_gesture_token_acquire(
    app_manager_navigation_gesture_token_t *token)
{
    if (token == NULL)
    {
        return false;
    }
    token->epoch = 0U;
    if (!_navigation_access_begin())
    {
        return false;
    }
    bool acquired = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (atomic_load(&s_accepting) && s_pending_count == 0U)
    {
        token->epoch = s_epoch;
        acquired = true;
    }
    xSemaphoreGive(s_lock);
    _navigation_access_end();
    return acquired;
}

bool app_manager_navigation_gesture_token_is_current(
    const app_manager_navigation_gesture_token_t *token)
{
    if (token == NULL || !_navigation_access_begin())
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool current = _navigation_token_current_locked(token);
    xSemaphoreGive(s_lock);
    _navigation_access_end();
    return current;
}

esp_err_t app_manager_navigation_submit_gesture_back(
    const app_manager_navigation_gesture_token_t *token,
    app_manager_nav_completion_cb_t completion, void *context)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    bool screen_access_owned = false;

    if (token == NULL || !app_manager_mailbox_is_worker() ||
            !_navigation_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_manager_lifecycle_screen_access_begin())
    {
        goto access_end;
    }
    screen_access_owned = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!_navigation_token_current_locked(token))
    {
        goto access_end;
    }

    const char *source_app_id = NULL;
    app_manager_lifecycle_back_target_t target =
        app_manager_lifecycle_back_target(&source_app_id);
    if (target == APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE ||
            source_app_id == NULL)
    {
        goto access_end;
    }
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_BACK,
        .app_id = source_app_id,
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_DEFAULT,
        },
    };
    result = _navigation_post_locked(&request, source_app_id, false,
                                     completion, context, NULL);
    if (result == ESP_OK)
    {
        screen_access_owned = false;
    }

access_end:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
    if (screen_access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    _navigation_access_end();
    return result;
}

esp_err_t app_manager_navigation_submit_system_op(
    app_manager_nav_system_op_t op, app_manager_nav_completion_cb_t completion,
    void *context)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    bool screen_access_owned = false;

    if (op <= APP_MANAGER_NAV_SYSTEM_NONE ||
            op >= APP_MANAGER_NAV_SYSTEM_END ||
            !_navigation_access_begin())
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_manager_lifecycle_screen_access_begin())
    {
        goto access_end;
    }
    screen_access_owned = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_accepting))
    {
        goto exit;
    }
    navigation_command_t *command = _navigation_alloc_locked(NULL);
    if (command == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    command->system_op = op;
    command->completion = completion;
    command->completion_context = context;
    result = app_manager_ui_post(_navigation_execute, command);
    if (result != ESP_OK)
    {
        _navigation_free_locked(command);
        goto exit;
    }
    command->screen_access_owned = true;
    s_pending_count++;
    _navigation_epoch_advance_locked();
    screen_access_owned = false;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
access_end:
    if (screen_access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    _navigation_access_end();
    return result;
}

esp_err_t app_manager_navigation_submit_switcher_exit(
    const app_manager_nav_request_t *request,
    app_manager_nav_completion_cb_t completion, void *context)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    bool screen_access_owned = false;
    const char *app_id = NULL;

    if (request == NULL ||
            request->operation != APP_MANAGER_NAV_OP_EXIT ||
            request->app_id == NULL || !_navigation_access_begin())
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_manager_lifecycle_screen_access_begin())
    {
        goto access_end;
    }
    screen_access_owned = true;

    app_id = request->app_id;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_accepting))
    {
        goto exit;
    }
    /* The shared submit path copies the request by value (including any
     * page_id) before the UI worker executes it. */
    navigation_command_t *command = NULL;
    result = _navigation_post_locked(request, app_id, false, completion,
                                     context, &command);
    if (result != ESP_OK)
    {
        goto exit;
    }
    command->keep_switcher_alive = true;
    screen_access_owned = false;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
access_end:
    if (screen_access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    _navigation_access_end();
    return result;
}

esp_err_t app_manager_navigate(const app_manager_nav_request_t *request,
                               uint32_t timeout_ms)
{
    navigation_command_t *command = NULL;
    if (app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_navigation_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _navigation_submit(request, true, NULL, NULL, &command);
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
    return result;
}

esp_err_t app_manager_navigate_async(
    const app_manager_nav_request_t *request,
    app_manager_nav_completion_cb_t completion, void *completion_context)
{
    navigation_command_t *command = NULL;
    if (!_navigation_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _navigation_submit(request, false, completion,
                                          completion_context, &command);
    _navigation_access_end();
    return result;
}
