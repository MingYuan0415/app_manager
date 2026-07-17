#ifndef __APP_MANAGER_DISPLAY_H__
#define __APP_MANAGER_DISPLAY_H__

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Disable LVGL touch, pause rendering, and drain display transfers.
 * @return ESP_OK when prepared, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_sleep_prepare(void);

/**
 * @brief Resume LVGL after the existing panel hardware has been rebuilt.
 * @return ESP_OK when recovered, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_sleep_recover(void);

/**
 * @brief Redraw the active screen, then reset and enable the touch device.
 * @return ESP_OK when refreshed, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_refresh_and_enable_touch(void);

/**
 * @brief Report whether the LVGL worker is paused for display sleep.
 * @return true while the display is sleeping; false otherwise.
 */
bool app_manager_display_is_sleeping(void);

#endif /* __APP_MANAGER_DISPLAY_H__ */
