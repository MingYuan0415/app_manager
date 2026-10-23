#define DBG_TAG "app_control"
#ifndef HOST_TEST
    #include "mt_log.h"
#else
    #include "host_control_port.h"
#endif

#include "app_manager_display.h"
#include "app_manager_pm_internal.h"
#include "app_manager_pm_transition.h"
#ifndef HOST_TEST
    #include "app_manager_mailbox.h"
    #include "esp_timer.h"
    #include "nv_storage.h"
#endif
#include <string.h>

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
    atomic_store_explicit(&s_home_sequence_state, 1U,
                          memory_order_release);
    atomic_store_explicit(&s_brightness_temp_pending, false,
                          memory_order_release);
    s_power_wake_click_pending = false;
    s_power_long_press_seen = false;
    s_home_worker_wake_generation = 0U;
}

static esp_err_t _control_execute_input(const control_command_t *command)
{
    esp_err_t result = app_manager_control_handle_input(command->data.input.key,
        command->data.input.event,
        command->data.input.generation,
        command->data.input.event_us);
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
    if (state_was_suspended && s_screen_ops.is_suspended())
    {
        return ESP_OK;
    }
    esp_err_t result = app_manager_control_suspend_hardware();
    if (result == ESP_OK && !state_was_suspended)
    {
        s_screen_off_us = esp_timer_get_time();
        app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
    }
    return result;
}

static bool _control_can_apply_brightness(void)
{
    return !app_manager_display_is_sleeping() &&
           !s_screen_ops.is_suspended();
}

static esp_err_t _control_execute_brightness(uint8_t brightness)
{
    uint8_t previous = s_brightness;
    const bool apply_hardware = _control_can_apply_brightness();
    esp_err_t result = apply_hardware ?
                       app_manager_control_apply_brightness(brightness, true) :
                       ESP_OK;
    if (result != ESP_OK)
    {
        return result;
    }

    result = nv_storage_set_u8(NVS_KEY_BRIGHTNESS, brightness);
    if (result == ESP_OK)
    {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_brightness = brightness;
        xSemaphoreGive(s_state_lock);
    }
    else if (apply_hardware)
    {
        (void)app_manager_control_apply_brightness(previous, true);
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
        result = _control_can_apply_brightness() ?
                 app_manager_control_apply_brightness(
                     command->data.brightness, false) : ESP_OK;
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
    case CONTROL_STANDBY_NOW:
        s_standby_now_requested = true;
        result = _control_execute_suspend();
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
    if (!atomic_exchange(&s_wake_pending, false))
    {
        return true;
    }
    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
    {
        app_manager_control_standby_generation_abort();
    }
    return app_manager_control_handle_activity() == ESP_OK;
}

static bool _control_process_pending_power(void)
{
    const uint32_t events = atomic_exchange_explicit(
                                &s_power_pending_events, 0U,
                                memory_order_acq_rel);
    if (events == 0U)
    {
        return true;
    }

    atomic_fetch_and_explicit(&s_power_input_pending, ~events,
                              memory_order_acq_rel);

    const bool wake = atomic_exchange_explicit(&s_power_pending_wake, false,
        memory_order_acq_rel);
    if (wake || app_manager_control_screen_needs_wake())
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
            return false;
        }
        return true;
    }

    /* While ACTIVE only CLICK has an action; DOWN/UP/LONG are edge context
     * that is intentionally coalesced away when the queue is saturated. */
    if ((events & POWER_PENDING_EVENT(APP_MANAGER_KEY_EVENT_CLICK)) != 0U)
    {
        (void)app_manager_control_handle_input(APP_MANAGER_KEY_POWER,
                                               APP_MANAGER_KEY_EVENT_CLICK, 0U,
                                               esp_timer_get_time());
    }
    return true;
}

static bool _control_process_pending_touch_restore(void)
{
    if (!atomic_load_explicit(&s_touch_restore_pending, memory_order_acquire))
    {
        return false;
    }
    if (app_manager_control_screen_is_canonical_sleep())
    {
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
        return false;
    }
    if (app_manager_control_restore_active() == ESP_OK)
    {
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
    }
    return true;
}

static void _control_process_pending_standby_commit(void)
{
    if (!atomic_load_explicit(&s_standby_commit_pending,
                              memory_order_acquire))
    {
        return;
    }
    const uint32_t commit_epoch = atomic_load_explicit(
                                      &s_standby_commit_epoch,
                                      memory_order_acquire);
    const uint32_t commit_generation = atomic_load_explicit(
                                           &s_standby_commit_generation,
                                           memory_order_acquire);
    const bool current = commit_epoch != 0U &&
                         commit_epoch == atomic_load_explicit(
                             &s_standby_generation, memory_order_acquire) &&
                         commit_epoch == atomic_load_explicit(
                             &s_standby_bound_epoch, memory_order_acquire) &&
                         commit_generation == atomic_load_explicit(
                             &s_standby_system_generation,
                             memory_order_acquire) &&
                         !atomic_load_explicit(&s_standby_abort_pending,
                             memory_order_acquire);
    if (!current)
    {
        bool expected = true;
        (void)atomic_compare_exchange_strong_explicit(
            &s_standby_commit_pending, &expected, false,
            memory_order_acq_rel, memory_order_acquire);
        return;
    }

    /* Keep a valid commit pending until the prepare transition is visible to
     * the worker.  This closes the callback/worker scheduling window without
     * weakening generation and abort checks. */
    if (s_state != APP_MANAGER_PM_STANDBY_PREPARING)
    {
        return;
    }

    bool expected = true;
    if (!atomic_compare_exchange_strong_explicit(
                &s_standby_commit_pending, &expected, false,
                memory_order_acq_rel, memory_order_acquire))
    {
        return;
    }

    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING &&
            commit_epoch == atomic_load_explicit(&s_standby_generation,
                    memory_order_acquire) &&
            commit_generation == atomic_load_explicit(
                &s_standby_system_generation, memory_order_acquire) &&
            !atomic_load_explicit(&s_standby_abort_pending,
                                  memory_order_acquire))
    {
        app_manager_control_state_set(APP_MANAGER_PM_STANDBY);
    }
    else if (!atomic_load_explicit(&s_standby_abort_pending,
                                   memory_order_acquire))
    {
        /* State changed without aborting the same generation; leave the
         * request available for the next worker pass. */
        atomic_store_explicit(&s_standby_commit_pending, true,
                              memory_order_release);
    }
}

static void _control_process_pending_touch(void)
{
    if (!atomic_exchange(&s_touch_pending, false))
    {
        return;
    }
    (void)app_manager_control_handle_touch_activity(
        atomic_load(&s_touch_pending_generation));
}

static void _control_process_pending_brightness_temp(void)
{
    if (!atomic_exchange_explicit(&s_brightness_temp_pending, false,
                                  memory_order_acq_rel))
    {
        return;
    }

    const uint32_t generation = atomic_load_explicit(
                                    &s_brightness_temp_generation,
                                    memory_order_acquire);
    const uint8_t brightness = (uint8_t)atomic_load_explicit(
                                   &s_brightness_temp_value,
                                   memory_order_acquire);
    const esp_err_t result = _control_can_apply_brightness() ?
                             app_manager_control_apply_brightness(
                                 brightness, false) : ESP_OK;
    if (result != ESP_OK &&
            generation == atomic_load_explicit(&s_brightness_temp_generation,
                    memory_order_acquire))
    {
        atomic_store_explicit(&s_brightness_temp_pending, true,
                              memory_order_release);
    }
}

static bool _control_command_needs_wake_retry(const control_command_t *command,
        esp_err_t result)
{
    if (command->synchronous || result == ESP_OK)
    {
        return false;
    }

    bool wake_command = command->type == CONTROL_WAKE_SIGNAL ||
                        command->type == CONTROL_RESUME;
    if (command->type == CONTROL_INPUT)
    {
        wake_command = s_state != APP_MANAGER_PM_ACTIVE ||
                       s_screen_ops.is_suspended();
    }
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
    if (!atomic_load_explicit(&s_control_admitted, memory_order_acquire))
    {
        return;
    }
    int64_t now = esp_timer_get_time();
    int64_t idle_ms = (now - s_last_activity_us) / 1000;

    if (s_state == APP_MANAGER_PM_SCREEN_OFF)
    {
        if (!s_screen_ops.is_suspended() && app_manager_control_suspend_hardware() != ESP_OK)
        {
            return;
        }
        if ((s_standby_now_requested || s_standby_timeout_ms >= 0) &&
                (s_standby_now_requested ||
                 (now - s_screen_off_us) / 1000 >= s_standby_timeout_ms) &&
                s_standby_ops.request_standby != NULL)
        {
            if (s_standby_ops.is_standby_allowed != NULL &&
                    !s_standby_ops.is_standby_allowed())
            {
                return;
            }
            if (app_manager_control_standby_generation_begin() &&
                    s_standby_ops.request_standby() != ESP_OK)
            {
                app_manager_control_standby_generation_abort();
                app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
            }
            else
            {
                s_standby_now_requested = false;
            }
        }
        return;
    }
    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING ||
            s_state == APP_MANAGER_PM_STANDBY || s_off_timeout_ms < 0)
    {
        return;
    }
    if (s_state == APP_MANAGER_PM_ACTIVE && idle_ms >= s_dim_timeout_ms)
    {
        uint8_t dim = s_brightness < APP_CONTROL_DIM_BRIGHTNESS ?
                      s_brightness : APP_CONTROL_DIM_BRIGHTNESS;
        if (app_manager_control_apply_brightness(dim, false) == ESP_OK)
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
            atomic_store(&s_home_click_state, APP_MANAGER_HOME_CLICK_IDLE);
            atomic_store(&s_home_click_generation, 0U);
            s_home_click_deadline_us = 0;
            s_home_action_count = 0U;
            s_home_action_head = 0U;
            memset(s_home_action_fifo, 0, sizeof(s_home_action_fifo));
            while (atomic_flag_test_and_set_explicit(
                        &s_home_click_fallback_lock, memory_order_acquire))
            {
            }
            s_home_click_fallback_head = 0U;
            s_home_click_fallback_count = 0U;
            memset(s_home_click_fallback, 0, sizeof(s_home_click_fallback));
            atomic_flag_clear_explicit(&s_home_click_fallback_lock,
                                       memory_order_release);
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
        const TickType_t poll_ticks =
            pdMS_TO_TICKS(app_manager_control_home_click_wait_ms());
        if (xQueueReceive(s_queue, &command, poll_ticks) == pdTRUE)
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
        const bool restore_attempted =
            _control_process_pending_touch_restore();
        _control_process_pending_touch();
        app_manager_control_process_pending_home();
        if (!restore_attempted)
        {
            _control_poll_pm();
        }
    }
}

esp_err_t app_manager_control_post(control_command_t command)
{
    if (!app_manager_control_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
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
        app_manager_control_access_end();
        return ESP_OK;
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
        if (app_manager_control_input_needs_wake())
        {
            atomic_store_explicit(&s_power_pending_wake, true,
                                  memory_order_release);
        }
    }
    app_manager_control_access_end();
    return queued ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t app_manager_control_call(control_command_t command)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool access_owned = false;
    bool lock_owned = false;
#ifndef HOST_TEST
    /* The PM worker answers synchronous commands by dispatching parts of
     * them onto the UI worker; calling from the UI worker would deadlock.
     * Non-UI callers use this, UI callers must use app_manager_control_post.
     */
    if (app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif
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
