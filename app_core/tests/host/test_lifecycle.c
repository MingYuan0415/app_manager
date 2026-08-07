#include "app_manager.h"
#include "app_manager_builtin.h"
#include "app_manager_lifecycle.h"
#include "app_manager_lifecycle_internal.h"
#include "app_manager_mailbox.h"
#include "app_manager_presentation.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_SCREEN_CAPACITY 4U
#define TEST_TRACE_CAPACITY  96U
#define TEST_TIMEOUT_MS      2000U

struct _lv_obj_t
{
    bool live;
    uint32_t generation;
};

typedef struct page_state
{
    const char *name;
    const char *app_id;
    unsigned messages[APP_MANAGER_MSG_USER_END];
    unsigned pause_failures;
    unsigned stop_failures;
    bool mounted;
    bool lifecycle_active;
    bool new_intent_had_screen;
    bool has_arguments;
    app_manager_typed_blob_t arguments;
    lv_obj_t *last_screen;
    void *instance_memory;
} page_state_t;

typedef struct trace_event
{
    const page_state_t *page;
    app_manager_msg_type_t message;
} trace_event_t;

typedef struct nav_call
{
    app_manager_nav_request_t request;
    esp_err_t admission_result;
    esp_err_t completion_result;
    unsigned completions;
} nav_call_t;

typedef struct back_target_call
{
    app_manager_lifecycle_back_target_t target;
    const char *source_app_id;
} back_target_call_t;

typedef struct presentation_mock
{
    struct _lv_obj_t neutral;
    struct _lv_obj_t screens[TEST_SCREEN_CAPACITY];
    lv_obj_t *active;
    app_manager_presentation_completion_cb_t pending_completion;
    void *pending_context;
    bool running;
    unsigned delete_failures;
    unsigned create_failures;
    unsigned load_failures;
    unsigned start_failures;
    unsigned starts;
    unsigned finishes;
    unsigned capture_calls;
    lv_obj_t *last_capture_screen;
    bool last_start_had_page_target;
    bool last_start_target_active;
    bool last_start_source_inactive;
    app_manager_transition_effect_t last_effect;
    uint32_t last_duration_ms;
} presentation_mock_t;

static page_state_t s_a_root = {.name = "a/root", .app_id = "a"};
static page_state_t s_a_one = {.name = "a/one", .app_id = "a"};
static page_state_t s_a_two = {.name = "a/two", .app_id = "a"};
static page_state_t s_b_root = {.name = "b/root", .app_id = "b"};
static page_state_t s_c_root = {.name = "c/root", .app_id = "c"};
static page_state_t s_e_root = {.name = "e/root", .app_id = "e"};
static page_state_t *const s_page_states[] =
{
    &s_a_root,
    &s_a_one,
    &s_a_two,
    &s_b_root,
    &s_c_root,
};
static trace_event_t s_trace[TEST_TRACE_CAPACITY];
static size_t s_trace_count;
static presentation_mock_t s_presentation;
static FILE *s_lifecycle_log_output;
static int s_saved_stderr = -1;
static const char *s_callback_owner_app_id;
static const lv_draw_buf_t *s_test_capture_result;

static void _lifecycle_log_capture_begin(void)
{
    assert(s_lifecycle_log_output == NULL);
    assert(s_saved_stderr < 0);
    assert(fflush(stderr) == 0);
    s_saved_stderr = dup(STDERR_FILENO);
    assert(s_saved_stderr >= 0);
    s_lifecycle_log_output = tmpfile();
    assert(s_lifecycle_log_output != NULL);
    assert(dup2(fileno(s_lifecycle_log_output), STDERR_FILENO) >= 0);
}

static void _lifecycle_log_capture_end(void)
{
    long output_bytes;
    char *output;

    assert(s_lifecycle_log_output != NULL);
    assert(s_saved_stderr >= 0);
    assert(fflush(stderr) == 0);
    assert(dup2(s_saved_stderr, STDERR_FILENO) >= 0);
    assert(close(s_saved_stderr) == 0);
    s_saved_stderr = -1;

    assert(fseek(s_lifecycle_log_output, 0L, SEEK_END) == 0);
    output_bytes = ftell(s_lifecycle_log_output);
    assert(output_bytes >= 0L);
    assert(fseek(s_lifecycle_log_output, 0L, SEEK_SET) == 0);
    output = malloc((size_t)output_bytes + 1U);
    assert(output != NULL);
    assert(fread(output, 1U, (size_t)output_bytes,
                 s_lifecycle_log_output) == (size_t)output_bytes);
    output[output_bytes] = '\0';

#if CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG
    static const char *const expected[] =
    {
        "[I][app_lifecycle] [A][root] ON START\n",
        "[I][app_lifecycle] [A][root] ON MOUNT\n",
        "[I][app_lifecycle] [A][root] ON RESUME\n",
        "[I][app_lifecycle] [A][root] ON PAUSE\n",
        "[I][app_lifecycle] [A][root] ON UNMOUNT\n",
        "[I][app_lifecycle] [A][two] ON STOP\n",
        "[I][app_lifecycle] [A][root] ON NEW_INTENT\n",
    };
    for (size_t i = 0U; i < sizeof(expected) / sizeof(expected[0]); ++i)
    {
        assert(strstr(output, expected[i]) != NULL);
    }
#else
    assert(strstr(output, "[I][app_lifecycle] ") == NULL);
#endif

    free(output);
    assert(fclose(s_lifecycle_log_output) == 0);
    s_lifecycle_log_output = NULL;
}

void app_manager_back_gesture_navigation_begin(void)
{
}

void app_manager_back_gesture_navigation_settled(void)
{
}

static void _lifecycle_observer(
    const char *app_id, const char *page_id, app_manager_msg_type_t message,
    app_manager_lifecycle_observer_phase_t phase)
{
    (void)page_id;
    (void)message;
    if (phase == APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE)
    {
        assert(s_callback_owner_app_id == NULL);
        s_callback_owner_app_id = app_id;
    }
    else
    {
        assert(s_callback_owner_app_id != NULL);
        assert(strcmp(s_callback_owner_app_id, app_id) == 0);
        s_callback_owner_app_id = NULL;
    }
}

static void _page_handler(app_manager_msg_type_t message, void *user_data)
{
    page_state_t *state = user_data;
    bool pause_failed = false;
    assert(state != NULL);
    assert(s_callback_owner_app_id != NULL);
    assert(strcmp(s_callback_owner_app_id, state->app_id) == 0);
    assert(app_manager_this_page_userdata() == user_data);
    assert(message < APP_MANAGER_MSG_USER_END);
    assert(s_trace_count < TEST_TRACE_CAPACITY);
    s_trace[s_trace_count++] = (trace_event_t)
    {
        .page = state,
        .message = message,
    };
    state->messages[message]++;

    uint8_t *memory = app_manager_this_page_memory();
    assert(memory != NULL);
    lv_obj_t *screen = app_manager_this_page_screen();
    switch (message)
    {
    case APP_MANAGER_MSG_ONSTART:
        assert(screen == NULL);
        assert(memory[0] == 0U);
        state->instance_memory = memory;
        memory[0] = 0xA5U;
        break;
    case APP_MANAGER_MSG_ONMOUNT:
        assert(screen != NULL);
        state->last_screen = screen;
        break;
    case APP_MANAGER_MSG_ONRESUME:
    case APP_MANAGER_MSG_ONPAUSE:
    case APP_MANAGER_MSG_ONUNMOUNT:
        assert(screen != NULL);
        assert(screen == state->last_screen);
        break;
    case APP_MANAGER_MSG_ONSTOP:
        assert(screen == NULL);
        assert(memory[0] == 0xA5U);
        break;
    case APP_MANAGER_MSG_ONNEWINTENT:
        state->new_intent_had_screen = screen != NULL;
        break;
    case APP_MANAGER_MSG_USER_END:
    default:
        assert(false);
        break;
    }

    const app_manager_typed_blob_t *arguments =
        app_manager_this_page_arguments();
    state->has_arguments = arguments != NULL;
    if (arguments != NULL)
    {
        state->arguments = *arguments;
    }
    else
    {
        memset(&state->arguments, 0, sizeof(state->arguments));
    }

    if (message == APP_MANAGER_MSG_ONPAUSE && state->pause_failures > 0U)
    {
        state->pause_failures--;
        pause_failed = true;
        app_manager_this_page_report_cleanup_error(ESP_FAIL);
    }
    if (message == APP_MANAGER_MSG_ONSTOP && state->stop_failures > 0U)
    {
        state->stop_failures--;
        app_manager_this_page_report_cleanup_error(ESP_FAIL);
    }

    switch (message)
    {
    case APP_MANAGER_MSG_ONSTART:
        state->mounted = false;
        state->lifecycle_active = false;
        break;
    case APP_MANAGER_MSG_ONMOUNT:
        state->mounted = true;
        break;
    case APP_MANAGER_MSG_ONRESUME:
        state->lifecycle_active = true;
        break;
    case APP_MANAGER_MSG_ONPAUSE:
        if (!pause_failed)
        {
            state->lifecycle_active = false;
        }
        break;
    case APP_MANAGER_MSG_ONUNMOUNT:
        state->mounted = false;
        break;
    case APP_MANAGER_MSG_ONSTOP:
    case APP_MANAGER_MSG_ONNEWINTENT:
    case APP_MANAGER_MSG_USER_END:
    default:
        break;
    }
}

static page_state_t *_ops_record(
    const app_manager_page_context_t *context,
    app_manager_msg_type_t message)
{
    page_state_t *state = context->route_user_data;
    assert(state != NULL);
    assert(context->state == app_manager_this_page_memory());
    assert(context->route_user_data == app_manager_this_page_userdata());
    assert(context->screen == app_manager_this_page_screen());
    assert(context->arguments == app_manager_this_page_arguments());
    assert(s_callback_owner_app_id != NULL);
    assert(strcmp(s_callback_owner_app_id, state->app_id) == 0);
    assert(s_trace_count < TEST_TRACE_CAPACITY);
    s_trace[s_trace_count++] = (trace_event_t)
    {
        .page = state,
        .message = message,
    };
    state->messages[message]++;
    state->has_arguments = context->arguments != NULL;
    if (context->arguments != NULL)
    {
        state->arguments = *context->arguments;
    }
    else
    {
        memset(&state->arguments, 0, sizeof(state->arguments));
    }
    return state;
}

static void _ops_start(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context, APP_MANAGER_MSG_ONSTART);
    uint8_t *memory = context->state;
    assert(context->screen == NULL);
    assert(memory != NULL && memory[0] == 0U);
    state->instance_memory = memory;
    memory[0] = 0xA5U;
    state->mounted = false;
    state->lifecycle_active = false;
}

static void _ops_mount(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context, APP_MANAGER_MSG_ONMOUNT);
    assert(context->screen != NULL);
    state->last_screen = context->screen;
    state->mounted = true;
}

static void _ops_resume(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context, APP_MANAGER_MSG_ONRESUME);
    uint8_t *memory = context->state;
    assert(context->screen == state->last_screen);
    assert(memory != NULL);
    memory[1] = 0x5AU;
    state->lifecycle_active = true;
}

static esp_err_t _ops_pause(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context, APP_MANAGER_MSG_ONPAUSE);
    uint8_t *memory = context->state;
    assert(context->screen == state->last_screen);
    assert(memory != NULL && memory[1] == 0x5AU);
    if (state->pause_failures > 0U)
    {
        state->pause_failures--;
        return ESP_FAIL;
    }
    memory[1] = 0U;
    state->lifecycle_active = false;
    return ESP_OK;
}

static void _ops_unmount(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context, APP_MANAGER_MSG_ONUNMOUNT);
    assert(context->screen == state->last_screen);
    state->mounted = false;
}

static esp_err_t _ops_stop(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context, APP_MANAGER_MSG_ONSTOP);
    uint8_t *memory = context->state;
    assert(context->screen == NULL);
    assert(memory != NULL && memory[0] == 0xA5U);
    assert(memory[1] == 0U);
    if (state->stop_failures > 0U)
    {
        state->stop_failures--;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void _ops_new_intent(const app_manager_page_context_t *context)
{
    page_state_t *state = _ops_record(context,
                                      APP_MANAGER_MSG_ONNEWINTENT);
    state->new_intent_had_screen = context->screen != NULL;
}

static const app_manager_page_ops_t s_page_ops =
{
    .start = _ops_start,
    .mount = _ops_mount,
    .resume = _ops_resume,
    .pause = _ops_pause,
    .unmount = _ops_unmount,
    .stop = _ops_stop,
    .new_intent = _ops_new_intent,
};

static const app_manager_page_definition_t s_shared_page_definition =
{
    .handler = _page_handler,
    .memory_size = 16U,
};

static const app_manager_page_definition_t s_ops_page_definition =
{
    .ops = &s_page_ops,
    .memory_size = 16U,
};

static const app_manager_page_ops_t s_empty_page_ops = {0};

static const app_manager_page_definition_t s_empty_ops_page_definition =
{
    .ops = &s_empty_page_ops,
};

static const app_manager_page_route_t s_a_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_shared_page_definition,
        .user_data = &s_a_root,
    },
    {
        .page_id = "one",
        .definition = &s_shared_page_definition,
        .user_data = &s_a_one,
    },
    {
        .page_id = "two",
        .definition = &s_shared_page_definition,
        .user_data = &s_a_two,
    },
};

static const app_manager_page_route_t s_b_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_shared_page_definition,
        .user_data = &s_b_root,
    },
};

static const app_manager_page_route_t s_c_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_ops_page_definition,
        .user_data = &s_c_root,
    },
};

static const app_manager_page_route_t s_d_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_empty_ops_page_definition,
    },
};

static const app_manager_page_route_t s_e_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_ops_page_definition,
        .user_data = &s_e_root,
    },
};

static const app_manager_app_desc_t s_apps[] =
{
    {
        .name = "A",
        .id = "a",
        .root_page_id = "root",
        .page_routes = s_a_routes,
        .page_route_count = sizeof(s_a_routes) / sizeof(s_a_routes[0]),
        .flags = APP_MANAGER_APP_FLAG_PINNED,
    },
    {
        .name = "B",
        .id = "b",
        .root_page_id = "root",
        .page_routes = s_b_routes,
        .page_route_count = sizeof(s_b_routes) / sizeof(s_b_routes[0]),
    },
    {
        .name = "C",
        .id = "c",
        .root_page_id = "root",
        .page_routes = s_c_routes,
        .page_route_count = sizeof(s_c_routes) / sizeof(s_c_routes[0]),
    },
    {
        .name = "D",
        .id = "d",
        .root_page_id = "root",
        .page_routes = s_d_routes,
        .page_route_count = sizeof(s_d_routes) / sizeof(s_d_routes[0]),
    },
    {
        .name = "E",
        .id = "e",
        .root_page_id = "root",
        .page_routes = s_e_routes,
        .page_route_count = sizeof(s_e_routes) / sizeof(s_e_routes[0]),
    },
};

static void _presentation_reset(void)
{
    memset(&s_presentation, 0, sizeof(s_presentation));
    s_presentation.neutral.live = true;
    s_presentation.neutral.generation = 1U;
    s_presentation.active = &s_presentation.neutral;
}

static page_state_t *_presentation_page_for_screen(const lv_obj_t *screen)
{
    page_state_t *page = NULL;
    for (size_t index = 0;
            index < sizeof(s_page_states) / sizeof(s_page_states[0]); index++)
    {
        if (s_page_states[index]->mounted &&
                s_page_states[index]->last_screen == screen)
        {
            page = s_page_states[index];
            break;
        }
    }
    return page;
}

esp_err_t app_manager_presentation_init(void)
{
    _presentation_reset();
    return ESP_OK;
}

esp_err_t app_manager_presentation_deinit(void)
{
    assert(!s_presentation.running);
    return ESP_OK;
}

lv_obj_t *app_manager_presentation_create_page_screen(void)
{
    if (s_presentation.create_failures > 0U)
    {
        s_presentation.create_failures--;
        return NULL;
    }
    for (size_t index = 0; index < TEST_SCREEN_CAPACITY; index++)
    {
        struct _lv_obj_t *screen = &s_presentation.screens[index];
        if (!screen->live)
        {
            screen->live = true;
            screen->generation++;
            return screen;
        }
    }
    return NULL;
}

esp_err_t app_manager_presentation_delete_page_screen(lv_obj_t *screen)
{
    if (s_presentation.delete_failures > 0U)
    {
        s_presentation.delete_failures--;
        return ESP_FAIL;
    }
    if (screen == NULL || screen == &s_presentation.neutral ||
            !screen->live || screen == s_presentation.active ||
            s_presentation.running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    screen->live = false;
    return ESP_OK;
}

lv_obj_t *app_manager_presentation_neutral_screen(void)
{
    return &s_presentation.neutral;
}

esp_err_t app_manager_presentation_load_immediate(lv_obj_t *screen)
{
    if (s_presentation.load_failures > 0U)
    {
        s_presentation.load_failures--;
        return ESP_FAIL;
    }
    if (screen == NULL || !screen->live || s_presentation.running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_presentation.active = screen;
    return ESP_OK;
}

esp_err_t app_manager_presentation_start(
    lv_obj_t *from_screen, lv_obj_t *to_screen,
    app_manager_transition_effect_t effect, uint32_t duration_ms,
    app_manager_presentation_completion_cb_t completion, void *context)
{
    if (from_screen == NULL || to_screen == NULL || completion == NULL ||
            !from_screen->live || !to_screen->live ||
            from_screen != s_presentation.active || s_presentation.running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    page_state_t *source_page = _presentation_page_for_screen(from_screen);
    page_state_t *target_page = _presentation_page_for_screen(to_screen);
    s_presentation.starts++;
    s_presentation.last_start_had_page_target = target_page != NULL;
    s_presentation.last_start_target_active =
        target_page != NULL && target_page->lifecycle_active;
    s_presentation.last_start_source_inactive =
        source_page == NULL || !source_page->lifecycle_active;
    s_presentation.last_effect = effect;
    s_presentation.last_duration_ms = duration_ms;
    if (s_presentation.start_failures > 0U)
    {
        s_presentation.start_failures--;
        return ESP_FAIL;
    }
    s_presentation.active = to_screen;
    if (effect == APP_MANAGER_TRANSITION_NONE || duration_ms == 0U)
    {
        completion(context);
    }
    else
    {
        s_presentation.running = true;
        s_presentation.pending_completion = completion;
        s_presentation.pending_context = context;
    }
    return ESP_OK;
}

void app_manager_presentation_finish_now(void)
{
    if (!s_presentation.running)
    {
        return;
    }
    app_manager_presentation_completion_cb_t completion =
        s_presentation.pending_completion;
    void *context = s_presentation.pending_context;
    s_presentation.running = false;
    s_presentation.pending_completion = NULL;
    s_presentation.pending_context = NULL;
    s_presentation.finishes++;
    completion(context);
}

bool app_manager_presentation_is_running(void)
{
    return s_presentation.running;
}

esp_err_t app_manager_presentation_capture_source(
    lv_obj_t *screen, const lv_draw_buf_t **snapshot)
{
    s_presentation.capture_calls++;
    s_presentation.last_capture_screen = screen;
    if (snapshot != NULL)
    {
        *snapshot = s_test_capture_result;
    }
    return s_test_capture_result != NULL ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

lv_result_t lv_draw_buf_init(lv_draw_buf_t *draw_buf, uint32_t width,
                             uint32_t height, lv_color_format_t color_format,
                             uint32_t stride, void *data, uint32_t data_size)
{
    if (draw_buf == NULL || data == NULL || width == 0U || height == 0U ||
            stride == 0U)
    {
        return LV_RESULT_INVALID;
    }
    draw_buf->header.cf = color_format;
    draw_buf->header.w = width;
    draw_buf->header.h = height;
    draw_buf->header.stride = stride;
    draw_buf->data = data;
    draw_buf->data_size = data_size;
    return LV_RESULT_OK;
}

uint32_t lv_draw_buf_width_to_stride(uint32_t width,
                                     lv_color_format_t color_format)
{
    (void)color_format;
    return width * 2U;
}

void lv_draw_buf_flush_cache(const lv_draw_buf_t *draw_buf,
                             const lv_area_t *area)
{
    (void)draw_buf;
    (void)area;
}

const lv_font_t *app_theme_get_font(app_theme_font_id_t id)
{
    (void)id;
    return NULL;
}

static size_t _live_screen_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < TEST_SCREEN_CAPACITY; index++)
    {
        count += s_presentation.screens[index].live ? 1U : 0U;
    }
    return count;
}

static void _expect_start_handoff_complete(void)
{
    assert(s_presentation.last_start_had_page_target);
    assert(s_presentation.last_start_target_active);
    assert(s_presentation.last_start_source_inactive);
}

static void _trace_reset(void)
{
    memset(s_trace, 0, sizeof(s_trace));
    s_trace_count = 0U;
}

static void _expect_trace(const trace_event_t *expected, size_t count)
{
    if (s_trace_count != count)
    {
        (void)fprintf(stderr, "trace count: got=%zu expected=%zu\n",
                      s_trace_count, count);
    }
    assert(s_trace_count == count);
    for (size_t index = 0; index < count; index++)
    {
        if (s_trace[index].page != expected[index].page ||
                s_trace[index].message != expected[index].message)
        {
            (void)fprintf(stderr,
                          "trace[%zu]: got=%s/%d expected=%s/%d\n", index,
                          s_trace[index].page->name,
                          (int)s_trace[index].message,
                          expected[index].page->name,
                          (int)expected[index].message);
        }
        assert(s_trace[index].page == expected[index].page);
        assert(s_trace[index].message == expected[index].message);
    }
}

#define EVENT(page_, message_) \
    (trace_event_t) {.page = (page_), .message = (message_)}

static void _nav_completion(esp_err_t result, void *context)
{
    nav_call_t *call = context;
    call->completion_result = result;
    call->completions++;
}

static esp_err_t _nav_on_ui(void *context)
{
    nav_call_t *call = context;
    call->admission_result = app_manager_lifecycle_navigate(
                                 &call->request, _nav_completion, call);
    return call->admission_result;
}

static esp_err_t _navigate(nav_call_t *call)
{
    call->request.transition.effect = APP_MANAGER_TRANSITION_NONE;
    call->request.transition.duration_ms = 0U;
    call->request.transition.reserved = 0U;
    esp_err_t result = app_manager_ui_call(_nav_on_ui, call, TEST_TIMEOUT_MS);
    assert(result == call->admission_result);
    if (result == ESP_OK)
    {
        assert(call->completions == 1U);
        result = call->completion_result;
    }
    else
    {
        assert(call->completions == 0U);
    }
    return result;
}

static nav_call_t _request(app_manager_nav_operation_t operation,
                           const char *app_id, const char *page_id)
{
    nav_call_t call =
    {
        .request =
        {
            .operation = operation,
            .app_id = app_id,
            .page_id = page_id,
        },
    };
    return call;
}

static app_manager_typed_blob_t _arguments(uint16_t type, uint32_t value)
{
    app_manager_typed_blob_t arguments =
    {
        .version = APP_MANAGER_TYPED_BLOB_VERSION,
        .type = type,
        .size = sizeof(value),
    };
    memcpy(arguments.payload, &value, sizeof(value));
    return arguments;
}

static esp_err_t _screen_pause_on_ui(void *context)
{
    (void)context;
    return app_manager_lifecycle_screen_pause();
}

static esp_err_t _back_target_on_ui(void *context)
{
    back_target_call_t *call = context;
    call->target = app_manager_lifecycle_back_target(&call->source_app_id);
    return ESP_OK;
}

static void _expect_back_target(app_manager_lifecycle_back_target_t target,
                                const char *source_app_id)
{
    back_target_call_t call = {0};
    assert(app_manager_ui_call(_back_target_on_ui, &call, TEST_TIMEOUT_MS) ==
           ESP_OK);
    assert(call.target == target);
    if (source_app_id == NULL)
    {
        assert(call.source_app_id == NULL);
    }
    else
    {
        assert(call.source_app_id != NULL);
        assert(strcmp(call.source_app_id, source_app_id) == 0);
    }
}

static esp_err_t _screen_resume_on_ui(void *context)
{
    (void)context;
    return app_manager_lifecycle_screen_resume();
}

static esp_err_t _finish_on_ui(void *context)
{
    (void)context;
    app_manager_presentation_finish_now();
    return ESP_OK;
}

static esp_err_t _deinit_on_ui(void *context)
{
    (void)context;
    return app_manager_lifecycle_deinit();
}

static void _test_configuration_validation(void)
{
    app_manager_transition_t invalid =
    {
        .effect = APP_MANAGER_TRANSITION_FADE,
        .duration_ms = 20U,
        .reserved = 1U,
    };
    assert(app_manager_lifecycle_configure(
               APP_MANAGER_MAX_RESIDENT_APPS + 1U,
               APP_MANAGER_RESIDENT_REJECT, NULL, NULL, NULL, NULL) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_lifecycle_configure(
               2U, APP_MANAGER_RESIDENT_REJECT,
               &invalid, NULL, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(app_manager_lifecycle_configure(
               4U, APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND,
               NULL, NULL, NULL, NULL) == ESP_OK);
}

static void _test_run_and_page_navigation(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    call.request.has_arguments = true;
    call.request.arguments = _arguments(7U, UINT32_C(0x12345678));
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t run_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONSTART),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
    };
    _expect_trace(run_expected,
                  sizeof(run_expected) / sizeof(run_expected[0]));
    _expect_start_handoff_complete();
    assert(s_a_root.has_arguments && s_a_root.arguments.type == 7U);
    assert(_live_screen_count() == 1U);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE, NULL);

    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    call.request.has_arguments = true;
    call.request.arguments = _arguments(8U, UINT32_C(0xCAFEBABE));
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t intent_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONNEWINTENT),
    };
    _expect_trace(intent_expected,
                  sizeof(intent_expected) / sizeof(intent_expected[0]));
    assert(s_a_root.new_intent_had_screen);
    assert(s_a_root.arguments.type == 8U);

    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t open_one_expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONSTART),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONUNMOUNT),
    };
    _expect_trace(open_one_expected,
                  sizeof(open_one_expected) / sizeof(open_one_expected[0]));
    _expect_start_handoff_complete();
    assert(_live_screen_count() == 1U);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_PAGE, "a");

    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "two");
    assert(_navigate(&call) == ESP_OK);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_PAGE, "a");
    call = _request(APP_MANAGER_NAV_OP_BACK, NULL, NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t back_expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_two, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_two, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_a_two, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(back_expected,
                  sizeof(back_expected) / sizeof(back_expected[0]));
    _expect_start_handoff_complete();

    call = _request(APP_MANAGER_NAV_OP_BACK_TO, NULL, "root");
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "two");
    assert(_navigate(&call) == ESP_OK);
    unsigned starts_before = s_presentation.starts;
    call = _request(APP_MANAGER_NAV_OP_BACK_TO, NULL, "root");
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t back_to_expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONSTOP),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_two, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_two, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_a_two, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(back_to_expected,
                  sizeof(back_to_expected) / sizeof(back_to_expected[0]));
    _expect_start_handoff_complete();
    assert(s_presentation.starts == starts_before + 1U);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE, NULL);
}

static void _test_pause_and_stop_rollback(void)
{
    s_a_root.pause_failures = 1U;
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    const trace_event_t pause_fail_expected[] =
    {
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTART),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(pause_fail_expected,
                  sizeof(pause_fail_expected) /
                  sizeof(pause_fail_expected[0]));
    assert(app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("b", "root"));
    assert(_live_screen_count() == 1U);

    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_APP, "b");
    s_b_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    const trace_event_t stop_fail_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONUNMOUNT),
    };
    _expect_trace(stop_fail_expected,
                  sizeof(stop_fail_expected) /
                  sizeof(stop_fail_expected[0]));
    _expect_start_handoff_complete();
    assert(app_manager_is_actived("b"));
    assert(_live_screen_count() == 1U);

    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_is_actived("a"));
}

static void _test_presentation_start_failure_rollback(void)
{
    s_presentation.start_failures = 1U;
    nav_call_t call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    const trace_event_t expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONSTART),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(expected, sizeof(expected) / sizeof(expected[0]));
    _expect_start_handoff_complete();
    assert(app_manager_is_actived("a"));
    assert(app_page_is_actived("a", "root"));
    assert(!app_manager_is_page_present("a", "one"));
    assert(_live_screen_count() == 1U);
}

static void _test_forward_unmount_and_target_pause_failure(void)
{
    s_presentation.delete_failures = 1U;
    s_a_one.pause_failures = 1U;
    nav_call_t call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    const trace_event_t expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONSTART),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONPAUSE),
    };
    _expect_trace(expected, sizeof(expected) / sizeof(expected[0]));
    assert(app_page_is_actived("a", "one"));
    assert(app_manager_is_page_present("a", "root"));
    assert(_live_screen_count() == 2U);

    _trace_reset();
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    const trace_event_t pause_expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_one, APP_MANAGER_MSG_ONUNMOUNT),
    };
    _expect_trace(pause_expected,
                  sizeof(pause_expected) / sizeof(pause_expected[0]));
    assert(_live_screen_count() == 0U);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE, NULL);
    assert(app_manager_ui_call(_screen_resume_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    assert(app_page_is_actived("a", "one"));
    assert(_live_screen_count() == 1U);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_PAGE, "a");

    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    _expect_trace(NULL, 0U);
    assert(app_page_is_actived("a", "one"));
    assert(app_manager_is_page_present("a", "root"));
    assert(_live_screen_count() == 1U);

    call = _request(APP_MANAGER_NAV_OP_BACK, NULL, NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_page_is_actived("a", "root"));
}

static void _test_shared_definition_and_target_pause_rollback(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(s_a_routes[0].definition == s_b_routes[0].definition);
    assert(s_a_root.instance_memory != NULL);
    assert(s_b_root.instance_memory != NULL);
    assert(s_a_root.instance_memory != s_b_root.instance_memory);
    assert(app_manager_is_page_present("a", "root"));
    assert(app_manager_is_page_present("b", "root"));

    s_b_root.stop_failures = 1U;
    s_a_root.pause_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    const trace_event_t target_pause_fail_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
    };
    _expect_trace(target_pause_fail_expected,
                  sizeof(target_pause_fail_expected) /
                  sizeof(target_pause_fail_expected[0]));
    _expect_start_handoff_complete();
    assert(app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_get_running_apps() == 2U);
    assert(_live_screen_count() == 1U);

    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t cleanup_expected[] =
    {
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(cleanup_expected,
                  sizeof(cleanup_expected) / sizeof(cleanup_expected[0]));
    assert(app_manager_is_actived("a"));
    assert(app_manager_get_running_apps() == 1U);
}

static void _test_uncommitted_cleanup_ownership(void)
{
    s_a_root.pause_failures = 1U;
    s_a_one.stop_failures = 1U;
    nav_call_t call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    assert(_navigate(&call) == ESP_FAIL);
    assert(app_manager_is_actived("a"));
    assert(app_page_is_actived("a", "root"));
    assert(!app_manager_is_page_present("a", "one"));

    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t page_cleanup_expected[] =
    {
        EVENT(&s_a_one, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(page_cleanup_expected,
                  sizeof(page_cleanup_expected) /
                  sizeof(page_cleanup_expected[0]));
    assert(app_page_is_actived("a", "root"));

    s_a_root.pause_failures = 1U;
    s_b_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_FAIL);
    assert(app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("b", "root"));
    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_page_is_actived("a", "root"));
}

static void _test_async_transition_queries(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    call.request.transition.effect = APP_MANAGER_TRANSITION_FADE;
    call.request.transition.duration_ms = 50U;
    assert(app_manager_ui_call(_nav_on_ui, &call, TEST_TIMEOUT_MS) == ESP_OK);
    assert(call.completions == 0U);
    assert(app_manager_is_transitioning());
    assert(s_presentation.running);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE, NULL);
    assert(s_presentation.last_effect == APP_MANAGER_TRANSITION_FADE);
    assert(s_presentation.last_duration_ms == 50U);
    assert(!app_manager_is_actived("a"));
    assert(app_manager_get_active_app_id() == NULL);
    assert(!app_manager_is_page_present("a", "root"));
    assert(!app_manager_is_all_closed());
    assert(app_manager_get_running_apps() == 0U);

    assert(app_manager_ui_call(_finish_on_ui, NULL, TEST_TIMEOUT_MS) == ESP_OK);
    assert(call.completions == 1U && call.completion_result == ESP_OK);
    assert(!app_manager_is_transitioning());
    assert(app_manager_is_actived("b"));
    assert(_live_screen_count() == 1U);

    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    call.request.has_arguments = true;
    call.request.arguments = _arguments(11U, UINT32_C(0x11223344));
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    assert(s_trace_count > 1U);
    assert(s_trace[0].page == &s_b_root);
    assert(s_trace[0].message == APP_MANAGER_MSG_ONNEWINTENT);
    assert(!s_b_root.new_intent_had_screen);
    assert(s_b_root.arguments.type == 11U);
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
}

static void _test_resident_eviction_and_cleanup(void)
{
    /* The suite runs with four resident slots (one pinned home-style app),
     * matching the product policy. */
    assert(app_manager_get_running_apps() == 2U);
    nav_call_t fill = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&fill) == ESP_OK);
    fill = _request(APP_MANAGER_NAV_OP_RUN, "d", NULL);
    assert(_navigate(&fill) == ESP_OK);
    assert(app_manager_get_running_apps() == 4U);
    nav_call_t missing = _request(APP_MANAGER_NAV_OP_RUN, "missing", NULL);
    assert(_navigate(&missing) == ESP_ERR_NOT_FOUND);
    assert(app_manager_is_page_present("b", "root"));
    assert(app_manager_get_running_apps() == 4U);

    s_b_root.stop_failures = 1U;
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "e", NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    const trace_event_t eviction_fail_expected[] =
    {
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(eviction_fail_expected,
                  sizeof(eviction_fail_expected) /
                  sizeof(eviction_fail_expected[0]));
    assert(app_manager_get_running_apps() == 4U);

    call = _request(APP_MANAGER_NAV_OP_RUN, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_get_running_apps() == 4U);
    call = _request(APP_MANAGER_NAV_OP_RUN, "e", NULL);
    call.request.has_arguments = true;
    call.request.arguments = _arguments(9U, UINT32_C(0xA5A5A5A5));
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t ops_intent_expected[] =
    {
        EVENT(&s_e_root, APP_MANAGER_MSG_ONNEWINTENT),
    };
    _expect_trace(ops_intent_expected,
                  sizeof(ops_intent_expected) /
                  sizeof(ops_intent_expected[0]));
    assert(s_e_root.new_intent_had_screen);
    assert(s_e_root.has_arguments && s_e_root.arguments.type == 9U);
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);

    s_e_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    assert(_navigate(&call) == ESP_FAIL);
    /* The failed task stays resident and visible to recent-tasks, but is
     * hidden from the conservative public topology queries until its cleanup
     * retry succeeds. */
    assert(!app_manager_is_page_present("e", "root"));
    assert(app_manager_get_running_apps() == 4U);
    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(!app_manager_is_page_present("e", "root"));
    assert(app_manager_get_running_apps() == 1U);

    call = _request(APP_MANAGER_NAV_OP_EXIT, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_is_all_closed());

    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
}

static void _start_animated(nav_call_t *call)
{
    call->request.transition.effect = APP_MANAGER_TRANSITION_FADE;
    call->request.transition.duration_ms = 40U;
    assert(app_manager_ui_call(_nav_on_ui, call, TEST_TIMEOUT_MS) == ESP_OK);
    assert(call->completions == 0U);
    assert(app_manager_is_transitioning());
    _expect_start_handoff_complete();
}

static void _finish_transition_by_suspend(nav_call_t *call,
        const char *target_app, const char *target_page)
{
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    assert(call->completions == 1U && call->completion_result == ESP_OK);
    assert(_live_screen_count() == 0U);
    assert(app_manager_ui_call(_screen_resume_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    assert(app_manager_is_actived(target_app));
    assert(app_page_is_actived(target_app, target_page));
}

static void _test_suspend_fast_forward_targets(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    _start_animated(&call);
    _finish_transition_by_suspend(&call, "a", "one");

    call = _request(APP_MANAGER_NAV_OP_BACK, NULL, NULL);
    _start_animated(&call);
    _finish_transition_by_suspend(&call, "a", "root");

    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    _start_animated(&call);
    _finish_transition_by_suspend(&call, "b", "root");

    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    _start_animated(&call);
    _finish_transition_by_suspend(&call, "a", "root");
}

static void _test_stop_rollback_keeps_target(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    s_b_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    _trace_reset();
    _start_animated(&call);
    s_presentation.create_failures = 1U;
    assert(app_manager_ui_call(_finish_on_ui, NULL, TEST_TIMEOUT_MS) == ESP_OK);
    assert(call.completions == 1U && call.completion_result == ESP_FAIL);
    const trace_event_t target_fallback_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
    };
    _expect_trace(target_fallback_expected,
                  sizeof(target_fallback_expected) /
                  sizeof(target_fallback_expected[0]));
    assert(!app_manager_is_transitioning());
    assert(app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_get_running_apps() == 2U);
    assert(_live_screen_count() == 1U);

    call = _request(APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, NULL, NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    const trace_event_t cleanup_expected[] =
    {
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
    };
    _expect_trace(cleanup_expected,
                  sizeof(cleanup_expected) / sizeof(cleanup_expected[0]));
    assert(app_manager_is_actived("a"));
    assert(app_manager_get_running_apps() == 1U);
}

static void _test_stop_rollback_load_failure_suspend_cleanup(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    s_b_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    _trace_reset();
    _start_animated(&call);
    s_presentation.load_failures = 1U;
    assert(app_manager_ui_call(_finish_on_ui, NULL, TEST_TIMEOUT_MS) == ESP_OK);
    assert(call.completions == 1U && call.completion_result == ESP_FAIL);
    const trace_event_t fallback_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
    };
    _expect_trace(fallback_expected,
                  sizeof(fallback_expected) / sizeof(fallback_expected[0]));
    assert(app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_get_running_apps() == 2U);
    assert(_live_screen_count() == 2U);

    _trace_reset();
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    const trace_event_t pause_expected[] =
    {
        EVENT(&s_b_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_b_root, APP_MANAGER_MSG_ONSTOP),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONUNMOUNT),
    };
    _expect_trace(pause_expected,
                  sizeof(pause_expected) / sizeof(pause_expected[0]));
    assert(_live_screen_count() == 0U);
    assert(app_manager_get_running_apps() == 1U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    assert(app_manager_is_actived("a"));
    assert(_live_screen_count() == 1U);
}

static void _test_screen_suspend_and_delete_retry(void)
{
    s_presentation.delete_failures = 1U;
    _trace_reset();
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_FAIL);
    const trace_event_t delete_fail_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONUNMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
    };
    _expect_trace(delete_fail_expected,
                  sizeof(delete_fail_expected) /
                  sizeof(delete_fail_expected[0]));
    assert(app_manager_is_actived("a"));
    assert(_live_screen_count() == 1U);

    s_a_root.pause_failures = 1U;
    _trace_reset();
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_FAIL);
    const trace_event_t pause_fail_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONPAUSE),
    };
    _expect_trace(pause_fail_expected,
                  sizeof(pause_fail_expected) /
                  sizeof(pause_fail_expected[0]));
    assert(app_manager_is_actived("a"));

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    assert(!app_manager_is_actived("a"));
    assert(!app_manager_is_page_present("a", "root"));
    assert(!app_manager_is_all_closed());
    assert(app_manager_get_running_apps() == 1U);
    assert(_live_screen_count() == 0U);
    _trace_reset();
    assert(app_manager_ui_call(_screen_resume_on_ui, NULL, TEST_TIMEOUT_MS) ==
           ESP_OK);
    const trace_event_t resume_expected[] =
    {
        EVENT(&s_a_root, APP_MANAGER_MSG_ONMOUNT),
        EVENT(&s_a_root, APP_MANAGER_MSG_ONRESUME),
    };
    _expect_trace(resume_expected,
                  sizeof(resume_expected) / sizeof(resume_expected[0]));
    assert(app_manager_is_actived("a"));
}

static void _test_validation_and_final_exit(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    call.request.has_arguments = true;
    call.request.arguments = _arguments(1U, 1U);
    call.request.arguments.version++;
    assert(_navigate(&call) == ESP_ERR_INVALID_ARG);

    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "missing");
    assert(_navigate(&call) == ESP_ERR_NOT_FOUND);
    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "c", "one");
    assert(_navigate(&call) == ESP_ERR_NOT_FOUND);
    call = _request(APP_MANAGER_NAV_OP_EXIT, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_is_all_closed());
    assert(app_manager_get_running_apps() == 0U);
    assert(_live_screen_count() == 0U);
    assert(s_presentation.active == &s_presentation.neutral);
}

static void _test_ops_pause_cleanup_retry(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    uint8_t *instance_memory = s_c_root.instance_memory;
    lv_obj_t *screen = s_c_root.last_screen;
    assert(instance_memory != NULL && instance_memory[1] == 0x5AU);

    s_c_root.pause_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_FAIL);
    assert(app_manager_is_actived("c"));
    assert(app_page_is_actived("c", "root"));
    assert(s_c_root.instance_memory == instance_memory);
    assert(s_c_root.last_screen == screen);
    assert(s_c_root.mounted && s_c_root.lifecycle_active);
    assert(instance_memory[1] == 0x5AU);

    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(!app_manager_is_actived("c"));
    assert(app_manager_is_page_present("c", "root"));
    assert(s_c_root.instance_memory == instance_memory);
    assert(!s_c_root.mounted && !s_c_root.lifecycle_active);
    assert(instance_memory[1] == 0U);

    call = _request(APP_MANAGER_NAV_OP_EXIT, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_EXIT, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_is_all_closed());
}

static void _test_empty_ops_are_noop(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "d", NULL);
    _trace_reset();
    assert(_navigate(&call) == ESP_OK);
    _expect_trace(NULL, 0U);
    assert(app_manager_is_actived("d"));

    call = _request(APP_MANAGER_NAV_OP_EXIT, "d", NULL);
    assert(_navigate(&call) == ESP_OK);
    _expect_trace(NULL, 0U);
    assert(app_manager_is_all_closed());
}

static void _test_recent_tasks_snapshot(void)
{
    static uint8_t s_test_source_pixels[368U * 448U * 2U];
    static lv_draw_buf_t s_test_source_draw_buf;
    app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
    size_t count = 0U;
    nav_call_t call;

    /* The suite runs with four resident slots (one pinned home-style app),
     * so up to three business tasks are retained at once. */
    _trace_reset();
    assert(app_manager_is_all_closed());
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);

    /* Pinned home-style apps never appear in the task list. */
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 0U);

    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 2U);
    assert(strcmp(tasks[0].app_id, "c") == 0);
    assert(strcmp(tasks[0].display_name, "C") == 0);
    assert(strcmp(tasks[0].page_id, "root") == 0);
    assert(!tasks[0].preview_valid);
    assert(strcmp(tasks[1].app_id, "b") == 0);
    assert(!tasks[1].preview_valid);

    /* Capture is unavailable by default; recent previews stay invalid. */
    s_test_capture_result = NULL;
    const unsigned captures_before_invalid =
        s_presentation.capture_calls;
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(s_presentation.capture_calls == captures_before_invalid + 1U);
    assert(s_presentation.last_capture_screen == s_c_root.last_screen);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 2U);
    assert(!tasks[0].preview_valid);

    /* Enable source capture; leaving an app refreshes its preview. */
    memset(s_test_source_pixels, 0xAA, sizeof(s_test_source_pixels));
    s_test_source_draw_buf.header.cf = LV_COLOR_FORMAT_RGB565;
    s_test_source_draw_buf.header.w = 368U;
    s_test_source_draw_buf.header.h = 448U;
    s_test_source_draw_buf.header.stride = 368U * 2U;
    s_test_source_draw_buf.data = s_test_source_pixels;
    s_test_source_draw_buf.data_size = sizeof(s_test_source_pixels);
    s_test_capture_result = &s_test_source_draw_buf;
    const unsigned captures_before_valid = s_presentation.capture_calls;
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(s_presentation.capture_calls == captures_before_valid + 1U);
    assert(s_presentation.last_capture_screen == s_b_root.last_screen);
    call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "d", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(s_presentation.capture_calls == captures_before_valid + 2U);
    assert(s_presentation.last_capture_screen == s_c_root.last_screen);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 3U);
    assert(strcmp(tasks[0].app_id, "d") == 0);
    assert(!tasks[0].preview_valid);
    assert(strcmp(tasks[1].app_id, "c") == 0);
    assert(tasks[1].preview_valid);
    assert(tasks[1].preview != NULL);
    assert(tasks[1].preview->header.w == APP_MANAGER_RECENT_PREVIEW_WIDTH);
    assert(tasks[1].preview->header.h == APP_MANAGER_RECENT_PREVIEW_HEIGHT);
    assert(tasks[1].preview->header.stride ==
           APP_MANAGER_RECENT_PREVIEW_WIDTH * 2U);
    const uint8_t *pixels = tasks[1].preview->data;
    assert(pixels[0] == 0xAAU && pixels[1] == 0xAAU);
    assert(pixels[(APP_MANAGER_RECENT_PREVIEW_HEIGHT - 1U) *
                  APP_MANAGER_RECENT_PREVIEW_WIDTH * 2U] == 0xAAU);
    assert(strcmp(tasks[2].app_id, "b") == 0);
    assert(tasks[2].preview_valid);

    /* A fifth task evicts the oldest background task and its preview. */
    _trace_reset();
    const unsigned captures_before_eviction = s_presentation.capture_calls;
    call = _request(APP_MANAGER_NAV_OP_RUN, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(s_presentation.capture_calls == captures_before_eviction + 1U);
    assert(s_presentation.last_capture_screen != NULL);
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 3U);
    assert(strcmp(tasks[0].app_id, "e") == 0);
    assert(!tasks[0].preview_valid);
    assert(strcmp(tasks[1].app_id, "d") == 0);
    assert(tasks[1].preview_valid);
    assert(strcmp(tasks[2].app_id, "c") == 0);
    assert(tasks[2].preview_valid);

    /* EXIT invalidates the preview binding; re-running recaptures it. */
    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_EXIT, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 2U);
    assert(strcmp(tasks[0].app_id, "d") == 0);

    const unsigned captures_before_recapture = s_presentation.capture_calls;
    call = _request(APP_MANAGER_NAV_OP_RUN, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    /* RUN e captures d; RUN a captures e. */
    assert(s_presentation.capture_calls == captures_before_recapture + 2U);
    assert(s_presentation.last_capture_screen == s_e_root.last_screen);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 3U);
    assert(strcmp(tasks[0].app_id, "e") == 0);
    assert(tasks[0].preview_valid);

    /* Re-running an evicted task evicts the now-oldest background task. */
    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(!app_manager_is_page_present("c", "root"));
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 3U);
    assert(strcmp(tasks[0].app_id, "b") == 0);
    assert(!tasks[0].preview_valid);

    _trace_reset();
    s_test_capture_result = NULL;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_EXIT, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_EXIT, "d", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
}

static void _test_product_eviction_recents(void)
{
    static uint8_t s_test_source_pixels[368U * 448U * 2U];
    static lv_draw_buf_t s_test_source_draw_buf;
    app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
    size_t count = 0U;
    nav_call_t call;

    /* Product policy: Home (pinned) plus three business tasks. The fourth
     * business task evicts the oldest background task. */
    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    memset(s_test_source_pixels, 0x55, sizeof(s_test_source_pixels));
    s_test_source_draw_buf.header.cf = LV_COLOR_FORMAT_RGB565;
    s_test_source_draw_buf.header.w = 368U;
    s_test_source_draw_buf.header.h = 448U;
    s_test_source_draw_buf.header.stride = 368U * 2U;
    s_test_source_draw_buf.data = s_test_source_pixels;
    s_test_source_draw_buf.data_size = sizeof(s_test_source_pixels);
    s_test_capture_result = &s_test_source_draw_buf;

    call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "d", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_get_running_apps() == 4U);
    assert(app_manager_is_page_present("a", "root"));
    assert(app_manager_is_page_present("b", "root"));
    assert(app_manager_is_page_present("c", "root"));
    assert(app_manager_is_page_present("d", "root"));

    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_get_running_apps() == 4U);
    assert(!app_manager_is_page_present("b", "root"));
    assert(app_manager_is_page_present("a", "root"));
    assert(app_manager_is_page_present("c", "root"));
    assert(app_manager_is_page_present("d", "root"));
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 3U);
    assert(strcmp(tasks[0].app_id, "e") == 0);
    assert(!tasks[0].preview_valid);
    assert(strcmp(tasks[1].app_id, "d") == 0);
    assert(tasks[1].preview_valid);
    assert(strcmp(tasks[2].app_id, "c") == 0);
    assert(tasks[2].preview_valid);

    /* Leaving e refreshes its preview; the active task is never evicted. */
    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 3U);
    assert(strcmp(tasks[0].app_id, "e") == 0);
    assert(tasks[0].preview_valid);
    assert(strcmp(tasks[1].app_id, "d") == 0);
    assert(tasks[1].preview_valid);
    assert(strcmp(tasks[2].app_id, "c") == 0);
    assert(tasks[2].preview_valid);

    _trace_reset();
    s_test_capture_result = NULL;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "e", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_EXIT, "d", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_EXIT, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
}

static __attribute__((unused)) esp_err_t _capture_active_on_ui(void *context)
{
    (void)context;
    return app_manager_lifecycle_capture_active_recents();
}

static __attribute__((unused)) esp_err_t _clear_tasks_on_ui(void *context)
{
    (void)context;
    return app_manager_lifecycle_clear_background_tasks();
}

static void _test_task_switcher_helpers(void)
{
    static uint8_t s_test_source_pixels[368U * 448U * 2U];
    static lv_draw_buf_t s_test_source_draw_buf;
    app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
    size_t count = 0U;
    nav_call_t call;

    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 2U);
    assert(strcmp(tasks[0].app_id, "c") == 0);
    assert(tasks[0].active);
    assert(!tasks[0].cleanup_pending);
    assert(strcmp(tasks[1].app_id, "b") == 0);
    assert(!tasks[1].active);
    assert(!tasks[1].cleanup_pending);

    memset(s_test_source_pixels, 0x33, sizeof(s_test_source_pixels));
    s_test_source_draw_buf.header.cf = LV_COLOR_FORMAT_RGB565;
    s_test_source_draw_buf.header.w = 368U;
    s_test_source_draw_buf.header.h = 448U;
    s_test_source_draw_buf.header.stride = 368U * 2U;
    s_test_source_draw_buf.data = s_test_source_pixels;
    s_test_source_draw_buf.data_size = sizeof(s_test_source_pixels);
    s_test_capture_result = &s_test_source_draw_buf;
    const unsigned captures_before = s_presentation.capture_calls;
    assert(app_manager_ui_call(_capture_active_on_ui, NULL,
                               TEST_TIMEOUT_MS) == ESP_OK);
    assert(s_presentation.capture_calls == captures_before + 1U);
    assert(s_presentation.last_capture_screen == s_c_root.last_screen);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 2U);
    assert(strcmp(tasks[0].app_id, "c") == 0);
    assert(tasks[0].preview_valid);

    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    s_c_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "c", NULL);
    assert(_navigate(&call) == ESP_FAIL);
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 2U);
    assert(strcmp(tasks[0].app_id, "b") == 0);
    assert(tasks[0].active);
    assert(strcmp(tasks[1].app_id, "c") == 0);
    assert(tasks[1].cleanup_pending);

    /* An exact EXIT retry succeeds: the pending target is cleaned up by the
     * EXIT path itself instead of reporting NOT_FOUND. */
    call = _request(APP_MANAGER_NAV_OP_EXIT, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(!app_manager_is_page_present("c", "root"));

    /* Recreating and leaving the task again refreshes the picture. */
    call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_is_actived("c"));
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);
    s_c_root.stop_failures = 1U;
    call = _request(APP_MANAGER_NAV_OP_EXIT, "c", NULL);
    assert(_navigate(&call) == ESP_FAIL);
    call = _request(APP_MANAGER_NAV_OP_RUN, "c", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_is_actived("c"));
    call = _request(APP_MANAGER_NAV_OP_RUN, "b", NULL);
    assert(_navigate(&call) == ESP_OK);

    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    assert(app_manager_ui_call(_clear_tasks_on_ui, NULL,
                               TEST_TIMEOUT_MS) == ESP_OK);
    assert(!app_manager_is_page_present("b", "root"));
    assert(!app_manager_is_page_present("c", "root"));
    assert(app_manager_get_recent_tasks(tasks, APP_MANAGER_MAX_RECENT_TASKS,
                                        &count) == ESP_OK);
    assert(count == 0U);

    _trace_reset();
    call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
}

static void _prepare_deinit_cleanup_retry(void)
{
    nav_call_t call = _request(APP_MANAGER_NAV_OP_RUN, "a", NULL);
    assert(_navigate(&call) == ESP_OK);
    s_a_root.pause_failures = 1U;
    s_a_one.stop_failures = 2U;
    call = _request(APP_MANAGER_NAV_OP_OPEN_PAGE, "a", "one");
    assert(_navigate(&call) == ESP_FAIL);
    assert(app_page_is_actived("a", "root"));
    assert(!app_manager_is_page_present("a", "one"));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    _presentation_reset();
    _test_configuration_validation();
    assert(app_manager_register_builtin_descriptors(
               s_apps, sizeof(s_apps) / sizeof(s_apps[0])) == ESP_OK);
    assert(app_manager_builtin_registry_validate() == ESP_OK);
    assert(app_manager_mailbox_init() == ESP_OK);
    assert(app_manager_lifecycle_init() == ESP_OK);
    app_manager_lifecycle_host_set_observer(_lifecycle_observer);
    _expect_back_target(APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE, NULL);

    _lifecycle_log_capture_begin();
    _test_run_and_page_navigation();
    _test_presentation_start_failure_rollback();
    _test_forward_unmount_and_target_pause_failure();
    _test_pause_and_stop_rollback();
    _test_shared_definition_and_target_pause_rollback();
    _test_uncommitted_cleanup_ownership();
    _test_async_transition_queries();
    _test_resident_eviction_and_cleanup();
    _test_suspend_fast_forward_targets();
    _test_stop_rollback_keeps_target();
    _test_stop_rollback_load_failure_suspend_cleanup();
    _test_screen_suspend_and_delete_retry();
    _test_validation_and_final_exit();
    _test_ops_pause_cleanup_retry();
    _test_empty_ops_are_noop();
    _test_recent_tasks_snapshot();
    _test_product_eviction_recents();
    _test_task_switcher_helpers();
    _prepare_deinit_cleanup_retry();

    assert(app_manager_lifecycle_shutdown_begin_and_wait() == ESP_OK);
    assert(app_manager_mailbox_shutdown_call(_deinit_on_ui, NULL) == ESP_FAIL);
    assert(app_manager_mailbox_shutdown_call(_deinit_on_ui, NULL) == ESP_OK);
    assert(s_callback_owner_app_id == NULL);
    app_manager_lifecycle_host_set_observer(NULL);
    assert(app_manager_mailbox_deinit() == ESP_OK);
    app_manager_builtin_registry_reset();
    _lifecycle_log_capture_end();
    return 0;
}
