/** @file Host resource-mapping declarations used by App Manager tests. */
#ifndef __HOST_ESP_MMAP_ASSETS_H__
#define __HOST_ESP_MMAP_ASSETS_H__

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** @brief Opaque fake resource-assets handle. */
typedef struct mmap_assets_t *mmap_assets_handle_t;

/** @brief Fake resource-assets mount configuration. */
typedef struct mmap_assets_config
{
    const char *partition_label;
    int max_files;
    uint32_t checksum;
    struct mmap_assets_flags
    {
        unsigned int mmap_enable : 1;
        unsigned int use_fs : 1;
        unsigned int app_bin_check : 1;
        unsigned int full_check : 1;
        unsigned int metadata_check : 1;
        unsigned int reserved : 27;
    } flags;
} mmap_assets_config_t;

/** @brief Create a fake resource-assets mapping. */
esp_err_t mmap_assets_new(const mmap_assets_config_t *config,
                          mmap_assets_handle_t *ret_item);
/** @brief Release a fake resource-assets mapping. */
esp_err_t mmap_assets_del(mmap_assets_handle_t handle);
/** @brief Return one fake asset memory pointer. */
const uint8_t *mmap_assets_get_mem(mmap_assets_handle_t handle, int index);
/** @brief Return one fake asset size. */
int mmap_assets_get_size(mmap_assets_handle_t handle, int index);
/** @brief Return the fake stored-file count. */
int mmap_assets_get_stored_files(mmap_assets_handle_t handle);

#endif /* __HOST_ESP_MMAP_ASSETS_H__ */
