#ifndef __APP_MANAGER_BUILTIN_H__
#define __APP_MANAGER_BUILTIN_H__

#include "app_manager_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install an application descriptor array for a host fixture.
 *
 * @note This is an internal test seam, not a runtime provider-registration
 *       API. Nonempty firmware linker sections always take precedence.
 *
 * @param apps points to app_count descriptors valid for the manager lifetime.
 * @param app_count is the number of application descriptors.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_register_builtin_descriptors(
    const app_manager_app_desc_t *apps, size_t app_count);

/**
 * @brief Validate the selected builtin app and page registry.
 *
 * @return ESP_OK when all descriptors and roots are valid.
 */
esp_err_t app_manager_builtin_registry_validate(void);

/**
 * @brief Reset registry validation after lifecycle teardown.
 */
void app_manager_builtin_registry_reset(void);

/**
 * @brief Return the validated builtin application count.
 * @return Descriptor count, or zero before successful validation.
 */
size_t app_manager_builtin_app_count(void);

/**
 * @brief Enumerate one validated builtin application.
 * @param index is in [0, app_manager_builtin_app_count()).
 * @return Descriptor at index, or NULL when out of range.
 */
const app_manager_app_desc_t *app_manager_builtin_enumerate(size_t index);

/**
 * @brief Find a validated builtin application by identifier.
 * @param app_id identifies the builtin application.
 * @return Matching descriptor, or NULL when no descriptor matches.
 */
const app_manager_app_desc_t *app_manager_builtin_find_by_id(const char *app_id);

/**
 * @brief Find a validated static page by its qualified identifier.
 * @param app is a validated builtin application descriptor.
 * @param page_id identifies the page within the application.
 * @return Matching route, or NULL when no route matches.
 */
const app_manager_page_route_t *app_manager_builtin_find_page(
    const app_manager_app_desc_t *app, const char *page_id);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_BUILTIN_H__ */
