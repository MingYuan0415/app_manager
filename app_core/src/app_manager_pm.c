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
#include "app_manager_navigation.h"
#include "app_manager_lifecycle.h"
#include "app_manager_pm_internal.h"
#include "app_manager_pm_transition.h"
#ifndef HOST_TEST
    #include "esp_timer.h"
    #include "nv_storage.h"
#endif
#include <string.h>

static void _control_home_click_cancel(void);
static void _control_home_click_on_active(uint32_t generation,
        int64_t event_us);

static app_manager_home_action_t _control_home_single_action(void)
{
    bool valid = false;
    const bool is_home = app_manager_lifecycle_active_is_home_snapshot(&valid);
    if (!valid)
    {
        LOG_W("HOME single click ignored: active snapshot unavailable");
        return APP_MANAGER_HOME_ACTION_NONE;
    }
    return is_home ? APP_MANAGER_HOME_ACTION_MENU : APP_MANAGER_HOME_ACTION_ROOT;
}

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
            atomic_load(&s_home_click_state) == APP_MANAGER_HOME_CLICK_IDLE &&
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
            atomic_load(&s_home_click_state) == APP_MANAGER_HOME_CLICK_IDLE &&
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
            atomic_load(&s_home_click_state) != APP_MANAGER_HOME_CLICK_IDLE ||
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
           atomic_load(&s_home_click_state) == APP_MANAGER_HOME_CLICK_IDLE &&
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
    /* Publish the wake gate before cancelling so a concurrent producer can
     * never classify a post-suspend click as active after the cancel. */
    atomic_store_explicit(&s_input_wake_required, true,
                          memory_order_release);
    _control_home_click_cancel();
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

static void _control_home_click_cancel(void)
{
    atomic_store(&s_home_click_state, APP_MANAGER_HOME_CLICK_IDLE);
    atomic_store(&s_home_click_generation, 0U);
    s_home_click_event_us = 0;
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
    atomic_fetch_add_explicit(&s_home_click_fallback_epoch, 1U,
                              memory_order_acq_rel);
    atomic_flag_clear_explicit(&s_home_click_fallback_lock,
                               memory_order_release);
}

static void _control_home_click_drain_fallback(void)
{
    app_manager_home_click_record_t
    records[APP_MANAGER_HOME_CLICK_FALLBACK_DEPTH];
    uint32_t count = 0U;
    const uint32_t epoch = atomic_load_explicit(&s_home_click_fallback_epoch,
                           memory_order_acquire);

    while (atomic_flag_test_and_set_explicit(
                &s_home_click_fallback_lock, memory_order_acquire))
    {
    }
    count = s_home_click_fallback_count;
    for (uint32_t index = 0U; index < count; index++)
    {
        records[index] = s_home_click_fallback[
                             (s_home_click_fallback_head + index) %
                             APP_MANAGER_HOME_CLICK_FALLBACK_DEPTH];
    }
    s_home_click_fallback_head = 0U;
    s_home_click_fallback_count = 0U;
    atomic_flag_clear_explicit(&s_home_click_fallback_lock,
                               memory_order_release);

    for (uint32_t index = 0U; index < count; index++)
    {
        /* Records latched before a cancel (suspend, long press, deinit) are
         * stale: their clicks were classified before the wake gate. */
        if (records[index].epoch != epoch)
        {
            continue;
        }
        _control_home_click_on_active(records[index].generation,
                                      records[index].event_us);
    }
}

static void _control_home_action_push(uint32_t action)
{
    if (action == APP_MANAGER_HOME_ACTION_NONE)
    {
        return;
    }
    /* Repeated doubles coalesce into one task-switcher action wherever they
     * sit. */
    if (action == APP_MANAGER_HOME_ACTION_TASK_SWITCHER)
    {
        for (uint32_t index = 0U; index < s_home_action_count; index++)
        {
            const uint32_t slot =
                (s_home_action_head + index) %
                APP_MANAGER_HOME_ACTION_FIFO_DEPTH;
            if (s_home_action_fifo[slot] == APP_MANAGER_HOME_ACTION_TASK_SWITCHER)
            {
                return;
            }
        }
        if (s_home_action_count == APP_MANAGER_HOME_ACTION_FIFO_DEPTH)
        {
            /* The newest intent wins: replace the most recently queued
             * single with the double so no accepted click is dropped. */
            for (uint32_t index = s_home_action_count; index > 0U; index--)
            {
                const uint32_t slot =
                    (s_home_action_head + index - 1U) %
                    APP_MANAGER_HOME_ACTION_FIFO_DEPTH;
                if (s_home_action_fifo[slot] == APP_MANAGER_HOME_ACTION_ROOT ||
                        s_home_action_fifo[slot] == APP_MANAGER_HOME_ACTION_MENU)
                {
                    s_home_action_fifo[slot] =
                        APP_MANAGER_HOME_ACTION_TASK_SWITCHER;
                    return;
                }
            }
            return;
        }
    }
    else if (s_home_action_count == APP_MANAGER_HOME_ACTION_FIFO_DEPTH)
    {
        /* Repeated singles all navigate to the same root page; the newest
         * single intent equals an already queued one. */
        return;
    }
    const uint32_t tail =
        (s_home_action_head + s_home_action_count) %
        APP_MANAGER_HOME_ACTION_FIFO_DEPTH;
    s_home_action_fifo[tail] = action;
    s_home_action_count++;
}

static void _control_home_click_on_active(uint32_t generation,
        int64_t event_us)
{
    const uint32_t state = atomic_load(&s_home_click_state);
    const uint32_t window_generation = atomic_load(&s_home_click_generation);
    if (state == APP_MANAGER_HOME_CLICK_WAIT_SECOND &&
            window_generation != generation)
    {
        /* Compare admission times in absolute order so a stale fallback
         * record drained after a newer click cannot pair into a double. */
        const int64_t window_event = s_home_click_event_us;
        if (event_us <= window_event)
        {
            /* Out of order: this click predates the window. It was the real
             * first press; the window's click was the second. A double is
             * formed only when the two presses are close in time; otherwise
             * the older press resolves to the root page and the newer window
             * is preserved for a possible third press. */
            if (window_event - event_us <=
                    (int64_t)APP_CONTROL_HOME_DOUBLE_CLICK_WINDOW_MS * 1000LL)
            {
                _control_home_action_push(APP_MANAGER_HOME_ACTION_TASK_SWITCHER);
                atomic_store(&s_home_click_state, APP_MANAGER_HOME_CLICK_IDLE);
                s_home_click_event_us = 0;
                s_home_click_deadline_us = 0;
            }
            else
            {
                _control_home_action_push(_control_home_single_action());
            }
            return;
        }
        if (event_us <= s_home_click_deadline_us)
        {
            _control_home_action_push(APP_MANAGER_HOME_ACTION_TASK_SWITCHER);
            atomic_store(&s_home_click_state, APP_MANAGER_HOME_CLICK_IDLE);
            s_home_click_event_us = 0;
            s_home_click_deadline_us = 0;
            return;
        }
        /* The previous window expired before this click; resolve the single
         * press first and treat this click as the start of a new window. */
        _control_home_action_push(_control_home_single_action());
    }
    else if (state == APP_MANAGER_HOME_CLICK_WAIT_SECOND &&
             window_generation == generation)
    {
        /* Duplicate CLICK record of the same press: it must never pair with
         * itself into a double; resolve the pending single press and re-arm
         * the window from this record. */
        _control_home_action_push(_control_home_single_action());
    }
    atomic_store(&s_home_click_state, APP_MANAGER_HOME_CLICK_WAIT_SECOND);
    atomic_store(&s_home_click_generation, generation);
    s_home_click_event_us = event_us;
    s_home_click_deadline_us =
        event_us + (int64_t)APP_CONTROL_HOME_DOUBLE_CLICK_WINDOW_MS * 1000LL;
}

uint32_t app_manager_control_home_click_wait_ms(void)
{
    if (atomic_load(&s_home_click_state) != APP_MANAGER_HOME_CLICK_WAIT_SECOND)
    {
        return APP_CONTROL_POLL_MS;
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us >= s_home_click_deadline_us)
    {
        return 0U;
    }
    const int64_t remaining_us = s_home_click_deadline_us - now_us;
    const uint32_t remaining_ms =
        (uint32_t)((remaining_us + 999) / 1000);
    return remaining_ms < APP_CONTROL_POLL_MS ?
           remaining_ms : APP_CONTROL_POLL_MS;
}

static void _control_run_home_complete(esp_err_t result, void *context)
{
    (void)context;
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        LOG_W("HOME navigation failed: %d", (int)result);
    }
    atomic_store(&s_home_inflight, false);
}

void app_manager_control_process_pending_home(void)
{
    /* Drain the queue-full fallback clicks first so their admission times
     * are compared against the correct click windows. */
    _control_home_click_drain_fallback();

    const int64_t now_us = esp_timer_get_time();
    if (atomic_load(&s_home_click_state) == APP_MANAGER_HOME_CLICK_WAIT_SECOND &&
            now_us > s_home_click_deadline_us)
    {
        _control_home_action_push(_control_home_single_action());
        atomic_store(&s_home_click_state, APP_MANAGER_HOME_CLICK_IDLE);
        s_home_click_event_us = 0;
        s_home_click_deadline_us = 0;
    }
    if (s_home_action_count == 0U)
    {
        return;
    }
    if (!atomic_load_explicit(&s_control_admitted,
                              memory_order_acquire) ||
            atomic_load(&s_lifecycle_state) != CONTROL_READY)
    {
        return;
    }

    if (s_state == APP_MANAGER_PM_STANDBY_PREPARING)
    {
        app_manager_control_standby_generation_abort();
    }

    const uint32_t action =
        s_home_action_fifo[s_home_action_head % APP_MANAGER_HOME_ACTION_FIFO_DEPTH];
    const bool task_switcher =
        action == APP_MANAGER_HOME_ACTION_TASK_SWITCHER;
    const bool menu = action == APP_MANAGER_HOME_ACTION_MENU;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_home_inflight, &expected, true))
    {
        return;
    }

    esp_err_t result;
    if (task_switcher)
    {
        result = app_manager_navigation_submit_system_op(
                     APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW,
                     _control_run_home_complete, NULL);
    }
    else if (menu)
    {
        const app_manager_nav_request_t request =
        {
            .operation = APP_MANAGER_NAV_OP_RUN,
            .app_id = APP_MANAGER_ID_MENU,
        };
        result = app_manager_navigate_async(&request,
                                            _control_run_home_complete, NULL);
    }
    else
    {
        const app_manager_nav_request_t request =
        {
            .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
            .app_id = APP_MANAGER_ID_HOME,
            .page_id = APP_MANAGER_ID_HOME_ROOT,
        };
        result = app_manager_navigate_async(&request,
                                            _control_run_home_complete, NULL);
    }
    if (result != ESP_OK)
    {
        atomic_store(&s_home_inflight, false);
        if (result != ESP_ERR_NO_MEM && result != ESP_ERR_INVALID_STATE)
        {
            LOG_W("HOME navigation dispatch failed: %d", (int)result);
        }
        return;
    }
    s_home_action_fifo[s_home_action_head % APP_MANAGER_HOME_ACTION_FIFO_DEPTH] =
        APP_MANAGER_HOME_ACTION_NONE;
    s_home_action_head =
        (s_home_action_head + 1U) % APP_MANAGER_HOME_ACTION_FIFO_DEPTH;
    s_home_action_count--;
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
    bool *handled, int64_t event_us)
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
            _control_home_click_cancel();
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
        _control_home_click_cancel();
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
        _control_home_click_on_active(generation, event_us);
    }
    return result;
}

esp_err_t app_manager_control_handle_input(app_manager_key_t key,
        app_manager_key_event_t event, uint32_t generation, int64_t event_us)
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
                                            screen_off, &handled, event_us);
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
