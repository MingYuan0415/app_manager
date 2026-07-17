#ifndef __APP_MANAGER_BUILTIN_H__
#define __APP_MANAGER_BUILTIN_H__

#include "app_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Find a builtin application descriptor by identifier.
 *
 * @param app_id identifies the builtin application.
 *
 * @return Matching descriptor, or NULL when no descriptor matches.
 */
const app_builtin_app_desc_t *app_manager_builtin_find_by_id(
    const char *app_id);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_BUILTIN_H__ */
