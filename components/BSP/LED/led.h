#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

void led_flash_init(gpio_num_t gpio);

void led_set_state(gpio_num_t gpio, uint32_t level);

#ifdef __cplusplus
}
#endif