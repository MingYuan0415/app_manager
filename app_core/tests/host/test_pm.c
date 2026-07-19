#include "host_control_port.h"
#include "app_manager.h"
#include "app_manager_config.h"
#include "../../src/app_manager_control.h"
#include "../../src/app_manager_pm_internal.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __SANITIZE_THREAD__
    #define CONTROL_TEST_WAIT_SCALE (5)
#else
    #define CONTROL_TEST_WAIT_SCALE (1)
#endif

atomic_llong g_host_time_us;

static atomic_int s_adapter_pause_count;
static atomic_int s_adapter_resume_count;
static atomic_int s_display_refresh_count;
static atomic_int s_touch_enable_count;
static atomic_int s_screen_suspend_count;
static atomic_int s_screen_resume_count;
static atomic_int s_screen_commit_count;
static atomic_int s_brightness_restore_count;
static atomic_int s_standby_request_count;
static atomic_int s_standby_cancel_count;
static atomic_int s_ui_post_count;
static atomic_int s_run_home_count;
static atomic_bool s_fail_ui_post;
static atomic_bool s_block_failed_ui_post;
static atomic_bool s_failed_ui_post_entered;
static atomic_bool s_screen_suspended;
static atomic_bool s_adapter_sleeping;
static atomic_bool s_fail_lifecycle_pause;
static atomic_bool s_fail_adapter_pause;
static atomic_bool s_fail_adapter_recover;
static atomic_bool s_fail_display_refresh;
static atomic_bool s_fail_touch_enable;
static atomic_bool s_fail_screen_suspend;
static atomic_bool s_fail_screen_resume;
static atomic_bool s_fail_screen_commit;
static atomic_bool s_fail_brightness_restore;
static atomic_bool s_partial_fail_screen_suspend;
static atomic_bool s_partial_fail_screen_resume;
static atomic_bool s_block_screen_suspend;
static atomic_bool s_fail_nv_write;
static atomic_int s_temp_brightness;
static atomic_int s_hardware_brightness;
static atomic_bool s_trace_transition;
static atomic_int s_transition_sequence;
static atomic_int s_adapter_pause_step;
static atomic_int s_adapter_resume_step;
static atomic_int s_display_refresh_step;
static atomic_int s_screen_suspend_step;
static atomic_int s_screen_resume_step;
static atomic_int s_screen_commit_step;
static atomic_int s_brightness_restore_step;
static atomic_int s_touch_enable_step;
static bool s_nv_brightness_valid;
static uint8_t s_nv_brightness;
static bool s_nv_off_timeout_valid;
static uint32_t s_nv_off_timeout;
static bool s_nv_standby_timeout_valid;
static uint32_t s_nv_standby_timeout;
static app_manager_input_cb_t s_input_callback;
static void *s_input_user_data;
static atomic_bool s_interleaved_touch_result;
static atomic_bool s_inject_activity_on_suspended_query;
static atomic_int s_injected_activity_count;
static atomic_bool s_fail_input_register;
static atomic_bool s_block_input_register;
static atomic_bool s_input_register_entered;
static atomic_bool s_block_input_unregister;
static atomic_bool s_input_unregister_entered;
static _Thread_local bool s_in_ui_call;
static bool s_startup_touch_enabled;
static atomic_bool s_logical_touch_enabled;

static void _trace_transition(atomic_int *step)
{
    if (atomic_load(&s_trace_transition))
    {
        atomic_store(step, atomic_fetch_add(&s_transition_sequence, 1) + 1);
    }
}

static void *_interleaved_touch_notify(void *arg)
{
    (void)arg;
    atomic_store(&s_interleaved_touch_result,
                 app_manager_control_notify_touch_activity_from_isr());
    return NULL;
}

typedef struct control_thread_result
{
    esp_err_t result;
} control_thread_result_t;

static void *_open_admission_thread(void *arg)
{
    control_thread_result_t *thread = arg;
    thread->result = app_manager_control_open_admission();
    return NULL;
}

static void *_control_deinit_thread(void *arg)
{
    control_thread_result_t *thread = arg;
    thread->result = app_manager_control_deinit();
    return NULL;
}

esp_err_t app_manager_display_sleep_prepare(void)
{
    if (atomic_load(&s_adapter_sleeping))
    {
        return ESP_OK;
    }
    if (atomic_load(&s_fail_lifecycle_pause))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_logical_touch_enabled, false);
    _trace_transition(&s_adapter_pause_step);
    atomic_fetch_add(&s_adapter_pause_count, 1);
    if (atomic_load(&s_fail_adapter_pause))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_adapter_sleeping, true);
    return ESP_OK;
}

esp_err_t app_manager_display_sleep_recover(void)
{
    if (!atomic_load(&s_adapter_sleeping))
    {
        return ESP_OK;
    }
    _trace_transition(&s_adapter_resume_step);
    atomic_fetch_add(&s_adapter_resume_count, 1);
    if (atomic_load(&s_fail_adapter_recover))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_adapter_sleeping, false);
    return ESP_OK;
}

esp_err_t app_manager_display_refresh_and_enable_touch(void)
{
    _trace_transition(&s_display_refresh_step);
    atomic_fetch_add(&s_display_refresh_count, 1);
    if (atomic_load(&s_fail_display_refresh))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_logical_touch_enabled, true);
    return ESP_OK;
}

esp_err_t app_manager_display_refresh_without_touch(void)
{
    _trace_transition(&s_display_refresh_step);
    atomic_fetch_add(&s_display_refresh_count, 1);
    assert(!atomic_load(&s_logical_touch_enabled));
    return atomic_load(&s_fail_display_refresh) ? ESP_FAIL : ESP_OK;
}

esp_err_t app_manager_display_enable_touch(void)
{
    _trace_transition(&s_touch_enable_step);
    atomic_fetch_add(&s_touch_enable_count, 1);
    assert(!atomic_load(&s_adapter_sleeping));
    assert(!atomic_load(&s_screen_suspended));
    if (atomic_load(&s_fail_touch_enable))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_logical_touch_enabled, true);
    return ESP_OK;
}

esp_err_t app_manager_display_enable_startup_touch(void)
{
    s_startup_touch_enabled = true;
    atomic_store(&s_logical_touch_enabled, true);
    return ESP_OK;
}

esp_err_t app_manager_display_disable_startup_touch(void)
{
    s_startup_touch_enabled = false;
    atomic_store(&s_logical_touch_enabled, false);
    return ESP_OK;
}

bool app_manager_display_is_sleeping(void)
{
    return atomic_load(&s_adapter_sleeping);
}

esp_err_t nv_storage_get_u8(const char *key, uint8_t *value)
{
    if (strcmp(key, "brightness") != 0 || !s_nv_brightness_valid)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *value = s_nv_brightness;
    return ESP_OK;
}

esp_err_t nv_storage_set_u8(const char *key, uint8_t value)
{
    if (strcmp(key, "brightness") != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&s_fail_nv_write))
    {
        return ESP_FAIL;
    }
    s_nv_brightness = value;
    s_nv_brightness_valid = true;
    return ESP_OK;
}

esp_err_t nv_storage_get_u32(const char *key, uint32_t *value)
{
    if (strcmp(key, "pm_dim_to") == 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (strcmp(key, "pm_timeout") == 0 && s_nv_off_timeout_valid)
    {
        *value = s_nv_off_timeout;
        return ESP_OK;
    }
    if (strcmp(key, "pm_stby_dly") == 0 && s_nv_standby_timeout_valid)
    {
        *value = s_nv_standby_timeout;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nv_storage_set_u32(const char *key, uint32_t value)
{
    if (atomic_load(&s_fail_nv_write))
    {
        return ESP_FAIL;
    }
    if (strcmp(key, "pm_timeout") == 0)
    {
        s_nv_off_timeout = value;
        s_nv_off_timeout_valid = true;
        return ESP_OK;
    }
    if (strcmp(key, "pm_stby_dly") == 0)
    {
        s_nv_standby_timeout = value;
        s_nv_standby_timeout_valid = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t app_manager_ui_post(void (*callback)(void *), void *arg)
{
    atomic_fetch_add(&s_ui_post_count, 1);
    if (atomic_load(&s_fail_ui_post))
    {
        atomic_store(&s_failed_ui_post_entered, true);
        while (atomic_load(&s_block_failed_ui_post))
        {
            usleep(1000);
        }
        return ESP_ERR_NO_MEM;
    }
    callback(arg);
    return ESP_OK;
}

bool app_manager_is_initialized(void)
{
    return true;
}

esp_err_t app_manager_ui_call(app_manager_ui_call_fn callback, void *arg,
                              uint32_t timeout_ms)
{
    assert(callback != NULL);
    assert(timeout_ms == UINT32_MAX);
    assert(!s_in_ui_call);
    assert(!atomic_load(&s_adapter_sleeping));
    s_in_ui_call = true;
    const esp_err_t result = callback(arg);
    s_in_ui_call = false;
    return result;
}

esp_err_t app_manager_navigate_async(
    const app_manager_nav_request_t *request,
    app_manager_nav_completion_cb_t completion, void *context)
{
    atomic_fetch_add(&s_ui_post_count, 1);
    if (atomic_load(&s_fail_ui_post))
    {
        atomic_store(&s_failed_ui_post_entered, true);
        while (atomic_load(&s_block_failed_ui_post))
        {
            usleep(1000);
        }
        return ESP_ERR_NO_MEM;
    }
    assert(request != NULL);
    assert(request->operation == APP_MANAGER_NAV_OP_RUN);
    assert(request->app_id != NULL);
    assert(strcmp(request->app_id, APP_MANAGER_ID_HOME) == 0);
    atomic_fetch_add(&s_run_home_count, 1);
    if (completion != NULL)
    {
        completion(ESP_OK, context);
    }
    return ESP_OK;
}

static esp_err_t _screen_suspend(void)
{
    _trace_transition(&s_screen_suspend_step);
    atomic_fetch_add(&s_screen_suspend_count, 1);
    if (atomic_load(&s_fail_screen_suspend))
    {
        return ESP_FAIL;
    }
    while (atomic_load(&s_block_screen_suspend))
    {
        usleep(1000);
    }
    atomic_store(&s_screen_suspended, true);
    return atomic_load(&s_partial_fail_screen_suspend) ? ESP_FAIL : ESP_OK;
}

static esp_err_t _screen_resume_prepare(void)
{
    _trace_transition(&s_screen_resume_step);
    atomic_fetch_add(&s_screen_resume_count, 1);
    if (atomic_load(&s_fail_screen_resume))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_screen_suspended, true);
    return ESP_OK;
}

static esp_err_t _screen_resume_commit(void)
{
    assert(s_in_ui_call);
    assert(!atomic_load(&s_logical_touch_enabled));
    _trace_transition(&s_screen_commit_step);
    atomic_fetch_add(&s_screen_commit_count, 1);
    if (atomic_load(&s_fail_screen_commit) ||
            atomic_load(&s_partial_fail_screen_resume))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_screen_suspended, false);
    return ESP_OK;
}

static bool _screen_is_suspended(void)
{
    if (atomic_exchange(&s_inject_activity_on_suspended_query, false))
    {
        assert(app_manager_pm_notify_user_activity() == ESP_OK);
        atomic_fetch_add(&s_injected_activity_count, 1);
    }
    return atomic_load(&s_screen_suspended);
}

static bool _screen_is_suspend_committed(void)
{
    return atomic_load(&s_screen_suspended);
}

static esp_err_t _screen_set_brightness(uint8_t brightness)
{
    assert(s_in_ui_call);
    _trace_transition(&s_brightness_restore_step);
    atomic_fetch_add(&s_brightness_restore_count, 1);
    atomic_store(&s_hardware_brightness, brightness);
    atomic_store(&s_temp_brightness, brightness);
    return atomic_load(&s_fail_brightness_restore) ? ESP_FAIL : ESP_OK;
}

static esp_err_t _screen_set_brightness_temp(uint8_t brightness)
{
    assert(s_in_ui_call);
    _trace_transition(&s_brightness_restore_step);
    atomic_fetch_add(&s_brightness_restore_count, 1);
    atomic_store(&s_temp_brightness, brightness);
    return atomic_load(&s_fail_brightness_restore) ? ESP_FAIL : ESP_OK;
}

static uint8_t _screen_get_brightness(void)
{
    return (uint8_t)atomic_load(&s_hardware_brightness);
}

static esp_err_t _input_register(app_manager_input_cb_t callback, void *user_data)
{
    assert(s_startup_touch_enabled);
    atomic_store(&s_input_register_entered, true);
    while (atomic_load(&s_block_input_register))
    {
        usleep(1000);
    }
    if (atomic_load(&s_fail_input_register))
    {
        atomic_store(&s_input_register_entered, false);
        return ESP_FAIL;
    }
    s_input_callback = callback;
    s_input_user_data = user_data;
    atomic_store(&s_input_register_entered, false);
    return ESP_OK;
}

static esp_err_t _input_unregister(void)
{
    atomic_store(&s_input_unregister_entered, true);
    while (atomic_load(&s_block_input_unregister))
    {
        usleep(1000);
    }
    s_input_callback = NULL;
    s_input_user_data = NULL;
    atomic_store(&s_input_unregister_entered, false);
    return ESP_OK;
}

static esp_err_t _standby_request(void)
{
    atomic_fetch_add(&s_standby_request_count, 1);
    return ESP_OK;
}

static esp_err_t _standby_cancel(void)
{
    atomic_fetch_add(&s_standby_cancel_count, 1);
    return ESP_OK;
}

static void _set_time_ms(int64_t milliseconds)
{
    atomic_store(&g_host_time_us, milliseconds * 1000);
}

static void _wait_for_state_at(app_manager_pm_state_t expected, int line)
{
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            app_manager_pm_get_state() != expected; i++)
    {
        usleep(1000);
    }
    if (app_manager_pm_get_state() != expected)
    {
        fprintf(stderr, "line %d: state=%d expected=%d\n", line,
                (int)app_manager_pm_get_state(), (int)expected);
        abort();
    }
}

#define wait_for_state(expected) _wait_for_state_at((expected), __LINE__)

static void _wait_for_count(atomic_int *counter, int expected)
{
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            atomic_load(counter) < expected; i++)
    {
        usleep(1000);
    }
    assert(atomic_load(counter) >= expected);
}

static void _wait_for_suspended(bool expected)
{
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            atomic_load(&s_screen_suspended) != expected; i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_screen_suspended) == expected);
}

static void _reset_transition_trace(void)
{
    atomic_store(&s_transition_sequence, 0);
    atomic_store(&s_adapter_pause_step, 0);
    atomic_store(&s_adapter_resume_step, 0);
    atomic_store(&s_display_refresh_step, 0);
    atomic_store(&s_screen_suspend_step, 0);
    atomic_store(&s_screen_resume_step, 0);
    atomic_store(&s_screen_commit_step, 0);
    atomic_store(&s_brightness_restore_step, 0);
    atomic_store(&s_touch_enable_step, 0);
    atomic_store(&s_trace_transition, true);
}

static void _assert_suspend_transition_order(void)
{
    atomic_store(&s_trace_transition, false);
    assert(atomic_load(&s_transition_sequence) == 2);
    assert(atomic_load(&s_adapter_pause_step) == 1);
    assert(atomic_load(&s_screen_suspend_step) == 2);
}

static void _assert_resume_transition_order(void)
{
    atomic_store(&s_trace_transition, false);
    assert(atomic_load(&s_transition_sequence) == 6);
    assert(atomic_load(&s_screen_resume_step) == 1);
    assert(atomic_load(&s_adapter_resume_step) == 2);
    assert(atomic_load(&s_display_refresh_step) == 3);
    assert(atomic_load(&s_brightness_restore_step) == 4);
    assert(atomic_load(&s_screen_commit_step) == 5);
    assert(atomic_load(&s_touch_enable_step) == 6);
}

static void _reset_mocks(void)
{
    atomic_store(&g_host_time_us, 0);
    atomic_store(&s_adapter_pause_count, 0);
    atomic_store(&s_adapter_resume_count, 0);
    atomic_store(&s_display_refresh_count, 0);
    atomic_store(&s_touch_enable_count, 0);
    atomic_store(&s_screen_suspend_count, 0);
    atomic_store(&s_screen_resume_count, 0);
    atomic_store(&s_screen_commit_count, 0);
    atomic_store(&s_brightness_restore_count, 0);
    atomic_store(&s_standby_request_count, 0);
    atomic_store(&s_standby_cancel_count, 0);
    atomic_store(&s_ui_post_count, 0);
    atomic_store(&s_run_home_count, 0);
    atomic_store(&s_screen_suspended, false);
    atomic_store(&s_adapter_sleeping, false);
    atomic_store(&s_fail_lifecycle_pause, false);
    atomic_store(&s_fail_adapter_pause, false);
    atomic_store(&s_fail_adapter_recover, false);
    atomic_store(&s_fail_display_refresh, false);
    atomic_store(&s_fail_touch_enable, false);
    atomic_store(&s_fail_screen_suspend, false);
    atomic_store(&s_fail_screen_resume, false);
    atomic_store(&s_fail_screen_commit, false);
    atomic_store(&s_fail_brightness_restore, false);
    atomic_store(&s_partial_fail_screen_suspend, false);
    atomic_store(&s_partial_fail_screen_resume, false);
    atomic_store(&s_block_screen_suspend, false);
    atomic_store(&s_fail_ui_post, false);
    atomic_store(&s_block_failed_ui_post, false);
    atomic_store(&s_failed_ui_post_entered, false);
    atomic_store(&s_fail_nv_write, false);
    atomic_store(&s_temp_brightness, 255);
    atomic_store(&s_hardware_brightness, 255);
    atomic_store(&s_trace_transition, false);
    s_nv_brightness_valid = false;
    s_nv_off_timeout_valid = false;
    s_nv_standby_timeout_valid = false;
    s_input_callback = NULL;
    s_input_user_data = NULL;
    atomic_store(&s_interleaved_touch_result, true);
    atomic_store(&s_fail_input_register, false);
    atomic_store(&s_block_input_register, false);
    atomic_store(&s_input_register_entered, false);
    atomic_store(&s_block_input_unregister, false);
    atomic_store(&s_input_unregister_entered, false);
    s_startup_touch_enabled = false;
    atomic_store(&s_logical_touch_enabled, false);
}

int main(void)
{
    _reset_mocks();
    const app_manager_config_t config =
    {
        .screen_ops = {
            .suspend = _screen_suspend,
            .resume_prepare = _screen_resume_prepare,
            .resume_commit = _screen_resume_commit,
            .is_suspended = _screen_is_suspended,
            .is_suspend_committed = _screen_is_suspend_committed,
            .set_brightness = _screen_set_brightness,
            .set_brightness_temp = _screen_set_brightness_temp,
            .get_brightness = _screen_get_brightness,
        },
        .input_ops = {
            .register_handler = _input_register,
            .unregister_handler = _input_unregister,
        },
        .standby_ops = {
            .request_standby = _standby_request,
            .cancel_standby = _standby_cancel,
        },
    };

    assert(app_manager_control_init(&config) == ESP_OK);
    assert(s_input_callback == NULL);
    assert(app_manager_pm_notify_user_activity() == ESP_ERR_INVALID_STATE);
    _set_time_ms(60000);
    usleep(150000);
    assert(atomic_load(&s_screen_suspend_count) == 0);
    _set_time_ms(0);
    assert(app_manager_startup_commit() == ESP_OK);
    assert(s_input_callback != NULL);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(app_manager_screen_get_brightness() == 255);

    _reset_transition_trace();
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    _assert_suspend_transition_order();

    _reset_transition_trace();
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    _assert_resume_transition_order();

    _set_time_ms(15001);
    wait_for_state(APP_MANAGER_PM_DIM);
    assert(atomic_load(&s_temp_brightness) == 76);
    assert(app_manager_screen_get_brightness() == 255);

    int suspend_before_failure = atomic_load(&s_screen_suspend_count);
    atomic_store(&s_fail_screen_suspend, true);
    _set_time_ms(30001);
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_screen_suspend_count) == suspend_before_failure + 1);
    assert(atomic_load(&s_adapter_pause_count) > 0);
    assert(atomic_load(&s_adapter_resume_count) > 0);

    atomic_store(&s_fail_screen_suspend, false);
    _set_time_ms(45002);
    wait_for_state(APP_MANAGER_PM_DIM);
    _set_time_ms(60002);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_suspended));
    const int sleeping_brightness_calls =
        atomic_load(&s_brightness_restore_count);
    const int sleeping_hardware_brightness =
        atomic_load(&s_hardware_brightness);
    const int sleeping_temp_brightness = atomic_load(&s_temp_brightness);
    assert(app_manager_screen_set_brightness(200) == ESP_OK);
    for (int i = 0; i < 200 * CONTROL_TEST_WAIT_SCALE &&
            app_manager_screen_get_brightness() != 200; ++i)
    {
        usleep(1000);
    }
    assert(app_manager_screen_get_brightness() == 200);
    assert(app_manager_screen_set_brightness_temp(33) == ESP_OK);
    usleep(150000);
    assert(atomic_load(&s_hardware_brightness) ==
           sleeping_hardware_brightness);
    assert(atomic_load(&s_temp_brightness) == sleeping_temp_brightness);
    assert(atomic_load(&s_brightness_restore_count) ==
           sleeping_brightness_calls);
    int screen_suspend_before_standby = atomic_load(&s_screen_suspend_count);
    int screen_resume_before_standby = atomic_load(&s_screen_resume_count);
    int standby_before = atomic_load(&s_standby_request_count);
    _set_time_ms(65000);
    usleep(120000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_standby_request_count) == standby_before);
    _set_time_ms(65002);
    wait_for_state(APP_MANAGER_PM_STANDBY_PREPARING);
    assert(app_manager_screen_is_suspended());
    const uint32_t standby_epoch =
        app_manager_control_standby_generation_for_test();
    const uint32_t first_system_generation = standby_epoch + 1000U;
    assert(first_system_generation != 0U);
    assert(first_system_generation != standby_epoch);
    assert(!app_manager_pm_standby_commit_guard(0U, NULL));
    assert(app_manager_pm_standby_commit_guard(first_system_generation, NULL));
    app_manager_pm_standby_commit_callback(first_system_generation + 1U, NULL);
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_STANDBY_PREPARING);
    app_manager_pm_standby_commit_callback(first_system_generation, NULL);
    wait_for_state(APP_MANAGER_PM_STANDBY);
    assert(atomic_load(&s_screen_suspend_count) == screen_suspend_before_standby);
    assert(atomic_load(&s_screen_resume_count) == screen_resume_before_standby);

    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_standby_cancel_count) == 1);
    assert(!atomic_load(&s_screen_suspended));
    assert(atomic_load(&s_temp_brightness) == 200);

    assert(app_manager_screen_set_brightness(128) == ESP_OK);
    for (int i = 0; i < 200 * CONTROL_TEST_WAIT_SCALE &&
            app_manager_screen_get_brightness() != 128; i++)
    {
        usleep(1000);
    }
    assert(app_manager_screen_get_brightness() == 128);
    assert(s_nv_brightness_valid && s_nv_brightness == 128);
    atomic_store(&s_fail_nv_write, true);
    assert(app_manager_screen_set_brightness(100) == ESP_OK);
    usleep(20000);
    assert(app_manager_screen_get_brightness() == 128);
    assert(atomic_load(&s_hardware_brightness) == 128);
    atomic_store(&s_fail_nv_write, false);

    for (int i = 0; i < 64; i++)
    {
        assert(app_manager_screen_set_brightness_temp((uint8_t)(i + 1)) ==
               ESP_OK);
    }
    assert(app_manager_screen_set_brightness(137) == ESP_OK);
    for (int i = 0; i < 500 * CONTROL_TEST_WAIT_SCALE &&
            app_manager_screen_get_brightness() != 137; i++)
    {
        usleep(1000);
    }
    assert(app_manager_screen_get_brightness() == 137);
    assert(s_nv_brightness_valid && s_nv_brightness == 137);
    assert(atomic_load(&s_hardware_brightness) == 137);

    assert(app_manager_pm_set_timeout_ms(10000) == ESP_ERR_INVALID_ARG);
    assert(app_manager_pm_set_timeout_ms(-2) == ESP_ERR_INVALID_ARG);
    assert(app_manager_pm_set_timeout_ms(45000) == ESP_OK);
    assert(app_manager_pm_get_timeout_ms() == 45000);
    assert(app_manager_pm_set_standby_delay_ms(-2) == ESP_ERR_INVALID_ARG);
    assert(app_manager_pm_set_standby_delay_ms(-1) == ESP_OK);
    assert(app_manager_pm_get_standby_delay_ms() == -1);

    s_input_callback(APP_MANAGER_KEY_HOME, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    for (int i = 0; i < 200 * CONTROL_TEST_WAIT_SCALE &&
            atomic_load(&s_ui_post_count) == 0; i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_ui_post_count) == 1);
    assert(atomic_load(&s_run_home_count) == 1);

    atomic_store(&s_fail_ui_post, true);
    atomic_store(&s_block_failed_ui_post, true);
    s_input_callback(APP_MANAGER_KEY_HOME, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    for (int i = 0; i < 500 * CONTROL_TEST_WAIT_SCALE &&
            !atomic_load(&s_failed_ui_post_entered); i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_failed_ui_post_entered));
    for (int i = 1; i < 8; i++)
    {
        s_input_callback(APP_MANAGER_KEY_HOME, APP_MANAGER_KEY_EVENT_CLICK,
                         s_input_user_data);
    }
    assert(atomic_load(&s_run_home_count) == 1);
    atomic_store(&s_fail_ui_post, false);
    atomic_store(&s_block_failed_ui_post, false);
    for (int i = 0; i < 500 * CONTROL_TEST_WAIT_SCALE &&
            atomic_load(&s_run_home_count) == 1; i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_run_home_count) == 2);
    usleep(250000);
    assert(atomic_load(&s_run_home_count) == 2);

    /* A physical PWR click must survive a saturated control queue. Keep the
     * control task blocked in its UI handoff, fill all 16 command slots with
     * ordinary commands, then release it and verify the coalesced fallback
     * still turns the screen off. */
    atomic_store(&s_fail_ui_post, true);
    atomic_store(&s_block_failed_ui_post, true);
    atomic_store(&s_failed_ui_post_entered, false);
    s_input_callback(APP_MANAGER_KEY_HOME, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    for (int i = 0; i < 500 * CONTROL_TEST_WAIT_SCALE &&
            !atomic_load(&s_failed_ui_post_entered); i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_failed_ui_post_entered));
    for (int i = 0; i < 16; i++)
    {
        assert(app_manager_screen_set_brightness((uint8_t)(32 + i)) ==
               ESP_OK);
    }
    assert(app_manager_input_inject(APP_MANAGER_KEY_POWER,
                                    APP_MANAGER_KEY_EVENT_CLICK) ==
           ESP_ERR_NO_MEM);
    atomic_store(&s_fail_ui_post, false);
    atomic_store(&s_block_failed_ui_post, false);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_suspended));
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    int suspend_before_power = atomic_load(&s_screen_suspend_count);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int suspend_after_power = atomic_load(&s_screen_suspend_count);
    assert(suspend_after_power == suspend_before_power + 1);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_DOWN,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_UP,
                     s_input_user_data);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    usleep(100000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_screen_suspend_count) == suspend_after_power);

    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int failed_down_resume = atomic_load(&s_screen_resume_count) + 1;
    atomic_store(&s_fail_screen_resume, true);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_DOWN,
                     s_input_user_data);
    _wait_for_count(&s_screen_resume_count, failed_down_resume);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    atomic_store(&s_fail_screen_resume, false);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_UP,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    int suspend_before_recovered_click = atomic_load(&s_screen_suspend_count);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    usleep(100000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_screen_suspend_count) == suspend_before_recovered_click);

    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_UP,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    int suspend_before_orphan_click = atomic_load(&s_screen_suspend_count);
    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    usleep(100000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_screen_suspend_count) == suspend_before_orphan_click);

    atomic_store(&s_block_screen_suspend, true);
    int ordered_suspend = atomic_load(&s_screen_suspend_count) + 1;
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_suspend_count, ordered_suspend);
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    atomic_store(&s_block_screen_suspend, false);
    _wait_for_count(&s_screen_suspend_count, ordered_suspend + 1);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_suspended));
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    atomic_store(&s_block_screen_suspend, true);
    int failed_order_suspend = atomic_load(&s_screen_suspend_count) + 1;
    int failed_order_resume = atomic_load(&s_screen_resume_count) + 1;
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_suspend_count, failed_order_suspend);
    atomic_store(&s_fail_screen_resume, true);
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    atomic_store(&s_block_screen_suspend, false);
    _wait_for_count(&s_screen_resume_count, failed_order_resume);
    atomic_store(&s_fail_screen_resume, false);
    _wait_for_count(&s_screen_suspend_count, failed_order_suspend + 1);
    _wait_for_suspended(true);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_suspended));
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    atomic_store(&s_block_screen_suspend, true);
    int interleaved_suspend = atomic_load(&s_screen_suspend_count) + 1;
    int interleaved_resume = atomic_load(&s_screen_resume_count) + 2;
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_suspend_count, interleaved_suspend);
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    atomic_store(&s_block_screen_suspend, false);
    _wait_for_count(&s_screen_suspend_count, interleaved_suspend + 1);
    _wait_for_count(&s_screen_resume_count, interleaved_resume);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(!atomic_load(&s_screen_suspended));

    assert(app_manager_pm_set_timeout_ms(-1) == ESP_OK);
    assert(app_manager_pm_set_standby_delay_ms(1000) == ESP_OK);

    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int standby_before_interleaved_wake =
        atomic_load(&s_standby_request_count);
    atomic_store(&s_inject_activity_on_suspended_query, true);
    _set_time_ms(36003);
    _wait_for_count(&s_injected_activity_count, 1);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_standby_request_count) ==
           standby_before_interleaved_wake);

    s_input_callback(APP_MANAGER_KEY_POWER, APP_MANAGER_KEY_EVENT_CLICK,
                     s_input_user_data);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    const int standby_before_abort = atomic_load(&s_standby_request_count);
    _set_time_ms(37004);
    _wait_for_count(&s_standby_request_count, standby_before_abort + 1);
    wait_for_state(APP_MANAGER_PM_STANDBY_PREPARING);
    const uint32_t aborted_system_generation = first_system_generation + 1U;
    assert(app_manager_input_inject(APP_MANAGER_KEY_POWER,
                                    APP_MANAGER_KEY_EVENT_DOWN) == ESP_OK);
    assert(!app_manager_pm_standby_commit_guard(aborted_system_generation,
            NULL));
    app_manager_pm_standby_commit_callback(aborted_system_generation, NULL);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    _set_time_ms(38005);
    wait_for_state(APP_MANAGER_PM_STANDBY_PREPARING);
    app_manager_pm_standby_commit_callback(first_system_generation, NULL);
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_STANDBY_PREPARING);
    const uint32_t second_standby_epoch =
        app_manager_control_standby_generation_for_test();
    const uint32_t second_system_generation = first_system_generation + 2000U;
    assert(second_system_generation != second_standby_epoch);
    assert(app_manager_pm_standby_commit_guard(second_system_generation, NULL));
    app_manager_pm_standby_commit_callback(second_system_generation, NULL);
    wait_for_state(APP_MANAGER_PM_STANDBY);
    assert(app_manager_pm_notify_user_activity_from_isr());
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    atomic_store(&s_block_screen_suspend, true);
    int blocked_suspend_before = atomic_load(&s_screen_suspend_count);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_suspend_count, blocked_suspend_before + 1);
    assert(atomic_load(&s_screen_suspend_count) == blocked_suspend_before + 1);
    for (int i = 0; i < 16; i++)
    {
        assert(app_manager_screen_resume() == ESP_OK);
    }
    assert(app_manager_screen_resume() == ESP_ERR_NO_MEM);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    int blocked_resume_before = atomic_load(&s_screen_resume_count);
    atomic_store(&s_block_screen_suspend, false);
    _wait_for_count(&s_screen_resume_count, blocked_resume_before + 1);
    assert(atomic_load(&s_screen_resume_count) == blocked_resume_before + 1);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(!atomic_load(&s_screen_suspended));

    atomic_store(&s_fail_adapter_pause, true);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    usleep(100000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    atomic_store(&s_fail_adapter_pause, false);

    int partial_suspend_count = atomic_load(&s_screen_suspend_count) + 1;
    int partial_suspend_rollback = atomic_load(&s_screen_resume_count) + 1;
    atomic_store(&s_partial_fail_screen_suspend, true);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_suspend_count, partial_suspend_count);
    _wait_for_count(&s_screen_resume_count, partial_suspend_rollback);
    usleep(20000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(!atomic_load(&s_screen_suspended));
    atomic_store(&s_partial_fail_screen_suspend, false);

    atomic_store(&s_partial_fail_screen_suspend, true);
    atomic_store(&s_fail_screen_resume, true);
    int failed_active_rollback = atomic_load(&s_screen_resume_count) + 1;
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_resume_count, failed_active_rollback);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_suspended));
    atomic_store(&s_partial_fail_screen_suspend, false);
    atomic_store(&s_fail_screen_resume, false);
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    atomic_store(&s_partial_fail_screen_resume, true);
    int partial_resume_count = atomic_load(&s_screen_resume_count) + 1;
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    _wait_for_count(&s_screen_resume_count, partial_resume_count);
    _wait_for_suspended(true);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    atomic_store(&s_partial_fail_screen_resume, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    atomic_store(&s_partial_fail_screen_resume, true);
    atomic_store(&s_fail_screen_suspend, true);
    assert(app_manager_pm_notify_user_activity() == ESP_OK);
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_suspended));
    atomic_store(&s_partial_fail_screen_resume, false);
    atomic_store(&s_fail_screen_suspend, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(!atomic_load(&s_screen_suspended));

    /* ACTIVE + adapter-sleeping + gate-closed must normalize on generic wake. */
    int failed_suspend_resume = atomic_load(&s_screen_resume_count) + 1;
    atomic_store(&s_fail_screen_suspend, true);
    atomic_store(&s_fail_screen_resume, true);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_screen_resume_count, failed_suspend_resume);
    usleep(20000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(!atomic_load(&s_screen_suspended));
    assert(atomic_load(&s_adapter_sleeping));
    assert(!app_manager_control_notify_touch_activity_from_isr());
    int normalized_recover = atomic_load(&s_adapter_resume_count) + 1;
    atomic_store(&s_fail_screen_suspend, false);
    atomic_store(&s_fail_screen_resume, false);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_adapter_resume_count, normalized_recover);
    assert(!atomic_load(&s_adapter_sleeping));
    assert(!atomic_load(&s_screen_suspended));
    assert(app_manager_control_notify_touch_activity_from_isr());

    /* Every wake phase failure keeps PM OFF and rolls back to adapter sleep. */
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int failed_recover = atomic_load(&s_adapter_resume_count) + 1;
    int recover_rollback_suspend = atomic_load(&s_screen_suspend_count) + 1;
    atomic_store(&s_fail_adapter_recover, true);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_adapter_resume_count, failed_recover);
    _wait_for_count(&s_screen_suspend_count, recover_rollback_suspend);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_adapter_sleeping));
    atomic_store(&s_fail_adapter_recover, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int failed_refresh = atomic_load(&s_display_refresh_count) + 1;
    int refresh_rollback_suspend = atomic_load(&s_screen_suspend_count) + 1;
    atomic_store(&s_fail_display_refresh, true);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_display_refresh_count, failed_refresh);
    _wait_for_count(&s_screen_suspend_count, refresh_rollback_suspend);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_adapter_sleeping));
    atomic_store(&s_fail_display_refresh, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int failed_touch_enable = atomic_load(&s_touch_enable_count) + 1;
    int touch_enable_rollback_suspend =
        atomic_load(&s_screen_suspend_count) + 1;
    atomic_store(&s_fail_touch_enable, true);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_touch_enable_count, failed_touch_enable);
    _wait_for_count(&s_screen_suspend_count, touch_enable_rollback_suspend);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_adapter_sleeping));
    assert(!atomic_load(&s_logical_touch_enabled));
    atomic_store(&s_fail_touch_enable, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_logical_touch_enabled));

    /* A failed lifecycle PAUSE after RESUME must not pause the adapter or rail. */
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int adapter_before_failed_pause = atomic_load(&s_adapter_pause_count);
    int suspend_before_failed_adapter_rollback =
        atomic_load(&s_screen_suspend_count);
    int prepare_before_failed_adapter_rollback =
        atomic_load(&s_screen_resume_count);
    int refresh_before_failed_adapter_rollback =
        atomic_load(&s_display_refresh_count) + 1;
    atomic_store(&s_fail_display_refresh, true);
    atomic_store(&s_fail_lifecycle_pause, true);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_display_refresh_count,
                    refresh_before_failed_adapter_rollback);
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(!atomic_load(&s_adapter_sleeping));
    assert(atomic_load(&s_adapter_pause_count) ==
           adapter_before_failed_pause);
    assert(atomic_load(&s_screen_suspend_count) ==
           suspend_before_failed_adapter_rollback);
    assert(atomic_load(&s_screen_resume_count) ==
           prepare_before_failed_adapter_rollback + 1);
    assert(!app_manager_control_notify_touch_activity_from_isr());
    atomic_store(&s_fail_display_refresh, false);
    atomic_store(&s_fail_lifecycle_pause, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(app_manager_control_notify_touch_activity_from_isr());

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int failed_brightness = atomic_load(&s_brightness_restore_count) + 1;
    int brightness_rollback_suspend = atomic_load(&s_screen_suspend_count);
    adapter_before_failed_pause = atomic_load(&s_adapter_pause_count);
    atomic_store(&s_fail_brightness_restore, true);
    atomic_store(&s_fail_lifecycle_pause, true);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_brightness_restore_count, failed_brightness);
    usleep(20000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(!atomic_load(&s_adapter_sleeping));
    assert(atomic_load(&s_adapter_pause_count) ==
           adapter_before_failed_pause);
    assert(atomic_load(&s_screen_suspend_count) ==
           brightness_rollback_suspend);
    assert(!app_manager_control_notify_touch_activity_from_isr());
    atomic_store(&s_fail_brightness_restore, false);
    atomic_store(&s_fail_lifecycle_pause, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(app_manager_control_notify_touch_activity_from_isr());

    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int failed_commit = atomic_load(&s_screen_commit_count) + 1;
    int commit_rollback_suspend = atomic_load(&s_screen_suspend_count);
    adapter_before_failed_pause = atomic_load(&s_adapter_pause_count);
    atomic_store(&s_fail_screen_commit, true);
    atomic_store(&s_fail_lifecycle_pause, true);
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_screen_commit_count, failed_commit);
    usleep(20000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(!atomic_load(&s_adapter_sleeping));
    assert(atomic_load(&s_adapter_pause_count) ==
           adapter_before_failed_pause);
    assert(atomic_load(&s_screen_suspend_count) == commit_rollback_suspend);
    assert(!app_manager_control_notify_touch_activity_from_isr());
    atomic_store(&s_fail_screen_commit, false);
    atomic_store(&s_fail_lifecycle_pause, false);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(app_manager_control_notify_touch_activity_from_isr());

    /* Typed touch activity can undim, but can neither wake OFF nor survive a gate epoch. */
    uint32_t stale_touch_generation =
        app_manager_control_touch_generation_for_test();
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int resume_before_off_touch = atomic_load(&s_screen_resume_count);
    assert(!app_manager_control_notify_touch_activity_from_isr());
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_SCREEN_OFF);
    assert(atomic_load(&s_screen_resume_count) == resume_before_off_touch);

    app_manager_control_touch_snapshot_pause_for_test(true);
    pthread_t stale_isr_thread;
    assert(pthread_create(&stale_isr_thread, NULL,
                          _interleaved_touch_notify, NULL) == 0);
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            !app_manager_control_touch_snapshot_reached_for_test(); i++)
    {
        usleep(1000);
    }
    assert(app_manager_control_touch_snapshot_reached_for_test());
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(app_manager_control_touch_generation_for_test() !=
           stale_touch_generation);

    assert(app_manager_pm_set_timeout_ms(45000) == ESP_OK);
    _set_time_ms(53006);
    wait_for_state(APP_MANAGER_PM_DIM);
    int resume_before_dim_touch = atomic_load(&s_screen_resume_count);
    app_manager_control_touch_snapshot_pause_for_test(false);
    assert(pthread_join(stale_isr_thread, NULL) == 0);
    assert(!atomic_load(&s_interleaved_touch_result));
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_DIM);
    assert(app_manager_control_notify_touch_generation_for_test(
               stale_touch_generation));
    usleep(150000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_DIM);
    assert(app_manager_control_notify_touch_activity_from_isr());
    wait_for_state(APP_MANAGER_PM_ACTIVE);
    assert(atomic_load(&s_screen_resume_count) == resume_before_dim_touch);

    /* Failed UI touch restore keeps the gate closed until a generic wake retries it. */
    int failed_sleep_prepare = atomic_load(&s_adapter_pause_count) + 1;
    atomic_store(&s_fail_adapter_pause, true);
    atomic_store(&s_fail_display_refresh, true);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    _wait_for_count(&s_adapter_pause_count, failed_sleep_prepare);
    usleep(20000);
    assert(app_manager_pm_get_state() == APP_MANAGER_PM_ACTIVE);
    assert(!atomic_load(&s_screen_suspended));
    assert(!app_manager_control_notify_touch_activity_from_isr());
    int failed_touch_restore = atomic_load(&s_display_refresh_count) + 1;
    assert(app_manager_pm_notify_system_wake() == ESP_OK);
    _wait_for_count(&s_display_refresh_count, failed_touch_restore);
    assert(!app_manager_control_notify_touch_activity_from_isr());
    int successful_touch_restore = atomic_load(&s_display_refresh_count) + 1;
    atomic_store(&s_fail_adapter_pause, false);
    atomic_store(&s_fail_display_refresh, false);
    _wait_for_count(&s_display_refresh_count, successful_touch_restore);
    assert(app_manager_control_notify_touch_activity_from_isr());

    assert(app_manager_input_inject((app_manager_key_t)99,
                                    APP_MANAGER_KEY_EVENT_CLICK) == ESP_ERR_INVALID_ARG);
    assert(app_manager_pm_request_screen_off() == ESP_OK);
    wait_for_state(APP_MANAGER_PM_SCREEN_OFF);
    int resume_before_deinit = atomic_load(&s_screen_resume_count);
    int adapter_resume_before_deinit = atomic_load(&s_adapter_resume_count);
    int refresh_before_deinit = atomic_load(&s_display_refresh_count);
    int brightness_before_deinit = atomic_load(&s_brightness_restore_count);
    int commit_before_deinit = atomic_load(&s_screen_commit_count);
    atomic_store(&s_fail_screen_resume, true);
    assert(app_manager_control_deinit() == ESP_FAIL);
    assert(atomic_load(&s_screen_suspended));
    assert(atomic_load(&s_adapter_sleeping));
    assert(atomic_load(&s_adapter_resume_count) ==
           adapter_resume_before_deinit);
    assert(app_manager_pm_notify_user_activity() == ESP_ERR_INVALID_STATE);

    const int resume_during_failed_stop =
        atomic_load(&s_screen_resume_count);
    const int adapter_during_failed_stop =
        atomic_load(&s_adapter_resume_count);
    const int refresh_during_failed_stop =
        atomic_load(&s_display_refresh_count);
    const int commit_during_failed_stop = atomic_load(&s_screen_commit_count);
    const int brightness_during_failed_stop =
        atomic_load(&s_brightness_restore_count);
    const int suspend_during_failed_stop =
        atomic_load(&s_screen_suspend_count);
    const int home_during_failed_stop = atomic_load(&s_run_home_count);
    const uint32_t power_click =
        POWER_PENDING_EVENT(APP_MANAGER_KEY_EVENT_CLICK);
    atomic_store(&s_wake_pending, true);
    atomic_store(&s_power_pending_events, power_click);
    atomic_store(&s_power_pending_wake, true);
    atomic_store(&s_power_input_pending, power_click);
    atomic_store(&s_touch_restore_pending, true);
    atomic_store(&s_touch_pending_generation,
                 atomic_load(&s_touch_generation));
    atomic_store(&s_touch_pending, true);
    atomic_store(&s_brightness_temp_pending, true);
    atomic_store(&s_standby_commit_pending, true);
    atomic_store(&s_home_pending, true);
    const control_command_t queued_resume = {.type = CONTROL_RESUME};
    assert(xQueueSend(s_queue, &queued_resume, 0) == pdTRUE);
    usleep(250000);
    assert(atomic_load(&s_screen_resume_count) == resume_during_failed_stop);
    assert(atomic_load(&s_adapter_resume_count) == adapter_during_failed_stop);
    assert(atomic_load(&s_display_refresh_count) == refresh_during_failed_stop);
    assert(atomic_load(&s_screen_commit_count) == commit_during_failed_stop);
    assert(atomic_load(&s_brightness_restore_count) ==
           brightness_during_failed_stop);
    assert(atomic_load(&s_screen_suspend_count) == suspend_during_failed_stop);
    assert(atomic_load(&s_run_home_count) == home_during_failed_stop);
    assert(!atomic_load(&s_wake_pending));
    assert(atomic_load(&s_power_pending_events) == 0U);
    assert(!atomic_load(&s_power_pending_wake));
    assert(atomic_load(&s_power_input_pending) == 0U);
    assert(!atomic_load(&s_touch_restore_pending));
    assert(!atomic_load(&s_touch_pending));
    assert(!atomic_load(&s_brightness_temp_pending));
    assert(!atomic_load(&s_standby_commit_pending));
    assert(!atomic_load(&s_home_pending));

    atomic_store(&s_fail_screen_resume, false);
    assert(app_manager_control_deinit() == ESP_OK);
    assert(atomic_load(&s_screen_resume_count) == resume_before_deinit + 2);
    assert(atomic_load(&s_adapter_resume_count) ==
           adapter_resume_before_deinit + 1);
    assert(atomic_load(&s_display_refresh_count) == refresh_before_deinit);
    assert(atomic_load(&s_brightness_restore_count) == brightness_before_deinit);
    assert(atomic_load(&s_screen_commit_count) == commit_before_deinit);
    assert(atomic_load(&s_screen_suspended));
    assert(!atomic_load(&s_adapter_sleeping));
    assert(app_manager_pm_notify_user_activity() == ESP_ERR_INVALID_STATE);

    /* Shutdown rebuilds only the layers required by each partial state. */
    assert(app_manager_control_init(&config) == ESP_OK);
    assert(app_manager_control_open_admission() == ESP_OK);
    atomic_store(&s_screen_suspended, true);
    atomic_store(&s_adapter_sleeping, false);
    int mismatch_resume_count = atomic_load(&s_screen_resume_count) + 1;
    int mismatch_adapter_count = atomic_load(&s_adapter_resume_count);
    refresh_before_deinit = atomic_load(&s_display_refresh_count);
    brightness_before_deinit = atomic_load(&s_brightness_restore_count);
    commit_before_deinit = atomic_load(&s_screen_commit_count);
    assert(app_manager_control_deinit() == ESP_OK);
    assert(atomic_load(&s_screen_resume_count) == mismatch_resume_count);
    assert(atomic_load(&s_adapter_resume_count) == mismatch_adapter_count);
    assert(atomic_load(&s_display_refresh_count) == refresh_before_deinit);
    assert(atomic_load(&s_brightness_restore_count) == brightness_before_deinit);
    assert(atomic_load(&s_screen_commit_count) == commit_before_deinit);
    assert(atomic_load(&s_screen_suspended));

    assert(app_manager_control_init(&config) == ESP_OK);
    assert(app_manager_control_open_admission() == ESP_OK);
    atomic_store(&s_screen_suspended, false);
    atomic_store(&s_adapter_sleeping, true);
    mismatch_resume_count = atomic_load(&s_screen_resume_count);
    mismatch_adapter_count = atomic_load(&s_adapter_resume_count) + 1;
    refresh_before_deinit = atomic_load(&s_display_refresh_count);
    brightness_before_deinit = atomic_load(&s_brightness_restore_count);
    commit_before_deinit = atomic_load(&s_screen_commit_count);
    assert(app_manager_control_deinit() == ESP_OK);
    assert(atomic_load(&s_screen_resume_count) == mismatch_resume_count);
    assert(atomic_load(&s_adapter_resume_count) == mismatch_adapter_count);
    assert(atomic_load(&s_display_refresh_count) == refresh_before_deinit);
    assert(atomic_load(&s_brightness_restore_count) == brightness_before_deinit);
    assert(atomic_load(&s_screen_commit_count) == commit_before_deinit);
    assert(!atomic_load(&s_screen_suspended));
    assert(!atomic_load(&s_adapter_sleeping));

    assert(app_manager_control_init(&config) == ESP_OK);
    assert(app_manager_control_open_admission() == ESP_OK);
    atomic_store(&s_screen_suspended, false);
    atomic_store(&s_adapter_sleeping, false);
    mismatch_resume_count = atomic_load(&s_screen_resume_count);
    mismatch_adapter_count = atomic_load(&s_adapter_resume_count);
    refresh_before_deinit = atomic_load(&s_display_refresh_count);
    brightness_before_deinit = atomic_load(&s_brightness_restore_count);
    commit_before_deinit = atomic_load(&s_screen_commit_count);
    assert(app_manager_control_deinit() == ESP_OK);
    assert(atomic_load(&s_screen_resume_count) == mismatch_resume_count);
    assert(atomic_load(&s_adapter_resume_count) == mismatch_adapter_count);
    assert(atomic_load(&s_display_refresh_count) == refresh_before_deinit);
    assert(atomic_load(&s_brightness_restore_count) == brightness_before_deinit);
    assert(atomic_load(&s_screen_commit_count) == commit_before_deinit);

    atomic_store(&s_fail_brightness_restore, true);
    assert(app_manager_control_init(&config) == ESP_FAIL);
    assert(s_input_callback == NULL);
    assert(app_manager_control_open_admission() == ESP_ERR_INVALID_STATE);
    atomic_store(&s_fail_brightness_restore, false);
    assert(app_manager_control_init(&config) == ESP_OK);
    assert(app_manager_control_open_admission() == ESP_OK);
    assert(app_manager_control_deinit() == ESP_OK);

    assert(app_manager_control_init(&config) == ESP_OK);
    atomic_store(&s_fail_input_register, true);
    assert(app_manager_startup_commit() == ESP_FAIL);
    assert(!s_startup_touch_enabled);
    assert(s_input_callback == NULL);
    atomic_store(&s_fail_input_register, false);
    assert(app_manager_control_deinit() == ESP_OK);

    assert(app_manager_control_init(&config) == ESP_OK);
    assert(app_manager_display_enable_startup_touch() == ESP_OK);
    atomic_store(&s_input_register_entered, false);
    atomic_store(&s_block_input_register, true);
    control_thread_result_t open_thread = {.result = ESP_FAIL};
    pthread_t opener;
    assert(pthread_create(&opener, NULL, _open_admission_thread,
                          &open_thread) == 0);
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            !atomic_load(&s_input_register_entered); ++i)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_input_register_entered));

    control_thread_result_t deinit_thread = {.result = ESP_FAIL};
    pthread_t deinitializer;
    assert(pthread_create(&deinitializer, NULL, _control_deinit_thread,
                          &deinit_thread) == 0);
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            atomic_load(&s_lifecycle_state) != CONTROL_DEINITIALIZING; ++i)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_lifecycle_state) == CONTROL_DEINITIALIZING);
    atomic_store(&s_block_input_register, false);
    assert(pthread_join(opener, NULL) == 0);
    assert(pthread_join(deinitializer, NULL) == 0);
    assert(open_thread.result == ESP_ERR_INVALID_STATE);
    assert(deinit_thread.result == ESP_OK);
    assert(s_input_callback == NULL);
    assert(!atomic_load(&s_control_admitted));

    assert(app_manager_control_init(&config) == ESP_OK);
    assert(app_manager_display_enable_startup_touch() == ESP_OK);
    assert(app_manager_control_open_admission() == ESP_OK);
    atomic_store(&s_input_unregister_entered, false);
    atomic_store(&s_block_input_unregister, true);
    control_thread_result_t first_deinit = {.result = ESP_FAIL};
    pthread_t first_deinitializer;
    assert(pthread_create(&first_deinitializer, NULL,
                          _control_deinit_thread, &first_deinit) == 0);
    for (int i = 0; i < 1000 * CONTROL_TEST_WAIT_SCALE &&
            !atomic_load(&s_input_unregister_entered); ++i)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_input_unregister_entered));
    assert(app_manager_control_deinit() == ESP_ERR_INVALID_STATE);
    atomic_store(&s_block_input_unregister, false);
    assert(pthread_join(first_deinitializer, NULL) == 0);
    assert(first_deinit.result == ESP_OK);
    assert(s_input_callback == NULL);

    puts("app-control production-core tests passed");
    return 0;
}
