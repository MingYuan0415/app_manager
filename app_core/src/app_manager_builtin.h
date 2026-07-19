#ifndef __APP_MANAGER_BUILTIN_H__
#define __APP_MANAGER_BUILTIN_H__

#include "app_manager_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Value handle for one provider-owned builtin descriptor lease. */
typedef struct app_manager_builtin_lease
{
    uint16_t slot;       /**< Provider slot; opaque to callers. */
    uint16_t reserved;   /**< Provider-owned validation value. */
    uint32_t generation; /**< Provider-owned stale-handle guard. */
} app_manager_builtin_lease_t;

/**
 * @brief Install descriptor arrays for a host fixture.
 *
 * @note This is an internal test seam, not a runtime provider-registration
 *       API. Nonempty firmware linker sections always take precedence.
 *
 * @param apps points to app_count descriptors valid for the manager lifetime.
 * @param app_count is the number of application descriptors.
 * @param pages points to page_count descriptors valid for the manager lifetime.
 * @param page_count is the number of page descriptors.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_register_builtin_descriptors(
    const app_manager_app_desc_t *apps, size_t app_count,
    const app_manager_page_desc_t *pages, size_t page_count);

/**
 * @brief Validate the selected builtin app and page registry.
 *
 * @param max_page_memory_size is the largest page-private allocation supported
 *                             by the configured fixed arena.
 * @return ESP_OK when all descriptors and roots are valid.
 */
esp_err_t app_manager_builtin_registry_validate(size_t max_page_memory_size);

/**
 * @brief Reset registry validation after all leases have been released.
 * @return ESP_OK when reset, or ESP_ERR_INVALID_STATE while leased.
 */
esp_err_t app_manager_builtin_registry_reset(void);

/**
 * @brief Return the validated builtin application count.
 * @return Descriptor count, or zero before successful validation.
 */
size_t app_manager_builtin_app_count(void);

/**
 * @brief Return the validated static page count.
 * @return Descriptor count, or zero before successful validation.
 */
size_t app_manager_builtin_page_count(void);

/**
 * @brief Enumerate one validated builtin application.
 * @param index is in [0, app_manager_builtin_app_count()).
 * @return Descriptor at index, or NULL when out of range.
 */
const app_manager_app_desc_t *app_manager_builtin_enumerate(size_t index);

/**
 * @brief Enumerate one validated static page.
 * @param index is in [0, app_manager_builtin_page_count()).
 * @return Descriptor at index, or NULL when out of range.
 */
const app_manager_page_desc_t *app_manager_builtin_enumerate_page(size_t index);

/**
 * @brief Find a validated builtin application by identifier.
 * @param app_id identifies the builtin application.
 * @return Matching descriptor, or NULL when no descriptor matches.
 */
const app_manager_app_desc_t *app_manager_builtin_find_by_id(const char *app_id);

/**
 * @brief Find a validated static page by its qualified identifier.
 * @param app_id identifies the owning application.
 * @param page_id identifies the page within the application.
 * @return Matching descriptor, or NULL when no descriptor matches.
 */
const app_manager_page_desc_t *app_manager_builtin_find_page(
    const char *app_id, const char *page_id);

/**
 * @brief Acquire one builtin application descriptor lease.
 * @param app_id identifies the application to acquire.
 * @param out_lease receives a value-owned lease handle.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_builtin_acquire(const char *app_id,
                                      app_manager_builtin_lease_t *out_lease);

/**
 * @brief Return the application descriptor protected by a lease.
 * @param lease is a live lease returned by app_manager_builtin_acquire().
 * @return Protected descriptor, or NULL for a stale or invalid lease.
 */
const app_manager_app_desc_t *app_manager_builtin_lease_app(
    const app_manager_builtin_lease_t *lease);

/**
 * @brief Find an application page protected by a lease.
 * @param lease is a live lease returned by app_manager_builtin_acquire().
 * @param page_id identifies a page in the leased application.
 * @return Matching page descriptor, or NULL when absent or invalid.
 */
const app_manager_page_desc_t *app_manager_builtin_lease_find_page(
    const app_manager_builtin_lease_t *lease, const char *page_id);

/**
 * @brief Release one builtin provider lease and invalidate the handle.
 * @note A failed release preserves the lease and provider ownership for retry.
 * @param lease is a live value-owned lease handle.
 * @return ESP_OK when released, otherwise ESP_ERR_INVALID_STATE.
 */
esp_err_t app_manager_builtin_release(app_manager_builtin_lease_t *lease);

#ifdef HOST_TEST
/**
 * @brief Inject retryable provider-release failures in host tests.
 * @param count is the number of subsequent valid releases that fail.
 */
void app_manager_builtin_host_fail_releases(size_t count);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_BUILTIN_H__ */
