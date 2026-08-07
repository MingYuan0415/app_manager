#ifndef __APP_MANAGER_PRESENTATION_H__
#define __APP_MANAGER_PRESENTATION_H__

#include "app_manager_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Receive notification after a screen transition reaches stable state.
 * @param context is the caller-provided completion context.
 */
typedef void (*app_manager_presentation_completion_cb_t)(void *context);

/**
 * @brief Initialize the neutral screen and transition input barrier.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 * @note Call only from the LVGL worker after the default display is ready.
 */
esp_err_t app_manager_presentation_init(void);

/**
 * @brief Finish visual work and release presentation-owned LVGL objects.
 * @return ESP_OK on success or when already deinitialized.
 * @note Call only from the LVGL worker before the display is deleted.
 */
esp_err_t app_manager_presentation_deinit(void);

/**
 * @brief Create one independent page screen on the presentation display.
 * @return The new screen, or NULL when unavailable.
 * @note Call only from the LVGL worker.
 */
lv_obj_t *app_manager_presentation_create_page_screen(void);

/**
 * @brief Delete an inactive page screen.
 * @param screen is a screen returned by create_page_screen().
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 * @note A screen participating in a transition cannot be deleted.
 */
esp_err_t app_manager_presentation_delete_page_screen(lv_obj_t *screen);

/**
 * @brief Return the permanent screen used while no page UI is mounted.
 * @return The neutral screen while initialized, otherwise NULL.
 */
lv_obj_t *app_manager_presentation_neutral_screen(void);

/**
 * @brief Load a screen without animation.
 * @param screen is a valid screen on the presentation display.
 * @return ESP_OK when the screen is active, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_presentation_load_immediate(lv_obj_t *screen);

/**
 * @brief Start a screen transition.
 *
 * @param from_screen is the currently active screen.
 * @param to_screen is the destination screen.
 * @param effect is a resolved effect other than DEFAULT.
 * @param duration_ms is the duration in milliseconds.
 * @param completion runs on the LVGL worker after stable completion.
 * @param context is passed to completion.
 *
 * @return ESP_OK when started or synchronously completed, otherwise an
 *         ESP-IDF error.
 *
 * @note NONE and zero-duration transitions load and complete synchronously.
 *       When enabled for normal RGB565 output, animated transitions use
 *       presentation-owned full-resolution snapshots; unavailable snapshot
 *       resources automatically fall back to native LVGL screen animation.
 *       All other accepted transitions complete asynchronously.
 */
esp_err_t app_manager_presentation_start(
    lv_obj_t *from_screen, lv_obj_t *to_screen,
    app_manager_transition_effect_t effect, uint32_t duration_ms,
    app_manager_presentation_completion_cb_t completion, void *context);

/**
 * @brief Advance the active screen transition to normal LVGL completion.
 * @note Completion is delivered synchronously before this function returns.
 */
void app_manager_presentation_finish_now(void);

/**
 * @brief Report whether a transition or its completion delivery is pending.
 * @return true while presentation is transitioning; false otherwise.
 */
bool app_manager_presentation_is_running(void);

/**
 * @brief Capture one mounted screen into the transition source snapshot.
 *
 * Used by the lifecycle to refresh the recent-tasks preview of an app that is
 * about to leave the foreground. When snapshot capture is unavailable or the
 * screen is ineligible, the previous preview is preserved.
 *
 * @param screen is a valid mounted page screen.
 * @param snapshot receives the fixed transition source draw buffer, which is
 *        reused by the next transition and must not be retained.
 * @return ESP_OK when captured, ESP_ERR_INVALID_ARG for null arguments, or
 *         ESP_ERR_NOT_SUPPORTED when capture resources are unavailable.
 */
esp_err_t app_manager_presentation_capture_source(
    lv_obj_t *screen, const lv_draw_buf_t **snapshot);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_PRESENTATION_H__ */
