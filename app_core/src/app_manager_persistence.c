#define DBG_TAG "app_persistence"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_manager.h"
#include "app_manager_persistence.h"

#include "nv_storage.h"
#include "nvs.h"

#include <stddef.h>

esp_err_t app_manager_factory_reset_persisted_state(void)
{
    static const char *const keys[] =
    {
        NVS_KEY_BRIGHTNESS,
        NVS_KEY_DIM_TIMEOUT,
        NVS_KEY_OFF_TIMEOUT,
        NVS_KEY_STANDBY_TIMEOUT,
    };

    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i)
    {
        const esp_err_t result = nv_storage_erase_key(keys[i]);

        if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
        {
            return result;
        }
    }
    return ESP_OK;
}
