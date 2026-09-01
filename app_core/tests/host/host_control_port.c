#include "host_control_port.h"

#include <assert.h>
#include <stdbool.h>
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

BaseType_t xTaskCreatePinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_size, void *arg,
    uint32_t priority, TaskHandle_t *task, BaseType_t core_id)
{
    (void)name;
    (void)stack_size;
    (void)priority;
    assert(core_id == CONFIG_MAIN_PROJECT_TASK_CORE_ID);
    if (entry == NULL || task == NULL)
    {
        return pdFAIL;
    }
    host_task_start_t *start = malloc(sizeof(*start));
    if (start == NULL)
    {
        return pdFAIL;
    }
    start->entry = entry;
    start->arg = arg;
    pthread_t thread;
    if (pthread_create(&thread, NULL, _host_task_trampoline, start) != 0)
    {
        free(start);
        return pdFAIL;
    }
    (void)pthread_detach(thread);
    *task = (TaskHandle_t)(uintptr_t)thread;
    return pdPASS;
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

bool app_manager_lifecycle_active_is_home_snapshot(bool *valid)
{
    if (valid != NULL)
    {
        *valid = true;
    }
    return false;
}
