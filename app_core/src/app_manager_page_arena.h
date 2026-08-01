#ifndef APP_MANAGER_PAGE_ARENA_H
#define APP_MANAGER_PAGE_ARENA_H

#include "app_manager_types.h"

#include <stddef.h>

/** Default total storage reserved for page-private instance memory. */
#define APP_MANAGER_PAGE_ARENA_DEFAULT_BYTES (32U * 1024U)

/**
 * @brief Resolve the configured total page arena size.
 * @param arena_bytes is the configured size, or zero to select the default.
 * @return Effective total arena size in bytes.
 */
static inline size_t app_manager_page_arena_resolve_bytes(size_t arena_bytes)
{
    return arena_bytes == 0U ? APP_MANAGER_PAGE_ARENA_DEFAULT_BYTES :
           arena_bytes;
}

/**
 * @brief Calculate the aligned private-memory capacity of one page slot.
 * @param arena_bytes is the configured total size, or zero for the default.
 * @return Eight-byte-aligned capacity of one fixed page slot.
 */
static inline size_t app_manager_page_arena_stride(size_t arena_bytes)
{
    const size_t resolved_bytes =
        app_manager_page_arena_resolve_bytes(arena_bytes);
    return (resolved_bytes / APP_MANAGER_MAX_PAGE_INSTANCES) & ~(size_t)7U;
}

#endif /* APP_MANAGER_PAGE_ARENA_H */
