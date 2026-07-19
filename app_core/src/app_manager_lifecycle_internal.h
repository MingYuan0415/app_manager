#ifndef __APP_MANAGER_LIFECYCLE_INTERNAL_H__
#define __APP_MANAGER_LIFECYCLE_INTERNAL_H__

#include "app_manager_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Request and response storage used for one mailbox lifecycle call. */
typedef struct app_manager_lifecycle_request
{
    const char *app_id;         /**< Optional target application identifier. */
    const char *page_id;        /**< Optional target page identifier. */
    void *value;                /**< Pointer response storage. */
    uint32_t count;             /**< Integer response storage. */
    bool condition;             /**< Boolean response storage. */
} app_manager_lifecycle_request_t;

/**
 * @brief Validate a bounded application or page identifier.
 *
 * @param id identifies one application or page.
 *
 * @return true when id is nonempty and fits the internal storage.
 */
bool app_manager_lifecycle_id_is_valid(const char *id);
/**
 * @brief Query active application state on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the condition field is filled.
 */
esp_err_t app_manager_lifecycle_is_active_call(void *opaque);
/**
 * @brief Query the active application identifier on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the value field is filled.
 */
esp_err_t app_manager_lifecycle_get_active_id_call(void *opaque);
/**
 * @brief Query active page state on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the condition field is filled.
 */
esp_err_t app_manager_lifecycle_page_active_call(void *opaque);
/**
 * @brief Query page presence on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the condition field is filled.
 */
esp_err_t app_manager_lifecycle_page_present_call(void *opaque);
/**
 * @brief Query whether all applications are closed.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the condition field is filled.
 */
esp_err_t app_manager_lifecycle_all_closed_call(void *opaque);
/**
 * @brief Count running applications on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the count field is filled.
 */
esp_err_t app_manager_lifecycle_running_count_call(void *opaque);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_LIFECYCLE_INTERNAL_H__ */
