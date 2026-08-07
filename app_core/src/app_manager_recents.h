#ifndef __APP_MANAGER_RECENTS_H__
#define __APP_MANAGER_RECENTS_H__

#include "app_manager_types.h"

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate the fixed recent-tasks preview cache.
 *
 * @note UI-worker context only. Safe to call more than once; fails silently
 *       on PSRAM exhaustion and leaves the cache disabled.
 *
 * @return ESP_OK when every preview slot is allocated, otherwise an error.
 */
esp_err_t app_manager_recents_init(void);
/**
 * @brief Release every preview slot.
 *
 * @note UI-worker context only. Safe when the cache is disabled.
 */
void app_manager_recents_deinit(void);
/**
 * @brief Downsample a full-screen source snapshot into the task preview.
 *
 * @note UI-worker context only. The binding is created or updated and moved
 *       to the most recently used position. No-op when the cache is disabled.
 *
 * @param app_id is the stable application identifier.
 * @param source is the full-resolution RGB565 snapshot draw buffer.
 */
void app_manager_recents_capture(const char *app_id,
                                 const lv_draw_buf_t *source);
/**
 * @brief Clear the binding and validity of one task preview.
 *
 * @note UI-worker context only. Slot memory is retained for reuse.
 *
 * @param app_id is the stable application identifier.
 */
void app_manager_recents_invalidate(const char *app_id);
/**
 * @brief Return the captured preview for one task.
 *
 * @note UI-worker context only.
 *
 * @param app_id is the stable application identifier.
 * @param preview receives the fixed preview draw buffer when valid.
 * @return true when a captured preview is available.
 */
bool app_manager_recents_lookup(const char *app_id,
                                const lv_draw_buf_t **preview);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_RECENTS_H__ */
