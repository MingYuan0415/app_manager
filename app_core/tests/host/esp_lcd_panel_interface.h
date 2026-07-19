/** @file Minimal LCD panel interface used by App Manager host tests. */
#ifndef __HOST_ESP_LCD_PANEL_INTERFACE_H__
#define __HOST_ESP_LCD_PANEL_INTERFACE_H__

#include "esp_err.h"

typedef struct esp_lcd_panel_t esp_lcd_panel_t;

struct esp_lcd_panel_t
{
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *panel,
                             int x_start, int y_start,
                             int x_end, int y_end,
                             const void *color_data);
};

#endif /* __HOST_ESP_LCD_PANEL_INTERFACE_H__ */
