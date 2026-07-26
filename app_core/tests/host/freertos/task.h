/** @file Task notification declarations for display diagnostics tests. */
#ifndef __HOST_DISPLAY_DIAGNOSTICS_TASK_H__
#define __HOST_DISPLAY_DIAGNOSTICS_TASK_H__

#include "freertos/FreeRTOS.h"

BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks);
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t task);

#endif /* __HOST_DISPLAY_DIAGNOSTICS_TASK_H__ */
