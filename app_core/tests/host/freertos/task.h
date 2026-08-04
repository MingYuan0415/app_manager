/** @file Task notification declarations for display diagnostics tests. */
#ifndef __HOST_DISPLAY_DIAGNOSTICS_TASK_H__
#define __HOST_DISPLAY_DIAGNOSTICS_TASK_H__

#include "freertos/FreeRTOS.h"

BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks);
TaskHandle_t xTaskGetHandle(const char *name);
BaseType_t xTaskGetCoreID(TaskHandle_t task);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                                   uint32_t stack_depth, void *arg,
                                   UBaseType_t priority, TaskHandle_t *task,
                                   BaseType_t core_id);
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t task);

#endif /* __HOST_DISPLAY_DIAGNOSTICS_TASK_H__ */
