/**
 * @brief  应用管理器完整公共 API
 * @note   include app_manager_types.h + app_manager_ids.h + app_theme.h，不 include config.h。
 *         apps 与 main 均可 include。
 */

#ifndef __APP_MANAGER_H__
#define __APP_MANAGER_H__

#include "app_manager_types.h"
#include "app_manager_ids.h"
#include "app_theme.h"   /* app_theme_font_id_t */
/* struct app_manager_config 前向声明来自 app_manager_types.h */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 初始化与运行 ── */

/**
 * @brief 初始化应用管理器
 * @param cfg 配置结构体指针（完整类型在 app_manager_config.h，仅 main 需要）
 * @return ESP_OK 成功
 */
esp_err_t app_manager_init(const struct app_manager_config *cfg);
/**
 * @brief Stop the application manager and release every owned resource.
 * @return ESP_OK when released, otherwise a retryable cleanup error.
 */
esp_err_t app_manager_deinit(void);
/**
 * @brief Report whether the application manager is initialized.
 * @return true when ready; false otherwise.
 */
bool      app_manager_is_initialized(void);

/**
 * @brief Execute one navigation request and wait for a stable presentation.
 *
 * @note This API rejects calls from the UI worker. A queued request is
 *       cancelled when timeout expires before execution. Once execution has
 *       started, it reaches a stable state before returning ESP_ERR_TIMEOUT.
 *
 * @param request is copied into the fixed command pool.
 * @param timeout_ms is the wait limit, or UINT32_MAX for no limit.
 * @return Final navigation result, timeout, or an admission error.
 */
esp_err_t app_manager_navigate(const app_manager_nav_request_t *request,
                               uint32_t timeout_ms);

/**
 * @brief Queue one navigation request without blocking the caller.
 *
 * @note completion runs exactly once on the UI worker after admission succeeds.
 *       EXIT_SELF is accepted only inside a lifecycle callback; its owner ID
 *       is captured before the callback returns.
 *
 * @param request is copied into the fixed command pool.
 * @param completion receives the final result, or NULL to ignore it.
 * @param context is passed unchanged to completion.
 * @return ESP_OK when admitted, otherwise an admission error.
 */
esp_err_t app_manager_navigate_async(const app_manager_nav_request_t *request,
                                     app_manager_nav_completion_cb_t completion,
                                     void *context);

/* ── 查询 ── */

/**
 * @brief Report whether an application is active.
 * @param app_id identifies the application to query.
 * @return true when active; false otherwise or while the screen is paused.
 */
bool        app_manager_is_actived(const char *app_id);
/**
 * @brief Return the active application identifier.
 * @return Stable active identifier, or NULL when none is active or the screen
 *         is paused.
 */
const char *app_manager_get_active_app_id(void);
/**
 * @brief Report whether a page is active in an application.
 * @param app_id identifies the application.
 * @param pg_id identifies the page.
 * @return true when active; false otherwise or while the screen is paused.
 */
bool        app_page_is_actived(const char *app_id, const char *pg_id);
/**
 * @brief Report whether a page exists in an application.
 * @param app_id identifies the application.
 * @param pg_id identifies the page.
 * @return true when present; false when absent or topology queries are paused.
 * @note A false result during screen PAUSING/PAUSED does not mean the retained
 *       page instance was destroyed.
 */
bool        app_manager_is_page_present(const char *app_id, const char *pg_id);
/**
 * @brief Report whether every application is closed.
 * @return true when no application is running, including in the retained
 *         screen-PAUSED snapshot; false otherwise or during teardown.
 */
bool        app_manager_is_all_closed(void);
/**
 * @brief Return the number of running applications.
 * @return Current running-application count. While screen-PAUSED, returns the
 *         retained count captured after ONPAUSE; returns 0 during teardown.
 */
uint32_t    app_manager_get_running_apps(void);
/**
 * @brief Report whether a screen transition is currently in progress.
 * @return true while topology queries intentionally expose conservative state.
 */
bool        app_manager_is_transitioning(void);

/**
 * @brief Enable or disable the global edge-back gesture policy.
 *
 * @note The policy defaults to enabled after app_manager_init(). Disabling it
 *       cancels pointer tracking that has not submitted BACK, but does not
 *       revoke a navigation command already admitted by the gateway.
 *
 * @param enabled selects whether stable pages may expose edge-back capture.
 * @return ESP_OK when applied or retained for the next Screen resume,
 *         otherwise an admission or UI-dispatch error.
 */
esp_err_t app_manager_back_gesture_set_enabled(bool enabled);

/**
 * @brief Return the global edge-back gesture policy.
 * @return true when enabled; false before init, after deinit, or when disabled.
 * @note This does not report whether the current page has a back target.
 */
bool app_manager_back_gesture_is_enabled(void);

/* ── 页面私有内存（handler 内使用） ── */

/**
 * @brief Return private memory for the page handling the current callback.
 * @return Page-private memory, or NULL when unavailable.
 */
void     *app_manager_this_page_memory(void);
/**
 * @brief Return user data for the page handling the current callback.
 * @return Current page user data, or NULL.
 */
void     *app_manager_this_page_userdata(void);
/**
 * @brief Return the LVGL screen owned by the callback page.
 * @return Page screen while mounted, or NULL outside a mounted lifecycle.
 */
lv_obj_t *app_manager_this_page_screen(void);
/**
 * @brief Return the latest copied navigation arguments for the callback page.
 * @return Immutable typed blob, or NULL when no arguments were supplied.
 */
const app_manager_typed_blob_t *app_manager_this_page_arguments(void);
/**
 * @brief Report a retryable resource-release error from page teardown.
 *
 * @note Call this only from APP_MANAGER_MSG_ONPAUSE or
 *       APP_MANAGER_MSG_ONSTOP. The lifecycle keeps the page slot and retries
 *       the transition on the next navigation or manager-cleanup request.
 *
 * @param error is the non-ESP_OK teardown error to preserve.
 */
void      app_manager_this_page_report_cleanup_error(esp_err_t error);

/* ── Screen ctrl ops ── */

/**
 * @brief Queue a screen-off request to application control.
 * @return ESP_OK when accepted, not when hardware completion is confirmed.
 */
esp_err_t app_manager_screen_suspend(void);
/**
 * @brief Queue a screen-resume request.
 * @return ESP_OK when accepted, not when hardware completion is confirmed.
 */
esp_err_t app_manager_screen_resume(void);
/**
 * @brief Report the logical screen-suspend state.
 * @return true while logically suspended; false otherwise.
 */
bool     app_manager_screen_is_suspended(void);
/**
 * @brief Fence the initial active screen, then enable the panel and touch IRQ.
 * @return ESP_OK when the first visible frame commits, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_commit_initial(void);
/**
 * @brief Admit physical input and start inactivity timing after first display.
 * @note Call only after the initial frame and panel-enable transactions commit.
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 * @warning Call from the startup task, not the UI worker.
 */
esp_err_t app_manager_startup_commit(void);
/**
 * @brief Queue a hardware update followed by persistence on app-control.
 * @param brightness is the normal display brightness.
 * @return ESP_OK when accepted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_screen_set_brightness(uint8_t brightness);
/**
 * @brief Queue temporary brightness without changing the persisted value.
 * @param brightness is the temporary display brightness.
 * @return ESP_OK when accepted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_screen_set_brightness_temp(uint8_t brightness);
/**
 * @brief Return the current screen brightness.
 * @return Current persisted normal brightness.
 */
uint8_t   app_manager_screen_get_brightness(void);

/* ── 按键（input_cb_t / input_ops_t 定义在 app_manager_types.h） ── */

/**
 * @brief Inject a typed key event into the application-control task.
 * @param key identifies the logical key.
 * @param event identifies the logical key transition.
 * @return ESP_OK when accepted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_input_inject(app_manager_key_t key,
                                   app_manager_key_event_t event);

/* ── PM ── */

/**
 * @brief Queue a screen-off request.
 * @return ESP_OK when the control queue accepts it, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_pm_request_screen_off(void);
/**
 * @brief Notify application PM of task-context user activity.
 * @return ESP_OK when accepted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_pm_notify_user_activity(void);
/**
 * @brief Notify application PM of ISR-context user activity.
 * @return true when latched; false while application control is unavailable.
 */
bool      app_manager_pm_notify_user_activity_from_isr(void);
/**
 * @brief Notify application PM that system sleep has ended.
 * @return ESP_OK when accepted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_pm_notify_system_wake(void);
/**
 * @brief Return the current application PM state.
 * @return Current logical PM state.
 */
app_manager_pm_state_t app_manager_pm_get_state(void);
/**
 * @brief Validate a system-PM generation at the sleep-commit boundary.
 * @param generation identifies the system-PM request.
 * @param context is unused.
 * @return true when sleep may commit; false when wake intent won the race.
 */
bool     app_manager_pm_standby_commit_guard(uint32_t generation,
        void *context);
/**
 * @brief Latch a committed standby generation without blocking.
 * @param generation identifies the committed system-PM request.
 * @param context is unused.
 */
void     app_manager_pm_standby_commit_callback(uint32_t generation,
        void *context);
/**
 * @brief Check wake intent after input pause and before sleep commit.
 * @return true when standby preparation remains valid; false otherwise.
 */
bool     app_manager_pm_standby_prepare_guard(void);
/**
 * @brief Return the screen-off inactivity timeout in milliseconds.
 * @return Configured timeout, or -1 when disabled.
 */
int32_t  app_manager_pm_get_timeout_ms(void);
/**
 * @brief Set and persist the screen-off inactivity timeout.
 * @param timeout_ms is -1 to disable or a valid timeout in milliseconds.
 * @return ESP_OK when persisted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_pm_set_timeout_ms(int32_t timeout_ms);
/**
 * @brief Return the screen-off to standby delay in milliseconds.
 * @return Configured delay, or -1 when disabled.
 */
int32_t  app_manager_pm_get_standby_delay_ms(void);
/**
 * @brief Set and persist the screen-off to standby delay.
 * @param timeout_ms is -1 to disable or a valid delay in milliseconds.
 * @return ESP_OK when persisted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_pm_set_standby_delay_ms(int32_t timeout_ms);

/* ── Builtin registry discovery ── */

/**
 * @brief Discover application descriptors exported through the linker section.
 * @return Number of discovered application descriptors, or zero when invalid.
 */
int      app_manager_builtin_discover(void);
/**
 * @brief Open builtin descriptor iteration.
 * @return First descriptor, or NULL when none exists.
 */
const app_manager_app_desc_t *app_manager_builtin_list_open(void);
/**
 * @brief Return the descriptor after prev.
 * @param prev is the previously returned descriptor.
 * @return Next descriptor, or NULL at the end or for an invalid iterator.
 */
const app_manager_app_desc_t *app_manager_builtin_list_get_next(
    const app_manager_app_desc_t *prev);

/* ── UI dispatcher（ui_dispatch_fn 定义在 app_manager_types.h） ── */

/**
 * @brief Queue a non-blocking callback on the LVGL worker.
 * @param cb is the callback to enqueue.
 * @param arg is the opaque callback argument.
 * @return ESP_OK when queued, otherwise an admission error.
 */
esp_err_t app_manager_ui_post(void (*cb)(void *), void *arg);
/**
 * @brief Run a callback on the LVGL worker and wait for its result.
 *
 * A callback still queued at timeout is cancelled. If it has started, this call
 * waits for it to finish before returning ESP_ERR_TIMEOUT, so arg may be released
 * as soon as this function returns.
 *
 * @param cb is the fallible callback to execute.
 * @param arg is the opaque callback argument.
 * @param timeout_ms is the wait limit or UINT32_MAX.
 * @return Callback result, ESP_ERR_TIMEOUT, or an admission error.
 */
esp_err_t app_manager_ui_call(app_manager_ui_call_fn cb, void *arg,
                              uint32_t timeout_ms);
/**
 * @brief Return the UI dispatch entry used by the event bus.
 * @param out_fn receives the non-blocking UI dispatcher.
 * @return ESP_OK when available, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_get_ui_dispatch_fn(app_manager_ui_dispatch_fn *out_fn);

/* ── 字体 ── */

/**
 * @brief Return a loaded application-theme font.
 * @param id selects the theme font role.
 * @return Loaded font, or NULL when unavailable.
 */
const lv_font_t *app_manager_get_font(app_theme_font_id_t id);

/**
 * @brief Resolve one product semantic ID to a persistent mmap image.
 *
 * @param semantic_id is the nonzero product-owned image identity.
 * @param image receives a descriptor valid until app_manager_deinit().
 * @return ESP_OK when found; ESP_ERR_NOT_FOUND for an unknown ID; otherwise
 *         an argument or lifecycle error.
 */
esp_err_t app_manager_get_image(uint32_t semantic_id,
                                const lv_image_dsc_t **image);

/* ── Static registry export macros ── */

/**
 * @brief Export one builtin application descriptor.
 * @param n is the descriptor symbol suffix and display name.
 * @param icn is the optional icon pointer.
 * @param aid is the stable application identifier.
 * @param root_id identifies the application's static root page.
 * @param app_flags is a bitwise app_manager_app_flag_t value.
 * @param routes is the application's static page-route array.
 */
#define APP_MANAGER_APP_EXPORT(n, icn, aid, root_id, app_flags, routes) \
    __attribute__((section(".app_manager_apps"), used, aligned(sizeof(void *)))) \
    static const app_manager_app_desc_t _app_manager_app_##n = \
    { \
        .name = #n, \
        .icon = (const void *)(icn), \
        .id = (aid), \
        .root_page_id = (root_id), \
        .page_routes = (routes), \
        .page_route_count = sizeof(routes) / sizeof((routes)[0]), \
        .flags = (app_flags), \
    }

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_H__ */
