/** @file Minimal GPIO declarations used by App Manager host tests. */
#ifndef __HOST_DRIVER_GPIO_H__
#define __HOST_DRIVER_GPIO_H__

#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)

/** @brief Disable one fake GPIO interrupt. */
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);

#endif /* __HOST_DRIVER_GPIO_H__ */
