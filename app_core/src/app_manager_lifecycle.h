#ifndef __APP_MANAGER_LIFECYCLE_H__
#define __APP_MANAGER_LIFECYCLE_H__

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

#endif /* __APP_MANAGER_LIFECYCLE_H__ */
