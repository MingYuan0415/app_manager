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
 * @brief Start or resume the application identified by app_id.
 * @param app_id identifies a registered application.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_run(const char *app_id);
/**
 * @brief Stop the application identified by app_id.
 * @param app_id identifies a running application.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_exit(const char *app_id);
/** @brief Request exit of the application owning the current callback. */
void     app_manager_self_exit(void);

/* ── 查询 ── */

/**
 * @brief Report whether an application is active.
 * @param app_id identifies the application to query.
 * @return true when active; false otherwise.
 */
bool        app_manager_is_actived(const char *app_id);
/**
 * @brief Return the active application identifier.
 * @return Stable active identifier, or NULL when none is active.
 */
const char *app_manager_get_active_app_id(void);
/**
 * @brief Report whether a page is active in an application.
 * @param app_id identifies the application.
 * @param pg_id identifies the page.
 * @return true when active; false otherwise.
 */
bool        app_page_is_actived(const char *app_id, const char *pg_id);
/**
 * @brief Report whether a page exists in an application.
 * @param app_id identifies the application.
 * @param pg_id identifies the page.
 * @return true when present; false otherwise.
 */
bool        app_manager_is_page_present(const char *app_id, const char *pg_id);
/**
 * @brief Report whether every application is closed.
 * @return true when no application is running; false otherwise.
 */
bool        app_manager_is_all_closed(void);
/**
 * @brief Return the number of running applications.
 * @return Current running-application count.
 */
uint32_t    app_manager_get_running_apps(void);

/* ── 页面注册与创建 ── */

/**
 * @brief Register the root-page handler for the current application.
 * @param id identifies the root page.
 * @param handler receives lifecycle messages.
 * @param usr_data is retained as page user data.
 * @param mem_size requests zeroed page-private bytes.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_regist_msg_handler_ext(const char *id, app_page_msg_cb_t handler,
        void *usr_data, uint32_t mem_size);
/**
 * @brief Register a root-page handler without private memory.
 * @param id identifies the root page.
 * @param handler receives lifecycle messages.
 */
#define   app_manager_regist_msg_handler(id, handler) \
          app_manager_regist_msg_handler_ext((id), (handler), NULL, 0)

/**
 * @brief Create a page for the current application.
 * @param pg_id identifies the new page.
 * @param handler receives lifecycle messages.
 * @param usr_data is retained as page user data.
 * @param mem_size requests zeroed page-private bytes.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_create_page_ext(const char *pg_id, app_page_msg_cb_t handler,
                                      void *usr_data, uint32_t mem_size);
/**
 * @brief Create a page without private memory.
 * @param pg_id identifies the new page.
 * @param handler receives lifecycle messages.
 */
#define   app_manager_create_page(pg_id, handler) \
          app_manager_create_page_ext((pg_id), (handler), NULL, 0)

/**
 * @brief Create a page for an explicitly identified application.
 * @param app_id identifies the target application.
 * @param pg_id identifies the new page.
 * @param handler receives lifecycle messages.
 * @param user_data is retained as page user data.
 * @param mem_size requests zeroed page-private bytes.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_create_page_for_app_ext(const char *app_id, const char *pg_id,
        app_page_msg_cb_t handler,
        void *user_data, uint32_t mem_size);
/**
 * @brief Create an application page without private memory.
 * @param app_id identifies the target application.
 * @param pg_id identifies the new page.
 * @param handler receives lifecycle messages.
 */
#define   app_manager_create_page_for_app(app_id, pg_id, handler) \
          app_manager_create_page_for_app_ext((app_id), (pg_id), (handler), NULL, 0)

/* ── 页面导航 ── */

/**
 * @brief Navigate back to a named page in the active application.
 * @param pg_id identifies a page already on the active stack.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_goback_to_page(const char *pg_id);
/**
 * @brief Navigate back one page in the active application.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_goback(void);
/**
 * @brief Remove a page from the active application.
 * @param pg_id identifies the page to remove.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_remove_page(const char *pg_id);

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
 * @brief Report a retryable resource-release error from page teardown.
 *
 * @note Call this only from APP_MANAGER_MSG_ONPAUSE or
 *       APP_MANAGER_MSG_ONSTOP. The lifecycle keeps the page slot and retries
 *       the transition on the next navigation or manager-cleanup request.
 *
 * @param error is the non-ESP_OK teardown error to preserve.
 */
void      app_manager_this_page_report_cleanup_error(esp_err_t error);
/**
 * @brief Return user data for a named page.
 * @param pg_id identifies the page in the active application.
 * @return Page user data, or NULL when unavailable.
 */
void     *app_manager_get_page_userdata(const char *pg_id);
/**
 * @brief Replace user data for a named page.
 * @param pg_id identifies the page in the active application.
 * @param usr_data is the new retained user data.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_set_page_userdata(const char *pg_id, void *usr_data);

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
 * @brief Flush the initial active screen while the panel is still hidden.
 * @return ESP_OK when the fenced refresh completes, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_display_commit_initial(void);
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

/* ── Builtin app 发现 ── */

/**
 * @brief Discover descriptors exported through the builtin linker section.
 * @return Number of valid builtin descriptors.
 */
int      app_manager_builtin_discover(void);
/**
 * @brief Register a fallback builtin descriptor array.
 * @param descs points to descriptors valid for the manager lifetime.
 * @param count is the descriptor count.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_register_builtin_apps(const app_builtin_app_desc_t *descs,
        size_t count);
/**
 * @brief Open builtin descriptor iteration.
 * @return First descriptor, or NULL when none exists.
 */
const app_builtin_app_desc_t *app_manager_builtin_list_open(void);
/**
 * @brief Return the descriptor after prev.
 * @param prev is the previously returned descriptor.
 * @return Next descriptor, or NULL at the end or for an invalid iterator.
 */
const app_builtin_app_desc_t *app_manager_builtin_list_get_next(const app_builtin_app_desc_t *prev);

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

/* ── Builtin app 自注册宏 ── */

/**
 * @brief Export one builtin descriptor through the linker section.
 * @param n is the descriptor symbol suffix and display name.
 * @param icn is the optional icon pointer.
 * @param aid is the stable application identifier.
 * @param ent is the application entry function.
 */
#define BUILTIN_APP_EXPORT(n, icn, aid, ent) \
    __attribute__((section(".app_builtin_apps"), used)) \
    static const app_builtin_app_desc_t _builtin_##n = \
    { \
        .name = #n, \
        .icon = (const void *)(icn), \
        .id = (aid), \
        .entry = (ent), \
        .type = APP_MANAGER_APP_BUILTIN, \
    }

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_H__ */
