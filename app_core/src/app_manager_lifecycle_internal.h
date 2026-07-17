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
    app_page_msg_cb_t handler;  /**< Handler used for page creation. */
    void *user_data;            /**< User data used for page creation. */
    uint32_t memory_size;       /**< Requested page-private memory size. */
    void *value;                /**< Pointer response or setter value. */
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
 * @brief Execute a serialized application-run request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_run_call(void *opaque);
/**
 * @brief Execute a serialized application-exit request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_exit_call(void *opaque);
/**
 * @brief Execute a serialized self-exit request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_self_exit_call(void *opaque);
/**
 * @brief Execute a serialized page-create request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_create_call(void *opaque);
/**
 * @brief Execute a serialized one-page back request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_goback_call(void *opaque);
/**
 * @brief Execute a serialized named-page back request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_goback_to_call(void *opaque);
/**
 * @brief Execute a serialized page-removal request.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_remove_call(void *opaque);
/**
 * @brief Read page user data on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK when the page exists, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_get_userdata_call(void *opaque);
/**
 * @brief Set page user data on the lifecycle worker.
 * @param opaque points to an app_manager_lifecycle_request_t.
 * @return ESP_OK when the page exists, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_lifecycle_set_userdata_call(void *opaque);
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
