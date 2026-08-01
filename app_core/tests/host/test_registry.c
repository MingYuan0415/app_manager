#include "app_manager.h"
#include "app_manager_builtin.h"

#include <assert.h>
#include <stddef.h>

static void _page_handler(app_manager_msg_type_t message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static const app_manager_page_definition_t s_shared_definition =
{
    .handler = _page_handler,
    .memory_size = 16U,
};

static const app_manager_page_definition_t s_about_definition =
{
    .handler = _page_handler,
    .memory_size = 24U,
};

static const app_manager_page_route_t s_home_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_shared_definition,
        .user_data = (void *)"home-root",
    },
};

static const app_manager_page_route_t s_settings_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_shared_definition,
        .user_data = (void *)"settings-root",
    },
    {
        .page_id = "about",
        .definition = &s_about_definition,
        .user_data = (void *)"settings-about",
    },
};

static const app_manager_app_desc_t s_valid_apps[] =
{
    {
        .name = "home",
        .id = "home",
        .root_page_id = "root",
        .page_routes = s_home_routes,
        .page_route_count = sizeof(s_home_routes) /
        sizeof(s_home_routes[0]),
        .flags = APP_MANAGER_APP_FLAG_PINNED,
    },
    {
        .name = "settings",
        .id = "settings",
        .root_page_id = "root",
        .page_routes = s_settings_routes,
        .page_route_count = sizeof(s_settings_routes) /
        sizeof(s_settings_routes[0]),
    },
};

static void _register_valid(void)
{
    assert(app_manager_register_builtin_descriptors(
               s_valid_apps,
               sizeof(s_valid_apps) / sizeof(s_valid_apps[0])) == ESP_OK);
}

static void _test_valid_registry_and_shared_definition(void)
{
    _register_valid();
    assert(app_manager_builtin_registry_validate(32U) == ESP_OK);
    assert(app_manager_builtin_app_count() == 2U);
    assert(app_manager_builtin_enumerate(0U) == &s_valid_apps[0]);
    assert(app_manager_builtin_enumerate(2U) == NULL);
    assert(app_manager_builtin_find_by_id("settings") == &s_valid_apps[1]);
    assert(app_manager_builtin_find_page(&s_valid_apps[1], "about") ==
           &s_settings_routes[1]);
    assert(app_manager_builtin_find_page(&s_valid_apps[0], "root")->definition ==
           app_manager_builtin_find_page(&s_valid_apps[1], "root")->definition);
    assert(app_manager_builtin_find_page(&s_valid_apps[0], "about") == NULL);

    const app_manager_app_desc_t unknown_app = s_valid_apps[0];
    assert(app_manager_builtin_find_page(&unknown_app, "root") == NULL);
    assert(app_manager_register_builtin_descriptors(NULL, 0U) ==
           ESP_ERR_INVALID_STATE);

    app_manager_builtin_registry_reset();
    assert(app_manager_builtin_find_by_id("home") == NULL);
    assert(app_manager_builtin_app_count() == 0U);
}

static void _expect_invalid(const app_manager_app_desc_t *apps,
                            size_t app_count, size_t maximum_memory)
{
    assert(app_manager_register_builtin_descriptors(apps, app_count) == ESP_OK);
    assert(app_manager_builtin_registry_validate(maximum_memory) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_builtin_app_count() == 0U);
}

static void _test_invalid_app_descriptors(void)
{
    const app_manager_app_desc_t duplicate_apps[] =
    {
        s_valid_apps[0],
        {
            .name = "duplicate",
            .id = "home",
            .root_page_id = "root",
            .page_routes = s_home_routes,
            .page_route_count = 1U,
        },
    };
    _expect_invalid(duplicate_apps, 2U, 32U);

    app_manager_app_desc_t invalid_app = s_valid_apps[0];
    invalid_app.page_routes = NULL;
    _expect_invalid(&invalid_app, 1U, 32U);

    invalid_app = s_valid_apps[0];
    invalid_app.page_route_count = 0U;
    _expect_invalid(&invalid_app, 1U, 32U);

    assert(app_manager_register_builtin_descriptors(NULL, 1U) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_register_builtin_descriptors(s_valid_apps, 0U) ==
           ESP_ERR_INVALID_ARG);
}

static void _test_invalid_routes(void)
{
    const app_manager_page_route_t missing_root_routes[] =
    {
        {
            .page_id = "other",
            .definition = &s_shared_definition,
        },
    };
    app_manager_app_desc_t app =
    {
        .name = "missing",
        .id = "missing",
        .root_page_id = "root",
        .page_routes = missing_root_routes,
        .page_route_count = 1U,
    };
    _expect_invalid(&app, 1U, 32U);

    app_manager_page_route_t invalid_routes[] =
    {
        s_settings_routes[0],
        s_settings_routes[1],
    };
    app = s_valid_apps[1];
    app.page_routes = invalid_routes;

    invalid_routes[1].page_id = "root";
    _expect_invalid(&app, 1U, 32U);

    invalid_routes[1] = s_settings_routes[1];
    invalid_routes[1].definition = NULL;
    _expect_invalid(&app, 1U, 32U);

    const app_manager_page_definition_t null_handler_definition =
    {
        .handler = NULL,
        .memory_size = 8U,
    };
    invalid_routes[1] = s_settings_routes[1];
    invalid_routes[1].definition = &null_handler_definition;
    _expect_invalid(&app, 1U, 32U);

    const app_manager_page_definition_t oversized_definition =
    {
        .handler = _page_handler,
        .memory_size = 33U,
    };
    invalid_routes[1] = s_settings_routes[1];
    invalid_routes[1].definition = &oversized_definition;
    _expect_invalid(&app, 1U, 32U);

    invalid_routes[1] = s_settings_routes[1];
    invalid_routes[1].page_id = "";
    _expect_invalid(&app, 1U, 32U);
}

int main(void)
{
    _test_valid_registry_and_shared_definition();
    _test_invalid_app_descriptors();
    _test_invalid_routes();
    return 0;
}
