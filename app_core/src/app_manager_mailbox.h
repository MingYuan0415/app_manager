/**
 * @brief  应用管理器邮箱调度器 — 内部头
 * @note   固定 24 槽命令结构体池 + FreeRTOS 队列 + LVGL timer 定期排空。
 *         异步/同步双路径 + 重入检测。
 *         仅 app_core 内部使用，不对外暴露。
 */

#ifndef __APP_MANAGER_MAILBOX_H__
#define __APP_MANAGER_MAILBOX_H__

#include "esp_err.h"
#include "app_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 初始化 ── */

/**
 * @brief 初始化邮箱调度器
 * @return ESP_OK 成功
 */
esp_err_t app_manager_mailbox_init(void);
/**
 * @brief Stop mailbox admission, drain callbacks, and release resources.
 * @return ESP_OK when released, otherwise a retryable cleanup error.
 */
esp_err_t app_manager_mailbox_deinit(void);
/**
 * @brief Report whether the caller is the LVGL mailbox worker.
 * @return true when called from the worker, otherwise false.
 */
bool app_manager_mailbox_is_worker(void);
/**
 * @brief Execute a cleanup callback during mailbox shutdown.
 * @note Admission remains closed after the first call. A failed callback may
 *       be retried while the mailbox resources remain initialized.
 * @param cb is the callback to execute on the worker.
 * @param arg is the opaque callback argument.
 * @return Callback result or an admission error.
 */
esp_err_t app_manager_mailbox_shutdown_call(app_manager_ui_call_fn cb, void *arg);

/* ── 异步命令 ── */

/**
 * @brief 投递异步回调（入队后立即返回，不等待执行完成）
 * @param cb is the callback to enqueue.
 * @param arg is the opaque callback argument.
 * @return ESP_OK when queued, otherwise an admission error.
 */
esp_err_t app_manager_mailbox_post(void (*cb)(void *), void *arg);

/* ── 同步命令 ── */

/**
 * @brief 投递同步回调（阻塞等待回调执行完成后返回 result）
 * @param cb is the callback to execute.
 * @param arg is the opaque callback argument.
 * @param timeout_ms is the wait limit, or UINT32_MAX for an unbounded wait.
 * @return Callback result, timeout, or an admission error.
 *
 * @note  若当前线程 == LVGL 线程，直接调用 cb 不入队（重入保护）。
 *         否则入队后阻塞等待 done_sem，回调被执行后唤醒并返回 result。
 */
esp_err_t app_manager_mailbox_call(app_manager_ui_call_fn cb, void *arg,
                                   uint32_t timeout_ms);

/* ── UI dispatcher 导出 ── */

/**
 * @brief 获取 UI dispatch 函数指针（供 event_bus 注册）
 * @param out_fn 输出函数指针
 * @return ESP_OK when available, otherwise an ESP-IDF error.
 *
 * 返回的函数内部调用 app_manager_mailbox_post，满时返回 ESP_ERR_NO_MEM。
 */
esp_err_t app_manager_get_ui_dispatch_fn(app_manager_ui_dispatch_fn *out_fn);

#ifdef HOST_TEST
/**
 * @brief Pause or resume the host mailbox timer.
 * @param paused is true to pause timer delivery.
 */
void app_manager_mailbox_host_timer_pause(bool paused);
/** @brief Execute one host mailbox timer step. */
void app_manager_mailbox_host_timer_step(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_MAILBOX_H__ */
