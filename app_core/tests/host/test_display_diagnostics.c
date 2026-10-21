#include "app_manager_display_diagnostics_priv.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "host_display_diagnostics_port.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum display_diagnostics_event
{
    DISPLAY_DIAGNOSTICS_EVENT_FPS_ENABLE = 0,
    DISPLAY_DIAGNOSTICS_EVENT_FPS_RESET,
    DISPLAY_DIAGNOSTICS_EVENT_FPS_SAMPLE,
    DISPLAY_DIAGNOSTICS_EVENT_TASK_DELETE,
    DISPLAY_DIAGNOSTICS_EVENT_FPS_DISABLE,
} display_diagnostics_event_t;

struct _lv_display_t
{
    int marker;
    lv_event_cb_t event_callback;
    void *event_user_data;
};

struct _lv_event_t
{
    lv_event_code_t code;
    void *param;
};

typedef struct benchmark_begin_call
{
    uint32_t minimum_fps;
    size_t minimum_dma_largest;
    esp_err_t result;
    atomic_bool returned;
} benchmark_begin_call_t;

typedef struct presentation_wait_call
{
    uint32_t timeout_ms;
    esp_err_t result;
    atomic_bool returned;
} presentation_wait_call_t;

static struct _lv_display_t s_display;
static atomic_uint s_fps_enable_calls;
static atomic_uint s_fps_reset_calls;
static atomic_uint s_fps_sample_calls;
static atomic_uint s_bounded_lock_calls;
static atomic_uint s_recursive_lock_calls;
static atomic_uint s_main_bounded_lock_calls;
static atomic_uint s_unlock_calls;
static atomic_uint s_event_add_calls;
static atomic_uint s_event_remove_calls;
static atomic_bool s_fail_event_add;
static atomic_uint s_heap_free_calls;
static atomic_uint s_heap_largest_calls;
static atomic_uint s_fps_value;
static atomic_size_t s_internal_free;
static atomic_size_t s_internal_largest;
static atomic_size_t s_dma_free;
static atomic_size_t s_dma_largest;
static atomic_size_t s_psram_free;
static atomic_size_t s_psram_largest;
static atomic_bool s_render_task_available;
static atomic_uint s_render_stack_high_water;
static int s_render_task_marker;
static StackType_t s_internal_stack_marker;
static StackType_t s_psram_stack_marker;
static atomic_int s_fps_enable_result;
static atomic_int s_fps_sample_result;
static atomic_int s_fps_reset_result;
static atomic_int s_bounded_lock_result;
static atomic_uint s_bounded_lock_delay_us;
static atomic_bool s_block_fps_sample;
static atomic_bool s_fps_sample_entered;
static atomic_bool s_release_fps_sample;
static atomic_bool s_block_fps_reset;
static atomic_bool s_fps_reset_entered;
static atomic_bool s_release_fps_reset;
static atomic_bool s_submit_returned;
static atomic_bool s_manual_time;
static atomic_int_fast64_t s_manual_time_us;
static atomic_uint s_fps_sample_time_advance_us;
static atomic_size_t s_fps_sample_timestamp_count;
static atomic_int_fast64_t s_fps_sample_timestamps[16];
static display_diagnostics_event_t s_events[64];
static atomic_size_t s_event_count;
static _Thread_local uint32_t s_lvgl_lock_depth;
static pthread_t s_test_thread;

static void _record_event(display_diagnostics_event_t event)
{
    const size_t index = atomic_fetch_add(&s_event_count, 1U);
    assert(index < sizeof(s_events) / sizeof(s_events[0]));
    s_events[index] = event;
}

static void _record_task_delete(void)
{
    assert(s_display.event_callback == NULL);
    _record_event(DISPLAY_DIAGNOSTICS_EVENT_TASK_DELETE);
}

static void _reset(void)
{
    s_test_thread = pthread_self();
    s_lvgl_lock_depth = 0U;
    host_semaphore_test_reset();
    host_display_diagnostics_port_reset();
    host_display_diagnostics_port_set_delete_hook(_record_task_delete);
    atomic_store(&s_fps_enable_calls, 0U);
    atomic_store(&s_fps_reset_calls, 0U);
    atomic_store(&s_fps_sample_calls, 0U);
    atomic_store(&s_bounded_lock_calls, 0U);
    atomic_store(&s_recursive_lock_calls, 0U);
    atomic_store(&s_main_bounded_lock_calls, 0U);
    atomic_store(&s_unlock_calls, 0U);
    atomic_store(&s_event_add_calls, 0U);
    atomic_store(&s_event_remove_calls, 0U);
    atomic_store(&s_fail_event_add, false);
    atomic_store(&s_heap_free_calls, 0U);
    atomic_store(&s_heap_largest_calls, 0U);
    atomic_store(&s_fps_value, 42U);
    atomic_store(&s_internal_free, 180000U);
    atomic_store(&s_internal_largest, 90000U);
    atomic_store(&s_dma_free, 120000U);
    atomic_store(&s_dma_largest, 64000U);
    atomic_store(&s_psram_free, 6000000U);
    atomic_store(&s_psram_largest, 5900000U);
    atomic_store(&s_render_task_available, TEST_RENDER_TASK_EXPECTED != 0);
    atomic_store(&s_render_stack_high_water, 8192U);
    atomic_store(&s_fps_enable_result, ESP_OK);
    atomic_store(&s_fps_sample_result, ESP_OK);
    atomic_store(&s_fps_reset_result, ESP_OK);
    atomic_store(&s_bounded_lock_result, ESP_OK);
    atomic_store(&s_bounded_lock_delay_us, 0U);
    atomic_store(&s_block_fps_sample, false);
    atomic_store(&s_fps_sample_entered, false);
    atomic_store(&s_release_fps_sample, false);
    atomic_store(&s_block_fps_reset, false);
    atomic_store(&s_fps_reset_entered, false);
    atomic_store(&s_release_fps_reset, false);
    atomic_store(&s_submit_returned, false);
    atomic_store(&s_manual_time, false);
    atomic_store(&s_manual_time_us, 0);
    atomic_store(&s_fps_sample_time_advance_us, 0U);
    atomic_store(&s_fps_sample_timestamp_count, 0U);
    atomic_store(&s_event_count, 0U);
    s_display.event_callback = NULL;
    s_display.event_user_data = NULL;
}

void lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback,
                             lv_event_code_t filter, void *user_data)
{
    assert(display == &s_display);
    assert(callback != NULL);
    assert(filter == LV_EVENT_ALL);
    assert(s_display.event_callback == NULL);
    atomic_fetch_add(&s_event_add_calls, 1U);
    if (atomic_exchange(&s_fail_event_add, false))
    {
        return;
    }
    s_display.event_callback = callback;
    s_display.event_user_data = user_data;
}

uint32_t lv_display_get_event_count(lv_display_t *display)
{
    assert(display == &s_display);
    return s_display.event_callback == NULL ? 0U : 1U;
}

uint32_t lv_display_remove_event_cb_with_user_data(lv_display_t *display,
        lv_event_cb_t callback, void *user_data)
{
    assert(display == &s_display);
    if (s_display.event_callback != callback ||
            s_display.event_user_data != user_data)
    {
        return 0U;
    }
    s_display.event_callback = NULL;
    s_display.event_user_data = NULL;
    atomic_fetch_add(&s_event_remove_calls, 1U);
    return 1U;
}

lv_event_code_t lv_event_get_code(lv_event_t *event)
{
    assert(event != NULL);
    return event->code;
}

void *lv_event_get_param(lv_event_t *event)
{
    assert(event != NULL);
    return event->param;
}

static void _send_display_event(lv_event_code_t code, void *param)
{
    assert(s_display.event_callback != NULL);
    lv_event_t event =
    {
        .code = code,
        .param = param,
    };
    s_display.event_callback(&event);
}

static void _emit_frame_at(int64_t submit_us)
{
    lv_area_t area =
    {
        .x1 = 0,
        .y1 = 0,
        .x2 = 367,
        .y2 = 19,
    };
    atomic_store(&s_manual_time_us, submit_us - 500);
    _send_display_event(LV_EVENT_RENDER_START, NULL);
    atomic_store(&s_manual_time_us, submit_us - 200);
    _send_display_event(LV_EVENT_RENDER_READY, NULL);
    _send_display_event(LV_EVENT_FLUSH_START, &area);
    atomic_store(&s_manual_time_us, submit_us);
    app_manager_display_diagnostics_record_submit(&s_display, true, 7360U,
            100U, ESP_OK);
    atomic_store(&s_manual_time_us, submit_us + 100);
    _send_display_event(LV_EVENT_FLUSH_FINISH, &area);
    _send_display_event(LV_EVENT_FLUSH_WAIT_START, NULL);
    atomic_store(&s_manual_time_us, submit_us + 400);
    _send_display_event(LV_EVENT_FLUSH_WAIT_FINISH, NULL);
}

static void _record_submit_at(int64_t submit_us, bool final_flush,
                              esp_err_t result)
{
    atomic_store(&s_manual_time_us, submit_us);
    app_manager_display_diagnostics_record_submit(&s_display, final_flush,
            7360U, 100U, result);
}

static void _exercise_transition(app_manager_transition_effect_t effect,
                                 const uint32_t *intervals_us,
                                 size_t interval_count)
{
    assert(intervals_us != NULL && interval_count > 0U);
    int64_t submit_us = 1000000;
    atomic_store(&s_manual_time_us, submit_us - 10000);
    atomic_store(&s_manual_time, true);
    app_manager_display_diagnostics_transition_started(effect);
    _emit_frame_at(submit_us);
    for (size_t index = 0U; index < interval_count; ++index)
    {
        submit_us += intervals_us[index];
        if (index + 1U == interval_count)
        {
            atomic_store(&s_manual_time_us, submit_us - 600);
            app_manager_display_diagnostics_transition_completed();
        }
        _emit_frame_at(submit_us);
    }
}

static void _exercise_single_frame_transition(
    app_manager_transition_effect_t effect, uint32_t interval_us)
{
    const int64_t start_us = 1000000;
    const int64_t submit_us = start_us + interval_us;
    atomic_store(&s_manual_time_us, start_us);
    atomic_store(&s_manual_time, true);
    app_manager_display_diagnostics_transition_started(effect);
    atomic_store(&s_manual_time_us, submit_us - 600);
    app_manager_display_diagnostics_transition_completed();
    _emit_frame_at(submit_us);
}

static void _record_snapshot_prepare(app_manager_transition_effect_t effect,
                                     int64_t start_us, int64_t finish_us,
                                     bool fallback)
{
    atomic_store(&s_manual_time, true);
    atomic_store(&s_manual_time_us, start_us);
    app_manager_display_diagnostics_snapshot_prepare_started(effect);
    atomic_store(&s_manual_time_us, finish_us);
    app_manager_display_diagnostics_snapshot_prepare_finished(fallback);
}

/* The FPS stub bumps s_fps_sample_calls before _app_manager_display_diagnostics
 * _sample() has read the heap, so the counter alone does not mean a sample is
 * complete. A completed sample reads the internal free and largest pairs after
 * releasing the FPS lock, so wait for those side effects too. */
static void _wait_for_samples(uint32_t minimum_count)
{
    for (unsigned index = 0U;
            index < 500U &&
            (atomic_load(&s_fps_sample_calls) < minimum_count ||
             atomic_load(&s_heap_free_calls) < minimum_count * 2U ||
             atomic_load(&s_heap_largest_calls) < minimum_count * 2U); ++index)
    {
        (void)usleep(1000U);
    }
    assert(atomic_load(&s_fps_sample_calls) >= minimum_count);
    assert(atomic_load(&s_heap_free_calls) >= minimum_count * 2U);
    assert(atomic_load(&s_heap_largest_calls) >= minimum_count * 2U);
}

static void _wait_for_bounded_locks(uint32_t minimum_count)
{
    for (unsigned index = 0U;
            index < 500U &&
            atomic_load(&s_bounded_lock_calls) < minimum_count; ++index)
    {
        (void)usleep(1000U);
    }
    assert(atomic_load(&s_bounded_lock_calls) >= minimum_count);
}

static void _wait_for_flag(atomic_bool *flag)
{
    for (unsigned index = 0U;
            index < 500U && !atomic_load(flag); ++index)
    {
        (void)usleep(1000U);
    }
    assert(atomic_load(flag));
}

static void _wait_for_scheduled_samples(size_t minimum_count)
{
    for (unsigned index = 0U; index < 500U &&
            atomic_load(&s_fps_sample_timestamp_count) < minimum_count; ++index)
    {
        (void)usleep(1000U);
    }
    assert(atomic_load(&s_fps_sample_timestamp_count) >= minimum_count);
}

static void _advance_manual_time_after_timeout(TickType_t timeout_ticks)
{
    assert(atomic_load(&s_manual_time));
    atomic_fetch_add(&s_manual_time_us,
                     (int64_t)timeout_ticks * 1000LL);
}

int64_t esp_timer_get_time(void)
{
    if (atomic_load(&s_manual_time))
    {
        return atomic_load(&s_manual_time_us);
    }
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000LL;
}

esp_err_t esp_lv_adapter_fps_stats_enable(lv_display_t *display, bool enable)
{
    assert(display == &s_display);
    atomic_fetch_add(&s_fps_enable_calls, 1U);
    _record_event(enable ? DISPLAY_DIAGNOSTICS_EVENT_FPS_ENABLE :
                  DISPLAY_DIAGNOSTICS_EVENT_FPS_DISABLE);
    return atomic_load(&s_fps_enable_result);
}

esp_err_t esp_lv_adapter_fps_stats_reset(lv_display_t *display)
{
    assert(display == &s_display);
    assert(esp_lv_adapter_lock(-1) == ESP_OK);
    atomic_fetch_add(&s_fps_reset_calls, 1U);
    _record_event(DISPLAY_DIAGNOSTICS_EVENT_FPS_RESET);
    if (atomic_load(&s_block_fps_reset))
    {
        atomic_store(&s_fps_reset_entered, true);
        while (!atomic_load(&s_release_fps_reset))
        {
            (void)usleep(1000U);
        }
    }
    const esp_err_t result = atomic_load(&s_fps_reset_result);
    esp_lv_adapter_unlock();
    return result;
}

esp_err_t esp_lv_adapter_get_fps(lv_display_t *display, uint32_t *fps)
{
    assert(display == &s_display);
    assert(fps != NULL);
    assert(esp_lv_adapter_lock(-1) == ESP_OK);
    const uint32_t time_advance_us = atomic_load(
                                         &s_fps_sample_time_advance_us);
    if (time_advance_us > 0U)
    {
        const size_t index = atomic_fetch_add(
                                 &s_fps_sample_timestamp_count, 1U);
        assert(index < sizeof(s_fps_sample_timestamps) /
               sizeof(s_fps_sample_timestamps[0]));
        atomic_store(&s_fps_sample_timestamps[index], esp_timer_get_time());
        atomic_fetch_add(&s_manual_time_us, time_advance_us);
    }
    const uint32_t fps_value = atomic_load(&s_fps_value);
    if (atomic_load(&s_block_fps_sample))
    {
        atomic_store(&s_fps_sample_entered, true);
        while (!atomic_load(&s_release_fps_sample))
        {
            (void)usleep(1000U);
        }
    }
    *fps = fps_value;
    atomic_fetch_add(&s_fps_sample_calls, 1U);
    _record_event(DISPLAY_DIAGNOSTICS_EVENT_FPS_SAMPLE);
    const esp_err_t result = atomic_load(&s_fps_sample_result);
    esp_lv_adapter_unlock();
    return result;
}

esp_err_t esp_lv_adapter_lock(int32_t timeout_ms)
{
    if (timeout_ms == 250)
    {
        atomic_fetch_add(&s_bounded_lock_calls, 1U);
        if (pthread_equal(pthread_self(), s_test_thread))
        {
            atomic_fetch_add(&s_main_bounded_lock_calls, 1U);
        }
        const uint32_t delay_us = atomic_load(&s_bounded_lock_delay_us);
        if (delay_us > 0U)
        {
            (void)usleep(delay_us);
        }
        const esp_err_t result = atomic_load(&s_bounded_lock_result);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    else
    {
        assert(timeout_ms == -1);
        assert(s_lvgl_lock_depth > 0U);
        atomic_fetch_add(&s_recursive_lock_calls, 1U);
    }
    s_lvgl_lock_depth++;
    return ESP_OK;
}

void esp_lv_adapter_unlock(void)
{
    assert(s_lvgl_lock_depth > 0U);
    s_lvgl_lock_depth--;
    atomic_fetch_add(&s_unlock_calls, 1U);
}

static void *_record_submit(void *arg)
{
    (void)arg;
    app_manager_display_diagnostics_record_submit(&s_display, true, 7360U,
            100U, ESP_OK);
    atomic_store(&s_submit_returned, true);
    return NULL;
}

static void *_begin_benchmark(void *arg)
{
    benchmark_begin_call_t *call = arg;
    call->result = app_manager_display_diagnostics_begin_benchmark(
                       call->minimum_fps, call->minimum_dma_largest);
    atomic_store(&call->returned, true);
    return NULL;
}

static void *_wait_for_presentation(void *arg)
{
    presentation_wait_call_t *call = arg;
    call->result = app_manager_display_diagnostics_wait_for_presentation(
                       call->timeout_ms);
    atomic_store(&call->returned, true);
    return NULL;
}

static void _wait_for_begin_conflict(benchmark_begin_call_t *call,
                                     uint32_t reset_count)
{
    for (unsigned index = 0U; index < 500U &&
            !atomic_load(&call->returned) &&
            atomic_load(&s_fps_reset_calls) < reset_count; ++index)
    {
        (void)usleep(1000U);
    }
    assert(atomic_load(&call->returned) ||
           atomic_load(&s_fps_reset_calls) >= reset_count);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    atomic_fetch_add(&s_heap_free_calls, 1U);
    if ((caps & MALLOC_CAP_SPIRAM) != 0U)
    {
        return atomic_load(&s_psram_free);
    }
    return (caps & MALLOC_CAP_DMA) != 0U ? atomic_load(&s_dma_free) :
           atomic_load(&s_internal_free);
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    atomic_fetch_add(&s_heap_largest_calls, 1U);
    if ((caps & MALLOC_CAP_SPIRAM) != 0U)
    {
        return atomic_load(&s_psram_largest);
    }
    return (caps & MALLOC_CAP_DMA) != 0U ? atomic_load(&s_dma_largest) :
           atomic_load(&s_internal_largest);
}

TaskHandle_t xTaskGetHandle(const char *name)
{
    assert(name != NULL);
    assert(strcmp(name, TEST_RENDER_TASK_NAME) == 0);
    return atomic_load(&s_render_task_available) ? &s_render_task_marker :
           NULL;
}

BaseType_t xTaskGetCoreID(TaskHandle_t task)
{
    assert(task == &s_render_task_marker);
    return TEST_RENDER_TASK_CORE;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == &s_render_task_marker);
    return atomic_load(&s_render_stack_high_water);
}

StackType_t *xTaskGetStackStart(TaskHandle_t task)
{
    assert(task == &s_render_task_marker);
    return TEST_RENDER_TASK_PSRAM ? &s_psram_stack_marker :
           &s_internal_stack_marker;
}

bool esp_ptr_external_ram(const void *pointer)
{
    assert(pointer == &s_internal_stack_marker ||
           pointer == &s_psram_stack_marker);
    return pointer == &s_psram_stack_marker;
}

static void _test_start_semaphore_failures_cleanup_and_retry(void)
{
    for (unsigned create_call = 1U; create_call <= 2U; ++create_call)
    {
        _reset();
        host_semaphore_fail_create_on(create_call);
        assert(app_manager_display_diagnostics_start(&s_display) ==
               ESP_ERR_NO_MEM);
        assert(host_semaphore_create_count() == create_call);
        assert(host_semaphore_delete_count() == create_call - 1U);
        assert(host_semaphore_null_delete_count() == 0U);
        assert(atomic_load(&s_fps_enable_calls) == 0U);

        host_semaphore_test_reset();
        assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
        assert(app_manager_display_diagnostics_stop() == ESP_OK);
        assert(host_semaphore_create_count() == 2U);
        assert(host_semaphore_delete_count() == 2U);
        assert(host_semaphore_null_delete_count() == 0U);
    }
}

static void _test_start_sample_and_stop(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_start(&s_display) ==
           ESP_ERR_INVALID_STATE);
    assert(host_display_diagnostics_port_create_count() == 1U);
    assert(host_display_diagnostics_port_stack_depth() == 3072U);
    assert(host_display_diagnostics_port_stack_caps() ==
           (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(host_display_diagnostics_port_priority() == 1U);
    assert(host_display_diagnostics_port_core_id() ==
           CONFIG_MAIN_PROJECT_TASK_CORE_ID);
    assert(atomic_load(&s_event_add_calls) == 1U);

    _wait_for_samples(1U);
    assert(atomic_load(&s_bounded_lock_calls) >= 1U);
    assert(atomic_load(&s_recursive_lock_calls) >= 1U);
    assert(atomic_load(&s_heap_free_calls) >= 2U);
    assert(atomic_load(&s_heap_largest_calls) >= 2U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
    assert(host_display_diagnostics_port_delete_count() == 1U);
    assert(atomic_load(&s_event_remove_calls) == 1U);
    assert(atomic_load(&s_fps_enable_calls) == 2U);
    const size_t event_count = atomic_load(&s_event_count);
    assert(s_events[0] == DISPLAY_DIAGNOSTICS_EVENT_FPS_ENABLE);
    assert(s_events[event_count - 2U] ==
           DISPLAY_DIAGNOSTICS_EVENT_TASK_DELETE);
    assert(s_events[event_count - 1U] ==
           DISPLAY_DIAGNOSTICS_EVENT_FPS_DISABLE);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_passing_benchmark(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(0U, 14720U) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_ERR_INVALID_STATE);
    assert(app_manager_display_diagnostics_end_benchmark(NULL) ==
           ESP_ERR_INVALID_ARG);
    _wait_for_samples(1U);
    const uint32_t intervals_us[] =
    {
        APP_MANAGER_DISPLAY_INTERVAL_TARGET_US,
        APP_MANAGER_DISPLAY_INTERVAL_LONG_US + 333U,
    };
    _exercise_transition(APP_MANAGER_TRANSITION_FADE, intervals_us,
                         sizeof(intervals_us) / sizeof(intervals_us[0]));

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.sample_count >= 1U);
    assert(report.sample_error_count == 0U);
    assert(report.lvgl_lock_error_count == 0U);
    assert(report.fps_read_error_count == 0U);
    assert(report.minimum_fps == 42U);
    assert(report.fps_failure_count == 0U);
    assert(report.minimum_internal_free == 180000U);
    assert(report.minimum_internal_largest == 90000U);
    assert(report.minimum_dma_free == 120000U);
    assert(report.minimum_dma_largest == 64000U);
    assert(report.minimum_psram_free == 6000000U);
    assert(report.minimum_psram_largest == 5900000U);
    assert(report.render_task_found == (TEST_RENDER_TASK_EXPECTED != 0));
    assert(report.minimum_render_stack_high_water ==
           (TEST_RENDER_TASK_EXPECTED ? 8192U : 0U));
    assert(report.render_task_core_id ==
           (TEST_RENDER_TASK_EXPECTED ? TEST_RENDER_TASK_CORE : -1));
    assert(report.render_task_stack_in_psram ==
           (TEST_RENDER_TASK_PSRAM != 0));
    assert(report.dma_failure_count == 0U);
    assert(report.frame_submit_count == 3U);
    assert(report.panel_submit_count == 3U);
    assert(report.panel_pixel_count == 22080U);
    assert(report.submit_failure_count == 0U);
    assert(report.submit_wait_us == 300U);
    assert(report.maximum_submit_wait_us == 100U);
    assert(report.transition_start_count == 1U);
    assert(report.transition_complete_count == 1U);
    assert(report.transition_cancel_count == 0U);
    assert(report.active_frame_count == 3U);
    assert(report.active_duration_us == 85333U);
    assert(report.interval_count == 3U);
    assert(report.interval_within_target_count == 2U);
    assert(report.maximum_consecutive_long_intervals == 1U);
    assert(report.interval_p50_us ==
           APP_MANAGER_DISPLAY_INTERVAL_TARGET_US);
    assert(report.interval_p95_us == 42000U);
    assert(report.interval_p99_us == 42000U);
    assert(report.maximum_interval_us == 42000U);
    assert(report.render_count == 3U);
    assert(report.render_us == 900U);
    assert(report.maximum_render_us == 300U);
    assert(report.flush_count == 3U);
    assert(report.flush_pixel_count == 22080U);
    assert(report.flush_us == 900U);
    assert(report.maximum_flush_us == 300U);
    assert(report.flush_wait_count == 3U);
    assert(report.flush_wait_us == 900U);
    assert(report.maximum_flush_wait_us == 300U);
    const app_manager_display_effect_benchmark_report_t *fade =
        &report.effects[APP_MANAGER_TRANSITION_FADE -
                        APP_MANAGER_TRANSITION_NONE];
    assert(fade->effect == APP_MANAGER_TRANSITION_FADE);
    assert(fade->transition_start_count == 1U);
    assert(fade->active_frame_count == 3U);
    assert(fade->panel_submit_count == 3U);
    assert(fade->interval_p95_us == 42000U);
    const app_manager_display_effect_benchmark_report_t *push =
        &report.effects[APP_MANAGER_TRANSITION_PUSH_LEFT -
                        APP_MANAGER_TRANSITION_NONE];
    assert(push->effect == APP_MANAGER_TRANSITION_PUSH_LEFT);
    assert(push->transition_start_count == 0U);
    assert(report.passed);
    assert(atomic_load(&s_fps_reset_calls) == 1U);
    assert(app_manager_display_diagnostics_end_benchmark(&report) ==
           ESP_ERR_INVALID_STATE);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_snapshot_prepare_metrics_are_informational(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);

    _record_snapshot_prepare(APP_MANAGER_TRANSITION_FADE, 1000000,
                             1010000, false);
    _record_snapshot_prepare(APP_MANAGER_TRANSITION_FADE, 1020000,
                             1060000, true);
    _record_snapshot_prepare(APP_MANAGER_TRANSITION_PUSH_LEFT, 1070000,
                             1180000, false);
    app_manager_display_diagnostics_snapshot_prepare_finished(false);

    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_FADE, &interval_us, 1U);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.snapshot_prepare_count == 3U);
    assert(report.snapshot_prepare_us == 160000U);
    assert(report.maximum_snapshot_prepare_us == 110000U);
    assert(report.snapshot_prepare_p95_us == 110000U);
    assert(report.snapshot_fallback_count == 1U);
    assert(report.snapshot_prepare_p95_us >
           APP_MANAGER_DISPLAY_SNAPSHOT_PREPARE_TARGET_US);
    assert(report.passed);

    const app_manager_display_effect_benchmark_report_t *fade =
        &report.effects[APP_MANAGER_TRANSITION_FADE -
                        APP_MANAGER_TRANSITION_NONE];
    assert(fade->snapshot_prepare_count == 2U);
    assert(fade->snapshot_prepare_us == 50000U);
    assert(fade->maximum_snapshot_prepare_us == 40000U);
    assert(fade->snapshot_prepare_p95_us == 40000U);
    assert(fade->snapshot_fallback_count == 1U);

    const app_manager_display_effect_benchmark_report_t *push =
        &report.effects[APP_MANAGER_TRANSITION_PUSH_LEFT -
                        APP_MANAGER_TRANSITION_NONE];
    assert(push->snapshot_prepare_count == 1U);
    assert(push->snapshot_prepare_us == 110000U);
    assert(push->snapshot_prepare_p95_us == 110000U);
    assert(push->snapshot_fallback_count == 0U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_memory_metrics_reset_between_benchmarks(void)
{
    _reset();
    atomic_store(&s_internal_free, 100000U);
    atomic_store(&s_internal_largest, 50000U);
    atomic_store(&s_dma_free, 80000U);
    atomic_store(&s_dma_largest, 30000U);
    atomic_store(&s_psram_free, 4000000U);
    atomic_store(&s_psram_largest, 3900000U);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    const uint32_t first_sample = atomic_load(&s_fps_sample_calls) + 1U;
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(first_sample);

    app_manager_display_benchmark_report_t first_report;
    assert(app_manager_display_diagnostics_end_benchmark(&first_report) ==
           ESP_OK);
    assert(first_report.minimum_internal_free == 100000U);
    assert(first_report.minimum_internal_largest == 50000U);
    assert(first_report.minimum_dma_free == 80000U);
    assert(first_report.minimum_dma_largest == 30000U);
    assert(first_report.minimum_psram_free == 4000000U);
    assert(first_report.minimum_psram_largest == 3900000U);

    atomic_store(&s_internal_free, 200000U);
    atomic_store(&s_internal_largest, 100000U);
    atomic_store(&s_dma_free, 160000U);
    atomic_store(&s_dma_largest, 70000U);
    atomic_store(&s_psram_free, 6100000U);
    atomic_store(&s_psram_largest, 6000000U);
    const uint32_t second_sample = atomic_load(&s_fps_sample_calls) + 1U;
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(second_sample);

    app_manager_display_benchmark_report_t second_report;
    assert(app_manager_display_diagnostics_end_benchmark(&second_report) ==
           ESP_OK);
    assert(second_report.minimum_internal_free == 200000U);
    assert(second_report.minimum_internal_largest == 100000U);
    assert(second_report.minimum_dma_free == 160000U);
    assert(second_report.minimum_dma_largest == 70000U);
    assert(second_report.minimum_psram_free == 6100000U);
    assert(second_report.minimum_psram_largest == 6000000U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

#if TEST_RENDER_TASK_EXPECTED
static void _test_missing_render_task_is_reported(void)
{
    _reset();
    atomic_store(&s_render_task_available, false);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(!report.render_task_found);
    assert(report.minimum_render_stack_high_water == 0U);
    assert(!report.render_task_stack_in_psram);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}
#endif

static void _test_completion_waits_for_successful_final_submit(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);

    atomic_store(&s_manual_time, true);
    atomic_store(&s_manual_time_us, 1000000);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_FADE);
    _record_submit_at(1020000, true, ESP_OK);
    _record_submit_at(1053333, true, ESP_OK);
    atomic_store(&s_manual_time_us, 1060000);
    app_manager_display_diagnostics_transition_completed();
    _record_submit_at(1070000, false, ESP_OK);
    _record_submit_at(1080000, true, ESP_FAIL);
    _record_submit_at(1090000, true, ESP_OK);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.transition_start_count == 1U);
    assert(report.transition_complete_count == 1U);
    assert(report.transition_cancel_count == 0U);
    assert(report.active_frame_count == 3U);
    assert(report.active_duration_us == 90000U);
    assert(report.interval_count == 3U);
    assert(report.interval_within_target_count == 2U);
    assert(report.maximum_interval_us == 36667U);
    assert(report.frame_submit_count == 3U);
    assert(report.panel_submit_count == 5U);
    assert(report.submit_failure_count == 1U);
    assert(!report.passed);
    atomic_store(&s_manual_time, false);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_presentation_wait_blocks_until_final_submit(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    assert(app_manager_display_diagnostics_wait_for_presentation(0U) ==
           ESP_ERR_INVALID_ARG);

    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_FADE);
    app_manager_display_diagnostics_transition_completed();
    presentation_wait_call_t wait_call =
    {
        .timeout_ms = 1000U,
    };
    atomic_init(&wait_call.returned, false);
    pthread_t wait_thread;
    assert(pthread_create(&wait_thread, NULL, _wait_for_presentation,
                          &wait_call) == 0);
    (void)usleep(10000U);
    assert(!atomic_load(&wait_call.returned));

    app_manager_display_diagnostics_record_submit(&s_display, false, 7360U,
            100U, ESP_OK);
    (void)usleep(10000U);
    assert(!atomic_load(&wait_call.returned));
    app_manager_display_diagnostics_record_submit(&s_display, true, 7360U,
            100U, ESP_OK);
    assert(pthread_join(wait_thread, NULL) == 0);
    assert(wait_call.result == ESP_OK);

    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_PUSH_LEFT);
    app_manager_display_diagnostics_transition_completed();
    app_manager_display_diagnostics_record_submit(&s_display, true, 7360U,
            100U, ESP_OK);
    assert(app_manager_display_diagnostics_wait_for_presentation(1000U) ==
           ESP_OK);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.transition_start_count == 2U);
    assert(report.transition_complete_count == 2U);
    assert(report.transition_cancel_count == 0U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_presentation_wait_times_out_without_final_submit(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_REVEAL_RIGHT);
    app_manager_display_diagnostics_transition_completed();

    assert(app_manager_display_diagnostics_wait_for_presentation(5U) ==
           ESP_ERR_TIMEOUT);
    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.transition_start_count == 1U);
    assert(report.transition_complete_count == 0U);
    assert(report.transition_cancel_count == 0U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_missing_final_submit_cannot_pass(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);

    atomic_store(&s_manual_time, true);
    atomic_store(&s_manual_time_us, 2000000);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_PUSH_LEFT);
    _record_submit_at(2010000, true, ESP_OK);
    atomic_store(&s_manual_time_us, 2011000);
    app_manager_display_diagnostics_transition_completed();

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.transition_start_count == 1U);
    assert(report.transition_complete_count == 0U);
    assert(report.active_frame_count == 1U);
    assert(report.interval_count == 1U);
    assert(report.active_duration_us == 0U);
    assert(!report.passed);
    atomic_store(&s_manual_time, false);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_spans_do_not_cross_benchmark_generation(void)
{
    const lv_area_t area =
    {
        .x1 = 0,
        .y1 = 0,
        .x2 = 367,
        .y2 = 19,
    };
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    atomic_store(&s_manual_time, true);
    atomic_store(&s_manual_time_us, 3000000);
    _send_display_event(LV_EVENT_RENDER_START, NULL);
    _send_display_event(LV_EVENT_FLUSH_START, (void *)&area);
    _send_display_event(LV_EVENT_FLUSH_WAIT_START, NULL);

    app_manager_display_benchmark_report_t first_report;
    assert(app_manager_display_diagnostics_end_benchmark(&first_report) ==
           ESP_OK);
    assert(first_report.render_count == 0U);
    assert(first_report.flush_count == 0U);
    assert(first_report.flush_pixel_count == 0U);
    assert(first_report.flush_wait_count == 0U);

    atomic_store(&s_manual_time, false);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    atomic_store(&s_manual_time, true);
    atomic_store(&s_manual_time_us, 4000000);
    _send_display_event(LV_EVENT_RENDER_READY, NULL);
    _send_display_event(LV_EVENT_FLUSH_FINISH, (void *)&area);
    _send_display_event(LV_EVENT_FLUSH_WAIT_FINISH, NULL);
    _send_display_event(LV_EVENT_RENDER_START, NULL);
    atomic_store(&s_manual_time_us, 4000100);
    _send_display_event(LV_EVENT_RENDER_READY, NULL);
    _send_display_event(LV_EVENT_FLUSH_START, (void *)&area);
    atomic_store(&s_manual_time_us, 4000300);
    _send_display_event(LV_EVENT_FLUSH_FINISH, (void *)&area);
    _send_display_event(LV_EVENT_FLUSH_WAIT_START, NULL);
    atomic_store(&s_manual_time_us, 4000600);
    _send_display_event(LV_EVENT_FLUSH_WAIT_FINISH, NULL);

    app_manager_display_benchmark_report_t second_report;
    assert(app_manager_display_diagnostics_end_benchmark(&second_report) ==
           ESP_OK);
    assert(second_report.render_count == 1U);
    assert(second_report.render_us == 100U);
    assert(second_report.flush_count == 1U);
    assert(second_report.flush_pixel_count == 7360U);
    assert(second_report.flush_us == 200U);
    assert(second_report.flush_wait_count == 1U);
    assert(second_report.flush_wait_us == 300U);
    atomic_store(&s_manual_time, false);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_sampling_uses_absolute_benchmark_deadlines(void)
{
    _reset();
    atomic_store(&s_manual_time, true);
    atomic_store(&s_manual_time_us, 5000000);
    atomic_store(&s_fps_sample_time_advance_us, 2000U);
    host_display_diagnostics_port_set_timeout_hook(
        _advance_manual_time_after_timeout);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_scheduled_samples(3U);

    const int64_t first_us = atomic_load(&s_fps_sample_timestamps[0]);
    const int64_t second_us = atomic_load(&s_fps_sample_timestamps[1]);
    const int64_t third_us = atomic_load(&s_fps_sample_timestamps[2]);
    assert(second_us - first_us ==
           (int64_t)APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_MS * 1000LL);
    assert(third_us - second_us ==
           (int64_t)APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_MS * 1000LL);

    atomic_store(&s_fps_sample_time_advance_us, 0U);
    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.sample_count >= 3U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
    atomic_store(&s_manual_time, false);
}

static void _test_fps_read_error_classification_and_lock_wait(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    atomic_store(&s_bounded_lock_delay_us, 2000U);
    atomic_store(&s_fps_sample_result, ESP_FAIL);
    const uint32_t sample_calls = atomic_load(&s_fps_sample_calls);
    _wait_for_samples(sample_calls + 2U);
    atomic_store(&s_fps_sample_result, ESP_OK);
    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_FADE, &interval_us, 1U);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.sample_error_count > 0U);
    assert(report.lvgl_lock_error_count == 0U);
    assert(report.fps_read_error_count == report.sample_error_count);
    assert(report.maximum_fps_lock_wait_us >= 2000U);
    assert(!report.passed);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_histogram_ranges_and_percentile_clamp(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);

    const uint32_t one_ms_intervals_us[] = {31001U, 31999U, 50000U};
    _exercise_transition(APP_MANAGER_TRANSITION_FADE, one_ms_intervals_us,
                         3U);
    const uint32_t two_ms_intervals_us[] = {50001U, 51999U, 70000U};
    _exercise_transition(APP_MANAGER_TRANSITION_PUSH_LEFT,
                         two_ms_intervals_us, 3U);
    const uint32_t five_ms_intervals_us[] = {70001U, 74999U, 200000U};
    _exercise_transition(APP_MANAGER_TRANSITION_COVER_LEFT,
                         five_ms_intervals_us, 3U);
    const uint32_t clamped_interval_us = 177579U;
    _exercise_single_frame_transition(APP_MANAGER_TRANSITION_REVEAL_RIGHT,
                                      clamped_interval_us);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    const app_manager_display_effect_benchmark_report_t *fade =
        &report.effects[APP_MANAGER_TRANSITION_FADE -
                        APP_MANAGER_TRANSITION_NONE];
    const app_manager_display_effect_benchmark_report_t *push =
        &report.effects[APP_MANAGER_TRANSITION_PUSH_LEFT -
                        APP_MANAGER_TRANSITION_NONE];
    const app_manager_display_effect_benchmark_report_t *cover =
        &report.effects[APP_MANAGER_TRANSITION_COVER_LEFT -
                        APP_MANAGER_TRANSITION_NONE];
    const app_manager_display_effect_benchmark_report_t *reveal =
        &report.effects[APP_MANAGER_TRANSITION_REVEAL_RIGHT -
                        APP_MANAGER_TRANSITION_NONE];
    assert(fade->interval_p50_us == 32000U);
    assert(push->interval_p50_us == 52000U);
    assert(cover->interval_p50_us == 75000U);
    assert(reveal->interval_p50_us == clamped_interval_us);
    assert(reveal->interval_p95_us == clamped_interval_us);
    assert(reveal->interval_p99_us == clamped_interval_us);
    assert(report.interval_p50_us <= report.maximum_interval_us);
    assert(report.interval_p95_us <= report.maximum_interval_us);
    assert(report.interval_p99_us <= report.maximum_interval_us);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_low_fps_is_informational(void)
{
    _reset();
    atomic_store(&s_fps_value, 29U);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);
    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_PUSH_LEFT, &interval_us, 1U);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.fps_failure_count >= 1U);
    assert(report.dma_failure_count == 0U);
    assert(report.submit_failure_count == 0U);
    assert(report.passed);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_low_dma_fails_benchmark(void)
{
    _reset();
    atomic_store(&s_dma_largest, 14000U);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);
    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_PUSH_RIGHT, &interval_us, 1U);

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.fps_failure_count == 0U);
    assert(report.minimum_dma_largest == 14000U);
    assert(report.dma_failure_count >= 1U);
    assert(report.submit_failure_count == 0U);
    assert(!report.passed);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_submit_failure_fails_benchmark(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);
    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    atomic_store(&s_manual_time_us, 990000);
    atomic_store(&s_manual_time, true);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_COVER_LEFT);
    app_manager_display_diagnostics_record_submit(&s_display, false, 7360U,
            70U, ESP_FAIL);
    _emit_frame_at(1000000);
    _emit_frame_at(1000000 + interval_us);
    atomic_store(&s_manual_time_us, 1000000 + interval_us + 1000U);
    app_manager_display_diagnostics_transition_completed();

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.fps_failure_count == 0U);
    assert(report.dma_failure_count == 0U);
    assert(report.frame_submit_count == 2U);
    assert(report.submit_failure_count == 1U);
    assert(report.submit_wait_us == 270U);
    assert(report.maximum_submit_wait_us == 100U);
    assert(!report.passed);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_effect_attribution_and_cancellation(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_samples(1U);

    const uint32_t fade_interval_us =
        APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_FADE, &fade_interval_us, 1U);

    atomic_store(&s_manual_time_us, 2000000);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_PUSH_LEFT);
    _emit_frame_at(2010000);
    _emit_frame_at(2060001);
    atomic_store(&s_manual_time_us, 2061001);
    app_manager_display_diagnostics_transition_cancelled();

    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.transition_start_count == 2U);
    assert(report.transition_complete_count == 1U);
    assert(report.transition_cancel_count == 1U);
    assert(report.active_frame_count == 4U);
    assert(report.interval_count == 4U);
    assert(report.interval_within_target_count == 3U);
    assert(report.interval_p50_us == 10000U);
    assert(report.interval_p95_us == 50001U);
    assert(report.maximum_interval_us == 50001U);
    assert(!report.passed);

    const app_manager_display_effect_benchmark_report_t *fade =
        &report.effects[APP_MANAGER_TRANSITION_FADE -
                        APP_MANAGER_TRANSITION_NONE];
    const app_manager_display_effect_benchmark_report_t *push =
        &report.effects[APP_MANAGER_TRANSITION_PUSH_LEFT -
                        APP_MANAGER_TRANSITION_NONE];
    assert(fade->transition_complete_count == 1U);
    assert(fade->transition_cancel_count == 0U);
    assert(fade->interval_p95_us ==
           APP_MANAGER_DISPLAY_INTERVAL_TARGET_US);
    assert(push->transition_complete_count == 0U);
    assert(push->transition_cancel_count == 1U);
    assert(push->interval_p95_us == 50001U);
    assert(push->maximum_consecutive_long_intervals == 1U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_stop_lock_timeout_is_retryable(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    atomic_store(&s_bounded_lock_result, ESP_ERR_TIMEOUT);
    assert(app_manager_display_diagnostics_stop() == ESP_ERR_TIMEOUT);
    assert(host_display_diagnostics_port_delete_count() == 0U);
    assert(atomic_load(&s_event_remove_calls) == 0U);

    atomic_store(&s_bounded_lock_result, ESP_OK);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
    assert(host_display_diagnostics_port_delete_count() == 1U);
    assert(atomic_load(&s_event_remove_calls) == 1U);
}

static void _test_repeated_lvgl_lock_timeouts_are_recoverable(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    app_manager_display_diagnostics_record_submit(&s_display, true, 7360U,
            100U, ESP_OK);
    const uint32_t lock_calls = atomic_load(&s_bounded_lock_calls);
    atomic_store(&s_bounded_lock_delay_us, 1500U);
    atomic_store(&s_bounded_lock_result, ESP_ERR_TIMEOUT);
    /* Entering the fourth attempt proves the single diagnostics task committed
     * the preceding three failed samples. */
    _wait_for_bounded_locks(lock_calls + 4U);
    atomic_store(&s_bounded_lock_result, ESP_OK);
    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_REVEAL_RIGHT, &interval_us,
                         1U);

    const uint32_t main_lock_calls = atomic_load(&s_main_bounded_lock_calls);
    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(atomic_load(&s_main_bounded_lock_calls) == main_lock_calls);
    assert(report.sample_count >= 3U);
    assert(report.sample_error_count >= 3U);
    assert(report.lvgl_lock_error_count == report.sample_error_count);
    assert(report.fps_read_error_count == 0U);
    assert(report.maximum_fps_lock_wait_us >= 1500U);
    assert(report.minimum_fps == 0U);
    assert(report.minimum_dma_largest == 64000U);
    assert(!report.passed);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
    assert(host_display_diagnostics_port_delete_count() == 1U);
}

static void _test_benchmark_reset_lock_timeout_is_recoverable(void)
{
    _reset();
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    atomic_store(&s_bounded_lock_result, ESP_ERR_TIMEOUT);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_ERR_TIMEOUT);
    assert(atomic_load(&s_fps_reset_calls) == 0U);

    atomic_store(&s_bounded_lock_result, ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    assert(atomic_load(&s_fps_reset_calls) == 1U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_sampling_does_not_hold_benchmark_lock_while_waiting_for_lvgl(
    void)
{
    _reset();
    atomic_store(&s_block_fps_sample, true);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    _wait_for_flag(&s_fps_sample_entered);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_FADE);

    pthread_t submit_thread;
    assert(pthread_create(&submit_thread, NULL, _record_submit, NULL) == 0);
    _wait_for_flag(&s_submit_returned);

    atomic_store(&s_release_fps_sample, true);
    assert(pthread_join(submit_thread, NULL) == 0);
    app_manager_display_diagnostics_transition_completed();
    _wait_for_samples(1U);
    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(report.frame_submit_count == 1U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_stale_sample_does_not_commit_to_next_benchmark(void)
{
    _reset();
    atomic_store(&s_fps_value, 10U);
    atomic_store(&s_fps_sample_result, ESP_FAIL);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    atomic_store(&s_block_fps_sample, true);
    _wait_for_flag(&s_fps_sample_entered);

    app_manager_display_benchmark_report_t first_report;
    assert(app_manager_display_diagnostics_end_benchmark(&first_report) ==
           ESP_OK);
    assert(first_report.sample_count == 0U);

    atomic_store(&s_fps_value, 42U);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_OK);
    const uint32_t sample_calls = atomic_load(&s_fps_sample_calls);
    const uint32_t lock_calls = atomic_load(&s_bounded_lock_calls);
    atomic_store(&s_release_fps_sample, true);
    _wait_for_samples(sample_calls + 1U);
    atomic_store(&s_fps_sample_result, ESP_OK);
    _wait_for_samples(sample_calls + 2U);
    _wait_for_bounded_locks(lock_calls + 2U);
    const uint32_t interval_us = APP_MANAGER_DISPLAY_INTERVAL_TARGET_US;
    _exercise_transition(APP_MANAGER_TRANSITION_COVER_RIGHT, &interval_us, 1U);

    app_manager_display_benchmark_report_t second_report;
    assert(app_manager_display_diagnostics_end_benchmark(&second_report) ==
           ESP_OK);
    assert(second_report.sample_count >= 1U);
    assert(second_report.minimum_fps == 42U);
    assert(second_report.sample_error_count == 0U);
    assert(second_report.lvgl_lock_error_count == 0U);
    assert(second_report.fps_read_error_count == 0U);
    assert(second_report.fps_failure_count == 0U);
    assert(second_report.frame_submit_count == 2U);
    assert(second_report.passed);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_concurrent_begin_has_single_reset_owner(void)
{
    _reset();
    atomic_store(&s_block_fps_reset, true);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);

    benchmark_begin_call_t owner =
    {
        .minimum_fps = 30U,
        .minimum_dma_largest = 14720U,
    };
    atomic_init(&owner.returned, false);
    pthread_t owner_thread;
    assert(pthread_create(&owner_thread, NULL, _begin_benchmark, &owner) == 0);
    _wait_for_flag(&s_fps_reset_entered);

    benchmark_begin_call_t contender =
    {
        .minimum_fps = 31U,
        .minimum_dma_largest = 30000U,
    };
    atomic_init(&contender.returned, false);
    pthread_t contender_thread;
    assert(pthread_create(&contender_thread, NULL, _begin_benchmark,
                          &contender) == 0);
    _wait_for_begin_conflict(&contender, 2U);
    atomic_store(&s_release_fps_reset, true);
    assert(pthread_join(owner_thread, NULL) == 0);
    assert(pthread_join(contender_thread, NULL) == 0);

    assert(owner.result == ESP_OK);
    assert(contender.result == ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_fps_reset_calls) == 1U);
    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_failed_begin_releases_single_reset_owner(void)
{
    _reset();
    atomic_store(&s_block_fps_reset, true);
    atomic_store(&s_fps_reset_result, ESP_FAIL);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);

    benchmark_begin_call_t owner =
    {
        .minimum_fps = 30U,
        .minimum_dma_largest = 14720U,
    };
    atomic_init(&owner.returned, false);
    pthread_t owner_thread;
    assert(pthread_create(&owner_thread, NULL, _begin_benchmark, &owner) == 0);
    _wait_for_flag(&s_fps_reset_entered);

    benchmark_begin_call_t contender =
    {
        .minimum_fps = 31U,
        .minimum_dma_largest = 30000U,
    };
    atomic_init(&contender.returned, false);
    pthread_t contender_thread;
    assert(pthread_create(&contender_thread, NULL, _begin_benchmark,
                          &contender) == 0);
    _wait_for_begin_conflict(&contender, 2U);
    atomic_store(&s_release_fps_reset, true);
    assert(pthread_join(owner_thread, NULL) == 0);
    assert(pthread_join(contender_thread, NULL) == 0);

    assert(owner.result == ESP_FAIL);
    assert(contender.result == ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_fps_reset_calls) == 1U);
    atomic_store(&s_block_fps_reset, false);
    atomic_store(&s_fps_reset_result, ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(32U, 31000U) ==
           ESP_OK);
    assert(atomic_load(&s_fps_reset_calls) == 2U);
    app_manager_display_benchmark_report_t report;
    assert(app_manager_display_diagnostics_end_benchmark(&report) == ESP_OK);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

static void _test_start_failure_rolls_back_fps(void)
{
    _reset();
    host_display_diagnostics_port_fail_next_create();
    assert(app_manager_display_diagnostics_start(&s_display) ==
           ESP_ERR_NO_MEM);
    assert(host_display_diagnostics_port_create_count() == 0U);
    assert(host_display_diagnostics_port_delete_count() == 0U);
    assert(atomic_load(&s_fps_enable_calls) == 2U);
    assert(s_events[0] == DISPLAY_DIAGNOSTICS_EVENT_FPS_ENABLE);
    assert(s_events[1] == DISPLAY_DIAGNOSTICS_EVENT_FPS_DISABLE);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);

    _reset();
    atomic_store(&s_fail_event_add, true);
    assert(app_manager_display_diagnostics_start(&s_display) ==
           ESP_ERR_NO_MEM);
    assert(host_display_diagnostics_port_create_count() == 1U);
    assert(host_display_diagnostics_port_delete_count() == 1U);
    assert(atomic_load(&s_event_add_calls) == 1U);
    assert(atomic_load(&s_event_remove_calls) == 0U);
    assert(atomic_load(&s_fps_enable_calls) == 2U);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_OK);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);

    _reset();
    atomic_store(&s_fps_enable_result, ESP_FAIL);
    assert(app_manager_display_diagnostics_start(&s_display) == ESP_FAIL);
    assert(host_display_diagnostics_port_create_count() == 0U);
    assert(atomic_load(&s_fps_enable_calls) == 1U);
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
}

int main(void)
{
    _test_start_semaphore_failures_cleanup_and_retry();
    _test_start_sample_and_stop();
    _test_passing_benchmark();
    _test_snapshot_prepare_metrics_are_informational();
    _test_memory_metrics_reset_between_benchmarks();
#if TEST_RENDER_TASK_EXPECTED
    _test_missing_render_task_is_reported();
#endif
    _test_completion_waits_for_successful_final_submit();
    _test_presentation_wait_blocks_until_final_submit();
    _test_presentation_wait_times_out_without_final_submit();
    _test_missing_final_submit_cannot_pass();
    _test_spans_do_not_cross_benchmark_generation();
    _test_sampling_uses_absolute_benchmark_deadlines();
    _test_fps_read_error_classification_and_lock_wait();
    _test_histogram_ranges_and_percentile_clamp();
    _test_low_fps_is_informational();
    _test_low_dma_fails_benchmark();
    _test_submit_failure_fails_benchmark();
    _test_effect_attribution_and_cancellation();
    _test_stop_lock_timeout_is_retryable();
    _test_repeated_lvgl_lock_timeouts_are_recoverable();
    _test_benchmark_reset_lock_timeout_is_recoverable();
    _test_sampling_does_not_hold_benchmark_lock_while_waiting_for_lvgl();
    _test_stale_sample_does_not_commit_to_next_benchmark();
    _test_concurrent_begin_has_single_reset_owner();
    _test_failed_begin_releases_single_reset_owner();
    _test_start_failure_rolls_back_fps();
    return 0;
}
