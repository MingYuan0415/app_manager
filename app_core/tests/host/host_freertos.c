#include "host_freertos.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct host_queue
{
    void **buffer;
    int capacity;
    int head;
    int tail;
    int count;
    size_t item_size;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} host_queue_t;

typedef struct host_semaphore
{
    bool given;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} host_semaphore_t;

typedef struct host_timer
{
    void (*callback)(void *);
    void *arg;
    atomic_bool running;
    atomic_bool paused;
    atomic_uint pending_ticks;
    pthread_t thread;
    uint64_t period_us;
} host_timer_t;

atomic_uint g_host_semaphore_timeout_count = ATOMIC_VAR_INIT(0U);

static struct timespec _host_deadline_after_ticks(TickType_t ticks)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(ticks / 1000U);
    deadline.tv_nsec += (long)(ticks % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static bool _host_queue_wait_for_space(host_queue_t *queue,
                                       TickType_t ticks)
{
    bool ready = queue->count < queue->capacity;
    if (!ready && ticks == portMAX_DELAY)
    {
        while (queue->count >= queue->capacity)
        {
            (void)pthread_cond_wait(&queue->not_full, &queue->mutex);
        }
        ready = true;
    }
    else if (!ready && ticks != 0U)
    {
        const struct timespec deadline = _host_deadline_after_ticks(ticks);
        int wait_result = 0;
        while (queue->count >= queue->capacity && wait_result == 0)
        {
            wait_result = pthread_cond_timedwait(&queue->not_full,
                                                 &queue->mutex, &deadline);
        }
        ready = queue->count < queue->capacity;
    }
    return ready;
}

static bool _host_queue_wait_for_item(host_queue_t *queue,
                                      TickType_t ticks)
{
    bool ready = queue->count > 0;
    if (!ready && ticks == portMAX_DELAY)
    {
        while (queue->count <= 0)
        {
            (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
        }
        ready = true;
    }
    else if (!ready && ticks != 0U)
    {
        const struct timespec deadline = _host_deadline_after_ticks(ticks);
        int wait_result = 0;
        while (queue->count <= 0 && wait_result == 0)
        {
            wait_result = pthread_cond_timedwait(&queue->not_empty,
                                                 &queue->mutex, &deadline);
        }
        ready = queue->count > 0;
    }
    return ready;
}

static SemaphoreHandle_t _host_semaphore_create(bool given)
{
    host_semaphore_t *semaphore = calloc(1, sizeof(*semaphore));
    if (semaphore != NULL)
    {
        semaphore->given = given;
        (void)pthread_mutex_init(&semaphore->mutex, NULL);
        (void)pthread_cond_init(&semaphore->condition, NULL);
    }
    return semaphore;
}

static bool _host_semaphore_wait(host_semaphore_t *semaphore,
                                 TickType_t ticks)
{
    bool ready = semaphore->given;
    if (!ready && ticks == portMAX_DELAY)
    {
        while (!semaphore->given)
        {
            (void)pthread_cond_wait(&semaphore->condition,
                                    &semaphore->mutex);
        }
        ready = true;
    }
    else if (!ready && ticks != 0U)
    {
        const struct timespec deadline = _host_deadline_after_ticks(ticks);
        int wait_result = 0;
        while (!semaphore->given && wait_result == 0)
        {
            wait_result = pthread_cond_timedwait(&semaphore->condition,
                                                 &semaphore->mutex,
                                                 &deadline);
        }
        ready = semaphore->given;
        if (!ready && wait_result == ETIMEDOUT)
        {
            atomic_fetch_add(&g_host_semaphore_timeout_count, 1U);
        }
    }
    return ready;
}

static bool _host_timer_take_pending_tick(host_timer_t *timer)
{
    unsigned pending = atomic_load(&timer->pending_ticks);
    bool admitted = false;
    while (pending > 0U && !admitted)
    {
        admitted = atomic_compare_exchange_weak(&timer->pending_ticks,
                                                &pending, pending - 1U);
    }
    return admitted;
}

static void *_host_timer_thread(void *argument)
{
    host_timer_t *timer = argument;
    while (atomic_load(&timer->running))
    {
        (void)usleep((useconds_t)timer->period_us);
        const bool should_fire = !atomic_load(&timer->paused) ||
                                 _host_timer_take_pending_tick(timer);
        if (atomic_load(&timer->running) && should_fire &&
                timer->callback != NULL)
        {
            timer->callback(timer->arg);
        }
    }
    return NULL;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)(uintptr_t)pthread_self();
}

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    host_queue_t *queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
    {
        return NULL;
    }
    queue->buffer = calloc(length, sizeof(queue->buffer[0]));
    if (queue->buffer == NULL)
    {
        free(queue);
        return NULL;
    }
    queue->capacity = (int)length;
    queue->item_size = item_size;
    (void)pthread_mutex_init(&queue->mutex, NULL);
    (void)pthread_cond_init(&queue->not_full, NULL);
    (void)pthread_cond_init(&queue->not_empty, NULL);
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item,
                      TickType_t ticks)
{
    host_queue_t *queue = handle;
    BaseType_t result = pdFALSE;
    bool lock_owned = false;
    if (queue == NULL || item == NULL)
    {
        goto exit;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    lock_owned = true;
    if (!_host_queue_wait_for_space(queue, ticks))
    {
        goto exit;
    }
    void *copy = malloc(queue->item_size);
    if (copy == NULL)
    {
        goto exit;
    }
    memcpy(copy, item, queue->item_size);
    queue->buffer[queue->head] = copy;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count++;
    (void)pthread_cond_signal(&queue->not_empty);
    result = pdTRUE;

exit:
    if (lock_owned)
    {
        (void)pthread_mutex_unlock(&queue->mutex);
    }
    return result;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item,
                         TickType_t ticks)
{
    host_queue_t *queue = handle;
    BaseType_t result = pdFALSE;
    bool lock_owned = false;
    if (queue == NULL || item == NULL)
    {
        goto exit;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    lock_owned = true;
    if (!_host_queue_wait_for_item(queue, ticks))
    {
        goto exit;
    }
    memcpy(item, queue->buffer[queue->tail], queue->item_size);
    free(queue->buffer[queue->tail]);
    queue->buffer[queue->tail] = NULL;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count--;
    (void)pthread_cond_signal(&queue->not_full);
    result = pdTRUE;

exit:
    if (lock_owned)
    {
        (void)pthread_mutex_unlock(&queue->mutex);
    }
    return result;
}

void vQueueDelete(QueueHandle_t handle)
{
    host_queue_t *queue = handle;
    if (queue == NULL)
    {
        return;
    }
    for (int index = 0; index < queue->capacity; index++)
    {
        free(queue->buffer[index]);
    }
    free(queue->buffer);
    (void)pthread_mutex_destroy(&queue->mutex);
    (void)pthread_cond_destroy(&queue->not_full);
    (void)pthread_cond_destroy(&queue->not_empty);
    free(queue);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return _host_semaphore_create(false);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return _host_semaphore_create(true);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    host_semaphore_t *semaphore = handle;
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->mutex);
    BaseType_t result = pdFALSE;
    if (_host_semaphore_wait(semaphore, ticks))
    {
        semaphore->given = false;
        result = pdTRUE;
    }
    (void)pthread_mutex_unlock(&semaphore->mutex);
    return result;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    host_semaphore_t *semaphore = handle;
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->mutex);
    semaphore->given = true;
    (void)pthread_cond_signal(&semaphore->condition);
    (void)pthread_mutex_unlock(&semaphore->mutex);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    host_semaphore_t *semaphore = handle;
    if (semaphore == NULL)
    {
        return;
    }
    (void)pthread_mutex_destroy(&semaphore->mutex);
    (void)pthread_cond_destroy(&semaphore->condition);
    free(semaphore);
}

unsigned host_semaphore_timeout_count(void)
{
    return atomic_load(&g_host_semaphore_timeout_count);
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *output)
{
    if (args == NULL || args->callback == NULL || output == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    host_timer_t *timer = calloc(1, sizeof(*timer));
    if (timer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    timer->callback = args->callback;
    timer->arg = args->arg;
    *output = timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle,
                                   uint64_t period_us)
{
    host_timer_t *timer = handle;
    if (timer == NULL || period_us == 0U)
    {
        return ESP_FAIL;
    }
    timer->period_us = period_us;
    atomic_store(&timer->running, true);
    if (pthread_create(&timer->thread, NULL, _host_timer_thread, timer) != 0)
    {
        atomic_store(&timer->running, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
    host_timer_t *timer = handle;
    if (timer == NULL)
    {
        return ESP_FAIL;
    }
    if (atomic_load(&timer->running))
    {
        atomic_store(&timer->running, false);
        (void)pthread_join(timer->thread, NULL);
    }
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t handle)
{
    host_timer_t *timer = handle;
    esp_err_t result = esp_timer_stop(handle);
    if (result == ESP_OK)
    {
        free(timer);
    }
    return result;
}

void host_timer_set_paused(esp_timer_handle_t handle, bool paused)
{
    host_timer_t *timer = handle;
    if (timer != NULL)
    {
        atomic_store(&timer->paused, paused);
    }
}

void host_timer_step(esp_timer_handle_t handle)
{
    host_timer_t *timer = handle;
    if (timer != NULL)
    {
        atomic_fetch_add(&timer->pending_ticks, 1U);
    }
}
