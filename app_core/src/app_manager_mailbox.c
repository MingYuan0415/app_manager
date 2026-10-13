#define DBG_TAG "app_mailbox"

#ifndef HOST_TEST
    #include "mt_log.h"
    #include "esp_lv_adapter.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/queue.h"
    #include "freertos/semphr.h"
    #include "freertos/task.h"
    #include "lvgl.h"
#else
    #include "host_freertos.h"
    #define LOG_E(fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_W(fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_I(fmt, ...) fprintf(stderr, "[I][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_D(fmt, ...) ((void)0)
#endif

#include "app_manager_mailbox.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define APP_MANAGER_MAILBOX_SLOTS CONFIG_APP_MANAGER_MAILBOX_CAPACITY
#define APP_MANAGER_MAILBOX_PERIOD_MS      5
#define APP_MANAGER_MAILBOX_DRAIN_LIMIT    8
#define APP_MANAGER_MAILBOX_READY_MS       1000

_Static_assert(APP_MANAGER_MAILBOX_SLOTS <= UINT8_MAX,
               "mailbox indexes must fit in uint8_t");

typedef enum
{
    MAILBOX_COMMAND_VOID = 0,
    MAILBOX_COMMAND_RESULT,
} mailbox_command_type_t;

typedef struct mailbox_command
{
    bool                    in_use;
    bool                    synchronous;
    bool                    executing;
    bool                    completed;
    bool                    cancelled;
    mailbox_command_type_t  type;
    union mailbox_callback
    {
        void (*void_cb)(void *);
        app_manager_ui_call_fn result_cb;
    } callback;
    void                   *arg;
    esp_err_t               result;
    SemaphoreHandle_t       done;
#ifndef HOST_TEST
    StaticSemaphore_t       done_storage;
#endif
} mailbox_command_t;

static mailbox_command_t s_commands[APP_MANAGER_MAILBOX_SLOTS];
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_ready;
static TaskHandle_t s_worker_task;
static atomic_bool s_initialized;
static atomic_bool s_accepting;
static atomic_uint s_api_users;
static atomic_flag s_shutdown_busy = ATOMIC_FLAG_INIT;
static bool s_timer_created;

#ifndef HOST_TEST
    static StaticQueue_t s_queue_storage;
    static uint8_t s_queue_buffer[APP_MANAGER_MAILBOX_SLOTS * sizeof(mailbox_command_t *)];
    static StaticSemaphore_t s_lock_storage;
    static StaticSemaphore_t s_ready_storage;
    static lv_timer_t *s_lvgl_timer;
#else
    static esp_timer_handle_t s_host_timer;
    extern void app_manager_host_ui_step(void) __attribute__((weak));
#endif

static mailbox_command_t *_mailbox_command_alloc_locked(void)
{
    mailbox_command_t *command = NULL;

    for (size_t i = 0; i < APP_MANAGER_MAILBOX_SLOTS; i++)
    {
        if (!s_commands[i].in_use)
        {
            command = &s_commands[i];
            command->in_use = true;
            command->synchronous = false;
            command->executing = false;
            command->completed = false;
            command->cancelled = false;
            command->arg = NULL;
            command->result = ESP_OK;
            (void)xSemaphoreTake(command->done, 0);
            break;
        }
    }
    return command;
}

static void _mailbox_command_free_locked(mailbox_command_t *command)
{
    command->in_use = false;
    command->synchronous = false;
    command->executing = false;
    command->completed = false;
    command->cancelled = false;
    command->callback.void_cb = NULL;
    command->arg = NULL;
    command->result = ESP_OK;
}

static bool _mailbox_access_begin(void)
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

static void _mailbox_access_end(void)
{
    atomic_fetch_sub(&s_api_users, 1U);
}

static esp_err_t _mailbox_shutdown_barrier(void *arg)
{
    (void)arg;
    return ESP_OK;
}

static void _mailbox_execute(mailbox_command_t *command)
{
    bool cancelled = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (command->cancelled)
    {
        _mailbox_command_free_locked(command);
        cancelled = true;
    }
    else
    {
        command->executing = true;
    }
    xSemaphoreGive(s_lock);

    if (!cancelled)
    {
        if (command->type == MAILBOX_COMMAND_RESULT)
        {
            command->result = command->callback.result_cb(command->arg);
        }
        else
        {
            command->callback.void_cb(command->arg);
            command->result = ESP_OK;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        command->executing = false;
        command->completed = true;
        if (!command->synchronous)
        {
            _mailbox_command_free_locked(command);
        }
        else
        {
            xSemaphoreGive(command->done);
        }
        xSemaphoreGive(s_lock);
    }
}

static void _mailbox_drain(void)
{
    if (s_worker_task == NULL)
    {
        s_worker_task = xTaskGetCurrentTaskHandle();
        xSemaphoreGive(s_ready);
    }

    for (size_t i = 0; i < APP_MANAGER_MAILBOX_DRAIN_LIMIT; i++)
    {
        mailbox_command_t *command = NULL;
        if (xQueueReceive(s_queue, &command, 0) != pdTRUE)
        {
            break;
        }
        if (command != NULL)
        {
            _mailbox_execute(command);
        }
    }
}

#ifndef HOST_TEST
static void _mailbox_lvgl_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    _mailbox_drain();
}
#else
static void _mailbox_host_timer_cb(void *arg)
{
    (void)arg;
    _mailbox_drain();
    if (app_manager_host_ui_step != NULL)
    {
        app_manager_host_ui_step();
    }
}

void app_manager_mailbox_host_timer_pause(bool paused)
{
    host_timer_set_paused(s_host_timer, paused);
}

void app_manager_mailbox_host_timer_step(void)
{
    host_timer_step(s_host_timer);
}
#endif

static void _mailbox_reset_runtime(void)
{
    memset(s_commands, 0, sizeof(s_commands));
    s_queue = NULL;
    s_lock = NULL;
    s_ready = NULL;
    s_worker_task = NULL;
    s_timer_created = false;
    atomic_store(&s_api_users, 0U);
    atomic_store(&s_accepting, false);
    atomic_flag_clear(&s_shutdown_busy);
}

static bool _mailbox_create_primitives(void)
{
#ifndef HOST_TEST
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    s_ready = xSemaphoreCreateBinaryStatic(&s_ready_storage);
    s_queue = xQueueCreateStatic(APP_MANAGER_MAILBOX_SLOTS,
                                 sizeof(mailbox_command_t *),
                                 s_queue_buffer, &s_queue_storage);
#else
    s_lock = xSemaphoreCreateMutex();
    s_ready = xSemaphoreCreateBinary();
    s_queue = xQueueCreate(APP_MANAGER_MAILBOX_SLOTS, sizeof(mailbox_command_t *));
#endif
    bool created = s_lock != NULL && s_ready != NULL && s_queue != NULL;
    for (size_t i = 0; created && i < APP_MANAGER_MAILBOX_SLOTS; i++)
    {
#ifndef HOST_TEST
        s_commands[i].done = xSemaphoreCreateBinaryStatic(&s_commands[i].done_storage);
#else
        s_commands[i].done = xSemaphoreCreateBinary();
#endif
        created = s_commands[i].done != NULL;
    }
    return created;
}

static esp_err_t _mailbox_start_timer(void)
{
#ifndef HOST_TEST
    esp_err_t result = esp_lv_adapter_lock(-1);
    if (result != ESP_OK)
    {
        return result;
    }
    s_lvgl_timer = lv_timer_create(_mailbox_lvgl_timer_cb,
                                   APP_MANAGER_MAILBOX_PERIOD_MS, NULL);
    esp_lv_adapter_unlock();
    if (s_lvgl_timer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
#else
    const esp_timer_create_args_t args =
    {
        .callback = _mailbox_host_timer_cb,
        .arg = NULL,
        .name = "host_ui_worker",
    };
    if (esp_timer_create(&args, &s_host_timer) != ESP_OK)
    {
        return ESP_FAIL;
    }
    s_timer_created = true;
    if (esp_timer_start_periodic(
                s_host_timer, APP_MANAGER_MAILBOX_PERIOD_MS * 1000U) != ESP_OK)
    {
        return ESP_FAIL;
    }
#endif
    s_timer_created = true;
    return ESP_OK;
}

esp_err_t app_manager_mailbox_init(void)
{
    esp_err_t result = ESP_OK;
    if (atomic_load(&s_initialized))
    {
        return ESP_OK;
    }

    _mailbox_reset_runtime();
    if (!_mailbox_create_primitives())
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    result = _mailbox_start_timer();
    if (result != ESP_OK)
    {
        goto fail;
    }

    atomic_store(&s_initialized, true);
    if (xSemaphoreTake(s_ready, pdMS_TO_TICKS(APP_MANAGER_MAILBOX_READY_MS)) != pdTRUE)
    {
        LOG_E("LVGL worker did not service mailbox timer");
        result = ESP_ERR_TIMEOUT;
        goto fail;
    }

    atomic_store(&s_accepting, true);
    LOG_I("ready: slots=%d period=%dms drain=%d",
          APP_MANAGER_MAILBOX_SLOTS, APP_MANAGER_MAILBOX_PERIOD_MS,
          APP_MANAGER_MAILBOX_DRAIN_LIMIT);
    return ESP_OK;

fail:
    atomic_store(&s_initialized, true);
    atomic_store(&s_accepting, false);
    (void)app_manager_mailbox_deinit();
    return result;
}

static esp_err_t _mailbox_stop_admission(void)
{
    esp_err_t result = ESP_OK;
    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        atomic_store(&s_accepting, false);
        xSemaphoreGive(s_lock);
    }
    else
    {
        atomic_store(&s_accepting, false);
    }

    if (s_worker_task != NULL && s_queue != NULL && s_ready != NULL)
    {
        (void)xSemaphoreTake(s_ready, 0);
        mailbox_command_t barrier =
        {
            .in_use = true,
            .synchronous = true,
            .type = MAILBOX_COMMAND_RESULT,
            .callback.result_cb = _mailbox_shutdown_barrier,
            .done = s_ready,
        };
        mailbox_command_t *barrier_ptr = &barrier;
        if (xQueueSend(s_queue, &barrier_ptr, portMAX_DELAY) != pdTRUE)
        {
            return ESP_FAIL;
        }
        (void)xSemaphoreTake(s_ready, portMAX_DELAY);
        while (atomic_load(&s_api_users) != 0U)
        {
#ifdef HOST_TEST
            usleep(1000);
#else
            vTaskDelay(1);
#endif
        }
    }
    return result;
}

static esp_err_t _mailbox_delete_timer(void)
{
    esp_err_t result = ESP_OK;
#ifndef HOST_TEST
    if (s_timer_created)
    {
        result = esp_lv_adapter_lock(-1);
        if (result == ESP_OK)
        {
            lv_timer_delete(s_lvgl_timer);
            s_lvgl_timer = NULL;
            esp_lv_adapter_unlock();
        }
    }
#else
    if (s_timer_created)
    {
        (void)esp_timer_stop(s_host_timer);
        (void)esp_timer_delete(s_host_timer);
        s_host_timer = NULL;
    }
#endif
    return result;
}

static void _mailbox_release_primitives(void)
{
    for (size_t count = APP_MANAGER_MAILBOX_SLOTS; count > 0; count--)
    {
        mailbox_command_t *command = &s_commands[count - 1U];
        if (command->done != NULL)
        {
            vSemaphoreDelete(command->done);
            command->done = NULL;
        }
    }
    if (s_queue != NULL)
    {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_ready != NULL)
    {
        vSemaphoreDelete(s_ready);
        s_ready = NULL;
    }
    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

static void _mailbox_clear_runtime(void)
{
    atomic_store(&s_initialized, false);
    s_timer_created = false;
    s_worker_task = NULL;
    s_queue = NULL;
    s_ready = NULL;
    s_lock = NULL;
}

esp_err_t app_manager_mailbox_deinit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool shutdown_owned = !atomic_flag_test_and_set(&s_shutdown_busy);
    if (!shutdown_owned)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_load(&s_initialized))
    {
        result = ESP_OK;
        goto exit;
    }
    if (s_worker_task != NULL &&
            xTaskGetCurrentTaskHandle() == s_worker_task)
    {
        goto exit;
    }

    result = _mailbox_stop_admission();
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _mailbox_delete_timer();
    if (result != ESP_OK)
    {
        goto exit;
    }
    _mailbox_release_primitives();
    _mailbox_clear_runtime();
    result = ESP_OK;

exit:
    if (shutdown_owned)
    {
        atomic_flag_clear(&s_shutdown_busy);
    }
    return result;
}

static esp_err_t _mailbox_wait_for_result(mailbox_command_t *command,
        uint32_t timeout_ms)
{
    esp_err_t result;
    TickType_t timeout_ticks = timeout_ms == UINT32_MAX ?
                               portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    BaseType_t taken = xSemaphoreTake(command->done, timeout_ticks);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (taken != pdTRUE && !command->completed && !command->executing)
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
        _mailbox_command_free_locked(command);
        result = ESP_ERR_TIMEOUT;
        goto exit;
    }
    result = command->result;
    _mailbox_command_free_locked(command);

exit:
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t app_manager_mailbox_shutdown_call(app_manager_ui_call_fn cb, void *arg)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool shutdown_owned = false;
    bool lock_owned = false;
    if (cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    shutdown_owned = !atomic_flag_test_and_set(&s_shutdown_busy);
    if (!shutdown_owned)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_load(&s_initialized) || s_lock == NULL ||
            s_queue == NULL || s_ready == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_worker_task != NULL &&
            xTaskGetCurrentTaskHandle() == s_worker_task)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_initialized))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    atomic_store(&s_accepting, false);
    xSemaphoreGive(s_lock);
    lock_owned = false;

    (void)xSemaphoreTake(s_ready, 0);
    mailbox_command_t command =
    {
        .in_use = true,
        .synchronous = true,
        .type = MAILBOX_COMMAND_RESULT,
        .callback.result_cb = cb,
        .arg = arg,
        .done = s_ready,
    };
    mailbox_command_t *command_ptr = &command;
    if (xQueueSend(s_queue, &command_ptr, portMAX_DELAY) != pdTRUE)
    {
        result = ESP_FAIL;
        goto exit;
    }
    (void)xSemaphoreTake(s_ready, portMAX_DELAY);
    while (atomic_load(&s_api_users) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        vTaskDelay(1);
#endif
    }
    result = command.result;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
    if (shutdown_owned)
    {
        atomic_flag_clear(&s_shutdown_busy);
    }
    return result;
}

esp_err_t app_manager_mailbox_post(void (*cb)(void *), void *arg)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool access_owned = false;
    bool lock_owned = false;
    if (!_mailbox_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    access_owned = true;
    if (cb == NULL)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_initialized) || !atomic_load(&s_accepting))
    {
        goto exit;
    }
    mailbox_command_t *command = _mailbox_command_alloc_locked();
    if (command == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    command->type = MAILBOX_COMMAND_VOID;
    command->callback.void_cb = cb;
    command->arg = arg;

    if (xQueueSend(s_queue, &command, 0) != pdTRUE)
    {
        _mailbox_command_free_locked(command);
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    result = ESP_OK;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
    if (access_owned)
    {
        _mailbox_access_end();
    }
    return result;
}

esp_err_t app_manager_mailbox_call(app_manager_ui_call_fn cb, void *arg,
                                   uint32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool access_owned = false;
    bool lock_owned = false;
    if (!_mailbox_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    access_owned = true;
    if (cb == NULL)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    if (xTaskGetCurrentTaskHandle() == s_worker_task)
    {
        result = cb(arg);
        goto exit;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lock_owned = true;
    if (!atomic_load(&s_initialized) || !atomic_load(&s_accepting))
    {
        goto exit;
    }
    mailbox_command_t *command = _mailbox_command_alloc_locked();
    if (command == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    command->type = MAILBOX_COMMAND_RESULT;
    command->callback.result_cb = cb;
    command->arg = arg;
    command->synchronous = true;

    if (xQueueSend(s_queue, &command, 0) != pdTRUE)
    {
        _mailbox_command_free_locked(command);
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    xSemaphoreGive(s_lock);
    lock_owned = false;

    result = _mailbox_wait_for_result(command, timeout_ms);

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_lock);
    }
    if (access_owned)
    {
        _mailbox_access_end();
    }
    return result;
}

esp_err_t app_manager_ui_post(void (*cb)(void *), void *arg)
{
    return app_manager_mailbox_post(cb, arg);
}

esp_err_t app_manager_ui_call(app_manager_ui_call_fn cb, void *arg,
                              uint32_t timeout_ms)
{
    return app_manager_mailbox_call(cb, arg, timeout_ms);
}

esp_err_t app_manager_get_ui_dispatch_fn(app_manager_ui_dispatch_fn *out_fn)
{
    if (out_fn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_initialized) || !atomic_load(&s_accepting))
    {
        *out_fn = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    *out_fn = app_manager_ui_post;
    return ESP_OK;
}

bool app_manager_mailbox_is_worker(void)
{
    return atomic_load(&s_initialized) && s_worker_task != NULL &&
           xTaskGetCurrentTaskHandle() == s_worker_task;
}
