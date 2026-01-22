#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

void button_init(gpio_num_t pin);
int button_get_level(gpio_num_t pin);

#ifdef __cplusplus
}
#endif
