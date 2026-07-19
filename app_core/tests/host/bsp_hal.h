/** @file Host BSP handle types used by App Manager tests. */
#ifndef __HOST_BSP_HAL_H__
#define __HOST_BSP_HAL_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

/** @brief Host representation of physical TE synchronization parameters. */
typedef struct bsp_display_te_config
{
    bool enabled;
    int gpio_num;
    uint32_t bus_freq_hz;
    uint8_t data_lines;
    uint8_t bits_per_pixel;
    int intr_type;
} bsp_display_te_config_t;

/** @brief Host representation of the board display handles. */
typedef struct bsp_display_port
{
    uint16_t width;
    uint16_t height;
    esp_lcd_panel_handle_t panel;
    void *panel_io;
    esp_lcd_touch_handle_t touch;
    void *touch_io;
    bsp_display_te_config_t te;
} bsp_display_port_t;

#endif /* __HOST_BSP_HAL_H__ */
