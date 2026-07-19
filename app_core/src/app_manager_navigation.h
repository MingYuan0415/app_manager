#ifndef __APP_MANAGER_NAVIGATION_H__
#define __APP_MANAGER_NAVIGATION_H__

#include "app_manager_types.h"

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize the fixed navigation command pool. */
esp_err_t app_manager_navigation_init(void);

/**
 * @brief Stop navigation admission and release command synchronization state.
 * @return ESP_OK when every admitted command is complete.
 */
esp_err_t app_manager_navigation_deinit(void);

/**
 * @brief Complete all visual work before screen pause or lifecycle teardown.
 * @note Call only from the UI worker.
 */
void app_manager_navigation_finish_transition(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_NAVIGATION_H__ */
