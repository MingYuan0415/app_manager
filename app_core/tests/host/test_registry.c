#include "app_manager.h"
#include "app_manager_builtin.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void _page_handler(app_manager_msg_type_t message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static const app_manager_app_desc_t s_valid_apps[] =
{
    {
        .name = "home",
        .id = "home",
        .root_page_id = "root",
        .flags = APP_MANAGER_APP_FLAG_PINNED,
        .type = APP_MANAGER_APP_BUILTIN,
    },
    {
        .name = "settings",
        .id = "settings",
        .root_page_id = "root",
        .type = APP_MANAGER_APP_BUILTIN,
    },
};

static const app_manager_page_desc_t s_valid_pages[] =
{
    {
        .app_id = "home",
        .page_id = "root",
        .handler = _page_handler,
        .memory_size = 8U,
    },
    {
        .app_id = "settings",
        .page_id = "root",
        .handler = _page_handler,
        .memory_size = 16U,
    },
    {
        .app_id = "settings",
        .page_id = "about",
        .handler = _page_handler,
        .memory_size = 24U,
    },
};

static void _register_valid(void)
{
    assert(app_manager_register_builtin_descriptors(
               s_valid_apps,
               sizeof(s_valid_apps) / sizeof(s_valid_apps[0]),
               s_valid_pages,
               sizeof(s_valid_pages) / sizeof(s_valid_pages[0])) == ESP_OK);
}

static void _test_valid_registry_and_leases(void)
{
    _register_valid();
    assert(app_manager_builtin_registry_validate(32U) == ESP_OK);
    assert(app_manager_builtin_app_count() == 2U);
    assert(app_manager_builtin_page_count() == 3U);
    assert(app_manager_builtin_enumerate(0) == &s_valid_apps[0]);
    assert(app_manager_builtin_enumerate(2) == NULL);
    assert(app_manager_builtin_enumerate_page(2) == &s_valid_pages[2]);
    assert(app_manager_builtin_find_by_id("settings") == &s_valid_apps[1]);
    assert(app_manager_builtin_find_page("settings", "about") ==
           &s_valid_pages[2]);

    app_manager_builtin_lease_t leases[APP_MANAGER_MAX_RESIDENT_APPS];
    for (size_t index = 0; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        assert(app_manager_builtin_acquire("settings", &leases[index]) ==
               ESP_OK);
        assert(app_manager_builtin_lease_app(&leases[index]) ==
               &s_valid_apps[1]);
        assert(app_manager_builtin_lease_find_page(&leases[index], "about") ==
               &s_valid_pages[2]);
    }
    app_manager_builtin_lease_t overflow;
    assert(app_manager_builtin_acquire("home", &overflow) == ESP_ERR_NO_MEM);
    assert(app_manager_builtin_registry_reset() == ESP_ERR_INVALID_STATE);
    assert(app_manager_register_builtin_descriptors(NULL, 0, NULL, 0) ==
           ESP_ERR_INVALID_STATE);

    app_manager_builtin_host_fail_releases(1U);
    const app_manager_builtin_lease_t preserved = leases[0];
    assert(app_manager_builtin_release(&leases[0]) == ESP_FAIL);
    assert(memcmp(&leases[0], &preserved, sizeof(preserved)) == 0);
    assert(app_manager_builtin_lease_app(&leases[0]) == &s_valid_apps[1]);
    assert(app_manager_builtin_registry_reset() == ESP_ERR_INVALID_STATE);

    app_manager_builtin_lease_t stale = leases[0];
    assert(app_manager_builtin_release(&leases[0]) == ESP_OK);
    assert(app_manager_builtin_release(&stale) == ESP_ERR_INVALID_STATE);
    for (size_t index = 1; index < APP_MANAGER_MAX_RESIDENT_APPS; index++)
    {
        assert(app_manager_builtin_release(&leases[index]) == ESP_OK);
    }
    assert(app_manager_builtin_registry_reset() == ESP_OK);
    assert(app_manager_builtin_find_by_id("home") == NULL);
}

static void _expect_invalid(
    const app_manager_app_desc_t *apps, size_t app_count,
    const app_manager_page_desc_t *pages, size_t page_count,
    size_t maximum_memory)
{
    assert(app_manager_register_builtin_descriptors(
               apps, app_count, pages, page_count) == ESP_OK);
    assert(app_manager_builtin_registry_validate(maximum_memory) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_builtin_app_count() == 0U);
}

static void _test_invalid_descriptors(void)
{
    const app_manager_app_desc_t duplicate_apps[] =
    {
        s_valid_apps[0],
        {
            .name = "duplicate",
            .id = "home",
            .root_page_id = "root",
            .type = APP_MANAGER_APP_BUILTIN,
        },
    };
    _expect_invalid(duplicate_apps, 2U, s_valid_pages, 3U, 32U);

    const app_manager_app_desc_t missing_root_app =
    {
        .name = "missing",
        .id = "missing",
        .root_page_id = "root",
        .type = APP_MANAGER_APP_BUILTIN,
    };
    const app_manager_page_desc_t unrelated_page =
    {
        .app_id = "missing",
        .page_id = "other",
        .handler = _page_handler,
    };
    _expect_invalid(&missing_root_app, 1U, &unrelated_page, 1U, 32U);

    app_manager_page_desc_t invalid_pages[] =
    {
        s_valid_pages[0],
        s_valid_pages[1],
        s_valid_pages[2],
    };
    invalid_pages[2].page_id = "root";
    _expect_invalid(s_valid_apps, 2U, invalid_pages, 3U, 32U);

    invalid_pages[2] = s_valid_pages[2];
    invalid_pages[2].handler = NULL;
    _expect_invalid(s_valid_apps, 2U, invalid_pages, 3U, 32U);

    invalid_pages[2] = s_valid_pages[2];
    invalid_pages[2].memory_size = 33U;
    _expect_invalid(s_valid_apps, 2U, invalid_pages, 3U, 32U);

    app_manager_app_desc_t dynamic_apps[] =
    {
        s_valid_apps[0],
        s_valid_apps[1],
    };
    dynamic_apps[1].type = APP_MANAGER_APP_DYNAMIC;
    _expect_invalid(dynamic_apps, 2U, s_valid_pages, 3U, 32U);

    assert(app_manager_register_builtin_descriptors(
               NULL, 1U, s_valid_pages, 3U) == ESP_ERR_INVALID_ARG);
    assert(app_manager_register_builtin_descriptors(
               s_valid_apps, 2U, NULL, 3U) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    _test_valid_registry_and_leases();
    _test_invalid_descriptors();
    return 0;
}
