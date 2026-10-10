#define DBG_TAG "app_control"
#define DBG_LVL DBG_INFO
#ifdef HOST_TEST
    #include "host_control_port.h"
#endif

#include "app_manager.h"
#include "app_manager_control.h"
#include "app_manager_display.h"
#include "app_manager_pm_internal.h"
#include "app_manager_pm_transition.h"
#include "esp_timer.h"

#include <stdatomic.h>

#ifdef HOST_TEST
    static atomic_bool s_touch_snapshot_pause;
    static atomic_bool s_touch_snapshot_reached;
#endif

esp_err_t app_manager_startup_commit(void)
{
    if (!app_manager_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = app_manager_display_enable_startup_touch();
    if (result == ESP_OK)
    {
        result = app_manager_control_open_admission();
        if (result != ESP_OK)
        {
            (void)app_manager_display_disable_startup_touch();
        }
    }
    return result;
}

static esp_err_t _app_manager_input_inject_home(
    app_manager_key_event_t event, control_command_t command)
{
    if (!app_manager_control_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t generation = atomic_load_explicit(&s_home_sequence_state,
                          memory_order_acquire) &
                          HOME_SEQUENCE_GENERATION_MASK;
    const bool input_needs_wake = app_manager_control_input_needs_wake();
    if (event == APP_MANAGER_KEY_EVENT_DOWN ||
            event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        atomic_store_explicit(&s_standby_abort_pending, true,
                              memory_order_release);
    }
    if (input_needs_wake &&
            (event == APP_MANAGER_KEY_EVENT_DOWN ||
             event == APP_MANAGER_KEY_EVENT_CLICK))
    {
        atomic_store_explicit(&s_wake_pending, true,
                              memory_order_release);
    }
    if (event == APP_MANAGER_KEY_EVENT_DOWN)
    {
        generation = _app_manager_control_home_sequence_begin();
        if (input_needs_wake)
        {
            _app_manager_control_home_sequence_mark_wake(generation);
        }
    }
    command.data.input.generation = generation;
    if (event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        command.data.input.event_us = esp_timer_get_time();
    }

    if (event == APP_MANAGER_KEY_EVENT_LONG_PRESS ||
            event == APP_MANAGER_KEY_EVENT_DOUBLE_CLICK)
    {
        _app_manager_control_home_sequence_close(generation);
    }
    else if (event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        if (input_needs_wake)
        {
            _app_manager_control_home_sequence_mark_wake(generation);
        }
        const bool wake_only =
            _app_manager_control_home_sequence_is_wake(generation);
        atomic_store_explicit(&s_wake_pending, true, memory_order_release);
        if (wake_only)
        {
            _app_manager_control_home_sequence_close(generation);
            command.type = CONTROL_WAKE_SIGNAL;
            (void)xQueueSend(s_queue, &command, 0);
            app_manager_control_access_end();
            return ESP_OK;
        }
        if (xQueueSend(s_queue, &command, 0) != pdTRUE)
        {
            /* A saturated queue must not lose the navigation intent: latch
             * the click (with its admission time) for the worker to drain. */
            bool latched = false;
            while (atomic_flag_test_and_set_explicit(
                        &s_home_click_fallback_lock, memory_order_acquire))
            {
            }
            if (s_home_click_fallback_count <
                    APP_MANAGER_HOME_CLICK_FALLBACK_DEPTH)
            {
                const uint32_t tail =
                    (s_home_click_fallback_head +
                     s_home_click_fallback_count) %
                    APP_MANAGER_HOME_CLICK_FALLBACK_DEPTH;
                s_home_click_fallback[tail].generation = generation;
                s_home_click_fallback[tail].event_us =
                    command.data.input.event_us;
                s_home_click_fallback[tail].epoch =
                    atomic_load_explicit(&s_home_click_fallback_epoch,
                                         memory_order_relaxed);
                s_home_click_fallback_count++;
                latched = true;
            }
            atomic_flag_clear_explicit(&s_home_click_fallback_lock,
                                       memory_order_release);
            if (!latched)
            {
                app_manager_control_access_end();
                return ESP_ERR_NO_MEM;
            }
        }
        app_manager_control_access_end();
        return ESP_OK;
    }
    app_manager_control_access_end();
    return app_manager_control_post(command);
}

esp_err_t app_manager_input_inject(app_manager_key_t key,
                                   app_manager_key_event_t event)
{
    if (key < APP_MANAGER_KEY_HOME || key > APP_MANAGER_KEY_POWER ||
            event < APP_MANAGER_KEY_EVENT_DOWN ||
            event > APP_MANAGER_KEY_EVENT_DOUBLE_CLICK)
    {
        return ESP_ERR_INVALID_ARG;
    }
    control_command_t command =
    {
        .type = CONTROL_INPUT,
        .data.input = {.key = key, .event = event},
    };

    if (key == APP_MANAGER_KEY_HOME)
    {
        return _app_manager_input_inject_home(event, command);
    }
    return app_manager_control_post(command);
}

esp_err_t app_manager_pm_notify_user_activity(void)
{
    if (!app_manager_control_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_standby_abort_pending, true,
                          memory_order_release);
    control_command_t command = {.type = CONTROL_WAKE_SIGNAL};
    if (xQueueSend(s_queue, &command, 0) != pdTRUE)
    {
        atomic_store(&s_wake_pending, true);
    }
    app_manager_control_access_end();
    return ESP_OK;
}

bool IRAM_ATTR app_manager_pm_notify_user_activity_from_isr(void)
{
    if (!app_manager_control_access_begin())
    {
        return false;
    }
    atomic_store_explicit(&s_standby_abort_pending, true,
                          memory_order_release);
    control_command_t command = {.type = CONTROL_WAKE_SIGNAL};
    BaseType_t task_woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &command, &task_woken) != pdTRUE)
    {
        atomic_store(&s_wake_pending, true);
    }
    app_manager_control_access_end();
    return true;
}

bool IRAM_ATTR app_manager_control_notify_touch_activity_from_isr(void)
{
    bool accepted = false;
    if (!app_manager_control_access_begin())
    {
        return false;
    }
    uint32_t generation = atomic_load(&s_touch_generation);
#ifdef HOST_TEST
    if (atomic_load(&s_touch_snapshot_pause))
    {
        atomic_store(&s_touch_snapshot_reached, true);
        while (atomic_load(&s_touch_snapshot_pause))
        {
            usleep(1000);
        }
    }
#endif
    if (!atomic_load(&s_touch_activity_enabled))
    {
        goto exit;
    }
    if (generation != atomic_load(&s_touch_generation) ||
            !atomic_load(&s_touch_activity_enabled))
    {
        goto exit;
    }

    control_command_t command =
    {
        .type = CONTROL_TOUCH_ACTIVITY,
        .data.touch_generation = generation,
    };
    BaseType_t task_woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &command, &task_woken) != pdTRUE)
    {
        atomic_store(&s_touch_pending_generation, generation);
        atomic_store(&s_touch_pending, true);
    }
    accepted = true;

exit:
    app_manager_control_access_end();
    return accepted;
}

#ifdef HOST_TEST
uint32_t app_manager_control_touch_generation_for_test(void)
{
    return atomic_load(&s_touch_generation);
}

bool app_manager_control_notify_touch_generation_for_test(uint32_t generation)
{
    if (!app_manager_control_access_begin())
    {
        return false;
    }
    control_command_t command =
    {
        .type = CONTROL_TOUCH_ACTIVITY,
        .data.touch_generation = generation,
    };
    bool queued = xQueueSend(s_queue, &command, 0) == pdTRUE;
    app_manager_control_access_end();
    return queued;
}

void app_manager_control_touch_snapshot_pause_for_test(bool pause)
{
    if (pause)
    {
        atomic_store(&s_touch_snapshot_reached, false);
    }
    atomic_store(&s_touch_snapshot_pause, pause);
}

bool app_manager_control_touch_snapshot_reached_for_test(void)
{
    return atomic_load(&s_touch_snapshot_reached);
}

uint32_t app_manager_control_standby_generation_for_test(void)
{
    return atomic_load_explicit(&s_standby_generation, memory_order_acquire);
}
#endif

esp_err_t app_manager_pm_notify_system_wake(void)
{
    return app_manager_pm_notify_user_activity();
}

esp_err_t app_manager_pm_request_screen_off(void)
{
    return app_manager_screen_suspend();
}

esp_err_t app_manager_pm_request_standby(void)
{
    control_command_t command = {.type = CONTROL_STANDBY_NOW};
    return app_manager_control_post(command);
}

app_manager_pm_state_t app_manager_pm_get_state(void)
{
    if (!app_manager_control_access_begin())
    {
        return APP_MANAGER_PM_ACTIVE;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    app_manager_pm_state_t state = s_state;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();
    return state;
}

bool app_manager_pm_standby_commit_guard(uint32_t generation, void *context)
{
    (void)context;
    if (!app_manager_control_access_begin())
    {
        return false;
    }
    if (xSemaphoreTake(s_state_lock, 0) != pdTRUE)
    {
        app_manager_control_access_end();
        return false;
    }
    bool ready = app_manager_control_standby_commit_ready(generation);
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();
    return ready;
}

bool app_manager_pm_standby_prepare_guard(void)
{
    if (!app_manager_control_access_begin())
    {
        return false;
    }
    bool clear = !atomic_load_explicit(&s_standby_abort_pending,
                                       memory_order_acquire) &&
                 atomic_load_explicit(&s_power_pending_events,
                                      memory_order_acquire) == 0U &&
                 atomic_load_explicit(&s_power_input_pending,
                                      memory_order_acquire) == 0U &&
                 !atomic_load_explicit(&s_power_pending_wake,
                                       memory_order_acquire) &&
                 !atomic_load_explicit(&s_wake_pending,
                                       memory_order_acquire) &&
                 !_app_manager_control_home_wake_is_pending() &&
                 atomic_load(&s_home_click_state) ==
                 APP_MANAGER_HOME_CLICK_IDLE;
    app_manager_control_access_end();
    return clear;
}

void app_manager_pm_standby_commit_callback(uint32_t generation, void *context)
{
    (void)context;
    if (app_manager_control_access_begin())
    {
        const uint32_t bound_epoch = atomic_load_explicit(
                                         &s_standby_bound_epoch,
                                         memory_order_acquire);
        if (bound_epoch != 0U &&
                bound_epoch == atomic_load_explicit(&s_standby_generation,
                        memory_order_acquire) &&
                generation == atomic_load_explicit(&s_standby_system_generation,
                        memory_order_acquire) &&
                !atomic_load_explicit(&s_standby_abort_pending,
                                      memory_order_acquire))
        {
            atomic_store_explicit(&s_standby_commit_epoch, bound_epoch,
                                  memory_order_relaxed);
            atomic_store_explicit(&s_standby_commit_generation, generation,
                                  memory_order_relaxed);
            atomic_store_explicit(&s_standby_commit_pending, true,
                                  memory_order_release);
        }
        app_manager_control_access_end();
    }
}

int32_t app_manager_pm_get_timeout_ms(void)
{
    if (!app_manager_control_access_begin())
    {
        return APP_CONTROL_DEFAULT_OFF_MS;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int32_t timeout_ms = s_off_timeout_ms;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();
    return timeout_ms;
}

esp_err_t app_manager_pm_set_timeout_ms(int32_t timeout_ms)
{
    if (timeout_ms < -1 || (timeout_ms >= 0 && timeout_ms < s_dim_timeout_ms))
    {
        return ESP_ERR_INVALID_ARG;
    }
    control_command_t command =
    {
        .type = CONTROL_SET_OFF_TIMEOUT,
        .data.timeout_ms = timeout_ms,
    };
    return app_manager_control_call(command);
}

esp_err_t app_manager_pm_set_timeout_ms_async(int32_t timeout_ms)
{
    if (timeout_ms < -1 || (timeout_ms >= 0 && timeout_ms < s_dim_timeout_ms))
    {
        return ESP_ERR_INVALID_ARG;
    }
    control_command_t command =
    {
        .type = CONTROL_SET_OFF_TIMEOUT,
        .data.timeout_ms = timeout_ms,
    };
    return app_manager_control_post(command);
}

int32_t app_manager_pm_get_standby_delay_ms(void)
{
    if (!app_manager_control_access_begin())
    {
        return APP_CONTROL_DEFAULT_STANDBY_MS;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int32_t timeout_ms = s_standby_timeout_ms;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();
    return timeout_ms;
}

esp_err_t app_manager_pm_set_standby_delay_ms(int32_t timeout_ms)
{
    if (timeout_ms < -1)
    {
        return ESP_ERR_INVALID_ARG;
    }
    control_command_t command =
    {
        .type = CONTROL_SET_STANDBY_TIMEOUT,
        .data.timeout_ms = timeout_ms,
    };
    return app_manager_control_call(command);
}

esp_err_t app_manager_pm_set_standby_delay_ms_async(int32_t timeout_ms)
{
    if (timeout_ms < -1)
    {
        return ESP_ERR_INVALID_ARG;
    }
    control_command_t command =
    {
        .type = CONTROL_SET_STANDBY_TIMEOUT,
        .data.timeout_ms = timeout_ms,
    };
    return app_manager_control_post(command);
}

esp_err_t app_manager_screen_suspend(void)
{
    control_command_t command = {.type = CONTROL_SUSPEND};
    return app_manager_control_post(command);
}

esp_err_t app_manager_screen_resume(void)
{
    control_command_t command = {.type = CONTROL_RESUME};
    return app_manager_control_post(command);
}

bool app_manager_screen_is_suspended(void)
{
    app_manager_pm_state_t state = app_manager_pm_get_state();
    return state == APP_MANAGER_PM_SCREEN_OFF ||
           state == APP_MANAGER_PM_STANDBY_PREPARING ||
           state == APP_MANAGER_PM_STANDBY;
}

esp_err_t app_manager_screen_set_brightness(uint8_t brightness)
{
    control_command_t command =
    {
        .type = CONTROL_SET_BRIGHTNESS,
        .data.brightness = brightness,
    };
    return app_manager_control_post(command);
}

esp_err_t app_manager_screen_set_brightness_temp(uint8_t brightness)
{
    control_command_t command =
    {
        .type = CONTROL_SET_BRIGHTNESS_TEMP,
        .data.brightness = brightness,
    };
    return app_manager_control_post(command);
}

uint8_t app_manager_screen_get_brightness(void)
{
    if (!app_manager_control_access_begin())
    {
        return UINT8_MAX;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    uint8_t brightness = s_brightness;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();
    return brightness;
}
