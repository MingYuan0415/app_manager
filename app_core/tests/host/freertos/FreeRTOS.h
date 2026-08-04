/** @file FreeRTOS compatibility declarations for display diagnostics tests. */
#ifndef __HOST_DISPLAY_DIAGNOSTICS_FREERTOS_H__
#define __HOST_DISPLAY_DIAGNOSTICS_FREERTOS_H__

#include "host_freertos.h"

typedef unsigned UBaseType_t;
typedef uint32_t StackType_t;

#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)

#endif /* __HOST_DISPLAY_DIAGNOSTICS_FREERTOS_H__ */
