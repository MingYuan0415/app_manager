#ifndef __APP_MANAGER_BACK_GESTURE_H__
#define __APP_MANAGER_BACK_GESTURE_H__

#include "lvgl.h"

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the fixed System Layer back-gesture objects.
 * @param display owns the System Layer used by the gesture.
 * @param touch is the pointer input device associated with display.
 * @return ESP_OK when initialized, otherwise an ESP-IDF error.
 * @note Call only from the UI worker after navigation is initialized.
 */
esp_err_t app_manager_back_gesture_init(lv_display_t *display,
                                        lv_indev_t *touch);

/**
 * @brief Stop gesture API admission and cancel an uncommitted pointer sequence.
 * @return ESP_OK when tracking is cancelled, otherwise a dispatch error.
 * @note Call outside the UI worker before navigation teardown.
 */
esp_err_t app_manager_back_gesture_shutdown_begin(void);

/**
 * @brief Delete every System Layer object owned by the gesture controller.
 * @return ESP_OK when released or already deinitialized.
 * @note Call only from the UI worker after navigation commands are drained.
 */
esp_err_t app_manager_back_gesture_deinit(void);

/**
 * @brief Cancel pointer tracking before any navigation command executes.
 * @note A gesture BACK already admitted to navigation remains authoritative.
 */
void app_manager_back_gesture_navigation_begin(void);

/**
 * @brief Refresh effective capture after a navigation reaches stable state.
 */
void app_manager_back_gesture_navigation_settled(void);

/**
 * @brief Cancel tracking and hide capture before pointer input is disabled.
 */
void app_manager_back_gesture_screen_suspend(void);

/**
 * @brief Refresh effective capture before pointer input is enabled.
 */
void app_manager_back_gesture_screen_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_BACK_GESTURE_H__ */
