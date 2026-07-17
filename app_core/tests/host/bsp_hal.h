/** @file Host BSP handle types used by App Manager tests. */
#ifndef __HOST_BSP_HAL_H__
#define __HOST_BSP_HAL_H__

#include <stdint.h>

/** @brief Host representation of the board display handles. */
typedef struct bsp_display_port
{
    uint16_t width;
    uint16_t height;
    void *panel;
    void *panel_io;
    void *touch;
    void *touch_io;
} bsp_display_port_t;

#endif /* __HOST_BSP_HAL_H__ */
