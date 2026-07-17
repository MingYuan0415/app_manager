#include "host_freertos.h"
#include "app_manager.h"
#include "../../src/app_manager_lifecycle.h"
#include "../../src/app_manager_mailbox.h"

#include <assert.h>
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

static void _generic_handler(app_manager_msg_type_t message, void *param);

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
}

static esp_err_t _lifecycle_deinit_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_deinit();
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
    assert(app_manager_ui_call(_lifecycle_deinit_on_ui, NULL,
                               UINT32_MAX) == ESP_OK);
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
    assert(app_manager_is_all_closed());
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

int main(void)
{
    _reset_traces();
    assert(app_manager_mailbox_init() == ESP_OK);
    assert(app_manager_register_builtin_apps(s_apps,
            sizeof(s_apps) / sizeof(s_apps[0])) == ESP_OK);
    assert(app_manager_lifecycle_init(0) == ESP_OK);

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
    assert(app_manager_ui_call(_lifecycle_deinit_on_ui, NULL,
                               UINT32_MAX) == ESP_OK);
    _reset_traces();
    s_a_memory_size = 17;
    assert(app_manager_lifecycle_init(192) == ESP_OK);
    assert(app_manager_run("a") == ESP_ERR_NO_MEM);
    assert(app_manager_get_running_apps() == 0);
    assert(app_manager_ui_call(_lifecycle_deinit_on_ui, NULL,
                               UINT32_MAX) == ESP_OK);
    assert(app_manager_mailbox_deinit() == ESP_OK);

    puts("lifecycle production-core tests passed");
    return 0;
}
