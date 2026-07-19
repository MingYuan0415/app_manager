#include "app_manager.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define HOST_POST_CAPACITY 64U

typedef enum host_lifecycle_mode
{
    HOST_LIFECYCLE_COMPLETE_SYNC = 0,
    HOST_LIFECYCLE_REJECT,
    HOST_LIFECYCLE_COMPLETE_DEFERRED,
} host_lifecycle_mode_t;

typedef struct host_posted_call
{
    void (*callback)(void *);
    void *context;
} host_posted_call_t;

typedef struct host_sync_call
{
    app_manager_nav_request_t request;
    uint32_t timeout_ms;
    esp_err_t result;
    atomic_bool complete;
} host_sync_call_t;

typedef struct host_async_call
{
    app_manager_nav_request_t request;
    unsigned tag;
    esp_err_t result;
    atomic_bool started;
    atomic_bool complete;
} host_async_call_t;

static pthread_mutex_t s_post_lock = PTHREAD_MUTEX_INITIALIZER;
static host_posted_call_t s_posts[HOST_POST_CAPACITY];
static size_t s_post_count;
static _Thread_local bool s_ui_worker;
static host_lifecycle_mode_t s_lifecycle_mode;
static app_manager_lifecycle_nav_complete_t s_deferred_completion;
static void *s_deferred_context;
static bool s_presentation_running;
static unsigned s_presentation_finish_count;
static unsigned s_lifecycle_call_count;
static app_manager_nav_request_t s_observed_request;
static char s_observed_app_id[APP_MANAGER_ID_BYTES];
static char s_observed_page_id[APP_MANAGER_ID_BYTES];
static atomic_uint s_completion_count;
static atomic_int s_completion_result;
static atomic_bool s_screen_accepting;
static atomic_uint s_screen_access_count;
static atomic_uint s_callback_owner_query_count;
static const char *s_callback_app_id;
static app_manager_lifecycle_back_target_t s_back_target;
static const char *s_back_source_app_id;
static unsigned s_gesture_begin_count;
static unsigned s_gesture_settled_count;
static bool s_acquire_token_on_settled;
static bool s_settled_token_acquired;
static atomic_bool s_block_next_post;
static atomic_bool s_blocked_post_entered;
static atomic_bool s_blocked_post_release;
static unsigned s_ordered_completions[2];
static size_t s_ordered_completion_count;

static void _reset_observation(void)
{
    s_lifecycle_mode = HOST_LIFECYCLE_COMPLETE_SYNC;
    s_deferred_completion = NULL;
    s_deferred_context = NULL;
    s_presentation_running = false;
    s_presentation_finish_count = 0U;
    s_lifecycle_call_count = 0U;
    memset(&s_observed_request, 0, sizeof(s_observed_request));
    memset(s_observed_app_id, 0, sizeof(s_observed_app_id));
    memset(s_observed_page_id, 0, sizeof(s_observed_page_id));
    atomic_store(&s_completion_count, 0U);
    atomic_store(&s_completion_result, ESP_OK);
    assert(atomic_load(&s_screen_access_count) == 0U);
    atomic_store(&s_screen_accepting, true);
    atomic_store(&s_callback_owner_query_count, 0U);
    s_callback_app_id = NULL;
    s_back_target = APP_MANAGER_LIFECYCLE_BACK_TARGET_PAGE;
    s_back_source_app_id = "settings";
    s_gesture_begin_count = 0U;
    s_gesture_settled_count = 0U;
    s_acquire_token_on_settled = false;
    s_settled_token_acquired = false;
    atomic_store(&s_block_next_post, false);
    atomic_store(&s_blocked_post_entered, false);
    atomic_store(&s_blocked_post_release, false);
    s_ordered_completion_count = 0U;
    memset(s_ordered_completions, 0, sizeof(s_ordered_completions));
}

static size_t _post_count(void)
{
    pthread_mutex_lock(&s_post_lock);
    const size_t count = s_post_count;
    pthread_mutex_unlock(&s_post_lock);
    return count;
}

static bool _run_one_post(void)
{
    host_posted_call_t call = {0};

    pthread_mutex_lock(&s_post_lock);
    if (s_post_count > 0U)
    {
        call = s_posts[0];
        --s_post_count;
        memmove(&s_posts[0], &s_posts[1],
                s_post_count * sizeof(s_posts[0]));
    }
    pthread_mutex_unlock(&s_post_lock);
    if (call.callback == NULL)
    {
        return false;
    }

    s_ui_worker = true;
    call.callback(call.context);
    s_ui_worker = false;
    return true;
}

static void _run_all_posts(void)
{
    while (_run_one_post())
    {
    }
}

static void _complete_deferred(esp_err_t result)
{
    app_manager_lifecycle_nav_complete_t completion = s_deferred_completion;
    void *context = s_deferred_context;

    assert(completion != NULL);
    s_deferred_completion = NULL;
    s_deferred_context = NULL;
    s_presentation_running = false;
    s_ui_worker = true;
    completion(result, context);
    s_ui_worker = false;
}

static void _completion(esp_err_t result, void *context)
{
    assert(context == &s_completion_count);
    assert(s_ui_worker);
    assert(s_gesture_settled_count >= s_gesture_begin_count);
    atomic_store(&s_completion_result, result);
    atomic_fetch_add(&s_completion_count, 1U);
}

static void _ordered_completion(esp_err_t result, void *context)
{
    host_async_call_t *call = context;
    assert(result == ESP_OK);
    assert(s_ui_worker);
    assert(s_gesture_settled_count >= s_gesture_begin_count);
    assert(s_ordered_completion_count <
           sizeof(s_ordered_completions) /
           sizeof(s_ordered_completions[0]));
    s_ordered_completions[s_ordered_completion_count++] = call->tag;
}

static void _noop_post(void *context)
{
    (void)context;
    assert(s_ui_worker);
}

static void *_sync_navigation_thread(void *context)
{
    host_sync_call_t *call = context;
    call->result = app_manager_navigate(&call->request, call->timeout_ms);
    atomic_store(&call->complete, true);
    return NULL;
}

static void *_async_navigation_thread(void *context)
{
    host_async_call_t *call = context;
    atomic_store(&call->started, true);
    call->result = app_manager_navigate_async(
                       &call->request, _ordered_completion, call);
    atomic_store(&call->complete, true);
    return NULL;
}

bool app_manager_mailbox_is_worker(void)
{
    return s_ui_worker;
}

esp_err_t app_manager_ui_post(void (*callback)(void *), void *context)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (atomic_exchange(&s_block_next_post, false))
    {
        atomic_store(&s_blocked_post_entered, true);
        while (!atomic_load(&s_blocked_post_release))
        {
            usleep(1000);
        }
    }

    pthread_mutex_lock(&s_post_lock);
    if (s_post_count == HOST_POST_CAPACITY)
    {
        pthread_mutex_unlock(&s_post_lock);
        return ESP_ERR_NO_MEM;
    }
    s_posts[s_post_count++] = (host_posted_call_t)
    {
        .callback = callback,
        .context = context,
    };
    pthread_mutex_unlock(&s_post_lock);
    return ESP_OK;
}

esp_err_t app_manager_ui_call(app_manager_ui_call_fn callback, void *context,
                              uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (callback == NULL || s_ui_worker)
    {
        return ESP_ERR_INVALID_STATE;
    }

    _run_all_posts();
    s_ui_worker = true;
    const esp_err_t result = callback(context);
    s_ui_worker = false;
    return result;
}

esp_err_t app_manager_lifecycle_navigate(
    const app_manager_nav_request_t *request,
    app_manager_lifecycle_nav_complete_t completion, void *context)
{
    assert(s_ui_worker);
    assert(request != NULL);
    assert(completion != NULL);
    ++s_lifecycle_call_count;
    s_observed_request = *request;
    if (request->app_id != NULL)
    {
        (void)snprintf(s_observed_app_id, sizeof(s_observed_app_id), "%s",
                       request->app_id);
    }
    if (request->page_id != NULL)
    {
        (void)snprintf(s_observed_page_id, sizeof(s_observed_page_id), "%s",
                       request->page_id);
    }

    if (s_lifecycle_mode == HOST_LIFECYCLE_REJECT)
    {
        return ESP_FAIL;
    }
    if (s_lifecycle_mode == HOST_LIFECYCLE_COMPLETE_DEFERRED)
    {
        assert(s_deferred_completion == NULL);
        s_deferred_completion = completion;
        s_deferred_context = context;
        s_presentation_running = true;
        return ESP_OK;
    }
    completion(ESP_OK, context);
    return ESP_OK;
}

const char *app_manager_lifecycle_callback_app_id(void)
{
    assert(s_ui_worker);
    atomic_fetch_add(&s_callback_owner_query_count, 1U);
    return s_callback_app_id;
}

app_manager_lifecycle_back_target_t app_manager_lifecycle_back_target(
    const char **source_app_id)
{
    assert(s_ui_worker);
    assert(source_app_id != NULL);
    *source_app_id = s_back_target !=
                     APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE ?
                     s_back_source_app_id : NULL;
    return s_back_target;
}

void app_manager_back_gesture_navigation_begin(void)
{
    assert(s_ui_worker);
    s_gesture_begin_count++;
}

void app_manager_back_gesture_navigation_settled(void)
{
    assert(s_ui_worker);
    s_gesture_settled_count++;
    if (s_acquire_token_on_settled)
    {
        app_manager_navigation_gesture_token_t token = {0};
        s_settled_token_acquired =
            app_manager_navigation_gesture_token_acquire(&token);
    }
}

bool app_manager_lifecycle_screen_access_begin(void)
{
    atomic_fetch_add(&s_screen_access_count, 1U);
    const bool admitted = atomic_load(&s_screen_accepting);
    if (!admitted)
    {
        assert(atomic_fetch_sub(&s_screen_access_count, 1U) > 0U);
    }
    return admitted;
}

void app_manager_lifecycle_screen_access_end(void)
{
    assert(atomic_fetch_sub(&s_screen_access_count, 1U) > 0U);
}

bool app_manager_presentation_is_running(void)
{
    return s_presentation_running;
}

void app_manager_presentation_finish_now(void)
{
    ++s_presentation_finish_count;
    if (s_deferred_completion != NULL)
    {
        app_manager_lifecycle_nav_complete_t completion =
            s_deferred_completion;
        void *context = s_deferred_context;
        s_deferred_completion = NULL;
        s_deferred_context = NULL;
        s_presentation_running = false;
        completion(ESP_OK, context);
    }
}

static app_manager_nav_request_t _request(void)
{
    return (app_manager_nav_request_t)
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = "settings",
        .page_id = "network",
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_PUSH_LEFT,
            .duration_ms = 220U,
        },
    };
}

static void _reentrant_completion(esp_err_t result, void *context)
{
    assert(context == &s_completion_count);
    assert(s_ui_worker);
    assert(result == ESP_OK);
    assert(s_gesture_settled_count >= s_gesture_begin_count);
    const unsigned previous = atomic_fetch_add(&s_completion_count, 1U);
    if (previous == 0U)
    {
        const app_manager_nav_request_t request = _request();
        assert(app_manager_navigate_async(
                   &request, _reentrant_completion,
                   &s_completion_count) == ESP_OK);
    }
}

static void _test_ui_worker_rejected(void)
{
    app_manager_nav_request_t request = _request();
    s_ui_worker = true;
    assert(app_manager_navigate(&request, UINT32_MAX) ==
           ESP_ERR_INVALID_STATE);
    s_ui_worker = false;
    assert(_post_count() == 0U);
}

static void _test_gesture_token_and_conditional_back(void)
{
    app_manager_navigation_gesture_token_t token = {0};
    s_ui_worker = true;
    assert(app_manager_navigation_gesture_token_acquire(&token));
    assert(token.epoch != 0U);
    assert(app_manager_navigation_gesture_token_is_current(&token));
    assert(app_manager_navigation_submit_gesture_back(
               &token, _completion, &s_completion_count) == ESP_OK);
    assert(!app_manager_navigation_gesture_token_is_current(&token));
    app_manager_navigation_gesture_token_t blocked = {0};
    assert(!app_manager_navigation_gesture_token_acquire(&blocked));
    s_ui_worker = false;

    s_acquire_token_on_settled = true;
    assert(_post_count() == 1U);
    assert(atomic_load(&s_screen_access_count) == 1U);
    assert(_run_one_post());
    assert(s_observed_request.operation == APP_MANAGER_NAV_OP_BACK);
    assert(strcmp(s_observed_app_id, "settings") == 0);
    assert(s_observed_page_id[0] == '\0');
    assert(s_observed_request.transition.effect ==
           APP_MANAGER_TRANSITION_DEFAULT);
    assert(s_observed_request.transition.duration_ms == 0U);
    assert(s_observed_request.transition.reserved == 0U);
    assert(atomic_load(&s_completion_count) == 1U);
    assert(s_gesture_begin_count == 1U);
    assert(s_gesture_settled_count == 1U);
    assert(s_settled_token_acquired);
    assert(!app_manager_navigation_gesture_token_is_current(&token));

    app_manager_navigation_gesture_token_t next = {0};
    assert(app_manager_navigation_gesture_token_acquire(&next));
    assert(next.epoch != token.epoch);
}

static void _test_stale_gesture_does_not_back_new_foreground(void)
{
    app_manager_navigation_gesture_token_t token = {0};
    s_ui_worker = true;
    assert(app_manager_navigation_gesture_token_acquire(&token));
    s_ui_worker = false;

    const app_manager_nav_request_t request = _request();
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    const size_t posts_before = _post_count();
    assert(posts_before == 1U);
    s_ui_worker = true;
    assert(!app_manager_navigation_gesture_token_is_current(&token));
    assert(app_manager_navigation_submit_gesture_back(
               &token, _completion, &s_completion_count) ==
           ESP_ERR_INVALID_STATE);
    s_ui_worker = false;
    assert(_post_count() == posts_before);
    assert(atomic_load(&s_screen_access_count) == 1U);

    assert(_run_one_post());
    assert(atomic_load(&s_completion_count) == 1U);
    assert(s_gesture_begin_count == 1U);
    assert(s_gesture_settled_count == 1U);
}

static void _test_gesture_post_failure_preserves_epoch(void)
{
    app_manager_navigation_gesture_token_t token = {0};
    s_ui_worker = true;
    assert(app_manager_navigation_gesture_token_acquire(&token));
    s_ui_worker = false;

    for (size_t index = 0; index < HOST_POST_CAPACITY; ++index)
    {
        assert(app_manager_ui_post(_noop_post, NULL) == ESP_OK);
    }
    s_ui_worker = true;
    assert(app_manager_navigation_submit_gesture_back(
               &token, _completion, &s_completion_count) == ESP_ERR_NO_MEM);
    assert(app_manager_navigation_gesture_token_is_current(&token));
    s_ui_worker = false;
    assert(atomic_load(&s_screen_access_count) == 0U);
    _run_all_posts();

    s_ui_worker = true;
    assert(app_manager_navigation_submit_gesture_back(
               &token, _completion, &s_completion_count) == ESP_OK);
    s_ui_worker = false;
    assert(_run_one_post());
    assert(atomic_load(&s_completion_count) == 1U);
}

static void _test_gesture_requires_back_target_and_ui_worker(void)
{
    app_manager_navigation_gesture_token_t token = {0};
    assert(app_manager_navigation_gesture_token_acquire(&token));
    assert(app_manager_navigation_submit_gesture_back(
               &token, _completion, &s_completion_count) ==
           ESP_ERR_INVALID_STATE);
    s_back_target = APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE;
    s_ui_worker = true;
    assert(app_manager_navigation_submit_gesture_back(
               &token, _completion, &s_completion_count) ==
           ESP_ERR_INVALID_STATE);
    assert(app_manager_navigation_gesture_token_is_current(&token));
    s_ui_worker = false;
    assert(_post_count() == 0U);
    assert(atomic_load(&s_screen_access_count) == 0U);
    assert(atomic_load(&s_completion_count) == 0U);
}

static void _test_concurrent_submission_preserves_fifo(void)
{
    host_async_call_t first =
    {
        .request = _request(),
        .tag = 1U,
    };
    host_async_call_t second =
    {
        .request = _request(),
        .tag = 2U,
    };
    first.request.app_id = "first";
    second.request.app_id = "second";
    pthread_t first_thread;
    pthread_t second_thread;

    atomic_store(&s_block_next_post, true);
    assert(pthread_create(&first_thread, NULL, _async_navigation_thread,
                          &first) == 0);
    for (unsigned attempt = 0;
            attempt < 1000U && !atomic_load(&s_blocked_post_entered);
            ++attempt)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_blocked_post_entered));
    assert(pthread_create(&second_thread, NULL, _async_navigation_thread,
                          &second) == 0);
    for (unsigned attempt = 0;
            attempt < 1000U && !atomic_load(&second.started); ++attempt)
    {
        usleep(1000);
    }
    assert(atomic_load(&second.started));
    usleep(5000);
    assert(_post_count() == 0U);

    atomic_store(&s_blocked_post_release, true);
    assert(pthread_join(first_thread, NULL) == 0);
    assert(pthread_join(second_thread, NULL) == 0);
    assert(first.result == ESP_OK);
    assert(second.result == ESP_OK);
    assert(_post_count() == 2U);
    _run_all_posts();
    assert(s_ordered_completion_count == 2U);
    assert(s_ordered_completions[0] == 1U);
    assert(s_ordered_completions[1] == 2U);
    assert(atomic_load(&s_screen_access_count) == 0U);
}

static void _test_deep_copy_and_completion(void)
{
    char app_id[APP_MANAGER_ID_BYTES] = "settings";
    char page_id[APP_MANAGER_ID_BYTES] = "network";
    app_manager_nav_request_t request = _request();
    request.app_id = app_id;
    request.page_id = page_id;
    request.has_arguments = true;
    request.arguments.version = APP_MANAGER_TYPED_BLOB_VERSION;
    request.arguments.type = 7U;
    request.arguments.size = 3U;
    memcpy(request.arguments.payload, "abc", 3U);

    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    assert(atomic_load(&s_screen_access_count) == 1U);
    memset(app_id, 'x', strlen(app_id));
    memset(page_id, 'y', strlen(page_id));
    memset(request.arguments.payload, 'z', 3U);
    assert(_run_one_post());
    assert(strcmp(s_observed_app_id, "settings") == 0);
    assert(strcmp(s_observed_page_id, "network") == 0);
    assert(memcmp(s_observed_request.arguments.payload, "abc", 3U) == 0);
    assert(atomic_load(&s_completion_count) == 1U);
    assert(atomic_load(&s_completion_result) == ESP_OK);
    assert(atomic_load(&s_screen_access_count) == 0U);
}

static void _test_rejection_completes_once(void)
{
    app_manager_nav_request_t request = _request();
    s_lifecycle_mode = HOST_LIFECYCLE_REJECT;
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    assert(_run_one_post());
    assert(atomic_load(&s_completion_count) == 1U);
    assert(atomic_load(&s_completion_result) == ESP_FAIL);
    assert(s_gesture_begin_count == 1U);
    assert(s_gesture_settled_count == 1U);
}

static void _test_completion_reentry_uses_fifo(void)
{
    const app_manager_nav_request_t request = _request();
    assert(app_manager_navigate_async(
               &request, _reentrant_completion,
               &s_completion_count) == ESP_OK);
    assert(_run_one_post());
    assert(atomic_load(&s_completion_count) == 1U);
    assert(_post_count() == 1U);
    assert(atomic_load(&s_screen_access_count) == 1U);

    assert(_run_one_post());
    assert(atomic_load(&s_completion_count) == 2U);
    assert(s_lifecycle_call_count == 2U);
    assert(_post_count() == 0U);
    assert(atomic_load(&s_screen_access_count) == 0U);
}

static void _test_exit_self_copies_callback_owner(void)
{
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT_SELF,
    };
    s_callback_app_id = "background-owner";
    s_ui_worker = true;
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    s_ui_worker = false;
    assert(atomic_load(&s_callback_owner_query_count) == 1U);
    assert(atomic_load(&s_screen_access_count) == 1U);
    s_callback_app_id = NULL;
    assert(_run_one_post());
    assert(strcmp(s_observed_app_id, "background-owner") == 0);
    assert(atomic_load(&s_completion_count) == 1U);
    assert(atomic_load(&s_screen_access_count) == 0U);
}

static void _test_exit_self_rejects_non_ui_owner_lookup(void)
{
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT_SELF,
    };
    s_callback_app_id = "concurrent-owner";
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) ==
           ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_callback_owner_query_count) == 0U);
    assert(atomic_load(&s_screen_access_count) == 0U);
    assert(_post_count() == 0U);
}

static void _test_exit_self_rejects_explicit_owner(void)
{
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT_SELF,
        .app_id = "not-self",
    };
    s_ui_worker = true;
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) ==
           ESP_ERR_INVALID_ARG);
    s_ui_worker = false;
    assert(atomic_load(&s_callback_owner_query_count) == 0U);
    assert(atomic_load(&s_screen_access_count) == 0U);
    assert(_post_count() == 0U);
}

static void _test_post_failure_releases_screen_access(void)
{
    for (size_t index = 0; index < HOST_POST_CAPACITY; ++index)
    {
        assert(app_manager_ui_post(_noop_post, NULL) == ESP_OK);
    }
    app_manager_nav_request_t request = _request();
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_screen_access_count) == 0U);
    _run_all_posts();
}

static void _test_fixed_pool_limit(void)
{
    app_manager_nav_request_t request = _request();
    for (size_t index = 0; index < APP_MANAGER_NAV_COMMAND_CAPACITY; ++index)
    {
        assert(app_manager_navigate_async(&request, _completion,
                                          &s_completion_count) == ESP_OK);
    }
    assert(atomic_load(&s_screen_access_count) ==
           APP_MANAGER_NAV_COMMAND_CAPACITY);
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_ERR_NO_MEM);
    _run_all_posts();
    assert(atomic_load(&s_completion_count) ==
           APP_MANAGER_NAV_COMMAND_CAPACITY);
    assert(atomic_load(&s_screen_access_count) == 0U);
}

static void _test_timeout_before_execution(void)
{
    app_manager_nav_request_t request = _request();
    const unsigned calls_before = s_lifecycle_call_count;
    s_acquire_token_on_settled = true;
    assert(app_manager_navigate(&request, 1U) == ESP_ERR_TIMEOUT);
    assert(atomic_load(&s_screen_access_count) == 1U);
    assert(_run_one_post());
    assert(s_lifecycle_call_count == calls_before);
    assert(atomic_load(&s_screen_access_count) == 0U);
    assert(s_gesture_begin_count == 0U);
    assert(s_gesture_settled_count == 1U);
    assert(s_settled_token_acquired);

    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    assert(_run_one_post());
    assert(atomic_load(&s_completion_count) == 1U);
}

static void _test_timeout_after_execution(void)
{
    host_sync_call_t call =
    {
        .request = _request(),
        .timeout_ms = 5U,
    };
    pthread_t thread;
    s_lifecycle_mode = HOST_LIFECYCLE_COMPLETE_DEFERRED;
    assert(pthread_create(&thread, NULL, _sync_navigation_thread, &call) == 0);
    for (unsigned attempt = 0; attempt < 1000U && _post_count() == 0U;
            ++attempt)
    {
        usleep(1000);
    }
    assert(_run_one_post());
    assert(atomic_load(&s_screen_access_count) == 1U);
    usleep(20000);
    assert(!atomic_load(&call.complete));
    _complete_deferred(ESP_OK);
    assert(pthread_join(thread, NULL) == 0);
    assert(call.result == ESP_ERR_TIMEOUT);
    assert(atomic_load(&s_screen_access_count) == 0U);
}

static void _test_screen_pause_drains_admitted_navigation(void)
{
    app_manager_nav_request_t request = _request();
    s_lifecycle_mode = HOST_LIFECYCLE_COMPLETE_DEFERRED;
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    assert(atomic_load(&s_screen_access_count) == 1U);
    assert(_run_one_post());
    assert(s_deferred_completion != NULL);

    atomic_store(&s_screen_accepting, false);
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) ==
           ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_screen_access_count) == 1U);

    _complete_deferred(ESP_OK);
    assert(atomic_load(&s_completion_count) == 1U);
    assert(atomic_load(&s_screen_access_count) == 0U);
    atomic_store(&s_screen_accepting, true);
}

static void _test_deinit_finishes_pending(void)
{
    app_manager_nav_request_t request = _request();
    s_lifecycle_mode = HOST_LIFECYCLE_COMPLETE_DEFERRED;
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) == ESP_OK);
    assert(_run_one_post());
    assert(s_presentation_running);
    assert(atomic_load(&s_screen_access_count) == 1U);
    assert(app_manager_navigation_deinit() == ESP_OK);
    assert(s_presentation_finish_count == 1U);
    assert(atomic_load(&s_completion_count) == 1U);
    assert(atomic_load(&s_screen_access_count) == 0U);
    assert(app_manager_navigate_async(&request, _completion,
                                      &s_completion_count) ==
           ESP_ERR_INVALID_STATE);
}

int main(void)
{
    assert(app_manager_navigation_init() == ESP_OK);
    _reset_observation();
    _test_ui_worker_rejected();
    _reset_observation();
    _test_gesture_token_and_conditional_back();
    _reset_observation();
    _test_stale_gesture_does_not_back_new_foreground();
    _reset_observation();
    _test_gesture_post_failure_preserves_epoch();
    _reset_observation();
    _test_gesture_requires_back_target_and_ui_worker();
    _reset_observation();
    _test_concurrent_submission_preserves_fifo();
    _reset_observation();
    _test_deep_copy_and_completion();
    _reset_observation();
    _test_rejection_completes_once();
    _reset_observation();
    _test_completion_reentry_uses_fifo();
    _reset_observation();
    _test_exit_self_copies_callback_owner();
    _reset_observation();
    _test_exit_self_rejects_non_ui_owner_lookup();
    _reset_observation();
    _test_exit_self_rejects_explicit_owner();
    _reset_observation();
    _test_post_failure_releases_screen_access();
    _reset_observation();
    _test_fixed_pool_limit();
    _reset_observation();
    _test_timeout_before_execution();
    _reset_observation();
    _test_timeout_after_execution();
    _reset_observation();
    _test_screen_pause_drains_admitted_navigation();
    _reset_observation();
    _test_deinit_finishes_pending();
    assert(app_manager_navigation_init() == ESP_OK);
    assert(app_manager_navigation_deinit() == ESP_OK);
    puts("navigation host tests passed");
    return 0;
}
