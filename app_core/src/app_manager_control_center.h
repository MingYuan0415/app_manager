#ifndef __APP_MANAGER_CONTROL_CENTER_H__
#define __APP_MANAGER_CONTROL_CENTER_H__

#include "app_manager_types.h"

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the System Layer control-center surface and top pull band.
 * @param display is the presentation display.
 * @param touch is the pointer device used by the pull band.
 * @return ESP_OK when created, otherwise an ESP-IDF error.
 * @note Call only from the LVGL worker after the presentation initializes.
 */
esp_err_t app_manager_control_center_init(void *display, void *touch);

/**
 * @brief Copy the product control-center ops used by the visible surface.
 * @param ops is the immutable ops table, or NULL to hide every bridge tile.
 * @note Call before app_manager_control_center_init().
 */
void app_manager_control_center_set_ops(
    const app_manager_control_center_ops_t *ops);

/**
 * @brief Delete the control-center surface and pull band.
 * @return ESP_OK when released, otherwise an ESP-IDF error.
 * @note Call only from the LVGL worker.
 */
esp_err_t app_manager_control_center_deinit(void);

/**
 * @brief Open the control center, or close it when already visible.
 * @return ESP_OK when the surface reached the intended state.
 * @note Call only from the LVGL worker through a navigation system command.
 */
esp_err_t app_manager_control_center_show(void);

/**
 * @brief Hide the control center synchronously before a navigation executes.
 * @note Call only from the LVGL worker.
 */
void app_manager_control_center_navigation_begin(void);

/**
 * @brief Report whether the control-center surface is currently visible.
 * @return true while visible; false otherwise.
 */
bool app_manager_control_center_is_visible(void);

/** @brief Stop the pull band and hide the surface for display sleep. */
void app_manager_control_center_screen_suspend(void);

/** @brief Recompute the pull band after display resume. */
void app_manager_control_center_screen_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_CONTROL_CENTER_H__ */