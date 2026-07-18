#ifndef __APP_MANAGER_LIFECYCLE_H__
#define __APP_MANAGER_LIFECYCLE_H__

#include "app_manager_types.h"

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Initialize fixed lifecycle pools and the page-memory arena.
 * @param arena_bytes is the total private page-memory budget, or zero for the default.
 * @return ESP_OK when initialized, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_init(size_t arena_bytes);
/**
 * @brief Deinitialize lifecycle state on the mailbox worker.
 * @return ESP_OK when every page released its resources, otherwise the first
 *         retryable page-cleanup error.
 */
esp_err_t app_manager_lifecycle_deinit(void);
/**
 * @brief Force lifecycle cleanup when the mailbox worker is unavailable.
 * @note Page handlers are not invoked by this emergency path.
 */
void app_manager_lifecycle_force_deinit(void);

/**
 * @brief Pause the active page for display sleep on the mailbox worker.
 * @return ESP_OK when paused or already paused, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_screen_pause(void);
/**
 * @brief Resume the paused page on the mailbox worker.
 * @return ESP_OK when resumed or already active, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_screen_resume(void);
/**
 * @brief Report whether application and page mutations are currently allowed.
 * @return true only while the screen lifecycle is active.
 */
bool app_manager_lifecycle_screen_mutation_allowed(void);
/**
 * @brief Admit one public lifecycle API call while the screen is active.
 * @return true when admitted; false once screen PAUSE has begun.
 */
bool app_manager_lifecycle_screen_access_begin(void);
/** @brief Release one public lifecycle API admission. */
void app_manager_lifecycle_screen_access_end(void);
/**
 * @brief Wait for lifecycle API calls admitted before screen PAUSE to finish.
 * @note Call only outside the mailbox worker, after screen PAUSE succeeds.
 * @return ESP_OK when drained, otherwise ESP_ERR_INVALID_STATE.
 */
esp_err_t app_manager_lifecycle_screen_access_wait(void);
/**
 * @brief Close lifecycle API admission and drain the current manager epoch.
 * @note Call only outside the mailbox worker before lifecycle shutdown.
 * @return ESP_OK when admission is closed and prior calls are drained.
 */
esp_err_t app_manager_lifecycle_shutdown_begin_and_wait(void);
/**
 * @brief Read the retained running-application count while screen-paused.
 * @param count receives the snapshot when available.
 * @return true only while the screen lifecycle is PAUSED.
 */
bool app_manager_lifecycle_screen_running_snapshot(uint32_t *count);

#ifdef HOST_TEST
/** @brief Observation points around a host-test lifecycle callback. */
typedef enum app_manager_lifecycle_observer_phase
{
    APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE = 0, /**< Before the page handler. */
    APP_MANAGER_LIFECYCLE_OBSERVER_AFTER,      /**< After the page handler. */
} app_manager_lifecycle_observer_phase_t;

/**
 * @brief Observe page lifecycle callbacks in host tests.
 * @param app_id identifies the application receiving the callback.
 * @param page_id identifies the page receiving the callback.
 * @param message identifies the lifecycle callback.
 * @param phase identifies whether the handler is about to run or has returned.
 */
typedef void (*app_manager_lifecycle_observer_t)(
    const char *app_id, const char *page_id, app_manager_msg_type_t message,
    app_manager_lifecycle_observer_phase_t phase);

/**
 * @brief Install or clear the host-test lifecycle observer.
 * @param observer receives lifecycle observations, or NULL to clear it.
 */
void app_manager_lifecycle_host_set_observer(
    app_manager_lifecycle_observer_t observer);

/** @brief Host-test hook invoked after a public lifecycle API admission. */
typedef void (*app_manager_lifecycle_admission_hook_t)(void);

/**
 * @brief Install or clear the host-test public API admission hook.
 * @param hook is invoked after admission and before argument checks or enqueue.
 */
void app_manager_lifecycle_host_set_admission_hook(
    app_manager_lifecycle_admission_hook_t hook);
#endif

#endif /* __APP_MANAGER_LIFECYCLE_H__ */
