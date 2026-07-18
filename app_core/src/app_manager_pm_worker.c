#define DBG_TAG "app_control"
#define DBG_LVL DBG_INFO
#ifndef HOST_TEST
    #include "mt_log.h"
#else
    #include "host_control_port.h"
#endif

#include "app_manager_pm_internal.h"
#include "app_manager_pm_transition.h"
#ifndef HOST_TEST
    #include "esp_timer.h"
    #include "nv_storage.h"
#endif

static void _control_discard_shutdown_pending(void)
{
    atomic_store_explicit(&s_wake_pending, false, memory_order_release);
    atomic_store_explicit(&s_power_pending_events, 0U, memory_order_release);
    atomic_store_explicit(&s_power_pending_wake, false, memory_order_release);
    atomic_store_explicit(&s_power_input_pending, 0U, memory_order_release);
    atomic_store_explicit(&s_touch_pending, false, memory_order_release);
    atomic_store_explicit(&s_touch_restore_pending, false,
                          memory_order_release);
    atomic_store_explicit(&s_standby_commit_pending, false,
                          memory_order_release);
    atomic_store_explicit(&s_home_pending, false, memory_order_release);
    atomic_store_explicit(&s_brightness_temp_pending, false,
                          memory_order_release);
    s_power_wake_click_pending = false;
    s_power_long_press_seen = false;
}

static esp_err_t _control_execute_input(const control_command_t *command)
{
    esp_err_t result = app_manager_control_handle_input(command->data.input.key,
                       command->data.input.event);
    if (command->data.input.key == APP_MANAGER_KEY_POWER)
    {
        atomic_fetch_and_explicit(
            &s_power_input_pending,
            ~POWER_PENDING_EVENT(command->data.input.event),
            memory_order_acq_rel);
    }
    return result;
}

static esp_err_t _control_execute_suspend(void)
{
    bool state_was_suspended = s_state == APP_MANAGER_PM_SCREEN_OFF ||
                               s_state == APP_MANAGER_PM_STANDBY;
    esp_err_t result = ESP_OK;
    if (state_was_suspended && s_screen_ops.is_suspended())
    {
        goto exit;
    }
    result = app_manager_control_suspend_hardware();
    if (result == ESP_OK && !state_was_suspended)
    {
        s_screen_off_us = esp_timer_get_time();
        app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
    }

exit:
    return result;
}

static esp_err_t _control_execute_brightness(uint8_t brightness)
{
    uint8_t previous = s_brightness;
    esp_err_t result = s_screen_ops.set_brightness(brightness);
    if (result == ESP_OK)
    {
        result = nv_storage_set_u8(NVS_KEY_BRIGHTNESS, brightness);
        if (result == ESP_OK)
        {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_brightness = brightness;
            xSemaphoreGive(s_state_lock);
        }
        else
        {
            (void)s_screen_ops.set_brightness(previous);
        }
    }
    return result;
}

static esp_err_t _control_execute_timeout(
    const char *key, int32_t timeout_ms, int32_t *target)
{
    esp_err_t result = nv_storage_set_u32(key, (uint32_t)timeout_ms);
    if (result == ESP_OK)
    {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        *target = timeout_ms;
        xSemaphoreGive(s_state_lock);
    }
    return result;
}

static esp_err_t _control_execute(const control_command_t *command)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    switch (command->type)
    {
    case CONTROL_WAKE_SIGNAL:
        if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
        {
            app_manager_control_standby_generation_abort();
        }
        result = app_manager_control_handle_activity();
        break;
    case CONTROL_TOUCH_ACTIVITY:
        result = app_manager_control_handle_touch_activity(command->data.touch_generation);
        break;
    case CONTROL_INPUT:
        result = _control_execute_input(command);
        break;
    case CONTROL_SUSPEND:
        result = _control_execute_suspend();
        break;
    case CONTROL_RESUME:
        result = app_manager_control_handle_activity();
        break;
    case CONTROL_SET_BRIGHTNESS:
        result = _control_execute_brightness(command->data.brightness);
        break;
    case CONTROL_SET_BRIGHTNESS_TEMP:
        result = s_screen_ops.set_brightness_temp(command->data.brightness);
        break;
    case CONTROL_SET_OFF_TIMEOUT:
        result = _control_execute_timeout(
                     NVS_KEY_OFF_TIMEOUT, command->data.timeout_ms,
                     &s_off_timeout_ms);
        break;
    case CONTROL_SET_STANDBY_TIMEOUT:
        result = _control_execute_timeout(
                     NVS_KEY_STANDBY_TIMEOUT, command->data.timeout_ms,
                     &s_standby_timeout_ms);
        break;
    case CONTROL_STOP:
        _control_discard_shutdown_pending();
        result = app_manager_control_prepare_shutdown();
        break;
    }
    return result;
}

static bool _control_process_pending_wake(void)
{
    bool success = true;
    if (!atomic_exchange(&s_wake_pending, false))
    {
        goto exit;
    }
    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
    {
        app_manager_control_standby_generation_abort();
    }
    success = app_manager_control_handle_activity() == ESP_OK;

exit:
    return success;
}

static bool _control_process_pending_power(void)
{
    bool success = true;
    const uint32_t events = atomic_exchange_explicit(
                                &s_power_pending_events, 0U,
                                memory_order_acq_rel);
    if (events == 0U)
    {
        goto exit;
    }

    atomic_fetch_and_explicit(&s_power_input_pending, ~events,
                              memory_order_acq_rel);

    const bool wake = atomic_exchange_explicit(&s_power_pending_wake, false,
                      memory_order_acq_rel);
    if (wake || app_manager_control_power_event_is_wake())
    {
        if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
        {
            app_manager_control_standby_generation_abort();
        }
        /* A coalesced DOWN/UP/CLICK is one wake intent.  Do not replay the
         * individual edges after recovery or a short press could toggle twice. */
        if (app_manager_control_handle_activity() != ESP_OK)
        {
            atomic_fetch_or_explicit(&s_power_pending_events, events,
                                     memory_order_release);
            atomic_store_explicit(&s_power_pending_wake, true,
                                  memory_order_release);
            success = false;
            goto exit;
        }
        goto exit;
    }

    /* While ACTIVE only CLICK has an action; DOWN/UP/LONG are edge context
     * that is intentionally coalesced away when the queue is saturated. */
    if ((events & POWER_PENDING_EVENT(APP_MANAGER_KEY_EVENT_CLICK)) != 0U)
    {
        (void)app_manager_control_handle_input(APP_MANAGER_KEY_POWER,
                                               APP_MANAGER_KEY_EVENT_CLICK);
    }

exit:
    return success;
}

static void _control_process_pending_touch_restore(void)
{
    if (!atomic_load_explicit(&s_touch_restore_pending, memory_order_acquire))
    {
        goto exit;
    }
    if (app_manager_control_screen_is_canonical_sleep())
    {
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
        goto exit;
    }
    if (app_manager_control_restore_active() == ESP_OK)
    {
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
    }

exit:
    return;
}

static void _control_process_pending_standby_commit(void)
{
    if (!atomic_exchange_explicit(&s_standby_commit_pending, false,
                                  memory_order_acq_rel))
    {
        goto exit;
    }
    const uint32_t commit_epoch = atomic_load_explicit(
                                      &s_standby_commit_epoch,
                                      memory_order_acquire);
    const uint32_t commit_generation = atomic_load_explicit(
                                           &s_standby_commit_generation,
                                           memory_order_acquire);
    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING &&
            commit_epoch != 0U &&
            commit_epoch == atomic_load_explicit(&s_standby_generation,
                    memory_order_acquire) &&
            commit_epoch == atomic_load_explicit(&s_standby_bound_epoch,
                    memory_order_acquire) &&
            commit_generation == atomic_load_explicit(
                &s_standby_system_generation, memory_order_acquire) &&
            !atomic_load_explicit(&s_standby_abort_pending,
                                  memory_order_acquire))
    {
        app_manager_control_state_set(APP_MANAGER_PM_STANDBY);
    }

exit:
    return;
}

static void _control_process_pending_touch(void)
{
    if (!atomic_exchange(&s_touch_pending, false))
    {
        goto exit;
    }
    (void)app_manager_control_handle_touch_activity(
        atomic_load(&s_touch_pending_generation));

exit:
    return;
}

static void _control_process_pending_brightness_temp(void)
{
    if (!atomic_exchange_explicit(&s_brightness_temp_pending, false,
                                  memory_order_acq_rel))
    {
        goto exit;
    }

    const uint32_t generation = atomic_load_explicit(
                                    &s_brightness_temp_generation,
                                    memory_order_acquire);
    const uint8_t brightness = (uint8_t)atomic_load_explicit(
                                   &s_brightness_temp_value,
                                   memory_order_acquire);
    const esp_err_t result = s_screen_ops.set_brightness_temp(brightness);
    if (result != ESP_OK &&
            generation == atomic_load_explicit(&s_brightness_temp_generation,
                    memory_order_acquire))
    {
        atomic_store_explicit(&s_brightness_temp_pending, true,
                              memory_order_release);
    }

exit:
    return;
}

static bool _control_command_needs_wake_retry(const control_command_t *command,
        esp_err_t result)
{
    bool wake_command = false;
    if (command->synchronous || result == ESP_OK)
    {
        goto exit;
    }

    wake_command = command->type == CONTROL_WAKE_SIGNAL ||
                   command->type == CONTROL_RESUME;
    if (command->type == CONTROL_INPUT)
    {
        wake_command = s_state != APP_MANAGER_PM_ACTIVE ||
                       s_screen_ops.is_suspended();
    }

exit:
    return wake_command;
}

static void _control_wake_retry_delay(void)
{
#ifdef HOST_TEST
    usleep(APP_CONTROL_POLL_MS * 1000U);
#else
    vTaskDelay(pdMS_TO_TICKS(APP_CONTROL_POLL_MS));
#endif
}

static void _control_poll_pm(void)
{
    int64_t now = esp_timer_get_time();
    int64_t idle_ms = (now - s_last_activity_us) / 1000;

    if (s_state == APP_MANAGER_PM_SCREEN_OFF)
    {
        if (!s_screen_ops.is_suspended() && app_manager_control_suspend_hardware() != ESP_OK)
        {
            goto exit;
        }
        if (s_standby_timeout_ms >= 0 &&
                (now - s_screen_off_us) / 1000 >= s_standby_timeout_ms &&
                s_standby_ops.request_standby != NULL)
        {
            if (app_manager_control_standby_generation_begin() &&
                    s_standby_ops.request_standby() != ESP_OK)
            {
                app_manager_control_standby_generation_abort();
                app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
            }
        }
        goto exit;
    }
    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING ||
            s_state == APP_MANAGER_PM_STANDBY || s_off_timeout_ms < 0)
    {
        goto exit;
    }
    if (s_state == APP_MANAGER_PM_ACTIVE && idle_ms >= s_dim_timeout_ms)
    {
        uint8_t dim = s_brightness < APP_CONTROL_DIM_BRIGHTNESS ?
                      s_brightness : APP_CONTROL_DIM_BRIGHTNESS;
        if (s_screen_ops.set_brightness_temp(dim) == ESP_OK)
        {
            app_manager_control_state_set(APP_MANAGER_PM_DIM);
        }
    }
    else if (s_state == APP_MANAGER_PM_DIM && idle_ms >= s_off_timeout_ms)
    {
        if (app_manager_control_suspend_hardware() == ESP_OK)
        {
            s_screen_off_us = now;
            app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
        }
    }

exit:
    return;
}

void app_manager_control_task(void *arg)
{
    (void)arg;
    bool wake_retry_pending = false;
    xSemaphoreGive(s_task_ready);
    for (;;)
    {
        if (atomic_load_explicit(&s_lifecycle_state, memory_order_acquire) ==
                CONTROL_DEINITIALIZING)
        {
            wake_retry_pending = false;
            _control_discard_shutdown_pending();
        }
        else if (wake_retry_pending)
        {
            if (app_manager_control_handle_activity() != ESP_OK)
            {
                _control_wake_retry_delay();
                continue;
            }
            wake_retry_pending = false;
        }

        bool command_wake_retry = false;
        control_command_t command;
        if (xQueueReceive(s_queue, &command,
                          pdMS_TO_TICKS(APP_CONTROL_POLL_MS)) == pdTRUE)
        {
            for (size_t drained = 0; drained < APP_CONTROL_QUEUE_DEPTH; drained++)
            {
                const bool deinitializing = atomic_load_explicit(
                                                &s_lifecycle_state,
                                                memory_order_acquire) ==
                                            CONTROL_DEINITIALIZING;
                esp_err_t result = deinitializing &&
                                   command.type != CONTROL_STOP ?
                                   ESP_ERR_INVALID_STATE :
                                   _control_execute(&command);
                command_wake_retry =
                    _control_command_needs_wake_retry(&command, result);
                if (command.synchronous)
                {
                    s_call_result = result;
                    xSemaphoreGive(s_call_done);
                }
                if (command.type == CONTROL_STOP && result == ESP_OK)
                {
                    s_task = NULL;
                    xSemaphoreGive(s_task_stopped);
                    vTaskDelete(NULL);
                    return;
                }
                if (command.type == CONTROL_STOP || command_wake_retry ||
                        drained + 1U == APP_CONTROL_QUEUE_DEPTH ||
                        xQueueReceive(s_queue, &command, 0) != pdTRUE)
                {
                    break;
                }
            }
        }
        if (atomic_load_explicit(&s_lifecycle_state, memory_order_acquire) ==
                CONTROL_DEINITIALIZING)
        {
            wake_retry_pending = false;
            _control_discard_shutdown_pending();
            continue;
        }
        if (command_wake_retry && app_manager_control_handle_activity() != ESP_OK)
        {
            wake_retry_pending = true;
            _control_wake_retry_delay();
            continue;
        }
        if (!_control_process_pending_wake())
        {
            wake_retry_pending = true;
            _control_wake_retry_delay();
            continue;
        }
        if (!_control_process_pending_power())
        {
            _control_wake_retry_delay();
            continue;
        }
        _control_process_pending_brightness_temp();
        _control_process_pending_standby_commit();
        _control_process_pending_touch_restore();
        _control_process_pending_touch();
        app_manager_control_process_pending_home();
        _control_poll_pm();
    }
}

esp_err_t app_manager_control_post(control_command_t command)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    command.synchronous = false;
    if (command.type == CONTROL_SET_BRIGHTNESS_TEMP)
    {
        atomic_store_explicit(&s_brightness_temp_value,
                              command.data.brightness,
                              memory_order_relaxed);
        atomic_fetch_add_explicit(&s_brightness_temp_generation, 1U,
                                  memory_order_release);
        atomic_store_explicit(&s_brightness_temp_pending, true,
                              memory_order_release);
        result = ESP_OK;
        app_manager_control_access_end();
        goto exit;
    }
    if (command.type == CONTROL_SET_BRIGHTNESS)
    {
        _app_manager_control_clear_brightness_preview();
    }
    if (command.type == CONTROL_INPUT &&
            command.data.input.key == APP_MANAGER_KEY_POWER)
    {
        atomic_fetch_or_explicit(&s_power_input_pending,
                                 POWER_PENDING_EVENT(command.data.input.event),
                                 memory_order_release);
        atomic_store_explicit(&s_standby_abort_pending, true,
                              memory_order_release);
    }
    const bool queued = xQueueSend(s_queue, &command, 0) == pdTRUE;
    if (!queued && command.type == CONTROL_INPUT &&
            command.data.input.key == APP_MANAGER_KEY_POWER)
    {
        atomic_fetch_or_explicit(&s_power_pending_events,
                                 POWER_PENDING_EVENT(command.data.input.event),
                                 memory_order_release);
        if (app_manager_control_power_event_is_wake())
        {
            atomic_store_explicit(&s_power_pending_wake, true,
                                  memory_order_release);
        }
    }
    result = queued ? ESP_OK : ESP_ERR_NO_MEM;
    app_manager_control_access_end();

exit:
    return result;
}

esp_err_t app_manager_control_call(control_command_t command)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool access_owned = false;
    bool lock_owned = false;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    access_owned = true;
    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    lock_owned = true;
    if (atomic_load(&s_lifecycle_state) != CONTROL_READY)
    {
        goto exit;
    }
    (void)xSemaphoreTake(s_call_done, 0);
    command.synchronous = true;
    if (xQueueSend(s_queue, &command, portMAX_DELAY) != pdTRUE)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    xSemaphoreTake(s_call_done, portMAX_DELAY);
    result = s_call_result;

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
