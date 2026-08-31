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
    app_manager_recent_task_t *tasks; /**< Recent-tasks output storage. */
    size_t task_capacity;       /**< Capacity of the tasks array. */
    size_t tasks_filled;        /**< Tasks copied by the recent-tasks call. */
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
/**
 * @brief Copy the recent-tasks snapshot on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK after the tasks array is filled.
 */
esp_err_t app_manager_lifecycle_recent_tasks_call(void *opaque);

/**
 * @brief Stop every eligible background task on the lifecycle worker.
 *
 * @note UI-worker context only, after the active task is Home. Destroys all
 *       non-pinned, non-hidden resident applications including one that was
 *       foreground when the task switcher opened. Partially stopped tasks are
 *       kept with their cleanup ownership for a later retry.
 *
 * @return ESP_OK when every task stopped, otherwise the first cleanup error.
 */
esp_err_t app_manager_lifecycle_clear_background_tasks(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_LIFECYCLE_INTERNAL_H__ */
