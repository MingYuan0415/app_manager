/** @file ESP-IDF task-capability declarations for display diagnostics tests. */
#ifndef __HOST_DISPLAY_DIAGNOSTICS_IDF_ADDITIONS_H__
#define __HOST_DISPLAY_DIAGNOSTICS_IDF_ADDITIONS_H__

#include "freertos/task.h"

BaseType_t xTaskCreateWithCaps(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *arg,
                               UBaseType_t priority, TaskHandle_t *task,
                               UBaseType_t memory_caps);
StackType_t *xTaskGetStackStart(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);

#endif /* __HOST_DISPLAY_DIAGNOSTICS_IDF_ADDITIONS_H__ */
