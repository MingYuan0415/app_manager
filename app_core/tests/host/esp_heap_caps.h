/** @file Heap capability declarations used by display diagnostics host tests. */
#ifndef __HOST_ESP_HEAP_CAPS_H__
#define __HOST_ESP_HEAP_CAPS_H__

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT     (UINT32_C(1) << 2)
#define MALLOC_CAP_DMA      (UINT32_C(1) << 3)
#define MALLOC_CAP_SPIRAM   (UINT32_C(1) << 10)
#define MALLOC_CAP_INTERNAL (UINT32_C(1) << 11)

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps);
void heap_caps_free(void *memory);

#endif /* __HOST_ESP_HEAP_CAPS_H__ */
