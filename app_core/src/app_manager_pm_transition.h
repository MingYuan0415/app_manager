#ifndef __APP_MANAGER_PM_TRANSITION_H__
#define __APP_MANAGER_PM_TRANSITION_H__

#include "app_manager_types.h"

/**
 * @brief Publish the current application-control PM state.
 * @param state is the state visible to external PM queries.
 */
void app_manager_control_state_set(app_manager_pm_state_t state);

/**
 * @brief Report whether both display layers have committed sleep.
 * @return true when hardware and LVGL are sleeping; false otherwise.
 */
bool app_manager_control_screen_is_canonical_sleep(void);

/**
 * @brief Report whether a pending power event must be treated as wake intent.
 * @return true when display recovery takes priority; false while active.
 */
bool app_manager_control_power_event_is_wake(void);

/**
 * @brief Establish a standby epoch without losing concurrent wake intent.
 * @return true when system standby may be requested; false when aborted.
 */
bool app_manager_control_standby_generation_begin(void);

/** @brief Invalidate the current standby epoch before display recovery. */
void app_manager_control_standby_generation_abort(void);

/**
 * @brief Commit the display hardware to its canonical sleep state.
 * @return ESP_OK on success, otherwise a retryable transition error.
 */
esp_err_t app_manager_control_suspend_hardware(void);

/**
 * @brief Recover only the hardware and LVGL worker required for teardown.
 * @note This does not resume pages, repaint, restore brightness, or show LCD.
 * @return ESP_OK on success, otherwise a retryable transition error.
 */
esp_err_t app_manager_control_prepare_shutdown(void);

/**
 * @brief Restore display hardware, brightness, and touch to active state.
 * @return ESP_OK on success, otherwise a retryable transition error.
 */
esp_err_t app_manager_control_restore_active(void);

/**
 * @brief Record activity and restore active display state when required.
 * @return ESP_OK on success, otherwise a retryable transition error.
 */
esp_err_t app_manager_control_handle_activity(void);

/**
 * @brief Process touch activity admitted under a display generation.
 * @param generation identifies the touch-admission epoch.
 * @return ESP_OK when handled or stale, otherwise a transition error.
 */
esp_err_t app_manager_control_handle_touch_activity(uint32_t generation);

/** @brief Dispatch a coalesced HOME request to the UI worker. */
void app_manager_control_process_pending_home(void);

/**
 * @brief Apply one typed key event to the PM and navigation state machines.
 * @param key identifies the logical key.
 * @param event identifies the key transition.
 * @return ESP_OK on success, otherwise a transition or dispatch error.
 */
esp_err_t app_manager_control_handle_input(app_manager_key_t key,
        app_manager_key_event_t event);

#endif /* __APP_MANAGER_PM_TRANSITION_H__ */
