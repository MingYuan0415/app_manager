#include "host_freertos.h"
#include "app_manager.h"
#include "../../src/app_manager_lifecycle.h"
#include "../../src/app_manager_lifecycle_internal.h"
#include "../../src/app_manager_mailbox.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct trace
{
    int start;
    int resume;
    int pause;
    int stop;
} trace_t;

typedef struct observer_event
{
    const char *app_id;
    const char *page_id;
    app_manager_msg_type_t message;
    app_manager_lifecycle_observer_phase_t phase;
} observer_event_t;

static trace_t s_a_root;
static trace_t s_b_root;
static trace_t s_c_root;
static trace_t s_fail_root;
static trace_t s_child;
static trace_t s_generic;
static trace_t s_nested_root;
static trace_t s_cleanup_root;
static bool s_defer_child;
static bool s_child_goback_on_pause;
static bool s_self_exit_on_stop;
static bool s_fail_run_b_on_stop;
static bool s_expect_private_memory;
static bool s_expect_zero_memory;
static bool s_attempt_navigation_during_teardown;
static int s_teardown_run_rejections;
static int s_teardown_page_rejections;
static size_t s_a_memory_size = 64;
static size_t s_large_memory_size = 3000;
static int s_cleanup_failures_remaining;
static uint8_t *s_cleanup_page_memory;
static bool s_cleanup_resource_live;
static bool s_expect_cleanup_zero_memory;
static observer_event_t s_observer_events[8];
static size_t s_observer_event_count;
static atomic_bool s_admission_hook_enabled;
static atomic_bool s_admission_reached;
static atomic_bool s_admission_release;
static atomic_bool s_pause_drain_started;
static atomic_bool s_pause_drain_complete;
static atomic_bool s_shutdown_complete;
static atomic_int s_concurrent_api_result;
static atomic_int s_pause_drain_result;
static atomic_int s_shutdown_result;

static void _generic_handler(app_manager_msg_type_t message, void *param);

static void _lifecycle_observer(
    const char *app_id, const char *page_id, app_manager_msg_type_t message,
    app_manager_lifecycle_observer_phase_t phase)
{
    assert(s_observer_event_count <
           sizeof(s_observer_events) / sizeof(s_observer_events[0]));
    observer_event_t *event = &s_observer_events[s_observer_event_count++];
    event->app_id = app_id;
    event->page_id = page_id;
    event->message = message;
    event->phase = phase;
}

static void _admission_hook(void)
{
    if (!atomic_load(&s_admission_hook_enabled))
    {
        return;
    }
    atomic_store(&s_admission_reached, true);
    while (!atomic_load(&s_admission_release))
    {
        usleep(1000);
    }
}

static void _wait_for_flag(atomic_bool *flag)
{
    for (size_t attempt = 0; attempt < 2000U; attempt++)
    {
        if (atomic_load(flag))
        {
            return;
        }
        usleep(1000);
    }
    assert(atomic_load(flag));
}

esp_err_t app_theme_set_font(app_theme_font_id_t id, const lv_font_t *font)
{
    (void)id;
    (void)font;
    return ESP_OK;
}

const lv_font_t *app_theme_get_font(app_theme_font_id_t id)
{
    (void)id;
    return NULL;
}

static void _trace_message(trace_t *trace, app_manager_msg_type_t message)
{
    switch (message)
    {
    case APP_MANAGER_MSG_ONSTART:
        trace->start++;
        break;
    case APP_MANAGER_MSG_ONRESUME:
        trace->resume++;
        break;
    case APP_MANAGER_MSG_ONPAUSE:
        trace->pause++;
        break;
    case APP_MANAGER_MSG_ONSTOP:
        trace->stop++;
        break;
    case APP_MANAGER_MSG_USER_END:
        break;
    }
}

static void _attempt_teardown_navigation(app_manager_msg_type_t message)
{
    if (!s_attempt_navigation_during_teardown ||
            (message != APP_MANAGER_MSG_ONPAUSE &&
             message != APP_MANAGER_MSG_ONSTOP))
    {
        return;
    }

    assert(app_manager_run("a") == ESP_ERR_INVALID_STATE);
    s_teardown_run_rejections++;
    assert(app_manager_create_page_ext("teardown-child", _generic_handler,
                                       NULL, 0) == ESP_ERR_INVALID_STATE);
    s_teardown_page_rejections++;
}

static void _child_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_child, message);
    if (message == APP_MANAGER_MSG_ONSTART)
    {
        uint8_t *memory = app_manager_this_page_memory();
        assert(memory != NULL);
        assert(((uintptr_t)memory & 7U) == 0);
        if (s_expect_zero_memory)
        {
            for (size_t i = 0; i < 64; i++)
            {
                assert(memory[i] == 0);
            }
        }
        memset(memory, 0xA5, 64);
    }
    if (message == APP_MANAGER_MSG_ONPAUSE && s_child_goback_on_pause)
    {
        s_child_goback_on_pause = false;
        assert(app_manager_goback() == ESP_OK);
    }
}

static void _a_root_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_a_root, message);
    _attempt_teardown_navigation(message);
    if (message == APP_MANAGER_MSG_ONSTART)
    {
        void *memory = app_manager_this_page_memory();
        assert((memory != NULL) == (s_a_memory_size > 0));
        if (memory != NULL)
        {
            assert(((uintptr_t)memory & 7U) == 0);
        }
    }
    if (message == APP_MANAGER_MSG_ONRESUME && s_defer_child)
    {
        s_defer_child = false;
        s_expect_zero_memory = true;
        assert(app_manager_create_page_ext("child", _child_handler, NULL, 64) == ESP_OK);
    }
    if (message == APP_MANAGER_MSG_ONSTOP && s_self_exit_on_stop)
    {
        s_self_exit_on_stop = false;
        app_manager_self_exit();
    }
}

static void _b_root_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_b_root, message);
    _attempt_teardown_navigation(message);
    if (message == APP_MANAGER_MSG_ONSTART)
    {
        assert(app_manager_this_page_memory() == NULL);
    }
}

static void _c_root_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_c_root, message);
    _attempt_teardown_navigation(message);
}

static void _fail_root_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_fail_root, message);
    if (message == APP_MANAGER_MSG_ONSTOP && s_fail_run_b_on_stop)
    {
        s_fail_run_b_on_stop = false;
        assert(app_manager_run("b") == ESP_OK);
    }
}

static void _generic_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_generic, message);
    if (message == APP_MANAGER_MSG_ONSTART)
    {
        assert((app_manager_this_page_memory() != NULL) == s_expect_private_memory);
    }
}

static void _nested_root_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_nested_root, message);
}

static void _cleanup_root_handler(app_manager_msg_type_t message, void *param)
{
    (void)param;
    _trace_message(&s_cleanup_root, message);
    uint8_t *memory = app_manager_this_page_memory();
    assert(memory != NULL);
    if (message == APP_MANAGER_MSG_ONSTART)
    {
        if (s_expect_cleanup_zero_memory)
        {
            for (size_t index = 0; index < 64U; index++)
            {
                assert(memory[index] == 0U);
            }
        }
        s_cleanup_page_memory = memory;
        memset(memory, 0xA5, 64U);
        s_cleanup_resource_live = true;
    }
    else if (message == APP_MANAGER_MSG_ONPAUSE)
    {
        assert(memory == s_cleanup_page_memory);
        if (s_cleanup_failures_remaining > 0)
        {
            s_cleanup_failures_remaining--;
            app_manager_this_page_report_cleanup_error(ESP_FAIL);
        }
        else
        {
            s_cleanup_resource_live = false;
        }
    }
}

static void _cleanup_resource_callback(void)
{
    assert(s_cleanup_resource_live);
    assert(s_cleanup_page_memory != NULL);
    s_cleanup_page_memory[0]++;
}

static esp_err_t _entry_a(void)
{
    return app_manager_regist_msg_handler_ext("root", _a_root_handler,
            NULL, (uint32_t)s_a_memory_size);
}

static esp_err_t _entry_b(void)
{
    return app_manager_regist_msg_handler_ext("root", _b_root_handler, NULL, 0);
}

static esp_err_t _entry_c(void)
{
    return app_manager_regist_msg_handler_ext("root", _c_root_handler, NULL, 0);
}

static esp_err_t _entry_fail(void)
{
    esp_err_t result = app_manager_regist_msg_handler_ext("root",
                       _fail_root_handler,
                       NULL, 0);
    return result == ESP_OK ? ESP_FAIL : result;
}

static esp_err_t _entry_empty(void)
{
    return ESP_OK;
}

static esp_err_t _entry_large(void)
{
    return app_manager_regist_msg_handler_ext("root", _generic_handler, NULL,
            (uint32_t)s_large_memory_size);
}

static esp_err_t _entry_nested(void)
{
    esp_err_t result = app_manager_run("b");
    if (result != ESP_OK)
    {
        return result;
    }
    return app_manager_regist_msg_handler_ext("root", _nested_root_handler,
            NULL, 0);
}

static esp_err_t _entry_cleanup(void)
{
    return app_manager_regist_msg_handler_ext(
               "root", _cleanup_root_handler, NULL, 64U);
}

static const app_builtin_app_desc_t s_apps[] =
{
    {.name = "A", .id = "a", .entry = _entry_a, .type = APP_MANAGER_APP_BUILTIN},
    {.name = "B", .id = "b", .entry = _entry_b, .type = APP_MANAGER_APP_BUILTIN},
    {.name = "C", .id = "c", .entry = _entry_c, .type = APP_MANAGER_APP_BUILTIN},
    {.name = "Fail", .id = "fail", .entry = _entry_fail, .type = APP_MANAGER_APP_BUILTIN},
    {.name = "Empty", .id = "empty", .entry = _entry_empty, .type = APP_MANAGER_APP_BUILTIN},
    {.name = "Large", .id = "large", .entry = _entry_large, .type = APP_MANAGER_APP_BUILTIN},
    {.name = "Nested", .id = "nested", .entry = _entry_nested, .type = APP_MANAGER_APP_BUILTIN},
    {
        .name = "Cleanup", .id = "cleanup", .entry = _entry_cleanup,
        .type = APP_MANAGER_APP_BUILTIN
    },
};

static void _reset_traces(void)
{
    memset(&s_a_root, 0, sizeof(s_a_root));
    memset(&s_b_root, 0, sizeof(s_b_root));
    memset(&s_c_root, 0, sizeof(s_c_root));
    memset(&s_fail_root, 0, sizeof(s_fail_root));
    memset(&s_child, 0, sizeof(s_child));
    memset(&s_generic, 0, sizeof(s_generic));
    memset(&s_nested_root, 0, sizeof(s_nested_root));
    memset(&s_cleanup_root, 0, sizeof(s_cleanup_root));
    s_defer_child = false;
    s_child_goback_on_pause = false;
    s_self_exit_on_stop = false;
    s_fail_run_b_on_stop = false;
    s_expect_private_memory = false;
    s_expect_zero_memory = false;
    s_attempt_navigation_during_teardown = false;
    s_teardown_run_rejections = 0;
    s_teardown_page_rejections = 0;
    s_cleanup_failures_remaining = 0;
    s_cleanup_page_memory = NULL;
    s_cleanup_resource_live = false;
    s_expect_cleanup_zero_memory = false;
    memset(s_observer_events, 0, sizeof(s_observer_events));
    s_observer_event_count = 0;
}

static esp_err_t _lifecycle_deinit_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_deinit();
}

static esp_err_t _screen_pause_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_screen_pause();
}

static esp_err_t _screen_resume_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_screen_resume();
}

static esp_err_t _screen_pause(void)
{
    return app_manager_ui_call(_screen_pause_on_ui, NULL, UINT32_MAX);
}

static esp_err_t _screen_resume(void)
{
    return app_manager_ui_call(_screen_resume_on_ui, NULL, UINT32_MAX);
}

static esp_err_t _lifecycle_deinit(void)
{
    esp_err_t result = app_manager_lifecycle_shutdown_begin_and_wait();
    if (result == ESP_OK)
    {
        result = app_manager_ui_call(_lifecycle_deinit_on_ui, NULL,
                                     UINT32_MAX);
    }
    return result;
}

static void *_concurrent_run_thread(void *arg)
{
    (void)arg;
    atomic_store(&s_concurrent_api_result, app_manager_run("b"));
    return NULL;
}

static void *_screen_pause_and_drain_thread(void *arg)
{
    (void)arg;
    esp_err_t result = _screen_pause();
    if (result == ESP_OK)
    {
        atomic_store(&s_pause_drain_started, true);
        result = app_manager_lifecycle_screen_access_wait();
    }
    atomic_store(&s_pause_drain_result, result);
    atomic_store(&s_pause_drain_complete, true);
    return NULL;
}

static void *_lifecycle_shutdown_thread(void *arg)
{
    (void)arg;
    esp_err_t result = app_manager_lifecycle_shutdown_begin_and_wait();
    if (result == ESP_OK)
    {
        result = app_manager_mailbox_shutdown_call(
                     _lifecycle_deinit_on_ui, NULL);
    }
    atomic_store(&s_shutdown_result, result);
    atomic_store(&s_shutdown_complete, true);
    return NULL;
}

static esp_err_t _expect_serial_mutations_rejected_on_ui(void *arg)
{
    (void)arg;
    app_manager_lifecycle_request_t request =
    {
        .app_id = "a",
        .page_id = "root",
        .handler = _generic_handler,
    };

    assert(app_manager_lifecycle_run_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_exit_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_self_exit_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_create_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_goback_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_goback_to_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_remove_call(&request) == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_set_userdata_call(&request) == ESP_ERR_INVALID_STATE);

    request.condition = true;
    assert(app_manager_lifecycle_is_active_call(&request) == ESP_OK);
    assert(!request.condition);
    request.value = (void *)1;
    assert(app_manager_lifecycle_get_active_id_call(&request) == ESP_OK);
    assert(request.value == NULL);
    request.condition = true;
    assert(app_manager_lifecycle_page_active_call(&request) == ESP_OK);
    assert(!request.condition);
    request.condition = false;
    assert(app_manager_lifecycle_page_present_call(&request) == ESP_OK);
    assert(request.condition);
    request.count = 0;
    assert(app_manager_lifecycle_running_count_call(&request) == ESP_OK);
    assert(request.count == 1U);
    return ESP_OK;
}

static void _test_screen_pause_without_app(void)
{
    assert(app_manager_lifecycle_screen_pause() == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_screen_resume() == ESP_ERR_INVALID_STATE);
    assert(app_manager_lifecycle_screen_mutation_allowed());

    assert(_screen_pause() == ESP_OK);
    assert(_screen_pause() == ESP_OK);
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    assert(!app_manager_is_actived("a"));
    assert(app_manager_get_active_app_id() == NULL);
    assert(app_manager_get_running_apps() == 0U);
    assert(app_manager_is_all_closed());

    assert(_screen_resume() == ESP_OK);
    assert(_screen_resume() == ESP_OK);
    assert(app_manager_lifecycle_screen_mutation_allowed());
}

static void _test_lifecycle_observer(void)
{
    _reset_traces();
    app_manager_lifecycle_host_set_observer(_lifecycle_observer);
    assert(app_manager_run("a") == ESP_OK);
    app_manager_lifecycle_host_set_observer(NULL);

    assert(s_observer_event_count == 4U);
    for (size_t index = 0; index < s_observer_event_count; index++)
    {
        assert(strcmp(s_observer_events[index].app_id, "a") == 0);
        assert(strcmp(s_observer_events[index].page_id, "root") == 0);
        assert(s_observer_events[index].phase ==
               (index % 2U == 0U ? APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE :
                APP_MANAGER_LIFECYCLE_OBSERVER_AFTER));
    }
    assert(s_observer_events[0].message == APP_MANAGER_MSG_ONSTART);
    assert(s_observer_events[1].message == APP_MANAGER_MSG_ONSTART);
    assert(s_observer_events[2].message == APP_MANAGER_MSG_ONRESUME);
    assert(s_observer_events[3].message == APP_MANAGER_MSG_ONRESUME);
    assert(app_manager_exit("a") == ESP_OK);
}

static void _test_pause_drains_admitted_api(void)
{
    _reset_traces();
    assert(app_manager_run("a") == ESP_OK);
    atomic_store(&s_admission_hook_enabled, true);
    atomic_store(&s_admission_reached, false);
    atomic_store(&s_admission_release, false);
    atomic_store(&s_pause_drain_started, false);
    atomic_store(&s_pause_drain_complete, false);
    atomic_store(&s_concurrent_api_result, ESP_FAIL);
    atomic_store(&s_pause_drain_result, ESP_FAIL);
    app_manager_lifecycle_host_set_admission_hook(_admission_hook);

    pthread_t api_thread;
    pthread_t pause_thread;
    assert(pthread_create(&api_thread, NULL, _concurrent_run_thread,
                          NULL) == 0);
    _wait_for_flag(&s_admission_reached);
    assert(pthread_create(&pause_thread, NULL,
                          _screen_pause_and_drain_thread, NULL) == 0);
    _wait_for_flag(&s_pause_drain_started);
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    usleep(10000);
    assert(!atomic_load(&s_pause_drain_complete));

    atomic_store(&s_admission_release, true);
    assert(pthread_join(api_thread, NULL) == 0);
    assert(pthread_join(pause_thread, NULL) == 0);
    atomic_store(&s_admission_hook_enabled, false);
    app_manager_lifecycle_host_set_admission_hook(NULL);

    assert(atomic_load(&s_concurrent_api_result) == ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_pause_drain_result) == ESP_OK);
    assert(_screen_resume() == ESP_OK);
    assert(app_manager_get_running_apps() == 1U);
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_exit("a") == ESP_OK);
}

static void _test_screen_pause_lifecycle(void)
{
    _reset_traces();
    int original_user_data = 1;
    int rejected_user_data = 2;
    assert(app_manager_run("a") == ESP_OK);
    assert(app_manager_set_page_userdata("root", &original_user_data) == ESP_OK);

    s_attempt_navigation_during_teardown = true;
    assert(_screen_pause() == ESP_OK);
    s_attempt_navigation_during_teardown = false;
    assert(_screen_pause() == ESP_OK);
    assert(s_a_root.start == 1 && s_a_root.resume == 1 &&
           s_a_root.pause == 1 && s_a_root.stop == 0);
    assert(s_teardown_run_rejections == 1);
    assert(s_teardown_page_rejections == 1);
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    app_manager_mailbox_host_timer_pause(true);
    assert(!app_manager_is_actived("a"));
    assert(app_manager_get_active_app_id() == NULL);
    assert(!app_page_is_actived("a", "root"));
    assert(!app_manager_is_page_present("a", "root"));
    assert(!app_manager_is_all_closed());
    assert(app_manager_get_running_apps() == 1U);

    assert(app_manager_run("b") == ESP_ERR_INVALID_STATE);
    assert(app_manager_exit("a") == ESP_ERR_INVALID_STATE);
    app_manager_self_exit();
    assert(app_manager_create_page_ext("child", _generic_handler,
                                       NULL, 0) == ESP_ERR_INVALID_STATE);
    assert(app_manager_create_page_for_app_ext(
               "a", "child", _generic_handler, NULL,
               0) == ESP_ERR_INVALID_STATE);
    assert(app_manager_goback() == ESP_ERR_INVALID_STATE);
    assert(app_manager_goback_to_page("root") == ESP_ERR_INVALID_STATE);
    assert(app_manager_remove_page("root") == ESP_ERR_INVALID_STATE);
    assert(app_manager_set_page_userdata(
               "root", &rejected_user_data) == ESP_ERR_INVALID_STATE);
    assert(app_manager_get_page_userdata("root") == NULL);
    assert(app_manager_get_running_apps() == 1U);
    app_manager_mailbox_host_timer_pause(false);
    assert(app_manager_ui_call(_expect_serial_mutations_rejected_on_ui, NULL,
                               UINT32_MAX) == ESP_OK);

    assert(_screen_resume() == ESP_OK);
    assert(_screen_resume() == ESP_OK);
    assert(app_manager_lifecycle_screen_mutation_allowed());
    assert(app_manager_is_actived("a"));
    assert(app_page_is_actived("a", "root"));
    assert(s_a_root.start == 1 && s_a_root.resume == 2 &&
           s_a_root.pause == 1 && s_a_root.stop == 0);
    assert(app_manager_get_page_userdata("root") == &original_user_data);

    s_defer_child = true;
    assert(_screen_pause() == ESP_OK);
    assert(_screen_resume() == ESP_OK);
    assert(app_page_is_actived("a", "child"));
    assert(s_a_root.start == 1);
    assert(s_child.start == 1 && s_child.resume == 1);
    assert(app_manager_exit("a") == ESP_OK);
    assert(app_manager_is_all_closed());
}

static void _test_screen_pause_cleanup_retry(void)
{
    _reset_traces();
    s_cleanup_failures_remaining = 1;
    s_expect_cleanup_zero_memory = true;
    assert(app_manager_run("cleanup") == ESP_OK);
    uint8_t *retained_memory = s_cleanup_page_memory;

    assert(_screen_pause() == ESP_FAIL);
    assert(app_manager_lifecycle_screen_mutation_allowed());
    assert(app_manager_is_actived("cleanup"));
    assert(app_page_is_actived("cleanup", "root"));
    assert(s_cleanup_resource_live);
    assert(s_cleanup_root.pause == 1);

    assert(_screen_pause() == ESP_OK);
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    assert(!s_cleanup_resource_live);
    assert(s_cleanup_page_memory == retained_memory);
    assert(!app_manager_is_page_present("cleanup", "root"));
    assert(app_manager_get_running_apps() == 1U);
    assert(s_cleanup_root.start == 1 && s_cleanup_root.resume == 1 &&
           s_cleanup_root.pause == 2 && s_cleanup_root.stop == 0);

    assert(_screen_resume() == ESP_OK);
    assert(s_cleanup_root.start == 1 && s_cleanup_root.resume == 2);
    assert(app_manager_is_page_present("cleanup", "root"));
    assert(app_manager_get_running_apps() == 1U);
    assert(app_manager_exit("cleanup") == ESP_OK);
}

static void _test_paused_deinit_stops_without_resume(void)
{
    _reset_traces();
    assert(app_manager_run("a") == ESP_OK);
    assert(_screen_pause() == ESP_OK);
    assert(s_a_root.start == 1 && s_a_root.resume == 1 &&
           s_a_root.pause == 1 && s_a_root.stop == 0);

    assert(_lifecycle_deinit() == ESP_OK);
    assert(s_a_root.start == 1 && s_a_root.resume == 1 &&
           s_a_root.pause == 1 && s_a_root.stop == 1);
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    assert(app_manager_lifecycle_init(0) == ESP_OK);
}

static void _test_cleanup_failure_retains_page_slot(void)
{
    _reset_traces();
    s_cleanup_failures_remaining = 3;
    s_expect_cleanup_zero_memory = true;
    assert(app_manager_run("cleanup") == ESP_OK);
    uint8_t *retained_memory = s_cleanup_page_memory;
    assert(retained_memory != NULL);

    for (int attempt = 0; attempt < 3; attempt++)
    {
        assert(app_manager_exit("cleanup") == ESP_FAIL);
        assert(app_manager_is_actived("cleanup"));
        assert(app_manager_is_page_present("cleanup", "root"));
        assert(app_manager_get_running_apps() == 1U);
        assert(s_cleanup_page_memory == retained_memory);
        _cleanup_resource_callback();
    }

    assert(app_manager_exit("cleanup") == ESP_OK);
    assert(!s_cleanup_resource_live);
    assert(app_manager_is_all_closed());

    s_expect_cleanup_zero_memory = true;
    assert(app_manager_run("cleanup") == ESP_OK);
    assert(s_cleanup_page_memory == retained_memory);
    assert(app_manager_exit("cleanup") == ESP_OK);
    assert(!s_cleanup_resource_live);
}

static void _test_teardown_only_deinit(void)
{
    _reset_traces();
    assert(app_manager_run("a") == ESP_OK);
    assert(app_manager_run("b") == ESP_OK);
    assert(app_manager_run("c") == ESP_OK);
    assert(app_manager_get_running_apps() == 3);

    assert(s_a_root.start == 1 && s_a_root.resume == 1 &&
           s_a_root.pause == 1 && s_a_root.stop == 0);
    assert(s_b_root.start == 1 && s_b_root.resume == 1 &&
           s_b_root.pause == 1 && s_b_root.stop == 0);
    assert(s_c_root.start == 1 && s_c_root.resume == 1 &&
           s_c_root.pause == 0 && s_c_root.stop == 0);

    s_attempt_navigation_during_teardown = true;
    assert(_lifecycle_deinit() == ESP_OK);
    s_attempt_navigation_during_teardown = false;

    assert(s_a_root.start == 1 && s_a_root.resume == 1 &&
           s_a_root.pause == 1 && s_a_root.stop == 1);
    assert(s_b_root.start == 1 && s_b_root.resume == 1 &&
           s_b_root.pause == 1 && s_b_root.stop == 1);
    assert(s_c_root.start == 1 && s_c_root.resume == 1 &&
           s_c_root.pause == 1 && s_c_root.stop == 1);
    assert(s_teardown_run_rejections == 4);
    assert(s_teardown_page_rejections == 4);
    assert(s_generic.start == 0 && s_generic.resume == 0 &&
           s_generic.pause == 0 && s_generic.stop == 0);
    assert(app_manager_get_running_apps() == 0);
    assert(!app_manager_is_all_closed());
    assert(!app_manager_is_page_present("a", "root"));
    assert(!app_manager_is_page_present("b", "root"));
    assert(!app_manager_is_page_present("c", "root"));

    _reset_traces();
    assert(app_manager_lifecycle_init(0) == ESP_OK);
    assert(app_manager_run("a") == ESP_OK);
    assert(s_a_root.start == 1 && s_a_root.resume == 1);
    assert(app_manager_exit("a") == ESP_OK);
    assert(s_a_root.pause == 1 && s_a_root.stop == 1);
    assert(app_manager_get_running_apps() == 0);
}

static void _test_shutdown_releases_admitted_api(void)
{
    assert(app_manager_lifecycle_init(0) == ESP_OK);
    atomic_store(&s_admission_hook_enabled, true);
    atomic_store(&s_admission_reached, false);
    atomic_store(&s_admission_release, false);
    atomic_store(&s_concurrent_api_result, ESP_FAIL);
    atomic_store(&s_shutdown_complete, false);
    atomic_store(&s_shutdown_result, ESP_FAIL);
    app_manager_lifecycle_host_set_admission_hook(_admission_hook);

    pthread_t api_thread;
    pthread_t shutdown_thread;
    assert(pthread_create(&api_thread, NULL, _concurrent_run_thread,
                          NULL) == 0);
    _wait_for_flag(&s_admission_reached);
    assert(pthread_create(&shutdown_thread, NULL,
                          _lifecycle_shutdown_thread, NULL) == 0);
    for (size_t attempt = 0; attempt < 2000U &&
            app_manager_lifecycle_screen_mutation_allowed(); attempt++)
    {
        usleep(1000);
    }
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    usleep(10000);
    assert(!atomic_load(&s_shutdown_complete));
    atomic_store(&s_admission_release, true);
    assert(pthread_join(api_thread, NULL) == 0);
    assert(pthread_join(shutdown_thread, NULL) == 0);
    atomic_store(&s_admission_hook_enabled, false);
    app_manager_lifecycle_host_set_admission_hook(NULL);
    assert(atomic_load(&s_concurrent_api_result) == ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_shutdown_result) == ESP_OK);
    assert(app_manager_mailbox_deinit() == ESP_OK);

    assert(app_manager_mailbox_init() == ESP_OK);
    assert(app_manager_lifecycle_init(0) == ESP_OK);
    assert(_screen_pause() == ESP_OK);
    assert(app_manager_lifecycle_screen_access_wait() == ESP_OK);
    assert(_screen_resume() == ESP_OK);
    assert(_lifecycle_deinit() == ESP_OK);
    assert(app_manager_mailbox_deinit() == ESP_OK);
}

int main(void)
{
    _reset_traces();
    assert(app_manager_mailbox_init() == ESP_OK);
    assert(app_manager_register_builtin_apps(s_apps,
            sizeof(s_apps) / sizeof(s_apps[0])) == ESP_OK);
    assert(!app_manager_lifecycle_screen_mutation_allowed());
    assert(app_manager_run("a") == ESP_ERR_INVALID_STATE);
    assert(!app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("a", "root"));
    assert(app_manager_get_page_userdata("root") == NULL);
    assert(app_manager_get_running_apps() == 0U);
    assert(!app_manager_is_all_closed());
    assert(app_manager_lifecycle_init(0) == ESP_OK);

    _test_screen_pause_without_app();
    _test_lifecycle_observer();
    _test_pause_drains_admitted_api();
    _test_screen_pause_lifecycle();
    _test_screen_pause_cleanup_retry();
    _test_paused_deinit_stops_without_resume();
    _reset_traces();

    assert(app_manager_run("a") == ESP_OK);
    assert(app_manager_is_actived("a"));
    assert(app_manager_is_page_present("a", "root"));
    assert(s_a_root.start == 1 && s_a_root.resume == 1);
    assert(app_manager_get_running_apps() == 1);

    assert(app_manager_run("b") == ESP_OK);
    assert(app_manager_is_page_present("a", "root"));
    assert(app_manager_is_page_present("b", "root"));
    assert(s_a_root.pause == 1);
    assert(s_b_root.start == 1 && s_b_root.resume == 1);
    assert(app_manager_get_running_apps() == 2);

    assert(app_manager_exit("b") == ESP_OK);
    assert(app_manager_is_actived("a"));
    assert(s_b_root.pause == 1 && s_b_root.stop == 1);
    assert(s_a_root.resume == 2);

    assert(app_manager_run("b") == ESP_OK);
    s_defer_child = true;
    assert(app_manager_exit("b") == ESP_OK);
    assert(app_page_is_actived("a", "child"));
    assert(s_child.start == 1 && s_child.resume == 1);
    assert(app_manager_goback() == ESP_OK);
    assert(!app_manager_is_page_present("a", "child"));
    assert(app_page_is_actived("a", "root"));
    assert(s_child.pause == 1 && s_child.stop == 1);

    s_expect_zero_memory = true;
    assert(app_manager_create_page_ext("child", _child_handler, NULL, 64) == ESP_OK);
    assert(app_manager_goback() == ESP_OK);
    s_expect_zero_memory = true;
    assert(app_manager_create_page_ext("child", _child_handler, NULL, 64) == ESP_OK);
    assert(app_manager_goback() == ESP_OK);
    assert(s_child.start == 3 && s_child.stop == 3);

    s_expect_zero_memory = true;
    assert(app_manager_create_page_ext("child", _child_handler, NULL, 64) == ESP_OK);
    s_child_goback_on_pause = true;
    assert(app_manager_run("b") == ESP_OK);
    assert(app_manager_is_actived("b"));
    assert(app_manager_is_page_present("a", "child"));
    assert(app_manager_get_running_apps() == 2);
    assert(app_manager_exit("b") == ESP_OK);
    assert(app_page_is_actived("a", "child"));
    assert(app_manager_goback() == ESP_OK);
    assert(s_child.start == 4 && s_child.stop == 4);

    int a_resume_before_failure = s_a_root.resume;
    s_fail_run_b_on_stop = true;
    s_defer_child = true;
    s_expect_zero_memory = true;
    assert(app_manager_run("fail") == ESP_FAIL);
    assert(app_manager_is_actived("a"));
    assert(!app_manager_is_actived("b"));
    assert(app_page_is_actived("a", "child"));
    assert(app_manager_get_running_apps() == 1);
    assert(s_fail_root.start == 1 && s_fail_root.resume == 1);
    assert(s_fail_root.pause == 1 && s_fail_root.stop == 1);
    assert(s_a_root.resume == a_resume_before_failure + 1);
    assert(app_manager_goback() == ESP_OK);

    assert(app_manager_run("empty") == ESP_ERR_INVALID_STATE);
    assert(app_manager_is_actived("a"));
    assert(app_manager_get_running_apps() == 1);
    assert(app_manager_run("large") == ESP_ERR_NO_MEM);
    assert(app_manager_get_running_apps() == 1);

    s_expect_private_memory = false;
    char page_id[16];
    for (int i = 1; i <= 11; i++)
    {
        snprintf(page_id, sizeof(page_id), "page-%d", i);
        assert(app_manager_create_page_ext(page_id, _generic_handler, NULL, 0) == ESP_OK);
    }
    assert(app_manager_create_page_ext("page-overflow", _generic_handler,
                                       NULL, 0) == ESP_ERR_NO_MEM);
    assert(app_manager_goback_to_page("root") == ESP_OK);
    assert(app_page_is_actived("a", "root"));

    assert(app_manager_exit("a") == ESP_OK);
    assert(app_manager_is_all_closed());
    assert(app_manager_get_running_apps() == 0);
    assert(s_a_root.start == 1 && s_a_root.stop == 1);

    assert(app_manager_run("nested") == ESP_OK);
    assert(app_manager_is_actived("b"));
    assert(app_manager_is_page_present("nested", "root"));
    assert(app_manager_is_page_present("b", "root"));
    assert(app_manager_get_running_apps() == 2);
    assert(s_nested_root.start == 1 && s_nested_root.resume == 1 &&
           s_nested_root.pause == 1);
    assert(app_manager_exit("b") == ESP_OK);
    assert(app_manager_is_actived("nested"));
    assert(s_nested_root.resume == 2);
    assert(app_manager_exit("nested") == ESP_OK);
    assert(app_manager_is_all_closed());

    assert(app_manager_run("a") == ESP_OK);
    assert(app_manager_run("b") == ESP_OK);
    s_self_exit_on_stop = true;
    assert(app_manager_exit("a") == ESP_OK);
    assert(app_manager_is_actived("b"));
    assert(app_manager_get_running_apps() == 1);
    assert(app_manager_exit("b") == ESP_OK);
    assert(app_manager_is_all_closed());

    _test_cleanup_failure_retains_page_slot();
    _test_teardown_only_deinit();
    assert(_lifecycle_deinit() == ESP_OK);
    _reset_traces();
    s_a_memory_size = 17;
    assert(app_manager_lifecycle_init(192) == ESP_OK);
    assert(app_manager_run("a") == ESP_ERR_NO_MEM);
    assert(app_manager_get_running_apps() == 0);
    assert(_lifecycle_deinit() == ESP_OK);
    _test_shutdown_releases_admitted_api();

    puts("lifecycle production-core tests passed");
    return 0;
}
