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

#include "app_manager_control.h"
#include "app_manager_display.h"
#include "app_manager_pm_internal.h"
#include "app_manager_pm_transition.h"
#ifndef HOST_TEST
    #include "esp_timer.h"
    #include "nv_storage.h"
#endif

void app_manager_control_state_set(app_manager_pm_state_t state)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_state_lock);
    if (state == APP_MANAGER_PM_SCREEN_OFF ||
            state == APP_MANAGER_PM_STANDBY_PREPARING ||
            state == APP_MANAGER_PM_STANDBY)
    {
        atomic_store_explicit(&s_input_wake_required, true,
                              memory_order_release);
    }
}

static void _control_disable_touch_activity(void)
{
    atomic_fetch_add(&s_touch_generation, 1U);
    atomic_store(&s_touch_activity_enabled, false);
    atomic_store(&s_touch_pending, false);
}

static void _control_enable_touch_activity(void)
{
    atomic_fetch_add(&s_touch_generation, 1U);
    atomic_store(&s_touch_activity_enabled, true);
}

bool app_manager_control_screen_is_canonical_sleep(void)
{
    return s_screen_ops.is_suspend_committed() &&
           app_manager_display_is_sleeping();
}

static void _control_schedule_touch_restore(void)
{
    atomic_store_explicit(&s_touch_restore_pending, true,
                          memory_order_release);
}

bool app_manager_control_screen_needs_wake(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const app_manager_pm_state_t state = s_state;
    xSemaphoreGive(s_state_lock);
    return state == APP_MANAGER_PM_SCREEN_OFF ||
           state == APP_MANAGER_PM_STANDBY_PREPARING ||
           state == APP_MANAGER_PM_STANDBY || s_screen_ops.is_suspended() ||
           app_manager_display_is_sleeping();
}

bool app_manager_control_input_needs_wake(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const app_manager_pm_state_t state = s_state;
    xSemaphoreGive(s_state_lock);
    return state == APP_MANAGER_PM_SCREEN_OFF ||
           state == APP_MANAGER_PM_STANDBY_PREPARING ||
           state == APP_MANAGER_PM_STANDBY ||
           atomic_load_explicit(&s_input_wake_required,
                                memory_order_acquire);
}

bool app_manager_control_standby_generation_begin(void)
{
    bool ready = !atomic_exchange_explicit(&s_standby_abort_pending, false,
                                           memory_order_acq_rel);
    ready = ready &&
            atomic_load_explicit(&s_power_pending_events,
                                 memory_order_acquire) == 0U &&
            !atomic_load_explicit(&s_power_pending_wake,
                                  memory_order_acquire) &&
            !atomic_load_explicit(&s_wake_pending, memory_order_acquire) &&
            !_app_manager_control_home_wake_is_pending() &&
            atomic_load_explicit(&s_home_pending_generation,
                                 memory_order_acquire) == 0U &&
            atomic_load_explicit(&s_power_input_pending,
                                 memory_order_acquire) == 0U;
    if (!ready)
    {
        return false;
    }

    uint32_t generation = atomic_fetch_add_explicit(
                              &s_standby_generation, 1U,
                              memory_order_acq_rel) + 1U;
    if (generation == 0U)
    {
        (void)atomic_fetch_add_explicit(&s_standby_generation, 1U,
                                        memory_order_acq_rel);
    }
    atomic_store_explicit(&s_standby_commit_pending, false,
                          memory_order_release);
    atomic_store_explicit(&s_standby_bound_epoch, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_system_generation, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_commit_epoch, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_commit_generation, 0U,
                          memory_order_release);
    app_manager_control_state_set(APP_MANAGER_PM_STANDBY_PREPARING);

    ready = !atomic_load_explicit(&s_standby_abort_pending,
                                  memory_order_acquire) &&
            atomic_load_explicit(&s_power_pending_events,
                                 memory_order_acquire) == 0U &&
            !atomic_load_explicit(&s_power_pending_wake,
                                  memory_order_acquire) &&
            !atomic_load_explicit(&s_wake_pending, memory_order_acquire) &&
            !_app_manager_control_home_wake_is_pending() &&
            atomic_load_explicit(&s_home_pending_generation,
                                 memory_order_acquire) == 0U &&
            atomic_load_explicit(&s_power_input_pending,
                                 memory_order_acquire) == 0U;
    if (!ready)
    {
        app_manager_control_standby_generation_abort();
        app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
    }
    return ready;
}

bool app_manager_control_standby_commit_ready(uint32_t generation)
{
    if (generation == 0U)
    {
        return false;
    }
    /* Any PWR/HOME/wake intent accepted after the request invalidates this
     * generation. The system-PM worker must observe that before sleep. */
    if (atomic_load_explicit(&s_power_pending_events, memory_order_acquire) != 0U ||
            atomic_load_explicit(&s_power_pending_wake, memory_order_acquire) ||
            atomic_load_explicit(&s_wake_pending, memory_order_acquire) ||
            _app_manager_control_home_wake_is_pending() ||
            atomic_load_explicit(&s_home_pending_generation,
                                 memory_order_acquire) != 0U ||
            atomic_load_explicit(&s_power_input_pending, memory_order_acquire) != 0U)
    {
        return false;
    }
    if (atomic_load_explicit(&s_standby_abort_pending, memory_order_acquire))
    {
        return false;
    }
    if (s_state != APP_MANAGER_PM_STANDBY_PREPARING ||
            !app_manager_control_screen_is_canonical_sleep())
    {
        return false;
    }

    const uint32_t epoch = atomic_load_explicit(&s_standby_generation,
                           memory_order_acquire);
    uint32_t bound_epoch = atomic_load_explicit(&s_standby_bound_epoch,
                           memory_order_acquire);
    if (epoch == 0U)
    {
        return false;
    }
    if (bound_epoch == 0U)
    {
        /* system_pm owns its generation namespace. Publish the opaque value
         * before the epoch so the callback observes a complete binding. */
        atomic_store_explicit(&s_standby_system_generation, generation,
                              memory_order_relaxed);
        atomic_store_explicit(&s_standby_bound_epoch, epoch,
                              memory_order_release);
        bound_epoch = epoch;
    }
    if (bound_epoch != epoch ||
            atomic_load_explicit(&s_standby_system_generation,
                                 memory_order_acquire) != generation)
    {
        return false;
    }

    /* Close producer-before-guard races after publishing the binding. */
    return !atomic_load_explicit(&s_standby_abort_pending,
                                 memory_order_acquire) &&
           atomic_load_explicit(&s_power_pending_events,
                                memory_order_acquire) == 0U &&
           !atomic_load_explicit(&s_power_pending_wake,
                                 memory_order_acquire) &&
           !atomic_load_explicit(&s_wake_pending, memory_order_acquire) &&
           !_app_manager_control_home_wake_is_pending() &&
           atomic_load_explicit(&s_home_pending_generation,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&s_power_input_pending,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&s_standby_generation,
                                memory_order_acquire) == epoch;
}

void app_manager_control_standby_generation_abort(void)
{
    /* Invalidate a request before waking/resuming so a concurrently running
     * system-PM worker cannot commit the old generation. */
    atomic_fetch_add_explicit(&s_standby_generation, 1U,
                              memory_order_acq_rel);
    atomic_store_explicit(&s_standby_commit_pending, false,
                          memory_order_release);
    atomic_store_explicit(&s_standby_bound_epoch, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_system_generation, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_commit_epoch, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_commit_generation, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_standby_abort_pending, true,
                          memory_order_release);
}

static esp_err_t _control_rollback_resume_to_sleep(void)
{
    atomic_store_explicit(&s_input_wake_required, true,
                          memory_order_release);
    esp_err_t result = app_manager_display_sleep_prepare();
    if (result != ESP_OK)
    {
        LOG_W("adapter sleep rollback failed: %d", (int)result);
        return result;
    }

    result = s_screen_ops.suspend();
    if (result != ESP_OK)
    {
        LOG_W("screen sleep rollback failed: %d", (int)result);
    }
    else if (!app_manager_control_screen_is_canonical_sleep())
    {
        result = ESP_FAIL;
        LOG_W("screen sleep rollback did not reach suspended state");
    }
    return result;
}
static void _control_recover_failed_resume(void)
{
    (void)_control_rollback_resume_to_sleep();
    if (app_manager_control_screen_is_canonical_sleep())
    {
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
    }
    else
    {
        _control_schedule_touch_restore();
    }
}

static esp_err_t _control_resume_hardware(void)
{
    atomic_store_explicit(&s_input_wake_required, true,
                          memory_order_release);
    _control_disable_touch_activity();

    /* A previous rollback may have left a hidden panel with a live worker. */
    esp_err_t result = app_manager_display_sleep_prepare();
    if (result != ESP_OK)
    {
        if (!app_manager_control_screen_is_canonical_sleep())
        {
            _control_schedule_touch_restore();
        }
        return result;
    }

    result = s_screen_ops.resume_prepare();
    if (result != ESP_OK)
    {
        _control_recover_failed_resume();
        return result;
    }

    result = app_manager_display_sleep_recover();
    if (result != ESP_OK)
    {
        _control_recover_failed_resume();
        return result;
    }

    result = app_manager_display_refresh_without_touch();
    if (result != ESP_OK)
    {
        _control_recover_failed_resume();
        return result;
    }

    result = app_manager_control_apply_brightness(s_brightness, true);
    if (result != ESP_OK)
    {
        _control_recover_failed_resume();
        return result;
    }

    result = app_manager_control_commit_display();
    if (result != ESP_OK || s_screen_ops.is_suspended() ||
            app_manager_display_is_sleeping())
    {
        result = result != ESP_OK ? result : ESP_FAIL;
        _control_recover_failed_resume();
        return result;
    }

    result = app_manager_display_enable_touch();
    if (result != ESP_OK)
    {
        _control_recover_failed_resume();
        return result;
    }

    _control_enable_touch_activity();
    atomic_store_explicit(&s_input_wake_required, false,
                          memory_order_release);
    return result;
}

esp_err_t app_manager_control_suspend_hardware(void)
{
    atomic_store_explicit(&s_input_wake_required, true,
                          memory_order_release);
    _control_disable_touch_activity();

    esp_err_t result = app_manager_display_sleep_prepare();
    if (result != ESP_OK)
    {
        /* The adapter may also have failed while restoring lifecycle or LVGL
         * input. Keep the logical gate closed until a full active retry. */
        _control_schedule_touch_restore();
        return result;
    }

    result = s_screen_ops.suspend();
    if (result == ESP_OK && app_manager_control_screen_is_canonical_sleep())
    {
        return ESP_OK;
    }

    esp_err_t transition_error = result != ESP_OK ? result : ESP_FAIL;
    esp_err_t rollback_result = _control_resume_hardware();
    if (app_manager_control_screen_is_canonical_sleep())
    {
        /* A failed active rollback may still have completed the physical
         * off transaction. Never publish ACTIVE with the touch gate closed. */
        s_screen_off_us = esp_timer_get_time();
        app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
    }
    else if (rollback_result != ESP_OK)
    {
        LOG_W("screen suspend active rollback failed: %d",
              (int)rollback_result);
        _control_schedule_touch_restore();
    }
    else
    {
        atomic_store_explicit(&s_touch_restore_pending, false,
                              memory_order_release);
        s_last_activity_us = esp_timer_get_time();
        app_manager_control_state_set(APP_MANAGER_PM_ACTIVE);
    }
    return transition_error;
}

esp_err_t app_manager_control_prepare_shutdown(void)
{
    const app_manager_pm_state_t state = s_state;
    esp_err_t result = ESP_OK;
    _control_disable_touch_activity();

    if (state == APP_MANAGER_PM_STANDBY_PREPARING)
    {
        app_manager_control_standby_generation_abort();
    }
    if ((state == APP_MANAGER_PM_STANDBY_PREPARING ||
            state == APP_MANAGER_PM_STANDBY) &&
            s_standby_ops.cancel_standby != NULL)
    {
        result = s_standby_ops.cancel_standby();
        if (result == ESP_ERR_INVALID_STATE)
        {
            result = ESP_OK;
        }
        if (result != ESP_OK)
        {
            return result;
        }
    }

    if (app_manager_display_is_sleeping())
    {
        result = app_manager_display_sleep_prepare();
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (s_screen_ops.is_suspended())
    {
        result = s_screen_ops.resume_prepare();
        if (result != ESP_OK)
        {
            return result;
        }
    }
    return app_manager_display_sleep_recover();
}

esp_err_t app_manager_control_restore_active(void)
{
    app_manager_pm_state_t state = s_state;
    esp_err_t result = ESP_OK;
    if (state == APP_MANAGER_PM_STANDBY_PREPARING)
    {
        app_manager_control_standby_generation_abort();
    }
    if ((state == APP_MANAGER_PM_STANDBY_PREPARING ||
            state == APP_MANAGER_PM_STANDBY) &&
            s_standby_ops.cancel_standby != NULL)
    {
        result = s_standby_ops.cancel_standby();
        /* A wake callback runs after system-pm has completed its sleep hook,
         * so its request is already IDLE and cancel reports INVALID_STATE.
         * That is the committed-sleep path, not a failed resume. */
        if (result == ESP_ERR_INVALID_STATE &&
                (state == APP_MANAGER_PM_STANDBY_PREPARING ||
                 state == APP_MANAGER_PM_STANDBY))
        {
            result = ESP_OK;
        }
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (state == APP_MANAGER_PM_SCREEN_OFF ||
            state == APP_MANAGER_PM_STANDBY_PREPARING ||
            state == APP_MANAGER_PM_STANDBY ||
            s_screen_ops.is_suspended() ||
            app_manager_display_is_sleeping())
    {
        result = _control_resume_hardware();
    }
    else if (state == APP_MANAGER_PM_DIM)
    {
        result = app_manager_control_apply_brightness(s_brightness, false);
    }
    if (result == ESP_OK && !atomic_load(&s_touch_activity_enabled))
    {
        result = app_manager_display_refresh_and_enable_touch();
        if (result == ESP_OK)
        {
            _control_enable_touch_activity();
        }
    }
    if (result != ESP_OK && !app_manager_control_screen_is_canonical_sleep())
    {
        _control_schedule_touch_restore();
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_input_wake_required, false,
                              memory_order_release);
        s_last_activity_us = esp_timer_get_time();
        app_manager_control_state_set(APP_MANAGER_PM_ACTIVE);
    }
    return result;
}

esp_err_t app_manager_control_handle_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    return s_state == APP_MANAGER_PM_ACTIVE &&
           !s_screen_ops.is_suspended() &&
           atomic_load(&s_touch_activity_enabled) ?
           ESP_OK : app_manager_control_restore_active();
}

esp_err_t app_manager_control_handle_touch_activity(uint32_t generation)
{
    if (!atomic_load(&s_touch_activity_enabled) ||
            generation != atomic_load(&s_touch_generation) ||
            (s_state != APP_MANAGER_PM_ACTIVE &&
             s_state != APP_MANAGER_PM_DIM))
    {
        return ESP_OK;
    }
    return app_manager_control_handle_activity();
}

static void _control_run_home_complete(esp_err_t result, void *context)
{
    (void)context;
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        LOG_W("HOME navigation failed: %d", (int)result);
    }
    atomic_store(&s_home_pending_generation, 0U);
    atomic_store(&s_home_inflight, false);
}

void app_manager_control_process_pending_home(void)
{
    const uint32_t generation = atomic_load_explicit(
                                    &s_home_pending_generation,
                                    memory_order_acquire);
    if (generation == 0U ||
            !atomic_load_explicit(&s_control_admitted,
                                  memory_order_acquire) ||
            atomic_load(&s_lifecycle_state) != CONTROL_READY)
    {
        return;
    }

    if (_app_manager_control_home_sequence_is_wake(generation) ||
            s_home_worker_wake_generation == generation)
    {
        if (s_home_worker_wake_generation == generation)
        {
            s_home_worker_wake_generation = 0U;
        }
        uint32_t expected_generation = generation;
        (void)atomic_compare_exchange_strong_explicit(
            &s_home_pending_generation, &expected_generation, 0U,
            memory_order_acq_rel, memory_order_acquire);
        _app_manager_control_home_sequence_close(generation);
        return;
    }

    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
    {
        app_manager_control_standby_generation_abort();
    }
    _app_manager_control_home_sequence_close(generation);

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_home_inflight, &expected, true))
    {
        return;
    }

    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = APP_MANAGER_ID_HOME,
    };
    esp_err_t result = app_manager_navigate_async(
                           &request, _control_run_home_complete, NULL);
    if (result != ESP_OK)
    {
        atomic_store(&s_home_inflight, false);
        if (result != ESP_ERR_NO_MEM && result != ESP_ERR_INVALID_STATE)
        {
            LOG_W("HOME navigation dispatch failed: %d", (int)result);
        }
    }
}

static esp_err_t _control_handle_power_input(
    app_manager_key_event_t event, bool screen_off, bool *handled)
{
    esp_err_t result = ESP_OK;
    *handled = false;
    if (event == APP_MANAGER_KEY_EVENT_DOWN)
    {
        s_power_long_press_seen = false;
        if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
        {
            app_manager_control_standby_generation_abort();
        }
        if (screen_off)
        {
            s_power_wake_click_pending = true;
            result = app_manager_control_handle_activity();
            *handled = true;
            return result;
        }
        s_power_wake_click_pending = false;
    }
    else if (event == APP_MANAGER_KEY_EVENT_LONG_PRESS)
    {
        s_power_long_press_seen = true;
        s_power_wake_click_pending = false;
    }
    else if (event == APP_MANAGER_KEY_EVENT_UP)
    {
        if (screen_off)
        {
            if (!s_power_long_press_seen)
            {
                s_power_wake_click_pending = true;
            }
            result = app_manager_control_handle_activity();
            *handled = true;
        }
        s_power_long_press_seen = false;
    }
    else if (event == APP_MANAGER_KEY_EVENT_CLICK &&
             (s_power_wake_click_pending || screen_off))
    {
        s_power_wake_click_pending = false;
        s_power_long_press_seen = false;
        result = app_manager_control_handle_activity();
        *handled = true;
    }
    return result;
}

static esp_err_t _control_handle_home_input(
    app_manager_key_event_t event, uint32_t generation, bool screen_off,
    bool *handled)
{
    /* The atomic state protects producers and queue fallback; this worker-only
     * generation preserves the decision after a newer sequence is admitted. */
    esp_err_t result = ESP_OK;
    *handled = false;
    if (event == APP_MANAGER_KEY_EVENT_DOWN)
    {
        s_home_worker_wake_generation = screen_off ? generation : 0U;
        if (screen_off)
        {
            if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
            {
                app_manager_control_standby_generation_abort();
            }
            result = app_manager_control_handle_activity();
            *handled = true;
        }
    }
    else if (event == APP_MANAGER_KEY_EVENT_LONG_PRESS ||
             event == APP_MANAGER_KEY_EVENT_DOUBLE_CLICK)
    {
        if (s_home_worker_wake_generation == generation)
        {
            s_home_worker_wake_generation = 0U;
        }
        _app_manager_control_home_sequence_close(generation);
    }
    else if (event == APP_MANAGER_KEY_EVENT_CLICK &&
             (_app_manager_control_home_sequence_is_wake(generation) ||
              s_home_worker_wake_generation == generation ||
              screen_off))
    {
        if (s_home_worker_wake_generation == generation)
        {
            s_home_worker_wake_generation = 0U;
        }
        uint32_t expected_generation = generation;
        (void)atomic_compare_exchange_strong_explicit(
            &s_home_pending_generation, &expected_generation, 0U,
            memory_order_acq_rel, memory_order_acquire);
        _app_manager_control_home_sequence_close(generation);
        if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
        {
            app_manager_control_standby_generation_abort();
        }
        result = app_manager_control_handle_activity();
        *handled = true;
    }
    else if (event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        if (s_home_worker_wake_generation == generation)
        {
            s_home_worker_wake_generation = 0U;
        }
        _app_manager_control_home_sequence_close(generation);
    }
    return result;
}

esp_err_t app_manager_control_handle_input(app_manager_key_t key,
        app_manager_key_event_t event, uint32_t generation)
{
    esp_err_t result = ESP_OK;
    const bool screen_off = app_manager_control_screen_needs_wake();
    if (key == APP_MANAGER_KEY_POWER)
    {
        bool handled = false;
        result = _control_handle_power_input(event, screen_off, &handled);
        if (handled)
        {
            return result;
        }
    }
    else if (key == APP_MANAGER_KEY_HOME)
    {
        bool handled = false;
        result = _control_handle_home_input(event, generation,
                                            screen_off, &handled);
        if (handled)
        {
            return result;
        }
    }
    result = app_manager_control_handle_activity();
    if (result != ESP_OK)
    {
        return result;
    }
    if (key == APP_MANAGER_KEY_POWER && event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        result = app_manager_control_suspend_hardware();
        if (result == ESP_OK)
        {
            s_screen_off_us = esp_timer_get_time();
            app_manager_control_state_set(APP_MANAGER_PM_SCREEN_OFF);
        }
    }
    else if (key == APP_MANAGER_KEY_HOME && event == APP_MANAGER_KEY_EVENT_CLICK)
    {
        app_manager_control_process_pending_home();
        result = ESP_OK;
    }
    return result;
}
