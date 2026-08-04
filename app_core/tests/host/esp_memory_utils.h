/** @file Memory-region declarations used by display diagnostics host tests. */
#ifndef __HOST_ESP_MEMORY_UTILS_H__
#define __HOST_ESP_MEMORY_UTILS_H__

#include <stdbool.h>

bool esp_ptr_external_ram(const void *pointer);

#endif /* __HOST_ESP_MEMORY_UTILS_H__ */
