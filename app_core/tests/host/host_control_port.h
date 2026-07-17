#ifndef __HOST_CONTROL_PORT_H__
#define __HOST_CONTROL_PORT_H__

#include "host_freertos.h"

#include <stdatomic.h>

#define IRAM_ATTR

/** @brief Placeholder storage for a statically created host queue. */
typedef struct host_static_queue
{
    int unused; /**< Keeps the placeholder type complete. */
} StaticQueue_t;

/** @brief Placeholder storage for a statically created host semaphore. */
typedef struct host_static_semaphore
{
    int unused; /**< Keeps the placeholder type complete. */
} StaticSemaphore_t;

/** @brief Create a host queue while accepting FreeRTOS static storage. */
QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                 uint8_t *buffer, StaticQueue_t *storage);

/** @brief Create a host mutex while accepting FreeRTOS static storage. */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);

/** @brief Create a host binary semaphore using static storage. */
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);

/** @brief Send one item through the ISR-compatible host queue facade. */
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *task_woken);

/** @brief Start one pthread-backed FreeRTOS task facade. */
BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_size, void *arg, uint32_t priority,
                       TaskHandle_t *task);

/** @brief Delete another host task or exit the current task. */
void vTaskDelete(TaskHandle_t task);

/** @brief Host monotonic time controlled by the PM test. */
extern atomic_llong g_host_time_us;

/** @brief Return the PM test's controlled microsecond clock. */
int64_t esp_timer_get_time(void);

/** @brief Pause the fake LVGL adapter. */
esp_err_t esp_lv_adapter_pause(int32_t timeout_ms);

/** @brief Resume the fake LVGL adapter. */
esp_err_t esp_lv_adapter_resume(void);

/** @brief Store an 8-bit value in the fake NVS port. */
esp_err_t nv_storage_set_u8(const char *key, uint8_t value);

/** @brief Read an 8-bit value from the fake NVS port. */
esp_err_t nv_storage_get_u8(const char *key, uint8_t *value);

/** @brief Store a 32-bit value in the fake NVS port. */
esp_err_t nv_storage_set_u32(const char *key, uint32_t value);

/** @brief Read a 32-bit value from the fake NVS port. */
esp_err_t nv_storage_get_u32(const char *key, uint32_t *value);

#endif /* __HOST_CONTROL_PORT_H__ */
