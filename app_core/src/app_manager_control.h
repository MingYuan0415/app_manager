#ifndef __APP_MANAGER_CONTROL_H__
#define __APP_MANAGER_CONTROL_H__

#include "app_manager_config.h"

/**
 * @brief Start the application-control task and copy its dependencies.
 * @param config supplies the hardware, input, and standby callbacks.
 * @return ESP_OK when the task is ready, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_control_init(const app_manager_config_t *config);
/**
 * @brief Commit the initially fenced display from the UI worker.
 * @return ESP_OK when the panel and physical touch are enabled.
 */
esp_err_t app_manager_control_commit_display(void);
/**
 * @brief Open input and inactivity admission after the first frame is visible.
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_control_open_admission(void);
/**
 * @brief Stop application control and release all owned resources.
 * @return ESP_OK when stopped, otherwise a retryable cleanup error.
 */
esp_err_t app_manager_control_deinit(void);
/**
 * @brief Notify control of touch activity from interrupt context.
 * @return true when admitted; false while control is unavailable.
 */
bool app_manager_control_notify_touch_activity_from_isr(void);
#ifdef HOST_TEST
    /**
    * @brief Return the current touch generation for host tests.
    * @return Current touch-admission generation.
    */
    uint32_t app_manager_control_touch_generation_for_test(void);
    /**
    * @brief Inject one touch generation for host tests.
    * @param generation identifies the injected touch epoch.
    * @return true when queued; false when control is unavailable or full.
    */
    bool app_manager_control_notify_touch_generation_for_test(uint32_t generation);
    /**
    * @brief Pause or resume touch snapshot processing in host tests.
    * @param pause is true to hold the simulated ISR snapshot.
    */
    void app_manager_control_touch_snapshot_pause_for_test(bool pause);
    /**
    * @brief Report whether a paused host-test snapshot was reached.
    * @return true after the simulated ISR reaches its pause point.
    */
    bool app_manager_control_touch_snapshot_reached_for_test(void);
    /**
    * @brief Return the current standby generation for host tests.
    * @return Current application standby epoch.
    */
    uint32_t app_manager_control_standby_generation_for_test(void);
#endif

#endif /* __APP_MANAGER_CONTROL_H__ */
