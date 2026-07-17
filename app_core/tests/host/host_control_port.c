#include "host_control_port.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct host_task_start
{
    void (*entry)(void *);
    void *arg;
} host_task_start_t;

static void *_host_task_trampoline(void *opaque)
{
    host_task_start_t start = *(host_task_start_t *)opaque;
    free(opaque);
    start.entry(start.arg);
    return NULL;
}

QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                 uint8_t *buffer, StaticQueue_t *storage)
{
    (void)buffer;
    (void)storage;
    return xQueueCreate(length, item_size);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    (void)storage;
    return xSemaphoreCreateMutex();
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    (void)storage;
    return xSemaphoreCreateBinary();
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *task_woken)
{
    const BaseType_t result = xQueueSend(queue, item, 0U);
    if (task_woken != NULL)
    {
        *task_woken = result == pdTRUE ? pdTRUE : pdFALSE;
    }
    return result;
}

BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_size, void *arg, uint32_t priority,
                       TaskHandle_t *task)
{
    (void)name;
    (void)stack_size;
    (void)priority;
    BaseType_t result = pdFAIL;
    host_task_start_t *start = NULL;
    if (entry == NULL || task == NULL)
    {
        goto exit;
    }
    start = malloc(sizeof(*start));
    if (start == NULL)
    {
        goto exit;
    }
    start->entry = entry;
    start->arg = arg;
    pthread_t thread;
    if (pthread_create(&thread, NULL, _host_task_trampoline, start) != 0)
    {
        free(start);
        goto exit;
    }
    (void)pthread_detach(thread);
    *task = (TaskHandle_t)(uintptr_t)thread;
    result = pdPASS;

exit:
    return result;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL)
    {
        pthread_exit(NULL);
    }
    (void)pthread_cancel((pthread_t)(uintptr_t)task);
}

int64_t esp_timer_get_time(void)
{
    return atomic_load(&g_host_time_us);
}
