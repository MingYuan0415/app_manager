#define DBG_TAG "display_diag"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_manager_display_diagnostics_priv.h"

#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#define APP_MANAGER_DISPLAY_DIAGNOSTICS_TASK_STACK      3072U
#define APP_MANAGER_DISPLAY_DIAGNOSTICS_TASK_PRIORITY   1U
#define APP_MANAGER_DISPLAY_DIAGNOSTICS_LVGL_LOCK_MS    250
#ifndef APP_MANAGER_DISPLAY_DIAGNOSTICS_INTERVAL_MS
    #define APP_MANAGER_DISPLAY_DIAGNOSTICS_INTERVAL_MS 5000U
#endif
#ifndef APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_MS
    #define APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_MS 1000U
#endif
#define APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_US \
        ((int64_t)APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_MS * 1000LL)
#define APP_MANAGER_DISPLAY_DIAGNOSTICS_TASK_STACK_CAPS \
        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define APP_MANAGER_DISPLAY_DIAGNOSTICS_INTERNAL_CAPS \
        (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_MANAGER_DISPLAY_DIAGNOSTICS_DMA_CAPS \
        (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_MANAGER_DISPLAY_DIAGNOSTICS_PSRAM_CAPS \
        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Keep the histogram compact because diagnostics run during low-internal-memory
 * stress tests. Boundaries are dense around the 30/41/50 ms acceptance bands. */
static const uint32_t s_interval_bucket_limits_us[] =
{
    2000U, 4000U, 6000U, 8000U, 10000U, 12000U, 14000U, 16000U,
    18000U, 20000U, 22000U, 24000U, 26000U, 28000U, 30000U,
    31000U, 32000U, 33000U, APP_MANAGER_DISPLAY_INTERVAL_TARGET_US,
    34000U, 35000U, 36000U, 37000U, 38000U, 39000U, 40000U, 41000U,
    APP_MANAGER_DISPLAY_INTERVAL_LONG_US,
    42000U, 43000U, 44000U, 45000U, 46000U, 47000U, 48000U, 49000U,
    50000U, 52000U, 54000U, 56000U, 58000U, 60000U, 62000U, 64000U,
    66000U, 66667U, 68000U, 70000U, 75000U, 80000U, 85000U, 90000U,
    95000U, 100000U, 105000U, 110000U, 115000U, 120000U, 125000U,
    130000U, 135000U, 140000U, 145000U, 150000U, 155000U, 160000U,
    165000U, 170000U, 175000U, 180000U, 185000U, 190000U, 195000U,
    200000U, UINT32_MAX,
};

#define APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT \
    (sizeof(s_interval_bucket_limits_us) / \
     sizeof(s_interval_bucket_limits_us[0]))

typedef struct app_manager_display_timing_metrics
{
    atomic_uint count;
    atomic_uint_fast64_t total_us;
    atomic_uint maximum_us;
} app_manager_display_timing_metrics_t;

typedef struct app_manager_display_benchmark_metrics
{
    atomic_uint frame_submit_count;
    atomic_uint panel_submit_count;
    atomic_uint_fast64_t panel_pixel_count;
    atomic_uint submit_failure_count;
    atomic_uint_fast64_t submit_wait_us;
    atomic_uint maximum_submit_wait_us;
    atomic_uint transition_start_count;
    atomic_uint transition_complete_count;
    atomic_uint transition_cancel_count;
    atomic_uint snapshot_prepare_count;
    atomic_uint_fast64_t snapshot_prepare_us;
    atomic_uint maximum_snapshot_prepare_us;
    atomic_uint snapshot_fallback_count;
    atomic_uint snapshot_prepare_histogram[
        APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT];
    atomic_uint active_frame_count;
    atomic_uint_fast64_t active_duration_us;
    atomic_uint interval_count;
    atomic_uint interval_within_target_count;
    atomic_uint maximum_consecutive_long_intervals;
    atomic_uint maximum_interval_us;
    app_manager_display_timing_metrics_t render;
    app_manager_display_timing_metrics_t flush;
    atomic_uint_fast64_t flush_pixel_count;
    app_manager_display_timing_metrics_t flush_wait;
    atomic_uint interval_histogram[APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT];
} app_manager_display_benchmark_metrics_t;

typedef struct app_manager_display_event_span
{
    atomic_int_fast64_t start_us;
    atomic_int effect_index;
    atomic_uint benchmark_generation;
    atomic_uint_fast64_t pixel_count;
} app_manager_display_event_span_t;

static TaskHandle_t s_task;
static SemaphoreHandle_t s_task_stopped;
static SemaphoreHandle_t s_benchmark_lock;
static lv_display_t *s_display;
static bool s_event_registered;
static atomic_bool s_stopping = true;
static atomic_uint s_api_user_count;
static atomic_uint s_benchmark_writer_count;
static atomic_bool s_stop_requested;
static atomic_bool s_benchmark_active;
static atomic_bool s_benchmark_starting;
static atomic_uint s_benchmark_generation;
static atomic_int_fast64_t s_benchmark_start_us;
static atomic_uint s_sample_count;
static atomic_uint s_sample_error_count;
static atomic_uint s_lvgl_lock_error_count;
static atomic_uint s_fps_read_error_count;
static atomic_uint s_maximum_fps_lock_wait_us;
static atomic_uint s_minimum_fps;
static atomic_uint s_fps_failure_count;
static atomic_size_t s_minimum_internal_free;
static atomic_size_t s_minimum_internal_largest;
static atomic_size_t s_minimum_dma_free;
static atomic_size_t s_minimum_dma_largest;
static atomic_size_t s_minimum_psram_free;
static atomic_size_t s_minimum_psram_largest;
static atomic_uint s_dma_failure_count;
static atomic_uint s_minimum_fps_target;
static atomic_size_t s_minimum_dma_largest_target;
static app_manager_display_benchmark_metrics_t s_total_metrics;
static app_manager_display_benchmark_metrics_t s_effect_metrics[
    APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT];
static atomic_bool s_transition_active;
static atomic_bool s_transition_presentation_pending;
static atomic_int s_transition_effect_index;
static atomic_int_fast64_t s_transition_start_us;
static atomic_int_fast64_t s_last_final_submit_us;
static atomic_uint s_consecutive_long_intervals;
static app_manager_display_event_span_t s_render_span;
static app_manager_display_event_span_t s_flush_span;
static app_manager_display_event_span_t s_flush_wait_span;
static app_manager_display_event_span_t s_snapshot_prepare_span;

static bool _app_manager_display_diagnostics_api_enter(void)
{
    if (atomic_load_explicit(&s_stopping, memory_order_acquire))
    {
        return false;
    }
    atomic_fetch_add_explicit(&s_api_user_count, 1U, memory_order_acq_rel);
    if (!atomic_load_explicit(&s_stopping, memory_order_acquire))
    {
        return true;
    }
    atomic_fetch_sub_explicit(&s_api_user_count, 1U, memory_order_release);
    return false;
}

static void _app_manager_display_diagnostics_api_exit(void)
{
    atomic_fetch_sub_explicit(&s_api_user_count, 1U, memory_order_release);
}

static bool _app_manager_display_diagnostics_writer_enter(void)
{
    if (!atomic_load_explicit(&s_benchmark_active, memory_order_acquire) ||
            atomic_load_explicit(&s_stopping, memory_order_acquire))
    {
        return false;
    }
    atomic_fetch_add_explicit(&s_benchmark_writer_count, 1U,
                              memory_order_acq_rel);
    if (atomic_load_explicit(&s_benchmark_active, memory_order_acquire) &&
            !atomic_load_explicit(&s_stopping, memory_order_acquire))
    {
        return true;
    }
    atomic_fetch_sub_explicit(&s_benchmark_writer_count, 1U,
                              memory_order_release);
    return false;
}

static void _app_manager_display_diagnostics_writer_exit(void)
{
    atomic_fetch_sub_explicit(&s_benchmark_writer_count, 1U,
                              memory_order_release);
}

static int _app_manager_display_diagnostics_effect_index(
    app_manager_transition_effect_t effect)
{
    if (effect < APP_MANAGER_TRANSITION_NONE ||
            effect >= APP_MANAGER_TRANSITION_END)
    {
        return -1;
    }
    return (int)effect - (int)APP_MANAGER_TRANSITION_NONE;
}

static app_manager_display_benchmark_metrics_t *
_app_manager_display_diagnostics_active_effect(void)
{
    if (!atomic_load_explicit(&s_transition_active, memory_order_acquire))
    {
        return NULL;
    }
    const int effect_index = atomic_load_explicit(&s_transition_effect_index,
                             memory_order_relaxed);
    if (effect_index < 0 ||
            effect_index >= APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT)
    {
        return NULL;
    }
    return &s_effect_metrics[effect_index];
}

static void _app_manager_display_diagnostics_update_minimum_u32(
    atomic_uint *value, uint32_t candidate)
{
    uint32_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate < current &&
            !atomic_compare_exchange_weak_explicit(
                value, &current, candidate, memory_order_relaxed,
                memory_order_relaxed))
    {
    }
}

static void _app_manager_display_diagnostics_update_minimum_size(
    atomic_size_t *value, size_t candidate)
{
    size_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate < current &&
            !atomic_compare_exchange_weak_explicit(
                value, &current, candidate, memory_order_relaxed,
                memory_order_relaxed))
    {
    }
}

static void _app_manager_display_diagnostics_update_maximum_u32(
    atomic_uint *value, uint32_t candidate)
{
    uint32_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate > current &&
            !atomic_compare_exchange_weak_explicit(
                value, &current, candidate, memory_order_relaxed,
                memory_order_relaxed))
    {
    }
}

static void _app_manager_display_diagnostics_record_timing(
    app_manager_display_timing_metrics_t *timing, uint32_t elapsed_us,
    bool increment_count)
{
    if (increment_count)
    {
        atomic_fetch_add_explicit(&timing->count, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&timing->total_us, elapsed_us,
                              memory_order_relaxed);
    _app_manager_display_diagnostics_update_maximum_u32(
        &timing->maximum_us, elapsed_us);
}

static size_t _app_manager_display_diagnostics_interval_bucket(
    uint32_t interval_us)
{
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT; ++index)
    {
        if (interval_us <= s_interval_bucket_limits_us[index])
        {
            return index;
        }
    }
    return APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT - 1U;
}

static void _app_manager_display_diagnostics_record_interval(
    app_manager_display_benchmark_metrics_t *metrics, uint32_t interval_us,
    uint32_t consecutive_long_intervals)
{
    atomic_fetch_add_explicit(&metrics->interval_count, 1U,
                              memory_order_relaxed);
    if (interval_us <= APP_MANAGER_DISPLAY_INTERVAL_TARGET_US)
    {
        atomic_fetch_add_explicit(&metrics->interval_within_target_count, 1U,
                                  memory_order_relaxed);
    }
    _app_manager_display_diagnostics_update_maximum_u32(
        &metrics->maximum_consecutive_long_intervals,
        consecutive_long_intervals);
    _app_manager_display_diagnostics_update_maximum_u32(
        &metrics->maximum_interval_us, interval_us);
    const size_t bucket = _app_manager_display_diagnostics_interval_bucket(
                              interval_us);
    atomic_fetch_add_explicit(&metrics->interval_histogram[bucket], 1U,
                              memory_order_relaxed);
}

static void _app_manager_display_diagnostics_record_snapshot_prepare(
    app_manager_display_benchmark_metrics_t *metrics, uint32_t elapsed_us,
    bool fallback)
{
    atomic_fetch_add_explicit(&metrics->snapshot_prepare_count, 1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&metrics->snapshot_prepare_us, elapsed_us,
                              memory_order_relaxed);
    _app_manager_display_diagnostics_update_maximum_u32(
        &metrics->maximum_snapshot_prepare_us, elapsed_us);
    if (fallback)
    {
        atomic_fetch_add_explicit(&metrics->snapshot_fallback_count, 1U,
                                  memory_order_relaxed);
    }
    const size_t bucket = _app_manager_display_diagnostics_interval_bucket(
                              elapsed_us);
    atomic_fetch_add_explicit(&metrics->snapshot_prepare_histogram[bucket],
                              1U, memory_order_relaxed);
}

static void _app_manager_display_diagnostics_reset_timing(
    app_manager_display_timing_metrics_t *timing)
{
    atomic_store_explicit(&timing->count, 0U, memory_order_relaxed);
    atomic_store_explicit(&timing->total_us, 0U, memory_order_relaxed);
    atomic_store_explicit(&timing->maximum_us, 0U, memory_order_relaxed);
}

static void _app_manager_display_diagnostics_reset_metrics(
    app_manager_display_benchmark_metrics_t *metrics)
{
    atomic_store_explicit(&metrics->frame_submit_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->panel_submit_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->panel_pixel_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->submit_failure_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->submit_wait_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->maximum_submit_wait_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->transition_start_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->transition_complete_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->transition_cancel_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->snapshot_prepare_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->snapshot_prepare_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->maximum_snapshot_prepare_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->snapshot_fallback_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->active_frame_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->active_duration_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->interval_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->interval_within_target_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->maximum_consecutive_long_intervals, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&metrics->maximum_interval_us, 0U,
                          memory_order_relaxed);
    _app_manager_display_diagnostics_reset_timing(&metrics->render);
    _app_manager_display_diagnostics_reset_timing(&metrics->flush);
    atomic_store_explicit(&metrics->flush_pixel_count, 0U,
                          memory_order_relaxed);
    _app_manager_display_diagnostics_reset_timing(&metrics->flush_wait);
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT; ++index)
    {
        atomic_store_explicit(&metrics->interval_histogram[index], 0U,
                              memory_order_relaxed);
        atomic_store_explicit(&metrics->snapshot_prepare_histogram[index], 0U,
                              memory_order_relaxed);
    }
}

static void _app_manager_display_diagnostics_reset_span(
    app_manager_display_event_span_t *span)
{
    atomic_store_explicit(&span->start_us, 0, memory_order_relaxed);
    atomic_store_explicit(&span->effect_index, -1, memory_order_relaxed);
    atomic_store_explicit(&span->benchmark_generation, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&span->pixel_count, 0U, memory_order_relaxed);
}

static void _app_manager_display_diagnostics_reset_spans(void)
{
    _app_manager_display_diagnostics_reset_span(&s_render_span);
    _app_manager_display_diagnostics_reset_span(&s_flush_span);
    _app_manager_display_diagnostics_reset_span(&s_flush_wait_span);
    _app_manager_display_diagnostics_reset_span(&s_snapshot_prepare_span);
}

static void _app_manager_display_diagnostics_reset_benchmark(void)
{
    atomic_store_explicit(&s_sample_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_sample_error_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_lvgl_lock_error_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_fps_read_error_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_maximum_fps_lock_wait_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_fps, UINT_MAX, memory_order_relaxed);
    atomic_store_explicit(&s_fps_failure_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_minimum_internal_free, SIZE_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_internal_largest, SIZE_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_dma_free, SIZE_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_dma_largest, SIZE_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_psram_free, SIZE_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_psram_largest, SIZE_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&s_dma_failure_count, 0U, memory_order_relaxed);
    _app_manager_display_diagnostics_reset_metrics(&s_total_metrics);
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT; ++index)
    {
        _app_manager_display_diagnostics_reset_metrics(
            &s_effect_metrics[index]);
    }
    atomic_store_explicit(&s_transition_active, false, memory_order_relaxed);
    atomic_store_explicit(&s_transition_presentation_pending, false,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_effect_index, -1,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_start_us, 0, memory_order_relaxed);
    atomic_store_explicit(&s_last_final_submit_us, 0, memory_order_relaxed);
    atomic_store_explicit(&s_consecutive_long_intervals, 0U,
                          memory_order_relaxed);
    _app_manager_display_diagnostics_reset_spans();
}

typedef enum app_manager_display_timing_kind
{
    APP_MANAGER_DISPLAY_TIMING_RENDER = 0,
    APP_MANAGER_DISPLAY_TIMING_FLUSH,
    APP_MANAGER_DISPLAY_TIMING_FLUSH_WAIT,
} app_manager_display_timing_kind_t;

static app_manager_display_timing_metrics_t *
_app_manager_display_diagnostics_timing(
    app_manager_display_benchmark_metrics_t *metrics,
    app_manager_display_timing_kind_t kind)
{
    switch (kind)
    {
    case APP_MANAGER_DISPLAY_TIMING_RENDER:
        return &metrics->render;
    case APP_MANAGER_DISPLAY_TIMING_FLUSH:
        return &metrics->flush;
    case APP_MANAGER_DISPLAY_TIMING_FLUSH_WAIT:
        return &metrics->flush_wait;
    default:
        return NULL;
    }
}

static int _app_manager_display_diagnostics_active_effect_index(void)
{
    if (!atomic_load_explicit(&s_transition_active, memory_order_acquire))
    {
        return -1;
    }
    const int effect_index = atomic_load_explicit(&s_transition_effect_index,
                             memory_order_relaxed);
    return effect_index >= 0 &&
           effect_index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT ?
           effect_index : -1;
}

static uint32_t _app_manager_display_diagnostics_elapsed_us(
    int64_t start_us, int64_t finish_us)
{
    if (finish_us <= start_us)
    {
        return 0U;
    }
    const uint64_t elapsed_us = (uint64_t)(finish_us - start_us);
    return elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
}

static void _app_manager_display_diagnostics_span_start(
    app_manager_display_event_span_t *span, uint64_t pixel_count)
{
    atomic_store_explicit(&span->effect_index,
                          _app_manager_display_diagnostics_active_effect_index(),
                          memory_order_relaxed);
    atomic_store_explicit(&span->benchmark_generation,
                          atomic_load_explicit(&s_benchmark_generation,
                                  memory_order_relaxed),
                          memory_order_relaxed);
    atomic_store_explicit(&span->pixel_count, pixel_count,
                          memory_order_relaxed);
    atomic_store_explicit(&span->start_us, esp_timer_get_time(),
                          memory_order_release);
}

static void _app_manager_display_diagnostics_span_finish(
    app_manager_display_event_span_t *span,
    app_manager_display_timing_kind_t kind)
{
    const int64_t start_us = atomic_exchange_explicit(&span->start_us, 0,
                             memory_order_acq_rel);
    if (start_us == 0)
    {
        return;
    }
    const uint32_t generation = atomic_load_explicit(
                                    &span->benchmark_generation,
                                    memory_order_relaxed);
    if (generation == 0U || generation != atomic_load_explicit(
                &s_benchmark_generation, memory_order_acquire))
    {
        return;
    }
    const uint32_t elapsed_us = _app_manager_display_diagnostics_elapsed_us(
                                    start_us, esp_timer_get_time());
    _app_manager_display_diagnostics_record_timing(
        _app_manager_display_diagnostics_timing(&s_total_metrics, kind),
        elapsed_us, true);
    const uint64_t pixel_count = atomic_load_explicit(&span->pixel_count,
                                 memory_order_relaxed);
    if (kind == APP_MANAGER_DISPLAY_TIMING_FLUSH)
    {
        atomic_fetch_add_explicit(&s_total_metrics.flush_pixel_count,
                                  pixel_count, memory_order_relaxed);
    }

    const int effect_index = atomic_load_explicit(&span->effect_index,
                             memory_order_relaxed);
    if (effect_index >= 0 &&
            effect_index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT)
    {
        _app_manager_display_diagnostics_record_timing(
            _app_manager_display_diagnostics_timing(
                &s_effect_metrics[effect_index], kind),
            elapsed_us, true);
        if (kind == APP_MANAGER_DISPLAY_TIMING_FLUSH)
        {
            atomic_fetch_add_explicit(
                &s_effect_metrics[effect_index].flush_pixel_count,
                pixel_count, memory_order_relaxed);
        }
    }
}

static uint64_t _app_manager_display_diagnostics_area_pixels(
    const lv_area_t *area)
{
    if (area == NULL || area->x2 < area->x1 || area->y2 < area->y1)
    {
        return 0U;
    }
    return (uint64_t)((int64_t)area->x2 - area->x1 + 1) *
           (uint64_t)((int64_t)area->y2 - area->y1 + 1);
}

static void _app_manager_display_diagnostics_display_event_cb(
    lv_event_t *event)
{
    if (!_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }

    switch (lv_event_get_code(event))
    {
    case LV_EVENT_RENDER_START:
        _app_manager_display_diagnostics_span_start(&s_render_span, 0U);
        break;
    case LV_EVENT_RENDER_READY:
        _app_manager_display_diagnostics_span_finish(
            &s_render_span, APP_MANAGER_DISPLAY_TIMING_RENDER);
        break;
    case LV_EVENT_FLUSH_START:
    {
        const uint64_t pixel_count =
            _app_manager_display_diagnostics_area_pixels(
                lv_event_get_param(event));
        _app_manager_display_diagnostics_span_start(&s_flush_span,
                pixel_count);
        break;
    }
    case LV_EVENT_FLUSH_FINISH:
        _app_manager_display_diagnostics_span_finish(
            &s_flush_span, APP_MANAGER_DISPLAY_TIMING_FLUSH);
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        _app_manager_display_diagnostics_span_start(&s_flush_wait_span, 0U);
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        _app_manager_display_diagnostics_span_finish(
            &s_flush_wait_span, APP_MANAGER_DISPLAY_TIMING_FLUSH_WAIT);
        break;
    default:
        break;
    }
    _app_manager_display_diagnostics_writer_exit();
}

static void _app_manager_display_diagnostics_finish_transition(
    bool cancelled, int64_t finish_us)
{
    if (!atomic_exchange_explicit(&s_transition_active, false,
                                  memory_order_acq_rel))
    {
        return;
    }
    const int effect_index = atomic_load_explicit(&s_transition_effect_index,
                             memory_order_relaxed);
    const int64_t start_us = atomic_load_explicit(&s_transition_start_us,
                             memory_order_relaxed);
    const uint32_t elapsed_us = _app_manager_display_diagnostics_elapsed_us(
                                    start_us, finish_us);
    atomic_uint *total_count = cancelled ?
                               &s_total_metrics.transition_cancel_count :
                               &s_total_metrics.transition_complete_count;
    atomic_fetch_add_explicit(total_count, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_total_metrics.active_duration_us, elapsed_us,
                              memory_order_relaxed);
    if (effect_index >= 0 &&
            effect_index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT)
    {
        app_manager_display_benchmark_metrics_t *effect_metrics =
            &s_effect_metrics[effect_index];
        atomic_uint *effect_count = cancelled ?
                                    &effect_metrics->transition_cancel_count :
                                    &effect_metrics->transition_complete_count;
        atomic_fetch_add_explicit(effect_count, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&effect_metrics->active_duration_us,
                                  elapsed_us, memory_order_relaxed);
    }
    atomic_store_explicit(&s_transition_effect_index, -1,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_presentation_pending, false,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_start_us, 0, memory_order_relaxed);
    atomic_store_explicit(&s_last_final_submit_us, 0, memory_order_relaxed);
    atomic_store_explicit(&s_consecutive_long_intervals, 0U,
                          memory_order_relaxed);
}

void app_manager_display_diagnostics_snapshot_prepare_started(
    app_manager_transition_effect_t effect)
{
    const int effect_index = _app_manager_display_diagnostics_effect_index(
                                 effect);
    if (effect_index < 0 ||
            !_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }
    atomic_store_explicit(&s_snapshot_prepare_span.effect_index, effect_index,
                          memory_order_relaxed);
    atomic_store_explicit(&s_snapshot_prepare_span.benchmark_generation,
                          atomic_load_explicit(&s_benchmark_generation,
                                  memory_order_relaxed),
                          memory_order_relaxed);
    atomic_store_explicit(&s_snapshot_prepare_span.start_us,
                          esp_timer_get_time(), memory_order_release);
    _app_manager_display_diagnostics_writer_exit();
}

void app_manager_display_diagnostics_snapshot_prepare_finished(bool fallback)
{
    if (!_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }
    const int64_t start_us = atomic_exchange_explicit(
                                 &s_snapshot_prepare_span.start_us, 0,
                                 memory_order_acq_rel);
    if (start_us != 0)
    {
        const uint32_t generation = atomic_load_explicit(
                                        &s_snapshot_prepare_span.benchmark_generation,
                                        memory_order_relaxed);
        if (generation != 0U && generation == atomic_load_explicit(
                    &s_benchmark_generation, memory_order_acquire))
        {
            const uint32_t elapsed_us =
                _app_manager_display_diagnostics_elapsed_us(start_us,
                    esp_timer_get_time());
            _app_manager_display_diagnostics_record_snapshot_prepare(
                &s_total_metrics, elapsed_us, fallback);
            const int effect_index = atomic_load_explicit(
                                         &s_snapshot_prepare_span.effect_index,
                                         memory_order_relaxed);
            if (effect_index >= 0 &&
                    effect_index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT)
            {
                _app_manager_display_diagnostics_record_snapshot_prepare(
                    &s_effect_metrics[effect_index], elapsed_us, fallback);
            }
        }
    }
    _app_manager_display_diagnostics_writer_exit();
}

void app_manager_display_diagnostics_transition_started(
    app_manager_transition_effect_t effect)
{
    const int effect_index = _app_manager_display_diagnostics_effect_index(
                                 effect);
    if (effect_index < 0 ||
            !_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }
    const int64_t start_us = esp_timer_get_time();
    if (atomic_load_explicit(&s_transition_active, memory_order_acquire))
    {
        _app_manager_display_diagnostics_finish_transition(true, start_us);
    }
    atomic_store_explicit(&s_transition_effect_index, effect_index,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_start_us, start_us,
                          memory_order_relaxed);
    atomic_store_explicit(&s_last_final_submit_us, start_us,
                          memory_order_relaxed);
    atomic_store_explicit(&s_consecutive_long_intervals, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_presentation_pending, false,
                          memory_order_relaxed);
    atomic_store_explicit(&s_transition_active, true, memory_order_release);
    atomic_fetch_add_explicit(&s_total_metrics.transition_start_count, 1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(
        &s_effect_metrics[effect_index].transition_start_count, 1U,
        memory_order_relaxed);
    _app_manager_display_diagnostics_writer_exit();
}

void app_manager_display_diagnostics_transition_completed(void)
{
    if (!_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }
    if (atomic_load_explicit(&s_transition_active, memory_order_acquire))
    {
        atomic_store_explicit(&s_transition_presentation_pending, true,
                              memory_order_release);
    }
    _app_manager_display_diagnostics_writer_exit();
}

void app_manager_display_diagnostics_transition_cancelled(void)
{
    if (!_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }
    _app_manager_display_diagnostics_finish_transition(true,
            esp_timer_get_time());
    _app_manager_display_diagnostics_writer_exit();
}

esp_err_t app_manager_display_diagnostics_wait_for_presentation(
    uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_app_manager_display_diagnostics_api_enter())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)timeout_ms * 1000LL;
    while (atomic_load_explicit(&s_transition_active, memory_order_acquire))
    {
        if (atomic_load_explicit(&s_stopping, memory_order_acquire))
        {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (esp_timer_get_time() >= deadline_us)
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    _app_manager_display_diagnostics_api_exit();
    return result;
}

static void _app_manager_display_diagnostics_sample(lv_display_t *display,
        bool benchmark_requested, uint32_t benchmark_generation)
{
    uint32_t fps = 0;
    const int64_t lock_start_us = esp_timer_get_time();
    const esp_err_t lock_result = esp_lv_adapter_lock(
                                      APP_MANAGER_DISPLAY_DIAGNOSTICS_LVGL_LOCK_MS);
    const uint32_t lock_wait_us =
        _app_manager_display_diagnostics_elapsed_us(lock_start_us,
            esp_timer_get_time());
    esp_err_t fps_result = lock_result;
    /* Flush callbacks record statistics while holding the LVGL lock. Keep
     * every LVGL wait outside s_benchmark_lock to preserve lock order. */
    if (lock_result == ESP_OK)
    {
        fps_result = esp_lv_adapter_get_fps(display, &fps);
        esp_lv_adapter_unlock();
    }
    const size_t internal_free = heap_caps_get_free_size(
                                     APP_MANAGER_DISPLAY_DIAGNOSTICS_INTERNAL_CAPS);
    const size_t internal_largest = heap_caps_get_largest_free_block(
                                        APP_MANAGER_DISPLAY_DIAGNOSTICS_INTERNAL_CAPS);
    const size_t dma_free = heap_caps_get_free_size(
                                APP_MANAGER_DISPLAY_DIAGNOSTICS_DMA_CAPS);
    const size_t dma_largest = heap_caps_get_largest_free_block(
                                   APP_MANAGER_DISPLAY_DIAGNOSTICS_DMA_CAPS);
    const size_t psram_free = heap_caps_get_free_size(
                                  APP_MANAGER_DISPLAY_DIAGNOSTICS_PSRAM_CAPS);
    const size_t psram_largest = heap_caps_get_largest_free_block(
                                     APP_MANAGER_DISPLAY_DIAGNOSTICS_PSRAM_CAPS);
    bool benchmark_committed = false;
    if (benchmark_requested &&
            xSemaphoreTake(s_benchmark_lock, portMAX_DELAY) == pdPASS)
    {
        if (atomic_load_explicit(&s_benchmark_active, memory_order_acquire) &&
                benchmark_generation == atomic_load_explicit(
                    &s_benchmark_generation, memory_order_relaxed))
        {
            benchmark_committed = true;
            atomic_fetch_add_explicit(&s_sample_count, 1U,
                                      memory_order_relaxed);
            _app_manager_display_diagnostics_update_maximum_u32(
                &s_maximum_fps_lock_wait_us, lock_wait_us);
            if (lock_result != ESP_OK)
            {
                atomic_fetch_add_explicit(&s_sample_error_count, 1U,
                                          memory_order_relaxed);
                atomic_fetch_add_explicit(&s_lvgl_lock_error_count, 1U,
                                          memory_order_relaxed);
            }
            else if (fps_result != ESP_OK)
            {
                atomic_fetch_add_explicit(&s_sample_error_count, 1U,
                                          memory_order_relaxed);
                atomic_fetch_add_explicit(&s_fps_read_error_count, 1U,
                                          memory_order_relaxed);
            }
            else
            {
                _app_manager_display_diagnostics_update_minimum_u32(
                    &s_minimum_fps, fps);
                if (fps < atomic_load_explicit(&s_minimum_fps_target,
                                               memory_order_relaxed))
                {
                    atomic_fetch_add_explicit(&s_fps_failure_count, 1U,
                                              memory_order_relaxed);
                }
            }
            _app_manager_display_diagnostics_update_minimum_size(
                &s_minimum_internal_free, internal_free);
            _app_manager_display_diagnostics_update_minimum_size(
                &s_minimum_internal_largest, internal_largest);
            _app_manager_display_diagnostics_update_minimum_size(
                &s_minimum_dma_free, dma_free);
            _app_manager_display_diagnostics_update_minimum_size(
                &s_minimum_dma_largest, dma_largest);
            _app_manager_display_diagnostics_update_minimum_size(
                &s_minimum_psram_free, psram_free);
            _app_manager_display_diagnostics_update_minimum_size(
                &s_minimum_psram_largest, psram_largest);
            if (dma_largest < atomic_load_explicit(
                        &s_minimum_dma_largest_target,
                        memory_order_relaxed))
            {
                atomic_fetch_add_explicit(&s_dma_failure_count, 1U,
                                          memory_order_relaxed);
            }
        }
        (void)xSemaphoreGive(s_benchmark_lock);
    }

    if (lock_result != ESP_OK)
    {
        LOG_W("LVGL lock sample failed: 0x%x", (unsigned)lock_result);
    }
    else if (fps_result != ESP_OK)
    {
        LOG_W("FPS read failed: 0x%x", (unsigned)fps_result);
    }
    if (benchmark_committed)
    {
        LOG_I("benchmark fps=%u lock_result=0x%x fps_result=0x%x lock_wait_us=%u internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u",
              (unsigned)fps, (unsigned)lock_result, (unsigned)fps_result,
              (unsigned)lock_wait_us,
              (unsigned)internal_free, (unsigned)internal_largest,
              (unsigned)dma_free, (unsigned)dma_largest,
              (unsigned)psram_free, (unsigned)psram_largest);
    }
    else if (!benchmark_requested)
    {
        LOG_I("fps=%u lock_result=0x%x fps_result=0x%x lock_wait_us=%u internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u",
              (unsigned)fps, (unsigned)lock_result, (unsigned)fps_result,
              (unsigned)lock_wait_us,
              (unsigned)internal_free, (unsigned)internal_largest,
              (unsigned)dma_free,
              (unsigned)dma_largest,
              (unsigned)psram_free, (unsigned)psram_largest);
    }
    (void)fps;
    (void)internal_free;
    (void)internal_largest;
    (void)dma_free;
    (void)dma_largest;
    (void)psram_free;
    (void)psram_largest;
}

static bool _app_manager_display_diagnostics_benchmark_snapshot(
    uint32_t *generation, int64_t *start_us)
{
    bool active = false;
    if (xSemaphoreTake(s_benchmark_lock, portMAX_DELAY) == pdPASS)
    {
        active = atomic_load_explicit(&s_benchmark_active,
                                      memory_order_relaxed);
        *generation = atomic_load_explicit(&s_benchmark_generation,
                                           memory_order_relaxed);
        *start_us = atomic_load_explicit(&s_benchmark_start_us,
                                         memory_order_relaxed);
        (void)xSemaphoreGive(s_benchmark_lock);
    }
    return active;
}

static void _app_manager_display_diagnostics_read_render_task(
    app_manager_display_benchmark_report_t *report)
{
#if CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT == 1
#if defined(CONFIG_LV_OS_NONE) && CONFIG_LV_OS_NONE
    const char *task_name = "lvgl";
#else
    const char *task_name = "swdraw";
#endif
    TaskHandle_t render_task = xTaskGetHandle(task_name);
    if (render_task == NULL)
    {
        return;
    }
    report->render_task_found = true;
    const BaseType_t core_id = xTaskGetCoreID(render_task);
    report->render_task_core_id =
        core_id == 0 || core_id == 1 ? (int8_t)core_id : -1;
    report->minimum_render_stack_high_water =
        (uint32_t)uxTaskGetStackHighWaterMark(render_task);
    const StackType_t *stack_start = xTaskGetStackStart(render_task);
    report->render_task_stack_in_psram = stack_start != NULL &&
                                         esp_ptr_external_ram(stack_start);
#else
    (void)report;
#endif
}

static TickType_t _app_manager_display_diagnostics_wait_ticks(
    int64_t deadline_us, int64_t now_us)
{
    const uint64_t remaining_us = deadline_us > now_us ?
                                  (uint64_t)(deadline_us - now_us) : 1U;
    const uint32_t remaining_ms =
        (uint32_t)((remaining_us + 999U) / 1000U);
    const TickType_t ticks = pdMS_TO_TICKS(remaining_ms);
    return ticks == 0U ? 1U : ticks;
}

static int64_t _app_manager_display_diagnostics_next_deadline_us(
    int64_t start_us, int64_t now_us)
{
    if (now_us <= start_us)
    {
        return start_us + APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_US;
    }
    const uint64_t elapsed_us = (uint64_t)(now_us - start_us);
    const uint64_t period_count =
        elapsed_us / (uint64_t)APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_US + 1U;
    return start_us + (int64_t)(period_count *
                                (uint64_t)APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_US);
}

static void _app_manager_display_diagnostics_task(void *arg)
{
    lv_display_t *display = arg;
    uint32_t scheduled_generation = 0U;
    int64_t benchmark_deadline_us = 0;
    while (!atomic_load_explicit(&s_stop_requested, memory_order_acquire))
    {
        uint32_t benchmark_generation = 0U;
        int64_t benchmark_start_us = 0;
        const bool benchmark_active =
            _app_manager_display_diagnostics_benchmark_snapshot(
                &benchmark_generation, &benchmark_start_us);
        if (benchmark_active)
        {
            if (scheduled_generation != benchmark_generation)
            {
                scheduled_generation = benchmark_generation;
                benchmark_deadline_us = benchmark_start_us +
                                        APP_MANAGER_DISPLAY_BENCHMARK_INTERVAL_US;
            }
            const int64_t now_us = esp_timer_get_time();
            if (now_us < benchmark_deadline_us)
            {
                (void)ulTaskNotifyTake(
                    pdTRUE,
                    _app_manager_display_diagnostics_wait_ticks(
                        benchmark_deadline_us, now_us));
                continue;
            }
            _app_manager_display_diagnostics_sample(display, true,
                                                    benchmark_generation);
            benchmark_deadline_us =
                _app_manager_display_diagnostics_next_deadline_us(
                    benchmark_start_us, esp_timer_get_time());
            continue;
        }

        scheduled_generation = 0U;
        benchmark_deadline_us = 0;
        const uint32_t notification_count = ulTaskNotifyTake(
                                                pdTRUE,
                                                pdMS_TO_TICKS(
                                                        APP_MANAGER_DISPLAY_DIAGNOSTICS_INTERVAL_MS));
        if (atomic_load_explicit(&s_stop_requested, memory_order_acquire))
        {
            break;
        }
        if (notification_count == 0U &&
                !_app_manager_display_diagnostics_benchmark_snapshot(
                    &benchmark_generation, &benchmark_start_us))
        {
            _app_manager_display_diagnostics_sample(display, false, 0U);
        }
    }

    (void)xSemaphoreGive(s_task_stopped);
    vTaskSuspend(NULL);
}

static uint32_t _app_manager_display_diagnostics_histogram_percentile_us(
    const atomic_uint *histogram, uint32_t count, uint32_t maximum_us,
    uint32_t percentile)
{
    if (count == 0U)
    {
        return 0U;
    }
    const uint64_t rank = ((uint64_t)count * percentile + 99U) / 100U;
    uint64_t cumulative = 0U;
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_INTERVAL_BUCKET_COUNT; ++index)
    {
        cumulative += atomic_load_explicit(&histogram[index],
                                           memory_order_relaxed);
        if (cumulative >= rank)
        {
            const uint32_t boundary_us = s_interval_bucket_limits_us[index];
            return boundary_us == UINT32_MAX || boundary_us > maximum_us ?
                   maximum_us : boundary_us;
        }
    }
    return maximum_us;
}

static uint32_t _app_manager_display_diagnostics_interval_percentile_us(
    const app_manager_display_benchmark_metrics_t *metrics,
    uint32_t percentile)
{
    return _app_manager_display_diagnostics_histogram_percentile_us(
               metrics->interval_histogram,
               atomic_load_explicit(&metrics->interval_count,
                                    memory_order_relaxed),
               atomic_load_explicit(&metrics->maximum_interval_us,
                                    memory_order_relaxed),
               percentile);
}

static uint32_t _app_manager_display_diagnostics_snapshot_prepare_p95_us(
    const app_manager_display_benchmark_metrics_t *metrics)
{
    return _app_manager_display_diagnostics_histogram_percentile_us(
               metrics->snapshot_prepare_histogram,
               atomic_load_explicit(&metrics->snapshot_prepare_count,
                                    memory_order_relaxed),
               atomic_load_explicit(&metrics->maximum_snapshot_prepare_us,
                                    memory_order_relaxed),
               95U);
}

static void _app_manager_display_diagnostics_copy_metrics(
    const app_manager_display_benchmark_metrics_t *metrics,
    app_manager_display_effect_benchmark_report_t *report)
{
    report->transition_start_count = atomic_load_explicit(
                                         &metrics->transition_start_count,
                                         memory_order_relaxed);
    report->transition_complete_count = atomic_load_explicit(
                                            &metrics->transition_complete_count,
                                            memory_order_relaxed);
    report->transition_cancel_count = atomic_load_explicit(
                                          &metrics->transition_cancel_count,
                                          memory_order_relaxed);
    report->snapshot_prepare_count = atomic_load_explicit(
                                         &metrics->snapshot_prepare_count,
                                         memory_order_relaxed);
    report->snapshot_prepare_us = atomic_load_explicit(
                                      &metrics->snapshot_prepare_us,
                                      memory_order_relaxed);
    report->maximum_snapshot_prepare_us = atomic_load_explicit(
            &metrics->maximum_snapshot_prepare_us, memory_order_relaxed);
    report->snapshot_prepare_p95_us =
        _app_manager_display_diagnostics_snapshot_prepare_p95_us(metrics);
    report->snapshot_fallback_count = atomic_load_explicit(
                                          &metrics->snapshot_fallback_count,
                                          memory_order_relaxed);
    report->active_frame_count = atomic_load_explicit(
                                     &metrics->active_frame_count,
                                     memory_order_relaxed);
    report->active_duration_us = atomic_load_explicit(
                                     &metrics->active_duration_us,
                                     memory_order_relaxed);
    report->interval_count = atomic_load_explicit(&metrics->interval_count,
                             memory_order_relaxed);
    report->interval_within_target_count = atomic_load_explicit(
            &metrics->interval_within_target_count, memory_order_relaxed);
    report->maximum_consecutive_long_intervals = atomic_load_explicit(
            &metrics->maximum_consecutive_long_intervals, memory_order_relaxed);
    report->interval_p50_us =
        _app_manager_display_diagnostics_interval_percentile_us(metrics, 50U);
    report->interval_p95_us =
        _app_manager_display_diagnostics_interval_percentile_us(metrics, 95U);
    report->interval_p99_us =
        _app_manager_display_diagnostics_interval_percentile_us(metrics, 99U);
    report->maximum_interval_us = atomic_load_explicit(
                                      &metrics->maximum_interval_us,
                                      memory_order_relaxed);
    report->render_count = atomic_load_explicit(&metrics->render.count,
                           memory_order_relaxed);
    report->render_us = atomic_load_explicit(&metrics->render.total_us,
                        memory_order_relaxed);
    report->maximum_render_us = atomic_load_explicit(
                                    &metrics->render.maximum_us,
                                    memory_order_relaxed);
    report->flush_count = atomic_load_explicit(&metrics->flush.count,
                          memory_order_relaxed);
    report->flush_pixel_count = atomic_load_explicit(
                                    &metrics->flush_pixel_count,
                                    memory_order_relaxed);
    report->flush_us = atomic_load_explicit(&metrics->flush.total_us,
                                            memory_order_relaxed);
    report->maximum_flush_us = atomic_load_explicit(
                                   &metrics->flush.maximum_us,
                                   memory_order_relaxed);
    report->flush_wait_count = atomic_load_explicit(
                                   &metrics->flush_wait.count,
                                   memory_order_relaxed);
    report->flush_wait_us = atomic_load_explicit(&metrics->flush_wait.total_us,
                            memory_order_relaxed);
    report->maximum_flush_wait_us = atomic_load_explicit(
                                        &metrics->flush_wait.maximum_us,
                                        memory_order_relaxed);
    report->panel_submit_count = atomic_load_explicit(
                                     &metrics->panel_submit_count,
                                     memory_order_relaxed);
    report->panel_pixel_count = atomic_load_explicit(
                                    &metrics->panel_pixel_count,
                                    memory_order_relaxed);
    report->submit_failure_count = atomic_load_explicit(
                                       &metrics->submit_failure_count,
                                       memory_order_relaxed);
    report->submit_wait_us = atomic_load_explicit(&metrics->submit_wait_us,
                             memory_order_relaxed);
    report->maximum_submit_wait_us = atomic_load_explicit(
                                         &metrics->maximum_submit_wait_us,
                                         memory_order_relaxed);
}

static bool _app_manager_display_diagnostics_benchmark_passed(
    const app_manager_display_benchmark_report_t *report)
{
    return report->sample_count > 0U &&
           report->sample_error_count == 0U &&
           report->dma_failure_count == 0U &&
           report->submit_failure_count == 0U &&
           report->transition_start_count > 0U &&
           report->transition_start_count ==
           report->transition_complete_count &&
           report->transition_cancel_count == 0U &&
           report->active_frame_count > 0U &&
           report->active_duration_us > 0U &&
           report->interval_count > 0U &&
           report->render_count > 0U &&
           report->flush_count > 0U &&
           report->panel_submit_count > 0U;
}

esp_err_t app_manager_display_diagnostics_start(lv_display_t *display)
{
    if (display == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL || s_task_stopped != NULL || s_benchmark_lock != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_NO_MEM;
    bool fps_enabled = false;
    atomic_store_explicit(&s_stopping, true, memory_order_release);
    s_display = display;
    s_event_registered = false;
    s_task_stopped = xSemaphoreCreateBinary();
    if (s_task_stopped == NULL)
    {
        goto failed;
    }
    s_benchmark_lock = xSemaphoreCreateMutex();
    if (s_benchmark_lock == NULL)
    {
        goto failed;
    }

    result = esp_lv_adapter_fps_stats_enable(display, true);
    if (result != ESP_OK)
    {
        goto failed;
    }
    fps_enabled = true;
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_benchmark_active, false, memory_order_release);
    atomic_store_explicit(&s_api_user_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_benchmark_writer_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_benchmark_start_us, 0, memory_order_relaxed);
    atomic_store_explicit(&s_benchmark_starting, false,
                          memory_order_relaxed);
    atomic_store_explicit(&s_benchmark_generation, 0U,
                          memory_order_relaxed);
    _app_manager_display_diagnostics_reset_benchmark();

    if (xTaskCreatePinnedToCoreWithCaps(
                _app_manager_display_diagnostics_task, "display_diag",
                APP_MANAGER_DISPLAY_DIAGNOSTICS_TASK_STACK, display,
                APP_MANAGER_DISPLAY_DIAGNOSTICS_TASK_PRIORITY, &s_task,
                CONFIG_MAIN_PROJECT_TASK_CORE_ID,
                APP_MANAGER_DISPLAY_DIAGNOSTICS_TASK_STACK_CAPS) != pdPASS)
    {
        result = ESP_ERR_NO_MEM;
        goto failed;
    }
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    LOG_I("task affinity name=display_diag core=%d",
          (int)xTaskGetCoreID(s_task));
#endif

    result = esp_lv_adapter_lock(
                 APP_MANAGER_DISPLAY_DIAGNOSTICS_LVGL_LOCK_MS);
    if (result != ESP_OK)
    {
        goto failed;
    }
    const uint32_t event_count = lv_display_get_event_count(display);
    lv_display_add_event_cb(display,
                            _app_manager_display_diagnostics_display_event_cb,
                            LV_EVENT_ALL, display);
    if (lv_display_get_event_count(display) != event_count + 1U)
    {
        esp_lv_adapter_unlock();
        result = ESP_ERR_NO_MEM;
        goto failed;
    }
    esp_lv_adapter_unlock();
    s_event_registered = true;
    atomic_store_explicit(&s_stopping, false, memory_order_release);
    return ESP_OK;

failed:
    if (s_task != NULL)
    {
        atomic_store_explicit(&s_stop_requested, true, memory_order_release);
        if (xTaskNotifyGive(s_task) == pdPASS &&
                xSemaphoreTake(s_task_stopped, portMAX_DELAY) == pdPASS)
        {
            vTaskDeleteWithCaps(s_task);
        }
        else
        {
            LOG_W("display diagnostics start rollback failed");
            return ESP_ERR_INVALID_STATE;
        }
        s_task = NULL;
    }
    s_display = NULL;
    if (fps_enabled)
    {
        const esp_err_t rollback_result = esp_lv_adapter_fps_stats_enable(
                                              display, false);
        if (rollback_result != ESP_OK)
        {
            LOG_W("FPS statistics rollback failed: 0x%x",
                  (unsigned)rollback_result);
        }
    }
    if (s_benchmark_lock != NULL)
    {
        vSemaphoreDelete(s_benchmark_lock);
        s_benchmark_lock = NULL;
    }
    if (s_task_stopped != NULL)
    {
        vSemaphoreDelete(s_task_stopped);
        s_task_stopped = NULL;
    }
    return result;
}

esp_err_t app_manager_display_diagnostics_stop(void)
{
    if (s_task == NULL)
    {
        return ESP_OK;
    }

    atomic_store_explicit(&s_stopping, true, memory_order_release);
    while (atomic_load_explicit(&s_api_user_count, memory_order_acquire) > 0U)
    {
        vTaskDelay(1U);
    }
    esp_err_t result = esp_lv_adapter_lock(
                           APP_MANAGER_DISPLAY_DIAGNOSTICS_LVGL_LOCK_MS);
    if (result != ESP_OK)
    {
        atomic_store_explicit(&s_stopping, false, memory_order_release);
        return result;
    }
    if (s_event_registered)
    {
        const uint32_t removed = lv_display_remove_event_cb_with_user_data(
                                     s_display,
                                     _app_manager_display_diagnostics_display_event_cb,
                                     s_display);
        if (removed != 1U)
        {
            LOG_W("display event callback removal count: %u",
                  (unsigned)removed);
        }
        s_event_registered = false;
    }
    esp_lv_adapter_unlock();
    if (xSemaphoreTake(s_benchmark_lock, portMAX_DELAY) != pdPASS)
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_benchmark_active, false, memory_order_release);
    (void)xSemaphoreGive(s_benchmark_lock);
    while (atomic_load_explicit(&s_benchmark_writer_count,
                                memory_order_acquire) > 0U)
    {
        vTaskDelay(1U);
    }
    _app_manager_display_diagnostics_reset_spans();
    atomic_store_explicit(&s_stop_requested, true, memory_order_release);
    if (xTaskNotifyGive(s_task) != pdPASS ||
            xSemaphoreTake(s_task_stopped, portMAX_DELAY) != pdPASS)
    {
        LOG_W("display diagnostics stop confirmation failed");
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDeleteWithCaps(s_task);
    s_task = NULL;
    result = esp_lv_adapter_fps_stats_enable(s_display, false);
    if (result != ESP_OK)
    {
        LOG_W("FPS statistics disable failed: 0x%x", (unsigned)result);
    }
    s_display = NULL;
    vSemaphoreDelete(s_task_stopped);
    vSemaphoreDelete(s_benchmark_lock);
    s_task_stopped = NULL;
    s_benchmark_lock = NULL;
    return ESP_OK;
}

void app_manager_display_diagnostics_record_submit(lv_display_t *display,
        bool final_flush, uint32_t pixel_count, uint32_t wait_us,
        esp_err_t result)
{
    if (!_app_manager_display_diagnostics_writer_enter())
    {
        return;
    }
    if (display != s_display)
    {
        _app_manager_display_diagnostics_writer_exit();
        return;
    }

    app_manager_display_benchmark_metrics_t *effect_metrics =
        _app_manager_display_diagnostics_active_effect();
    atomic_fetch_add_explicit(&s_total_metrics.panel_submit_count, 1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&s_total_metrics.panel_pixel_count, pixel_count,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&s_total_metrics.submit_wait_us, wait_us,
                              memory_order_relaxed);
    _app_manager_display_diagnostics_update_maximum_u32(
        &s_total_metrics.maximum_submit_wait_us, wait_us);
    if (effect_metrics != NULL)
    {
        atomic_fetch_add_explicit(&effect_metrics->panel_submit_count, 1U,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&effect_metrics->panel_pixel_count,
                                  pixel_count, memory_order_relaxed);
        atomic_fetch_add_explicit(&effect_metrics->submit_wait_us, wait_us,
                                  memory_order_relaxed);
        _app_manager_display_diagnostics_update_maximum_u32(
            &effect_metrics->maximum_submit_wait_us, wait_us);
    }
    if (result != ESP_OK)
    {
        atomic_fetch_add_explicit(&s_total_metrics.submit_failure_count, 1U,
                                  memory_order_relaxed);
        if (effect_metrics != NULL)
        {
            atomic_fetch_add_explicit(&effect_metrics->submit_failure_count,
                                      1U, memory_order_relaxed);
        }
    }
    else if (final_flush)
    {
        atomic_fetch_add_explicit(&s_total_metrics.frame_submit_count, 1U,
                                  memory_order_relaxed);
        if (effect_metrics != NULL)
        {
            atomic_fetch_add_explicit(&s_total_metrics.active_frame_count, 1U,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&effect_metrics->frame_submit_count, 1U,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&effect_metrics->active_frame_count, 1U,
                                      memory_order_relaxed);
            const int64_t now_us = esp_timer_get_time();
            const int64_t previous_us = atomic_exchange_explicit(
                                            &s_last_final_submit_us, now_us,
                                            memory_order_acq_rel);
            if (previous_us > 0)
            {
                const uint32_t interval_us =
                    _app_manager_display_diagnostics_elapsed_us(previous_us,
                        now_us);
                uint32_t consecutive_long_intervals = 0U;
                if (interval_us > APP_MANAGER_DISPLAY_INTERVAL_LONG_US)
                {
                    consecutive_long_intervals = atomic_fetch_add_explicit(
                                                     &s_consecutive_long_intervals,
                                                     1U, memory_order_relaxed) + 1U;
                }
                else
                {
                    atomic_store_explicit(&s_consecutive_long_intervals, 0U,
                                          memory_order_relaxed);
                }
                _app_manager_display_diagnostics_record_interval(
                    &s_total_metrics, interval_us,
                    consecutive_long_intervals);
                _app_manager_display_diagnostics_record_interval(
                    effect_metrics, interval_us,
                    consecutive_long_intervals);
            }
            if (atomic_load_explicit(&s_transition_presentation_pending,
                                     memory_order_acquire))
            {
                _app_manager_display_diagnostics_finish_transition(false,
                        now_us);
            }
        }
    }
    _app_manager_display_diagnostics_writer_exit();
}

esp_err_t app_manager_display_diagnostics_begin_benchmark(
    uint32_t minimum_fps, size_t minimum_dma_largest)
{
    if (minimum_fps == 0U || minimum_dma_largest == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_app_manager_display_diagnostics_api_enter())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool starting_claimed = false;
    bool lvgl_locked = false;
    if (s_task == NULL || s_display == NULL ||
            xSemaphoreTake(s_benchmark_lock, portMAX_DELAY) != pdPASS)
    {
        goto exit;
    }
    if (atomic_load_explicit(&s_benchmark_active, memory_order_relaxed) ||
            atomic_load_explicit(&s_benchmark_starting,
                                 memory_order_relaxed))
    {
        (void)xSemaphoreGive(s_benchmark_lock);
        goto exit;
    }
    atomic_store_explicit(&s_benchmark_starting, true, memory_order_release);
    starting_claimed = true;
    (void)xSemaphoreGive(s_benchmark_lock);

    result = esp_lv_adapter_lock(
                 APP_MANAGER_DISPLAY_DIAGNOSTICS_LVGL_LOCK_MS);
    if (result != ESP_OK)
    {
        goto exit;
    }
    lvgl_locked = true;
    result = esp_lv_adapter_fps_stats_reset(s_display);
    if (result != ESP_OK)
    {
        goto exit;
    }
    if (xSemaphoreTake(s_benchmark_lock, portMAX_DELAY) != pdPASS)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    _app_manager_display_diagnostics_reset_benchmark();
    atomic_store_explicit(&s_minimum_fps_target, minimum_fps,
                          memory_order_relaxed);
    atomic_store_explicit(&s_minimum_dma_largest_target,
                          minimum_dma_largest, memory_order_relaxed);
    atomic_store_explicit(&s_benchmark_start_us, esp_timer_get_time(),
                          memory_order_release);
    uint32_t generation = atomic_load_explicit(&s_benchmark_generation,
                          memory_order_relaxed) + 1U;
    if (generation == 0U)
    {
        generation = 1U;
    }
    atomic_store_explicit(&s_benchmark_generation, generation,
                          memory_order_relaxed);
    atomic_store_explicit(&s_benchmark_active, true, memory_order_release);
    if (xTaskNotifyGive(s_task) != pdPASS)
    {
        atomic_store_explicit(&s_benchmark_active, false,
                              memory_order_release);
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        result = ESP_OK;
    }
    atomic_store_explicit(&s_benchmark_starting, false, memory_order_release);
    starting_claimed = false;
    (void)xSemaphoreGive(s_benchmark_lock);

exit:
    if (starting_claimed)
    {
        atomic_store_explicit(&s_benchmark_starting, false,
                              memory_order_release);
    }
    if (lvgl_locked)
    {
        esp_lv_adapter_unlock();
    }
    _app_manager_display_diagnostics_api_exit();
    return result;
}

esp_err_t app_manager_display_diagnostics_end_benchmark(
    app_manager_display_benchmark_report_t *report)
{
    if (report == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_app_manager_display_diagnostics_api_enter())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_task == NULL ||
            xSemaphoreTake(s_benchmark_lock, portMAX_DELAY) != pdPASS)
    {
        goto exit;
    }
    if (!atomic_load_explicit(&s_benchmark_active, memory_order_relaxed))
    {
        (void)xSemaphoreGive(s_benchmark_lock);
        goto exit;
    }
    atomic_store_explicit(&s_benchmark_active, false, memory_order_release);
    while (atomic_load_explicit(&s_benchmark_writer_count,
                                memory_order_acquire) > 0U)
    {
        vTaskDelay(1U);
    }
    _app_manager_display_diagnostics_reset_spans();
    memset(report, 0, sizeof(*report));
    report->render_task_core_id = -1;
    report->sample_count = atomic_load_explicit(&s_sample_count,
                           memory_order_relaxed);
    const uint32_t minimum_fps = atomic_load_explicit(&s_minimum_fps,
                                 memory_order_relaxed);
    report->minimum_fps = minimum_fps == UINT_MAX ? 0U : minimum_fps;
    report->sample_error_count = atomic_load_explicit(&s_sample_error_count,
                                 memory_order_relaxed);
    report->lvgl_lock_error_count = atomic_load_explicit(
                                        &s_lvgl_lock_error_count,
                                        memory_order_relaxed);
    report->fps_read_error_count = atomic_load_explicit(
                                       &s_fps_read_error_count,
                                       memory_order_relaxed);
    report->maximum_fps_lock_wait_us = atomic_load_explicit(
                                           &s_maximum_fps_lock_wait_us,
                                           memory_order_relaxed);
    report->fps_failure_count = atomic_load_explicit(&s_fps_failure_count,
                                memory_order_relaxed);
    const size_t minimum_internal_free = atomic_load_explicit(
            &s_minimum_internal_free, memory_order_relaxed);
    report->minimum_internal_free = minimum_internal_free == SIZE_MAX ? 0U :
                                    minimum_internal_free;
    const size_t minimum_internal_largest = atomic_load_explicit(
            &s_minimum_internal_largest, memory_order_relaxed);
    report->minimum_internal_largest = minimum_internal_largest == SIZE_MAX ?
                                       0U : minimum_internal_largest;
    const size_t minimum_dma_free = atomic_load_explicit(
                                        &s_minimum_dma_free,
                                        memory_order_relaxed);
    report->minimum_dma_free = minimum_dma_free == SIZE_MAX ? 0U :
                               minimum_dma_free;
    const size_t minimum_dma_largest = atomic_load_explicit(
                                           &s_minimum_dma_largest,
                                           memory_order_relaxed);
    report->minimum_dma_largest = minimum_dma_largest == SIZE_MAX ? 0U :
                                  minimum_dma_largest;
    const size_t minimum_psram_free = atomic_load_explicit(
                                          &s_minimum_psram_free,
                                          memory_order_relaxed);
    report->minimum_psram_free = minimum_psram_free == SIZE_MAX ? 0U :
                                 minimum_psram_free;
    const size_t minimum_psram_largest = atomic_load_explicit(
            &s_minimum_psram_largest, memory_order_relaxed);
    report->minimum_psram_largest = minimum_psram_largest == SIZE_MAX ? 0U :
                                    minimum_psram_largest;
    _app_manager_display_diagnostics_read_render_task(report);
    report->dma_failure_count = atomic_load_explicit(&s_dma_failure_count,
                                memory_order_relaxed);
    app_manager_display_effect_benchmark_report_t aggregate = {0};
    _app_manager_display_diagnostics_copy_metrics(&s_total_metrics,
            &aggregate);
    report->frame_submit_count = atomic_load_explicit(
                                     &s_total_metrics.frame_submit_count,
                                     memory_order_relaxed);
    report->panel_submit_count = aggregate.panel_submit_count;
    report->panel_pixel_count = aggregate.panel_pixel_count;
    report->submit_failure_count = aggregate.submit_failure_count;
    report->submit_wait_us = aggregate.submit_wait_us;
    report->maximum_submit_wait_us = aggregate.maximum_submit_wait_us;
    report->transition_start_count = aggregate.transition_start_count;
    report->transition_complete_count = aggregate.transition_complete_count;
    report->transition_cancel_count = aggregate.transition_cancel_count;
    report->snapshot_prepare_count = aggregate.snapshot_prepare_count;
    report->snapshot_prepare_us = aggregate.snapshot_prepare_us;
    report->maximum_snapshot_prepare_us =
        aggregate.maximum_snapshot_prepare_us;
    report->snapshot_prepare_p95_us = aggregate.snapshot_prepare_p95_us;
    report->snapshot_fallback_count = aggregate.snapshot_fallback_count;
    report->active_frame_count = aggregate.active_frame_count;
    report->active_duration_us = aggregate.active_duration_us;
    report->interval_count = aggregate.interval_count;
    report->interval_within_target_count =
        aggregate.interval_within_target_count;
    report->maximum_consecutive_long_intervals =
        aggregate.maximum_consecutive_long_intervals;
    report->interval_p50_us = aggregate.interval_p50_us;
    report->interval_p95_us = aggregate.interval_p95_us;
    report->interval_p99_us = aggregate.interval_p99_us;
    report->maximum_interval_us = aggregate.maximum_interval_us;
    report->render_count = aggregate.render_count;
    report->render_us = aggregate.render_us;
    report->maximum_render_us = aggregate.maximum_render_us;
    report->flush_count = aggregate.flush_count;
    report->flush_pixel_count = aggregate.flush_pixel_count;
    report->flush_us = aggregate.flush_us;
    report->maximum_flush_us = aggregate.maximum_flush_us;
    report->flush_wait_count = aggregate.flush_wait_count;
    report->flush_wait_us = aggregate.flush_wait_us;
    report->maximum_flush_wait_us = aggregate.maximum_flush_wait_us;
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT; ++index)
    {
        report->effects[index].effect =
            (app_manager_transition_effect_t)(
                (int)APP_MANAGER_TRANSITION_NONE + (int)index);
        _app_manager_display_diagnostics_copy_metrics(
            &s_effect_metrics[index], &report->effects[index]);
    }
    report->passed = _app_manager_display_diagnostics_benchmark_passed(report);
    (void)xSemaphoreGive(s_benchmark_lock);
    result = ESP_OK;

exit:
    _app_manager_display_diagnostics_api_exit();
    return result;
}

#else

esp_err_t app_manager_display_diagnostics_start(lv_display_t *display)
{
    (void)display;
    return ESP_OK;
}

void app_manager_display_diagnostics_record_submit(lv_display_t *display,
        bool final_flush, uint32_t pixel_count, uint32_t wait_us,
        esp_err_t result)
{
    (void)display;
    (void)final_flush;
    (void)pixel_count;
    (void)wait_us;
    (void)result;
}

esp_err_t app_manager_display_diagnostics_begin_benchmark(
    uint32_t minimum_fps, size_t minimum_dma_largest)
{
    (void)minimum_fps;
    (void)minimum_dma_largest;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_manager_display_diagnostics_wait_for_presentation(
    uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_manager_display_diagnostics_end_benchmark(
    app_manager_display_benchmark_report_t *report)
{
    (void)report;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_manager_display_diagnostics_stop(void)
{
    return ESP_OK;
}

#endif
