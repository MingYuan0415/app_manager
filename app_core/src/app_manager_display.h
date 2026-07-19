#ifndef __APP_MANAGER_DISPLAY_H__
#define __APP_MANAGER_DISPLAY_H__

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Pause the foreground page, disable touch, and fence LVGL rendering.
 * @return ESP_OK when prepared, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_sleep_prepare(void);

/**
 * @brief Resume LVGL after the existing panel hardware has been rebuilt.
 * @note The foreground page remains paused until a refresh operation resumes it.
 * @return ESP_OK when recovered, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_sleep_recover(void);

/**
 * @brief Resume the foreground page, redraw it, then enable the touch device.
 * @return ESP_OK when refreshed, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_refresh_and_enable_touch(void);

/**
 * @brief Resume the foreground page and redraw while touch remains disabled.
 * @return ESP_OK when refreshed, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_refresh_without_touch(void);

/**
 * @brief Enable LVGL touch after the physical display commit succeeds.
 * @return ESP_OK when enabled, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_enable_touch(void);

/**
 * @brief Enable LVGL touch after the initial visible frame has committed.
 * @return ESP_OK when enabled, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_enable_startup_touch(void);

/**
 * @brief Disable LVGL touch after a startup-admission rollback.
 * @return ESP_OK when disabled, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_disable_startup_touch(void);

/**
 * @brief Report whether the LVGL worker is paused for display sleep.
 * @return true while the display is sleeping; false otherwise.
 */
bool app_manager_display_is_sleeping(void);

#endif /* __APP_MANAGER_DISPLAY_H__ */
