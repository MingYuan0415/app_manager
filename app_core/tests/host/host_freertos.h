#ifndef __HOST_FREERTOS_H__
#define __HOST_FREERTOS_H__

#include "esp_err.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

/** @brief Opaque host task handle. */
typedef void *TaskHandle_t;
/** @brief Opaque host queue handle. */
typedef void *QueueHandle_t;
/** @brief Opaque host semaphore handle. */
typedef void *SemaphoreHandle_t;
/** @brief Opaque host timer handle. */
typedef void *TimerHandle_t;
/** @brief Opaque host representation of an ESP timer. */
typedef void *esp_timer_handle_t;
/** @brief Host representation of a FreeRTOS tick count. */
typedef uint32_t TickType_t;
/** @brief Host representation of a FreeRTOS base result. */
typedef int BaseType_t;

#define portMAX_DELAY      ((TickType_t)UINT32_MAX)
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))

/** @brief Arguments used to create one host timer. */
typedef struct esp_timer_create_args
{
    void (*callback)(void *); /**< Timer callback. */
    void *arg;                /**< Callback context. */
    const char *name;         /**< Diagnostic timer name. */
} esp_timer_create_args_t;

/** @brief Shared count updated whenever a host semaphore wait times out. */
extern atomic_uint g_host_semaphore_timeout_count;

/** @brief Return the current host thread as a task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);

/** @brief Create a pthread-backed fixed-capacity queue. */
QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);

/** @brief Send one fixed-size item to a host queue. */
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks);

/** @brief Receive one fixed-size item from a host queue. */
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks);

/** @brief Delete a host queue and all queued copies. */
void vQueueDelete(QueueHandle_t queue);

/** @brief Create an initially empty binary semaphore. */
SemaphoreHandle_t xSemaphoreCreateBinary(void);

/** @brief Create an initially available mutex semaphore. */
SemaphoreHandle_t xSemaphoreCreateMutex(void);

/** @brief Take a host semaphore with FreeRTOS-style timeout semantics. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);

/** @brief Give a host semaphore and wake one waiter. */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

/** @brief Delete a host semaphore. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

/** @brief Reset host semaphore fault injection and allocation counters. */
void host_semaphore_test_reset(void);

/**
 * @brief Fail one host semaphore allocation by its 1-based create call.
 *
 * @param create_call is the call to fail, or zero to disable injection.
 */
void host_semaphore_fail_create_on(unsigned create_call);

/** @brief Return host semaphore create attempts since the last test reset. */
unsigned host_semaphore_create_count(void);

/** @brief Return non-NULL host semaphore deletions since the last test reset. */
unsigned host_semaphore_delete_count(void);

/** @brief Return NULL host semaphore deletion attempts since the last reset. */
unsigned host_semaphore_null_delete_count(void);

/**
 * @brief Return the number of host semaphore waits that reached their deadline.
 *
 * @return Monotonic timeout count for deterministic test synchronization.
 */
unsigned host_semaphore_timeout_count(void);

/** @brief Create a host ESP timer from a copied configuration. */
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *timer);

/** @brief Start a periodic host ESP timer thread. */
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,
                                   uint64_t period_us);

/** @brief Stop and join a running host ESP timer thread. */
esp_err_t esp_timer_stop(esp_timer_handle_t timer);

/** @brief Stop and delete a host ESP timer. */
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

/** @brief Pause or resume automatic host timer callbacks. */
void host_timer_set_paused(esp_timer_handle_t timer, bool paused);

/** @brief Admit one callback while a host timer is paused. */
void host_timer_step(esp_timer_handle_t timer);

#endif /* __HOST_FREERTOS_H__ */
