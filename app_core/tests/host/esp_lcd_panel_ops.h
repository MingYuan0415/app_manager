/** @file LCD panel declarations provided by the App Manager fixture. */
#ifndef __HOST_ESP_LCD_PANEL_OPS_H__
#define __HOST_ESP_LCD_PANEL_OPS_H__

#include "esp_err.h"
#include "esp_lcd_panel_interface.h"

/** @brief Opaque host LCD panel handle. */
typedef esp_lcd_panel_t *esp_lcd_panel_handle_t;

/** @brief Draw one bitmap through the fake LCD panel. */
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data);

#endif /* __HOST_ESP_LCD_PANEL_OPS_H__ */
