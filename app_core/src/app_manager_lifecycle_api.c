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

esp_err_t app_manager_run(const char *app_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(app_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .app_id = app_id,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_run_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

esp_err_t app_manager_exit(const char *app_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(app_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .app_id = app_id,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_exit_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

void app_manager_self_exit(void)
{
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_self_exit_call,
                                      &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return;
}

esp_err_t app_manager_create_page_ext(const char *page_id,
                                      app_page_msg_cb_t handler,
                                      void *user_data, uint32_t memory_size)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(page_id) || handler == NULL)
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .page_id = page_id,
        .handler = handler,
        .user_data = user_data,
        .memory_size = memory_size,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_create_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

esp_err_t app_manager_create_page_for_app_ext(const char *app_id,
        const char *page_id, app_page_msg_cb_t handler, void *user_data,
        uint32_t memory_size)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(app_id) ||
            !app_manager_lifecycle_id_is_valid(page_id) || handler == NULL)
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .app_id = app_id,
        .page_id = page_id,
        .handler = handler,
        .user_data = user_data,
        .memory_size = memory_size,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_create_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

esp_err_t app_manager_goback(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_goback_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

esp_err_t app_manager_goback_to_page(const char *page_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(page_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .page_id = page_id,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_goback_to_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

esp_err_t app_manager_remove_page(const char *page_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(page_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .page_id = page_id,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_remove_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

void *app_manager_get_page_userdata(const char *page_id)
{
    void *user_data = NULL;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(page_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .page_id = page_id,
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_get_userdata_call,
                                      &request);
    user_data = request.value;

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return user_data;
}

esp_err_t app_manager_set_page_userdata(const char *page_id, void *user_data)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!app_manager_lifecycle_id_is_valid(page_id))
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        .page_id = page_id,
        .value = user_data,
    };
    result = _app_manager_lifecycle_call(app_manager_lifecycle_set_userdata_call,
                                         &request);

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return result;
}

bool app_manager_is_actived(const char *app_id)
{
    bool active = false;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
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
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return active;
}

const char *app_manager_get_active_app_id(void)
{
    const char *app_id = NULL;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_get_active_id_call,
                                      &request);
    app_id = request.value;

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return app_id;
}

bool app_page_is_actived(const char *app_id, const char *page_id)
{
    bool active = false;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
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
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return active;
}

bool app_manager_is_page_present(const char *app_id, const char *page_id)
{
    bool present = false;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        goto exit;
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
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return present;
}

bool app_manager_is_all_closed(void)
{
    bool all_closed = false;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        uint32_t paused_count = 0;
        if (app_manager_lifecycle_screen_running_snapshot(&paused_count))
        {
            all_closed = paused_count == 0U;
        }
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_all_closed_call,
                                      &request);
    all_closed = request.condition;

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    return all_closed;
}

uint32_t app_manager_get_running_apps(void)
{
    uint32_t count = 0;
    bool access_owned = _app_manager_lifecycle_access_begin();
    if (!access_owned)
    {
        (void)app_manager_lifecycle_screen_running_snapshot(&count);
        goto exit;
    }
    app_manager_lifecycle_request_t request =
    {
        0
    };
    (void)_app_manager_lifecycle_call(app_manager_lifecycle_running_count_call,
                                      &request);
    count = request.count;

exit:
    if (access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
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
