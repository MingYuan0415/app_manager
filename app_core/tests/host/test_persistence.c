#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_manager.h"
#include "nv_storage.h"
#include "nvs.h"

static const char *const s_expected_keys[] =
{
    "brightness",
    "pm_dim_to",
    "pm_timeout",
    "pm_stby_dly",
};
static bool s_present[sizeof(s_expected_keys) / sizeof(s_expected_keys[0])];
static size_t s_erase_calls;
static size_t s_fail_call;

esp_err_t nv_storage_erase_key(const char *key)
{
    assert(s_erase_calls < sizeof(s_expected_keys) /
           sizeof(s_expected_keys[0]));
    assert(strcmp(key, s_expected_keys[s_erase_calls]) == 0);
    const size_t call = s_erase_calls++;

    if (call == s_fail_call)
    {
        return ESP_FAIL;
    }
    if (!s_present[call])
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_present[call] = false;
    return ESP_OK;
}

static void _test_reset(bool present)
{
    for (size_t i = 0U; i < sizeof(s_present) / sizeof(s_present[0]); ++i)
    {
        s_present[i] = present;
    }
    s_erase_calls = 0U;
    s_fail_call = SIZE_MAX;
}

static void _test_erases_all_settings(void)
{
    _test_reset(true);
    assert(app_manager_factory_reset_persisted_state() == ESP_OK);
    assert(s_erase_calls == sizeof(s_present) / sizeof(s_present[0]));
    for (size_t i = 0U; i < sizeof(s_present) / sizeof(s_present[0]); ++i)
    {
        assert(!s_present[i]);
    }

    s_erase_calls = 0U;
    assert(app_manager_factory_reset_persisted_state() == ESP_OK);
    assert(s_erase_calls == sizeof(s_present) / sizeof(s_present[0]));
}

static void _test_stops_on_storage_failure(void)
{
    _test_reset(true);
    s_fail_call = 1U;
    assert(app_manager_factory_reset_persisted_state() == ESP_FAIL);
    assert(s_erase_calls == 2U);
    assert(!s_present[0]);
    assert(s_present[1]);
    assert(s_present[2]);
    assert(s_present[3]);
}

int main(void)
{
    _test_erases_all_settings();
    _test_stops_on_storage_failure();
    return 0;
}
