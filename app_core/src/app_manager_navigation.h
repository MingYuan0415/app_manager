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

/** @brief Private system commands serialized by the navigation gateway. */
typedef enum
{
    APP_MANAGER_NAV_SYSTEM_NONE = 0, /**< Empty system slot. */
    APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW, /**< Open or close the task switcher. */
    APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_CLEAR_ALL, /**< Clear all tasks to Home. */
    APP_MANAGER_NAV_SYSTEM_CONTROL_CENTER_SHOW, /**< Open or close the control center. */
    APP_MANAGER_NAV_SYSTEM_END, /**< Internal range sentinel. */
} app_manager_nav_system_op_t;

/**
 * @brief Queue one private system command through the navigation gateway.
 *
 * System commands share the command pool, epoch, screen-access fence and
 * exactly-once completion semantics with ordinary navigation. They never hide
 * the task switcher; the switcher itself owns the visible state.
 *
 * @param op selects the system operation.
 * @param completion receives the final result on the UI worker and may be NULL.
 * @param context is forwarded to completion.
 * @return ESP_OK when admitted, otherwise an admission error.
 */
esp_err_t app_manager_navigation_submit_system_op(
    app_manager_nav_system_op_t op, app_manager_nav_completion_cb_t completion,
    void *context);

/**
 * @brief Queue one task-switcher EXIT that keeps the switcher visible.
 *
 * Unlike ordinary navigation, the executing command does not hide the task
 * switcher: the switcher shows its busy shell and rebuilds on completion. The
 * preservation is bound to this command only; commands queued before or after
 * it still hide the switcher when they execute.
 *
 * @param request must be an APP_MANAGER_NAV_OP_EXIT request.
 * @param completion receives the final result on the UI worker and may be NULL.
 * @param context is forwarded to completion.
 * @return ESP_OK when admitted, otherwise an admission error.
 */
esp_err_t app_manager_navigation_submit_switcher_exit(
    const app_manager_nav_request_t *request,
    app_manager_nav_completion_cb_t completion, void *context);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_NAVIGATION_H__ */
