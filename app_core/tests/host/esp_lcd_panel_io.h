/** @file LCD transport declarations provided by the App Manager fixture. */
#ifndef __HOST_ESP_LCD_PANEL_IO_H__
#define __HOST_ESP_LCD_PANEL_IO_H__

#include <stddef.h>
#include "esp_err.h"

/** @brief Opaque host LCD transport handle. */
typedef void *esp_lcd_panel_io_handle_t;

/** @brief Submit one parameter transaction to the fake LCD transport. */
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,
                                    int lcd_cmd,
                                    const void *param,
                                    size_t param_size);

#endif /* __HOST_ESP_LCD_PANEL_IO_H__ */
