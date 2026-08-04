/**
 * @brief Development-only display performance diagnostics.
 */

#ifndef __APP_MANAGER_DISPLAY_DIAGNOSTICS_H__
#define __APP_MANAGER_DISPLAY_DIAGNOSTICS_H__

#include "app_manager_types.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Final-frame interval target used by the benchmark report. */
#define APP_MANAGER_DISPLAY_INTERVAL_TARGET_US 33333U
/** @brief Final-frame interval treated as a long-frame run. */
#define APP_MANAGER_DISPLAY_INTERVAL_LONG_US   41667U
/** @brief Maximum accepted P95 snapshot preparation duration. */
#define APP_MANAGER_DISPLAY_SNAPSHOT_PREPARE_TARGET_US 100000U

/** @brief Number of concrete transition effects represented in a report. */
enum
{
    APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT =
        APP_MANAGER_TRANSITION_END - APP_MANAGER_TRANSITION_NONE,
};

/** @brief Display benchmark values attributed to one transition effect. */
typedef struct app_manager_display_effect_benchmark_report
{
    app_manager_transition_effect_t effect; /**< Concrete transition effect. */
    uint32_t transition_start_count;        /**< Animations that really started. */
    uint32_t transition_complete_count;     /**< Animations finally presented. */
    uint32_t transition_cancel_count;       /**< Started animations then cancelled. */
    uint32_t snapshot_prepare_count;         /**< Attempted snapshot preparations. */
    uint64_t snapshot_prepare_us;            /**< Sum of snapshot preparation time. */
    uint32_t maximum_snapshot_prepare_us;    /**< Longest snapshot preparation. */
    uint32_t snapshot_prepare_p95_us;        /**< Histogram-derived 95th percentile. */
    uint32_t snapshot_fallback_count;        /**< Preparations falling back to native LVGL. */
    uint32_t active_frame_count;            /**< Successful final flushes while active. */
    uint64_t active_duration_us;            /**< Sum of active animation durations. */
    uint32_t interval_count;                /**< Start-to-submit intervals covering active time. */
    uint32_t interval_within_target_count;  /**< Intervals no longer than 33,333 us. */
    uint32_t maximum_consecutive_long_intervals; /**< Longest run above 41,667 us. */
    uint32_t interval_p50_us;               /**< Histogram-derived 50th percentile. */
    uint32_t interval_p95_us;               /**< Histogram-derived 95th percentile. */
    uint32_t interval_p99_us;               /**< Histogram-derived 99th percentile. */
    uint32_t maximum_interval_us;           /**< Longest exact final-flush interval. */
    uint32_t render_count;                  /**< Completed LVGL render spans. */
    uint64_t render_us;                     /**< Sum of LVGL render durations. */
    uint32_t maximum_render_us;             /**< Longest LVGL render duration. */
    uint32_t flush_count;                   /**< LVGL flush callback calls. */
    uint64_t flush_pixel_count;             /**< Pixels submitted through LVGL flushes. */
    uint64_t flush_us;                      /**< Sum of LVGL flush callback durations. */
    uint32_t maximum_flush_us;              /**< Longest LVGL flush callback duration. */
    uint32_t flush_wait_count;              /**< Completed LVGL flush waits. */
    uint64_t flush_wait_us;                 /**< Sum of LVGL flush-wait durations. */
    uint32_t maximum_flush_wait_us;         /**< Longest LVGL flush-wait duration. */
    uint32_t panel_submit_count;             /**< Panel draw submissions. */
    uint64_t panel_pixel_count;              /**< Pixels in panel draw submissions. */
    uint32_t submit_failure_count;           /**< Failed panel submissions. */
    uint64_t submit_wait_us;                 /**< Sum of panel submit durations. */
    uint32_t maximum_submit_wait_us;         /**< Longest panel submit duration. */
} app_manager_display_effect_benchmark_report_t;

/** @brief Aggregated results from one display performance benchmark. */
typedef struct app_manager_display_benchmark_report
{
    uint32_t sample_count;                 /**< Active one-second samples. */
    uint32_t sample_error_count;           /**< Failed FPS/LVGL samples. */
    uint32_t lvgl_lock_error_count;        /**< Samples failing the LVGL lock. */
    uint32_t fps_read_error_count;         /**< Locked samples failing FPS read. */
    uint32_t maximum_fps_lock_wait_us;     /**< Longest outer FPS-lock wait. */
    uint32_t minimum_fps;                  /**< Lowest sampled adapter FPS. */
    uint32_t fps_failure_count;            /**< Samples below the FPS target. */
    size_t minimum_internal_free;          /**< Lowest internal 8-bit free heap. */
    size_t minimum_internal_largest;       /**< Lowest internal 8-bit free block. */
    size_t minimum_dma_free;               /**< Lowest DMA-capable internal heap. */
    size_t minimum_dma_largest;            /**< Lowest active DMA free block. */
    size_t minimum_psram_free;             /**< Lowest external 8-bit free heap. */
    size_t minimum_psram_largest;          /**< Lowest external 8-bit free block. */
    uint32_t minimum_render_stack_high_water; /**< Render-task minimum free stack. */
    bool render_task_found;                /**< True when the unique render task exists. */
    bool render_task_stack_in_psram;       /**< True when its stack starts in PSRAM. */
    uint32_t dma_failure_count;            /**< Samples below the DMA target. */
    uint32_t frame_submit_count;           /**< Final LVGL flush submissions. */
    uint32_t panel_submit_count;           /**< All panel draw submissions. */
    uint64_t panel_pixel_count;            /**< Pixels in panel draw submissions. */
    uint32_t submit_failure_count;         /**< Failed panel submissions. */
    uint64_t submit_wait_us;               /**< Sum of panel submit durations. */
    uint32_t maximum_submit_wait_us;       /**< Longest panel submit duration. */
    uint32_t transition_start_count;       /**< Animations that really started. */
    uint32_t transition_complete_count;    /**< Animations finally presented. */
    uint32_t transition_cancel_count;      /**< Started animations then cancelled. */
    uint32_t snapshot_prepare_count;        /**< Attempted snapshot preparations. */
    uint64_t snapshot_prepare_us;           /**< Sum of snapshot preparation time. */
    uint32_t maximum_snapshot_prepare_us;   /**< Longest snapshot preparation. */
    uint32_t snapshot_prepare_p95_us;       /**< Histogram-derived 95th percentile. */
    uint32_t snapshot_fallback_count;       /**< Preparations falling back to native LVGL. */
    uint32_t active_frame_count;           /**< Successful final flushes while active. */
    uint64_t active_duration_us;           /**< Sum of active animation durations. */
    uint32_t interval_count;               /**< Start-to-submit active intervals. */
    uint32_t interval_within_target_count; /**< Intervals no longer than 33,333 us. */
    uint32_t maximum_consecutive_long_intervals; /**< Longest run above 41,667 us. */
    uint32_t interval_p50_us;              /**< Histogram-derived 50th percentile. */
    uint32_t interval_p95_us;              /**< Histogram-derived 95th percentile. */
    uint32_t interval_p99_us;              /**< Histogram-derived 99th percentile. */
    uint32_t maximum_interval_us;          /**< Longest exact final-flush interval. */
    uint32_t render_count;                 /**< Completed LVGL render spans. */
    uint64_t render_us;                    /**< Sum of LVGL render durations. */
    uint32_t maximum_render_us;            /**< Longest LVGL render duration. */
    uint32_t flush_count;                  /**< LVGL flush callback calls. */
    uint64_t flush_pixel_count;            /**< Pixels submitted through LVGL flushes. */
    uint64_t flush_us;                     /**< Sum of LVGL flush callback durations. */
    uint32_t maximum_flush_us;             /**< Longest LVGL flush callback duration. */
    uint32_t flush_wait_count;             /**< Completed LVGL flush waits. */
    uint64_t flush_wait_us;                /**< Sum of LVGL flush-wait durations. */
    uint32_t maximum_flush_wait_us;        /**< Longest LVGL flush-wait duration. */
    app_manager_display_effect_benchmark_report_t effects[
        APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT]; /**< Per-effect matrix. */
    bool passed;                           /**< True when stability checks passed. */
} app_manager_display_benchmark_report_t;

/**
 * @brief Start threshold tracking for a display performance benchmark.
 *
 * @param minimum_fps is the minimum acceptable one-second FPS sample.
 * @param minimum_dma_largest is the minimum acceptable active DMA block.
 * @return ESP_OK when tracking starts, or an adapter state error.
 */
esp_err_t app_manager_display_diagnostics_begin_benchmark(
    uint32_t minimum_fps, size_t minimum_dma_largest);

/**
 * @brief Wait until the current transition reaches a successful final submit.
 *
 * @param timeout_ms is the bounded wait in milliseconds.
 * @return ESP_OK when no transition remains pending; ESP_ERR_TIMEOUT when the
 *         final submit does not arrive; otherwise a diagnostics state error.
 */
esp_err_t app_manager_display_diagnostics_wait_for_presentation(
    uint32_t timeout_ms);

/**
 * @brief Stop benchmark tracking and return its aggregated results.
 *
 * @param report receives the completed benchmark result.
 * @return ESP_OK when tracking stops, or an adapter state error.
 */
esp_err_t app_manager_display_diagnostics_end_benchmark(
    app_manager_display_benchmark_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_DISPLAY_DIAGNOSTICS_H__ */
