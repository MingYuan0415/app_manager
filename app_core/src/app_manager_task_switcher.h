#ifndef __APP_MANAGER_TASK_SWITCHER_H__
#define __APP_MANAGER_TASK_SWITCHER_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the system-level task-switcher surface on the Sys Layer.
 * @param display is the presentation display.
 * @return ESP_OK when created, otherwise an ESP-IDF error.
 * @note Call only from the LVGL worker after the presentation initializes.
 */
esp_err_t app_manager_task_switcher_init(void *display);

/**
 * @brief Delete the task-switcher surface and every preview reference.
 * @return ESP_OK when released, otherwise an ESP-IDF error.
 * @note Call only from the LVGL worker before the lifecycle releases the
 *       recent-tasks preview cache.
 */
esp_err_t app_manager_task_switcher_deinit(void);

/**
 * @brief Open the task switcher, or close it when already visible.
 *
 * Captures the current foreground task preview, copies the MRU task list and
 * builds the cards before the surface becomes interactive. A repeated open
 * while visible closes the surface back to the previous application.
 *
 * @return ESP_OK when the surface reached the intended state.
 * @note Call only from the LVGL worker through a navigation system command.
 */
esp_err_t app_manager_task_switcher_show(void);

/**
 * @brief Hide the task switcher synchronously before a navigation executes.
 * @note Call only from the LVGL worker.
 */
void app_manager_task_switcher_navigation_begin(void);

/**
 * @brief Report whether the task-switcher surface is currently visible.
 * @return true while visible or managing; false otherwise.
 */
bool app_manager_task_switcher_is_visible(void);

/**
 * @brief Hide the task switcher for display sleep.
 * @note Call only from the LVGL worker before the page pause sequence.
 */
void app_manager_task_switcher_screen_suspend(void);

/**
 * @brief Finish a clear-all system command from the navigation gateway.
 * @param result is the Home activation and background-cleanup result.
 * @note Call only from the LVGL worker. Success closes the switcher above
 *       Home; failure keeps it visible and rebuilds the remaining tasks.
 */
void app_manager_task_switcher_clear_all_finished(esp_err_t result);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_TASK_SWITCHER_H__ */
