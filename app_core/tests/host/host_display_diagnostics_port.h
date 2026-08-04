/** @file Observable task port for display diagnostics host tests. */
#ifndef __HOST_DISPLAY_DIAGNOSTICS_PORT_H__
#define __HOST_DISPLAY_DIAGNOSTICS_PORT_H__

#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void host_display_diagnostics_port_reset(void);
void host_display_diagnostics_port_fail_next_create(void);
size_t host_display_diagnostics_port_create_count(void);
size_t host_display_diagnostics_port_delete_count(void);
uint32_t host_display_diagnostics_port_stack_depth(void);
UBaseType_t host_display_diagnostics_port_stack_caps(void);
UBaseType_t host_display_diagnostics_port_priority(void);
BaseType_t host_display_diagnostics_port_core_id(void);
void host_display_diagnostics_port_set_delete_hook(void (*hook)(void));
void host_display_diagnostics_port_set_timeout_hook(
    void (*hook)(TickType_t timeout_ticks));

#endif /* __HOST_DISPLAY_DIAGNOSTICS_PORT_H__ */
