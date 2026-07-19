#define DBG_TAG "app_control"
#define DBG_LVL DBG_INFO
#ifdef HOST_TEST
    #include "host_control_port.h"
#endif

#include "app_manager.h"
#include "app_manager_control.h"
#include "app_manager_display.h"
#include "app_manager_pm_internal.h"

#include <stdatomic.h>

#ifdef HOST_TEST
    static atomic_bool s_touch_snapshot_pause;
    static atomic_bool s_touch_snapshot_reached;
#endif

esp_err_t app_manager_startup_commit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!app_manager_is_initialized())
    {
        goto exit;
    }
    result = app_manager_display_enable_startup_touch();
    if (result == ESP_OK)
    {
        result = app_manager_control_open_admission();
        if (result != ESP_OK)
        {
            (void)app_manager_display_disable_startup_touch();
        }
    }

exit:
    return result;
}

esp_err_t app_manager_input_inject(app_manager_key_t key,
                                   app_manager_key_event_t event)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (key < APP_MANAGER_KEY_HOME || key > APP_MANAGER_KEY_POWER ||
            event < APP_MANAGER_KEY_EVENT_DOWN ||
            event > APP_MANAGER_KEY_EVENT_DOUBLE_CLICK)
    {
        goto exit;
    }
    control_command_t command =
    {
        .type = CONTROL_INPUT,
        .data.input = {.key = key, .event = event},
    };

    if (key == APP_MANAGER_KEY_HOME && event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        if (!app_manager_control_access_begin())
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
        atomic_store(&s_wake_pending, true);
        if (!atomic_exchange(&s_home_pending, true))
        {
            (void)xQueueSend(s_queue, &command, 0);
        }
        app_manager_control_access_end();
        result = ESP_OK;
        goto exit;
    }
    result = app_manager_control_post(command);

exit:
    return result;
}

esp_err_t app_manager_pm_notify_user_activity(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    atomic_store_explicit(&s_standby_abort_pending, true,
                          memory_order_release);
    control_command_t command = {.type = CONTROL_WAKE_SIGNAL};
    if (xQueueSend(s_queue, &command, 0) != pdTRUE)
    {
        atomic_store(&s_wake_pending, true);
    }
    app_manager_control_access_end();
    result = ESP_OK;

exit:
    return result;
}

bool IRAM_ATTR app_manager_pm_notify_user_activity_from_isr(void)
{
    bool accepted = false;
    if (!app_manager_control_access_begin())
    {
        goto exit;
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
    accepted = true;

exit:
    return accepted;
}

bool IRAM_ATTR app_manager_control_notify_touch_activity_from_isr(void)
{
    bool accepted = false;
    bool access_owned = false;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    access_owned = true;
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
    if (access_owned)
    {
        app_manager_control_access_end();
    }
    return accepted;
}

#ifdef HOST_TEST
uint32_t app_manager_control_touch_generation_for_test(void)
{
    return atomic_load(&s_touch_generation);
}

bool app_manager_control_notify_touch_generation_for_test(uint32_t generation)
{
    bool queued = false;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    control_command_t command =
    {
        .type = CONTROL_TOUCH_ACTIVITY,
        .data.touch_generation = generation,
    };
    queued = xQueueSend(s_queue, &command, 0) == pdTRUE;
    app_manager_control_access_end();

exit:
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

app_manager_pm_state_t app_manager_pm_get_state(void)
{
    app_manager_pm_state_t state = APP_MANAGER_PM_ACTIVE;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    state = s_state;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();

exit:
    return state;
}

bool app_manager_pm_standby_commit_guard(uint32_t generation, void *context)
{
    (void)context;
    bool ready = false;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    if (xSemaphoreTake(s_state_lock, 0) != pdTRUE)
    {
        app_manager_control_access_end();
        goto exit;
    }
    ready = app_manager_control_standby_commit_ready(generation);
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();

exit:
    return ready;
}

bool app_manager_pm_standby_prepare_guard(void)
{
    bool clear = false;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    clear = !atomic_load_explicit(&s_standby_abort_pending,
                                  memory_order_acquire) &&
            atomic_load_explicit(&s_power_pending_events,
                                 memory_order_acquire) == 0U &&
            atomic_load_explicit(&s_power_input_pending,
                                 memory_order_acquire) == 0U &&
            !atomic_load_explicit(&s_power_pending_wake,
                                  memory_order_acquire) &&
            !atomic_load_explicit(&s_wake_pending,
                                  memory_order_acquire);
    app_manager_control_access_end();

exit:
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
    int32_t timeout_ms = APP_CONTROL_DEFAULT_OFF_MS;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    timeout_ms = s_off_timeout_ms;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();

exit:
    return timeout_ms;
}

esp_err_t app_manager_pm_set_timeout_ms(int32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (timeout_ms < -1 || (timeout_ms >= 0 && timeout_ms < s_dim_timeout_ms))
    {
        goto exit;
    }
    control_command_t command =
    {
        .type = CONTROL_SET_OFF_TIMEOUT,
        .data.timeout_ms = timeout_ms,
    };
    result = app_manager_control_call(command);

exit:
    return result;
}

int32_t app_manager_pm_get_standby_delay_ms(void)
{
    int32_t timeout_ms = APP_CONTROL_DEFAULT_STANDBY_MS;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    timeout_ms = s_standby_timeout_ms;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();

exit:
    return timeout_ms;
}

esp_err_t app_manager_pm_set_standby_delay_ms(int32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (timeout_ms < -1)
    {
        goto exit;
    }
    control_command_t command =
    {
        .type = CONTROL_SET_STANDBY_TIMEOUT,
        .data.timeout_ms = timeout_ms,
    };
    result = app_manager_control_call(command);

exit:
    return result;
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
    uint8_t brightness = UINT8_MAX;
    if (!app_manager_control_access_begin())
    {
        goto exit;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    brightness = s_brightness;
    xSemaphoreGive(s_state_lock);
    app_manager_control_access_end();

exit:
    return brightness;
}
