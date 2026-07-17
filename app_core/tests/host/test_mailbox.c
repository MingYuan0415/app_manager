#include "host_freertos.h"
#include "app_manager.h"
#include "../../src/app_manager_mailbox.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static atomic_int s_callback_count;
static pthread_mutex_t s_block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_block_cond = PTHREAD_COND_INITIALIZER;
static bool s_block_started;
static bool s_block_release;
static atomic_bool s_slow_started;
static atomic_bool s_slow_release;
static atomic_bool s_race_producers_running;

static int64_t _monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void _wait_for_count(int expected)
{
    for (int i = 0; i < 500 && atomic_load(&s_callback_count) < expected; i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_callback_count) == expected);
}

static void _count_callback(void *arg)
{
    (void)arg;
    atomic_fetch_add(&s_callback_count, 1);
}

static void _identity_callback(void *arg)
{
    pthread_t *thread = arg;
    *thread = pthread_self();
    atomic_fetch_add(&s_callback_count, 1);
}

static void _blocking_callback(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_block_lock);
    s_block_started = true;
    pthread_cond_broadcast(&s_block_cond);
    while (!s_block_release)
    {
        pthread_cond_wait(&s_block_cond, &s_block_lock);
    }
    pthread_mutex_unlock(&s_block_lock);
}

static void _start_blocking_worker(void)
{
    pthread_mutex_lock(&s_block_lock);
    s_block_started = false;
    s_block_release = false;
    pthread_mutex_unlock(&s_block_lock);
    assert(app_manager_ui_post(_blocking_callback, NULL) == ESP_OK);
    pthread_mutex_lock(&s_block_lock);
    while (!s_block_started)
    {
        pthread_cond_wait(&s_block_cond, &s_block_lock);
    }
    pthread_mutex_unlock(&s_block_lock);
}

static void _release_blocking_worker(void)
{
    pthread_mutex_lock(&s_block_lock);
    s_block_release = true;
    pthread_cond_broadcast(&s_block_cond);
    pthread_mutex_unlock(&s_block_lock);
}

static esp_err_t _result_callback(void *arg)
{
    atomic_fetch_add(&s_callback_count, 1);
    return (esp_err_t)(intptr_t)arg;
}

static esp_err_t _cancelled_callback(void *arg)
{
    (void)arg;
    atomic_fetch_add(&s_callback_count, 1);
    return ESP_OK;
}

typedef struct slow_call_result
{
    esp_err_t result;
    int64_t elapsed_ms;
} slow_call_result_t;

typedef struct deinit_result
{
    esp_err_t result;
    atomic_bool finished;
} deinit_result_t;

typedef struct blocking_call_result
{
    esp_err_t result;
} blocking_call_result_t;

static esp_err_t _slow_callback(void *arg)
{
    atomic_bool *started = arg;
    atomic_store(started, true);
    while (!atomic_load(&s_slow_release))
    {
        usleep(1000);
    }
    atomic_fetch_add(&s_callback_count, 1);
    return ESP_OK;
}

static void *_slow_caller(void *arg)
{
    slow_call_result_t *call = arg;
    int64_t begin = _monotonic_ms();
    call->result = app_manager_ui_call(_slow_callback, &s_slow_started, 30);
    call->elapsed_ms = _monotonic_ms() - begin;
    return NULL;
}

static void *_producer(void *arg)
{
    int count = (int)(intptr_t)arg;
    for (int i = 0; i < count; i++)
    {
        while (app_manager_ui_post(_count_callback, NULL) == ESP_ERR_NO_MEM)
        {
            usleep(1000);
        }
    }
    return NULL;
}

static void _worker_deinit_callback(void *arg)
{
    esp_err_t *result = arg;
    *result = app_manager_mailbox_deinit();
    atomic_fetch_add(&s_callback_count, 1);
}

static esp_err_t _blocking_result_callback(void *arg)
{
    _blocking_callback(NULL);
    return (esp_err_t)(intptr_t)arg;
}

static void *_blocking_caller(void *arg)
{
    blocking_call_result_t *call = arg;
    call->result = app_manager_ui_call(_blocking_result_callback,
                                       (void *)(intptr_t)ESP_ERR_NOT_FOUND,
                                       UINT32_MAX);
    return NULL;
}

static void *_deinit_caller(void *arg)
{
    deinit_result_t *deinit = arg;
    deinit->result = app_manager_mailbox_deinit();
    atomic_store(&deinit->finished, true);
    return NULL;
}

static void *_shutdown_race_producer(void *arg)
{
    (void)arg;
    while (atomic_load(&s_race_producers_running))
    {
        esp_err_t result = app_manager_ui_post(_count_callback, NULL);
        if (result == ESP_ERR_INVALID_STATE)
        {
            break;
        }
        assert(result == ESP_OK || result == ESP_ERR_NO_MEM);
        if (result == ESP_ERR_NO_MEM)
        {
            usleep(100);
        }
    }
    return NULL;
}

int main(void)
{
    app_manager_ui_dispatch_fn dispatcher = (app_manager_ui_dispatch_fn)(uintptr_t)1;
    assert(app_manager_get_ui_dispatch_fn(&dispatcher) == ESP_ERR_INVALID_STATE);
    assert(dispatcher == NULL);
    assert(app_manager_ui_post(_count_callback, NULL) == ESP_ERR_INVALID_STATE);
    assert(app_manager_mailbox_init() == ESP_OK);

    atomic_store(&s_callback_count, 0);
    pthread_t first_worker = 0;
    pthread_t second_worker = 0;
    assert(app_manager_ui_post(_identity_callback, &first_worker) == ESP_OK);
    assert(app_manager_ui_post(_identity_callback, &second_worker) == ESP_OK);
    _wait_for_count(2);
    assert(pthread_equal(first_worker, second_worker));
    assert(!pthread_equal(first_worker, pthread_self()));

    assert(app_manager_ui_call(_result_callback, (void *)(intptr_t)ESP_ERR_NOT_FOUND,
                               100) == ESP_ERR_NOT_FOUND);

    app_manager_mailbox_host_timer_pause(true);
    usleep(15000);
    atomic_store(&s_callback_count, 0);
    for (int i = 0; i < 20; i++)
    {
        assert(app_manager_ui_post(_count_callback, NULL) == ESP_OK);
    }
    usleep(15000);
    assert(atomic_load(&s_callback_count) == 0);
    app_manager_mailbox_host_timer_step();
    _wait_for_count(8);
    app_manager_mailbox_host_timer_step();
    _wait_for_count(16);
    app_manager_mailbox_host_timer_step();
    _wait_for_count(20);
    app_manager_mailbox_host_timer_pause(false);

    atomic_store(&s_callback_count, 0);
    _start_blocking_worker();
    for (int i = 0; i < 23; i++)
    {
        assert(app_manager_ui_post(_count_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_count_callback, NULL) == ESP_ERR_NO_MEM);
    _release_blocking_worker();
    _wait_for_count(23);

    atomic_store(&s_callback_count, 0);
    _start_blocking_worker();
    int64_t timeout_begin = _monotonic_ms();
    assert(app_manager_ui_call(_cancelled_callback, NULL, 10) == ESP_ERR_TIMEOUT);
    assert(_monotonic_ms() - timeout_begin < 80);
    _release_blocking_worker();
    usleep(30000);
    assert(atomic_load(&s_callback_count) == 0);

    atomic_store(&s_callback_count, 0);
    slow_call_result_t slow_result = {0};
    pthread_t caller;
    atomic_store(&s_slow_started, false);
    atomic_store(&s_slow_release, false);
    const unsigned timeout_count = host_semaphore_timeout_count();
    assert(pthread_create(&caller, NULL, _slow_caller, &slow_result) == 0);
    for (int i = 0; i < 100 && !atomic_load(&s_slow_started); i++)
    {
        usleep(1000);
    }
    assert(atomic_load(&s_slow_started));
    for (int i = 0;
            i < 500 && host_semaphore_timeout_count() == timeout_count; i++)
    {
        usleep(1000);
    }
    assert(host_semaphore_timeout_count() > timeout_count);
    atomic_store(&s_slow_release, true);
    assert(pthread_join(caller, NULL) == 0);
    assert(slow_result.result == ESP_ERR_TIMEOUT);
    assert(slow_result.elapsed_ms >= 20);
    assert(atomic_load(&s_callback_count) == 1);

    assert(app_manager_get_ui_dispatch_fn(&dispatcher) == ESP_OK);
    atomic_store(&s_callback_count, 0);
    assert(dispatcher(_count_callback, NULL) == ESP_OK);
    _wait_for_count(1);

    atomic_store(&s_callback_count, 0);
    pthread_t producers[4];
    for (size_t i = 0; i < 4; i++)
    {
        assert(pthread_create(&producers[i], NULL, _producer,
                              (void *)(intptr_t)100) == 0);
    }
    for (size_t i = 0; i < 4; i++)
    {
        assert(pthread_join(producers[i], NULL) == 0);
    }
    _wait_for_count(400);

    atomic_store(&s_callback_count, 0);
    esp_err_t worker_deinit_result = ESP_OK;
    assert(app_manager_ui_post(_worker_deinit_callback,
                               &worker_deinit_result) == ESP_OK);
    _wait_for_count(1);
    assert(worker_deinit_result == ESP_ERR_INVALID_STATE);

    _start_blocking_worker();
    deinit_result_t async_deinit = {.result = ESP_FAIL};
    atomic_init(&async_deinit.finished, false);
    pthread_t async_deinit_thread;
    assert(pthread_create(&async_deinit_thread, NULL, _deinit_caller,
                          &async_deinit) == 0);
    usleep(30000);
    assert(!atomic_load(&async_deinit.finished));
    _release_blocking_worker();
    assert(pthread_join(async_deinit_thread, NULL) == 0);
    assert(async_deinit.result == ESP_OK);
    assert(app_manager_ui_post(_count_callback, NULL) == ESP_ERR_INVALID_STATE);

    assert(app_manager_mailbox_init() == ESP_OK);

    pthread_mutex_lock(&s_block_lock);
    s_block_started = false;
    s_block_release = false;
    pthread_mutex_unlock(&s_block_lock);
    blocking_call_result_t blocking_call = {.result = ESP_FAIL};
    pthread_t blocking_call_thread;
    assert(pthread_create(&blocking_call_thread, NULL, _blocking_caller,
                          &blocking_call) == 0);
    pthread_mutex_lock(&s_block_lock);
    while (!s_block_started)
    {
        pthread_cond_wait(&s_block_cond, &s_block_lock);
    }
    pthread_mutex_unlock(&s_block_lock);

    deinit_result_t sync_deinit = {.result = ESP_FAIL};
    atomic_init(&sync_deinit.finished, false);
    pthread_t sync_deinit_thread;
    assert(pthread_create(&sync_deinit_thread, NULL, _deinit_caller,
                          &sync_deinit) == 0);
    usleep(30000);
    assert(!atomic_load(&sync_deinit.finished));
    _release_blocking_worker();
    assert(pthread_join(blocking_call_thread, NULL) == 0);
    assert(blocking_call.result == ESP_ERR_NOT_FOUND);
    assert(pthread_join(sync_deinit_thread, NULL) == 0);
    assert(sync_deinit.result == ESP_OK);

    assert(app_manager_mailbox_init() == ESP_OK);
    enum { SHUTDOWN_PRODUCERS = 8 };
    pthread_t shutdown_producers[SHUTDOWN_PRODUCERS];
    atomic_store(&s_race_producers_running, true);
    for (size_t i = 0; i < SHUTDOWN_PRODUCERS; i++)
    {
        assert(pthread_create(&shutdown_producers[i], NULL,
                              _shutdown_race_producer, NULL) == 0);
    }
    usleep(30000);
    assert(app_manager_mailbox_deinit() == ESP_OK);
    atomic_store(&s_race_producers_running, false);
    for (size_t i = 0; i < SHUTDOWN_PRODUCERS; i++)
    {
        assert(pthread_join(shutdown_producers[i], NULL) == 0);
    }

    assert(app_manager_mailbox_init() == ESP_OK);
    _start_blocking_worker();
    deinit_result_t first_deinit = {.result = ESP_FAIL};
    deinit_result_t second_deinit = {.result = ESP_FAIL};
    atomic_init(&first_deinit.finished, false);
    atomic_init(&second_deinit.finished, false);
    pthread_t first_deinit_thread;
    pthread_t second_deinit_thread;
    assert(pthread_create(&first_deinit_thread, NULL, _deinit_caller,
                          &first_deinit) == 0);
    usleep(10000);
    assert(pthread_create(&second_deinit_thread, NULL, _deinit_caller,
                          &second_deinit) == 0);
    assert(pthread_join(second_deinit_thread, NULL) == 0);
    assert(second_deinit.result == ESP_ERR_INVALID_STATE);
    _release_blocking_worker();
    assert(pthread_join(first_deinit_thread, NULL) == 0);
    assert(first_deinit.result == ESP_OK);

    assert(app_manager_mailbox_init() == ESP_OK);
    atomic_store(&s_callback_count, 0);
    assert(app_manager_mailbox_shutdown_call(_result_callback,
            (void *)(intptr_t)ESP_ERR_NOT_FOUND) == ESP_ERR_NOT_FOUND);
    assert(atomic_load(&s_callback_count) == 1);
    assert(app_manager_mailbox_shutdown_call(_result_callback,
            (void *)(intptr_t)ESP_OK) == ESP_OK);
    assert(atomic_load(&s_callback_count) == 2);
    assert(app_manager_ui_post(_count_callback, NULL) == ESP_ERR_INVALID_STATE);
    assert(app_manager_mailbox_deinit() == ESP_OK);

    puts("mailbox production-core tests passed");
    return 0;
}
