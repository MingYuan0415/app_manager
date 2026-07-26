#include "host_display_diagnostics_port.h"

#include "freertos/idf_additions.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef struct host_display_diagnostics_task
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t notification_ready;
    uint32_t notifications;
    bool deleted;
    void (*entry)(void *);
    void *arg;
} host_display_diagnostics_task_t;

static _Thread_local host_display_diagnostics_task_t *s_current_task;
static atomic_bool s_fail_next_create;
static atomic_uint s_create_count;
static atomic_uint s_delete_count;
static atomic_uint s_stack_depth;
static atomic_uint s_stack_caps;
static atomic_uint s_priority;
static void (*s_delete_hook)(void);
static void (*s_timeout_hook)(TickType_t timeout_ticks);

static struct timespec _host_display_diagnostics_deadline(TickType_t ticks)
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

static void *_host_display_diagnostics_task_run(void *arg)
{
    host_display_diagnostics_task_t *task = arg;
    s_current_task = task;
    task->entry(task->arg);
    s_current_task = NULL;
    return NULL;
}

void host_display_diagnostics_port_reset(void)
{
    atomic_store(&s_fail_next_create, false);
    atomic_store(&s_create_count, 0U);
    atomic_store(&s_delete_count, 0U);
    atomic_store(&s_stack_depth, 0U);
    atomic_store(&s_stack_caps, 0U);
    atomic_store(&s_priority, 0U);
    s_delete_hook = NULL;
    s_timeout_hook = NULL;
}

void host_display_diagnostics_port_fail_next_create(void)
{
    atomic_store(&s_fail_next_create, true);
}

size_t host_display_diagnostics_port_create_count(void)
{
    return atomic_load(&s_create_count);
}

size_t host_display_diagnostics_port_delete_count(void)
{
    return atomic_load(&s_delete_count);
}

uint32_t host_display_diagnostics_port_stack_depth(void)
{
    return atomic_load(&s_stack_depth);
}

UBaseType_t host_display_diagnostics_port_stack_caps(void)
{
    return atomic_load(&s_stack_caps);
}

UBaseType_t host_display_diagnostics_port_priority(void)
{
    return atomic_load(&s_priority);
}

void host_display_diagnostics_port_set_delete_hook(void (*hook)(void))
{
    s_delete_hook = hook;
}

void host_display_diagnostics_port_set_timeout_hook(
    void (*hook)(TickType_t timeout_ticks))
{
    s_timeout_hook = hook;
}

BaseType_t xTaskCreateWithCaps(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *arg,
                               UBaseType_t priority, TaskHandle_t *handle,
                               UBaseType_t memory_caps)
{
    (void)name;
    if (entry == NULL || handle == NULL ||
            atomic_exchange(&s_fail_next_create, false))
    {
        return pdFAIL;
    }

    host_display_diagnostics_task_t *task = calloc(1, sizeof(*task));
    if (task == NULL)
    {
        return pdFAIL;
    }
    task->entry = entry;
    task->arg = arg;
    (void)pthread_mutex_init(&task->lock, NULL);
    (void)pthread_cond_init(&task->notification_ready, NULL);
    if (pthread_create(&task->thread, NULL,
                       _host_display_diagnostics_task_run, task) != 0)
    {
        (void)pthread_cond_destroy(&task->notification_ready);
        (void)pthread_mutex_destroy(&task->lock);
        free(task);
        return pdFAIL;
    }

    atomic_fetch_add(&s_create_count, 1U);
    atomic_store(&s_stack_depth, stack_depth);
    atomic_store(&s_stack_caps, memory_caps);
    atomic_store(&s_priority, priority);
    *handle = task;
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t handle)
{
    host_display_diagnostics_task_t *task = handle;
    if (task == NULL)
    {
        return pdFAIL;
    }

    (void)pthread_mutex_lock(&task->lock);
    if (task->deleted)
    {
        (void)pthread_mutex_unlock(&task->lock);
        return pdFAIL;
    }
    task->notifications++;
    (void)pthread_cond_signal(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks)
{
    host_display_diagnostics_task_t *task = s_current_task;
    if (task == NULL)
    {
        return 0U;
    }

    (void)pthread_mutex_lock(&task->lock);
    if (task->notifications == 0U && !task->deleted && timeout_ticks != 0U)
    {
        if (timeout_ticks == portMAX_DELAY)
        {
            while (task->notifications == 0U && !task->deleted)
            {
                (void)pthread_cond_wait(&task->notification_ready,
                                        &task->lock);
            }
        }
        else
        {
            const struct timespec deadline =
                _host_display_diagnostics_deadline(timeout_ticks);
            while (task->notifications == 0U && !task->deleted &&
                    pthread_cond_timedwait(&task->notification_ready,
                                           &task->lock, &deadline) == 0)
            {
            }
        }
    }

    const uint32_t notifications = task->notifications;
    const bool timed_out = notifications == 0U && !task->deleted &&
                           timeout_ticks != 0U &&
                           timeout_ticks != portMAX_DELAY;
    if (clear_on_exit)
    {
        task->notifications = 0U;
    }
    else if (task->notifications > 0U)
    {
        task->notifications--;
    }
    (void)pthread_mutex_unlock(&task->lock);
    if (timed_out && s_timeout_hook != NULL)
    {
        s_timeout_hook(timeout_ticks);
    }
    return notifications;
}

void vTaskSuspend(TaskHandle_t handle)
{
    host_display_diagnostics_task_t *task = handle == NULL ?
                                            s_current_task : handle;
    if (task == NULL)
    {
        return;
    }

    (void)pthread_mutex_lock(&task->lock);
    while (!task->deleted)
    {
        (void)pthread_cond_wait(&task->notification_ready, &task->lock);
    }
    (void)pthread_mutex_unlock(&task->lock);
}

void vTaskDeleteWithCaps(TaskHandle_t handle)
{
    host_display_diagnostics_task_t *task = handle;
    if (task == NULL)
    {
        return;
    }

    (void)pthread_mutex_lock(&task->lock);
    task->deleted = true;
    (void)pthread_cond_broadcast(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    (void)pthread_join(task->thread, NULL);
    (void)pthread_cond_destroy(&task->notification_ready);
    (void)pthread_mutex_destroy(&task->lock);
    free(task);
    if (s_delete_hook != NULL)
    {
        s_delete_hook();
    }
    atomic_fetch_add(&s_delete_count, 1U);
}
