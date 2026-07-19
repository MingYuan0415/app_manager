#ifndef __APP_MANAGER_NAVIGATION_H__
#define __APP_MANAGER_NAVIGATION_H__

#include "app_manager_types.h"

#include <stdbool.h>
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

/** @brief Navigation ordering snapshot held by one edge gesture. */
typedef struct app_manager_navigation_gesture_token
{
    uint32_t epoch; /**< Navigation epoch captured at gesture start. */
} app_manager_navigation_gesture_token_t;

/**
 * @brief Capture the idle navigation epoch for a new edge gesture.
 * @param token receives the snapshot.
 * @return true only while navigation is accepting and has no pending command.
 */
bool app_manager_navigation_gesture_token_acquire(
    app_manager_navigation_gesture_token_t *token);

/**
 * @brief Check whether no navigation was admitted since token capture.
 * @param token is a snapshot returned by gesture_token_acquire().
 * @return true only while token still identifies the idle navigation epoch.
 */
bool app_manager_navigation_gesture_token_is_current(
    const app_manager_navigation_gesture_token_t *token);

/**
 * @brief Submit BACK only if token and the captured foreground App are current.
 * @param token is the gesture-start navigation epoch.
 * @param completion receives the final result on the UI worker and may be NULL.
 * @param context is forwarded to completion.
 * @return ESP_OK when admitted, otherwise an admission error.
 * @note Call only from the UI worker when the edge gesture commits.
 */
esp_err_t app_manager_navigation_submit_gesture_back(
    const app_manager_navigation_gesture_token_t *token,
    app_manager_nav_completion_cb_t completion, void *context);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_NAVIGATION_H__ */
