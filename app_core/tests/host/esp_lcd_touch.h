/** @file Minimal LCD touch declarations used by App Manager host tests. */
#ifndef __HOST_ESP_LCD_TOUCH_H__
#define __HOST_ESP_LCD_TOUCH_H__

#include "driver/gpio.h"

typedef struct esp_lcd_touch_s esp_lcd_touch_t;
typedef esp_lcd_touch_t *esp_lcd_touch_handle_t;

typedef struct esp_lcd_touch_config
{
    gpio_num_t int_gpio_num;
} esp_lcd_touch_config_t;

struct esp_lcd_touch_s
{
    esp_lcd_touch_config_t config;
};

#endif /* __HOST_ESP_LCD_TOUCH_H__ */
