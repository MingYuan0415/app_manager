#define DBG_TAG "app_control"
#define DBG_LVL DBG_INFO
#ifndef HOST_TEST
    #include "mt_log.h"
#else
    #include "host_control_port.h"
    #define LOG_E(fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_W(fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define LOG_I(fmt, ...) ((void)0)
#endif

#include "app_manager.h"
#include "app_manager_control.h"
#include "app_manager_display.h"
#include "app_manager_pm_internal.h"
#ifndef HOST_TEST
    #include "esp_timer.h"
    #include "nv_storage.h"
#endif
#include <string.h>

static void _control_release_primitives(void);

app_manager_screen_ctrl_ops_t s_screen_ops;
static app_manager_input_ops_t s_input_ops;
app_manager_standby_ops_t s_standby_ops;
QueueHandle_t s_queue;
SemaphoreHandle_t s_call_lock;
SemaphoreHandle_t s_call_done;
SemaphoreHandle_t s_state_lock;
SemaphoreHandle_t s_task_ready;
SemaphoreHandle_t s_task_stopped;
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_buffer[APP_CONTROL_QUEUE_DEPTH * sizeof(control_command_t)];
static StaticSemaphore_t s_call_lock_storage;
static StaticSemaphore_t s_call_done_storage;
static StaticSemaphore_t s_state_lock_storage;
static StaticSemaphore_t s_task_ready_storage;
static StaticSemaphore_t s_task_stopped_storage;
TaskHandle_t s_task;
esp_err_t s_call_result;
app_manager_pm_state_t s_state;
int64_t s_last_activity_us;
int64_t s_screen_off_us;
int32_t s_dim_timeout_ms = APP_CONTROL_DEFAULT_DIM_MS;
int32_t s_off_timeout_ms = APP_CONTROL_DEFAULT_OFF_MS;
int32_t s_standby_timeout_ms = APP_CONTROL_DEFAULT_STANDBY_MS;
uint8_t s_brightness = UINT8_MAX;
atomic_int s_lifecycle_state = ATOMIC_VAR_INIT(CONTROL_UNINITIALIZED);
atomic_uint s_active_accesses;
atomic_bool s_wake_pending;
atomic_uint s_power_pending_events;
atomic_bool s_power_pending_wake;
atomic_uint s_standby_generation;
atomic_uint s_standby_bound_epoch;
atomic_uint s_standby_system_generation;
atomic_uint s_standby_commit_epoch;
atomic_uint s_standby_commit_generation;
atomic_bool s_standby_commit_pending;
atomic_bool s_standby_abort_pending;
atomic_uint s_power_input_pending;
atomic_bool s_touch_pending;
atomic_uint s_touch_pending_generation;
atomic_uint s_touch_generation;
atomic_bool s_touch_activity_enabled;
atomic_bool s_touch_restore_pending;
atomic_bool s_home_pending;
atomic_bool s_home_inflight;
atomic_uint s_brightness_temp_value;
atomic_uint s_brightness_temp_generation;
atomic_bool s_brightness_temp_pending;
atomic_bool s_control_admitted;
static bool s_input_registered;
static atomic_flag s_deinit_busy = ATOMIC_FLAG_INIT;
bool s_power_wake_click_pending;
bool s_power_long_press_seen;

typedef struct control_brightness_request
{
    uint8_t brightness;
    bool persistent;
} control_brightness_request_t;

static bool _control_ready_access_begin(void)
{
    bool admitted = atomic_load(&s_lifecycle_state) == CONTROL_READY;
    if (admitted)
    {
        atomic_fetch_add(&s_active_accesses, 1U);
        admitted = atomic_load(&s_lifecycle_state) == CONTROL_READY;
        if (!admitted)
        {
            atomic_fetch_sub(&s_active_accesses, 1U);
        }
    }
    return admitted;
}

static esp_err_t _control_apply_brightness_on_ui(void *argument)
{
    const control_brightness_request_t *request = argument;
    return request->persistent ?
           s_screen_ops.set_brightness(request->brightness) :
           s_screen_ops.set_brightness_temp(request->brightness);
}

static esp_err_t _control_commit_display_on_ui(void *argument)
{
    (void)argument;
    return s_screen_ops.resume_commit();
}

esp_err_t app_manager_control_apply_brightness(uint8_t brightness,
        bool persistent)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const bool access_owned = _control_ready_access_begin();
    if (!access_owned || app_manager_display_is_sleeping())
    {
        goto exit;
    }
    const control_brightness_request_t request =
    {
        .brightness = brightness,
        .persistent = persistent,
    };
    result = app_manager_ui_call(_control_apply_brightness_on_ui,
                                 (void *)&request, UINT32_MAX);

exit:
    if (access_owned)
    {
        app_manager_control_access_end();
    }
    return result;
}

esp_err_t app_manager_control_commit_display(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const bool access_owned = _control_ready_access_begin();
    if (!access_owned || app_manager_display_is_sleeping())
    {
        goto exit;
    }
    result = app_manager_ui_call(_control_commit_display_on_ui,
                                 NULL, UINT32_MAX);

exit:
    if (access_owned)
    {
        app_manager_control_access_end();
    }
    return result;
}

bool app_manager_control_access_begin(void)
{
    bool admitted = atomic_load_explicit(&s_control_admitted,
                                         memory_order_acquire);
    if (admitted)
    {
        admitted = _control_ready_access_begin();
    }
    if (admitted)
    {
        admitted = atomic_load_explicit(&s_control_admitted,
                                        memory_order_acquire);
        if (!admitted)
        {
            app_manager_control_access_end();
        }
    }
    return admitted;
}

void app_manager_control_access_end(void)
{
    atomic_fetch_sub(&s_active_accesses, 1U);
}

static void _control_wait_for_accesses(void)
{
    while (atomic_load(&s_active_accesses) != 0U)
    {
#ifdef HOST_TEST
        usleep(1000);
#else
        taskYIELD();
#endif
    }
}

static void _control_input_callback(app_manager_key_t key,
                                    app_manager_key_event_t event,
                                    void *user_data)
{
    (void)user_data;
    (void)app_manager_input_inject(key, event);
}

static bool _control_config_valid(const app_manager_config_t *config)
{
    return config != NULL && config->screen_ops.suspend != NULL &&
           config->screen_ops.resume_prepare != NULL &&
           config->screen_ops.resume_commit != NULL &&
           config->screen_ops.is_suspended != NULL &&
           config->screen_ops.is_suspend_committed != NULL &&
           config->screen_ops.set_brightness != NULL &&
           config->screen_ops.set_brightness_temp != NULL &&
           config->screen_ops.get_brightness != NULL &&
           config->input_ops.register_handler != NULL &&
           config->input_ops.unregister_handler != NULL &&
           config->standby_ops.request_standby != NULL &&
           config->standby_ops.cancel_standby != NULL;
}

static bool _control_create_primitives(void)
{
    s_queue = xQueueCreateStatic(APP_CONTROL_QUEUE_DEPTH,
                                 sizeof(control_command_t), s_queue_buffer,
                                 &s_queue_storage);
    s_call_lock = xSemaphoreCreateMutexStatic(&s_call_lock_storage);
    s_call_done = xSemaphoreCreateBinaryStatic(&s_call_done_storage);
    s_state_lock = xSemaphoreCreateMutexStatic(&s_state_lock_storage);
    s_task_ready = xSemaphoreCreateBinaryStatic(&s_task_ready_storage);
    s_task_stopped = xSemaphoreCreateBinaryStatic(&s_task_stopped_storage);
    return s_queue != NULL && s_call_lock != NULL && s_call_done != NULL &&
           s_state_lock != NULL && s_task_ready != NULL && s_task_stopped != NULL;
}

static void _control_load_settings(void)
{
    s_dim_timeout_ms = APP_CONTROL_DEFAULT_DIM_MS;
    s_off_timeout_ms = APP_CONTROL_DEFAULT_OFF_MS;
    s_standby_timeout_ms = APP_CONTROL_DEFAULT_STANDBY_MS;
    s_brightness = UINT8_MAX;
    uint32_t value;
    if (nv_storage_get_u8(NVS_KEY_BRIGHTNESS, &s_brightness) != ESP_OK)
    {
        s_brightness = UINT8_MAX;
    }
    if (nv_storage_get_u32(NVS_KEY_DIM_TIMEOUT, &value) == ESP_OK)
    {
        int32_t stored = (int32_t)value;
        if (stored >= 0)
        {
            s_dim_timeout_ms = stored;
        }
    }
    if (nv_storage_get_u32(NVS_KEY_OFF_TIMEOUT, &value) == ESP_OK)
    {
        int32_t stored = (int32_t)value;
        if (stored == -1 || stored >= s_dim_timeout_ms)
        {
            s_off_timeout_ms = stored;
        }
    }
    if (nv_storage_get_u32(NVS_KEY_STANDBY_TIMEOUT, &value) == ESP_OK)
    {
        int32_t stored = (int32_t)value;
        if (stored >= -1)
        {
            s_standby_timeout_ms = stored;
        }
    }
}

static void _control_reset_runtime_state(void)
{
    s_state = APP_MANAGER_PM_ACTIVE;
    s_last_activity_us = esp_timer_get_time();
    s_screen_off_us = 0;
    atomic_store(&s_active_accesses, 0U);
    atomic_store(&s_wake_pending, false);
    atomic_store(&s_power_pending_events, 0U);
    atomic_store(&s_power_pending_wake, false);
    atomic_store(&s_power_input_pending, 0U);
    atomic_store(&s_standby_generation, 0U);
    atomic_store(&s_standby_bound_epoch, 0U);
    atomic_store(&s_standby_system_generation, 0U);
    atomic_store(&s_standby_commit_epoch, 0U);
    atomic_store(&s_standby_commit_generation, 0U);
    atomic_store(&s_standby_commit_pending, false);
    atomic_store(&s_standby_abort_pending, false);
    atomic_store(&s_touch_pending, false);
    atomic_store(&s_touch_pending_generation, 1U);
    atomic_store(&s_touch_generation, 1U);
    atomic_store(&s_touch_activity_enabled, true);
    atomic_store(&s_touch_restore_pending, false);
    atomic_store(&s_home_pending, false);
    atomic_store(&s_home_inflight, false);
    atomic_store(&s_brightness_temp_value, UINT8_MAX);
    atomic_store(&s_brightness_temp_generation, 0U);
    atomic_store(&s_brightness_temp_pending, false);
    atomic_store(&s_control_admitted, false);
    s_input_registered = false;
    s_power_wake_click_pending = false;
    s_power_long_press_seen = false;
}

static esp_err_t _control_start_task(void)
{
    if (xTaskCreate(app_manager_control_task, "app_control", APP_CONTROL_TASK_STACK,
                    NULL, APP_CONTROL_TASK_PRIORITY, &s_task) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_task_ready,
                       pdMS_TO_TICKS(APP_CONTROL_TASK_READY_MS)) != pdTRUE)
    {
        vTaskDelete(s_task);
        s_task = NULL;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_manager_control_init(const app_manager_config_t *config)
{
    int state = atomic_load(&s_lifecycle_state);
    if (state == CONTROL_READY)
    {
        return ESP_OK;
    }
    int expected = CONTROL_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_lifecycle_state, &expected,
                                        CONTROL_INITIALIZING))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_control_config_valid(config))
    {
        atomic_store(&s_lifecycle_state, CONTROL_UNINITIALIZED);
        return ESP_ERR_INVALID_ARG;
    }
    s_screen_ops = config->screen_ops;
    s_input_ops = config->input_ops;
    s_standby_ops = config->standby_ops;
    if (!_control_create_primitives())
    {
        _control_release_primitives();
        atomic_store(&s_lifecycle_state, CONTROL_UNINITIALIZED);
        return ESP_ERR_NO_MEM;
    }

    _control_load_settings();
    _control_reset_runtime_state();
    esp_err_t result = _control_start_task();
    if (result != ESP_OK)
    {
        _control_release_primitives();
        atomic_store(&s_lifecycle_state, CONTROL_UNINITIALIZED);
        return result;
    }
    atomic_store(&s_lifecycle_state, CONTROL_READY);
    result = app_manager_control_apply_brightness(s_brightness, true);
    if (result != ESP_OK)
    {
        (void)app_manager_control_deinit();
    }
    return result;
}

esp_err_t app_manager_control_open_admission(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    bool access_owned = _control_ready_access_begin();
    if (!access_owned)
    {
        goto exit;
    }
    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    lock_owned = true;
    if (atomic_load(&s_lifecycle_state) != CONTROL_READY)
    {
        goto exit;
    }
    if (atomic_load_explicit(&s_control_admitted, memory_order_acquire))
    {
        result = ESP_OK;
        goto exit;
    }

    s_last_activity_us = esp_timer_get_time();
    result = s_input_ops.register_handler(_control_input_callback, NULL);
    if (result != ESP_OK)
    {
        goto exit;
    }
    s_input_registered = true;
    if (atomic_load(&s_lifecycle_state) != CONTROL_READY)
    {
        result = s_input_ops.unregister_handler();
        if (result == ESP_OK)
        {
            s_input_registered = false;
            result = ESP_ERR_INVALID_STATE;
        }
        goto exit;
    }
    atomic_store_explicit(&s_control_admitted, true, memory_order_release);

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_call_lock);
    }
    if (access_owned)
    {
        app_manager_control_access_end();
    }
    return result;
}

static esp_err_t _control_stop_task(void)
{
    esp_err_t result = ESP_OK;
    if (s_input_registered && s_input_ops.unregister_handler != NULL)
    {
        result = s_input_ops.unregister_handler();
        if (result != ESP_OK)
        {
            return result;
        }
        s_input_registered = false;
    }

    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    (void)xSemaphoreTake(s_call_done, 0);
    (void)xSemaphoreTake(s_task_stopped, 0);
    control_command_t command =
    {
        .type = CONTROL_STOP,
        .synchronous = true,
    };
    if (xQueueSend(s_queue, &command, portMAX_DELAY) != pdTRUE)
    {
        result = ESP_ERR_NO_MEM;
    }
    else
    {
        (void)xSemaphoreTake(s_call_done, portMAX_DELAY);
        result = s_call_result;
        if (result == ESP_OK)
        {
            (void)xSemaphoreTake(s_task_stopped, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_call_lock);
    return result;
}

static void _control_release_primitives(void)
{
    if (s_task_stopped != NULL)
    {
        vSemaphoreDelete(s_task_stopped);
        s_task_stopped = NULL;
    }
    if (s_task_ready != NULL)
    {
        vSemaphoreDelete(s_task_ready);
        s_task_ready = NULL;
    }
    if (s_state_lock != NULL)
    {
        vSemaphoreDelete(s_state_lock);
        s_state_lock = NULL;
    }
    if (s_call_done != NULL)
    {
        vSemaphoreDelete(s_call_done);
        s_call_done = NULL;
    }
    if (s_call_lock != NULL)
    {
        vSemaphoreDelete(s_call_lock);
        s_call_lock = NULL;
    }
    if (s_queue != NULL)
    {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

static void _control_clear_runtime(void)
{
    memset(&s_screen_ops, 0, sizeof(s_screen_ops));
    memset(&s_input_ops, 0, sizeof(s_input_ops));
    memset(&s_standby_ops, 0, sizeof(s_standby_ops));
    atomic_store(&s_wake_pending, false);
    atomic_store(&s_power_pending_events, 0U);
    atomic_store(&s_power_pending_wake, false);
    atomic_store(&s_power_input_pending, 0U);
    atomic_store(&s_standby_generation, 0U);
    atomic_store(&s_standby_bound_epoch, 0U);
    atomic_store(&s_standby_system_generation, 0U);
    atomic_store(&s_standby_commit_epoch, 0U);
    atomic_store(&s_standby_commit_generation, 0U);
    atomic_store(&s_standby_commit_pending, false);
    atomic_store(&s_standby_abort_pending, false);
    atomic_store(&s_touch_pending, false);
    atomic_store(&s_touch_activity_enabled, false);
    atomic_store(&s_control_admitted, false);
    atomic_fetch_add(&s_touch_generation, 1U);
    _app_manager_control_clear_brightness_preview();
    s_power_wake_click_pending = false;
    s_power_long_press_seen = false;
    atomic_store(&s_lifecycle_state, CONTROL_UNINITIALIZED);
}

esp_err_t app_manager_control_deinit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const bool cleanup_owned = !atomic_flag_test_and_set_explicit(
                                   &s_deinit_busy, memory_order_acquire);
    if (!cleanup_owned)
    {
        goto exit;
    }
    result = ESP_OK;
    int state = atomic_load(&s_lifecycle_state);
    if (state == CONTROL_UNINITIALIZED)
    {
        goto exit;
    }
    if (state == CONTROL_INITIALIZING)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (state == CONTROL_READY)
    {
        int expected = CONTROL_READY;
        if (!atomic_compare_exchange_strong(&s_lifecycle_state, &expected,
                                            CONTROL_DEINITIALIZING))
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
    }

    atomic_store_explicit(&s_control_admitted, false, memory_order_release);

    _control_wait_for_accesses();
    result = _control_stop_task();
    if (result != ESP_OK)
    {
        goto exit;
    }
    _control_release_primitives();
    _control_clear_runtime();

exit:
    if (cleanup_owned)
    {
        atomic_flag_clear_explicit(&s_deinit_busy, memory_order_release);
    }
    return result;
}
