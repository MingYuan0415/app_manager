#include "app_manager.h"

#include "app_manager_lifecycle.h"
#include "app_manager_lifecycle_internal.h"

#include <stdint.h>
#ifdef HOST_TEST
    #include <stdatomic.h>
#endif

#define APP_MANAGER_UI_CALL_TIMEOUT_MS UINT32_MAX

#ifdef HOST_TEST
    static _Atomic(app_manager_lifecycle_admission_hook_t) s_host_admission_hook;
#endif

static bool _app_manager_lifecycle_access_begin(void)
{
    bool admitted = app_manager_lifecycle_screen_access_begin();
#ifdef HOST_TEST
    app_manager_lifecycle_admission_hook_t hook =
        atomic_load(&s_host_admission_hook);
    if (admitted && hook != NULL)
    {
        hook();
    }
#endif
    return admitted;
}

static esp_err_t _app_manager_lifecycle_call(
    app_manager_ui_call_fn callback,
    app_manager_lifecycle_request_t *request)
{
    return app_manager_ui_call(callback, request, APP_MANAGER_UI_CALL_TIMEOUT_MS);
}

bool app_manager_is_actived(const char *app_id)
{
    bool active = false;
    if (!_app_manager_lifecycle_access_begin())
    {
        return false;
    }
    if (!app_manager_lifecycle_id_is_valid(app_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .app_id = app_id,
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_is_active_call,
                                      &request);
    active = request.condition;

exit:
    app_manager_lifecycle_screen_access_end();
    return active;
}

const char *app_manager_get_active_app_id(void)
{
    if (!_app_manager_lifecycle_access_begin())
    {
        return NULL;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_get_active_id_call,
                                      &request);
    const char *app_id = request.value;
    app_manager_lifecycle_screen_access_end();
    return app_id;
}

bool app_page_is_actived(const char *app_id, const char *page_id)
{
    bool active = false;
    if (!_app_manager_lifecycle_access_begin())
    {
        return false;
    }
    if (!app_manager_lifecycle_id_is_valid(app_id) ||
            !app_manager_lifecycle_id_is_valid(page_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .app_id = app_id,
        .page_id = page_id,
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_page_active_call,
                                      &request);
    active = request.condition;

exit:
    app_manager_lifecycle_screen_access_end();
    return active;
}

bool app_manager_is_page_present(const char *app_id, const char *page_id)
{
    bool present = false;
    if (!_app_manager_lifecycle_access_begin())
    {
        return false;
    }
    if (!app_manager_lifecycle_id_is_valid(app_id) ||
            !app_manager_lifecycle_id_is_valid(page_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .app_id = app_id,
        .page_id = page_id,
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_page_present_call,
                                      &request);
    present = request.condition;

exit:
    app_manager_lifecycle_screen_access_end();
    return present;
}

bool app_manager_is_all_closed(void)
{
    if (!_app_manager_lifecycle_access_begin())
    {
        uint32_t paused_count = 0;
        if (app_manager_lifecycle_screen_running_snapshot(&paused_count))
        {
            return paused_count == 0U;
        }
        return false;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_all_closed_call,
                                      &request);
    bool all_closed = request.condition;
    app_manager_lifecycle_screen_access_end();
    return all_closed;
}

uint32_t app_manager_get_running_apps(void)
{
    if (!_app_manager_lifecycle_access_begin())
    {
        uint32_t count = 0;
        (void)app_manager_lifecycle_screen_running_snapshot(&count);
        return count;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_running_count_call,
                                      &request);
    uint32_t count = request.count;
    app_manager_lifecycle_screen_access_end();
    return count;
}

const lv_font_t *app_manager_get_font(app_theme_font_id_t id)
{
    return app_theme_get_font(id);
}

#ifdef HOST_TEST
void app_manager_lifecycle_host_set_admission_hook(
    app_manager_lifecycle_admission_hook_t hook)
{
    atomic_store(&s_host_admission_hook, hook);
}
#endif
