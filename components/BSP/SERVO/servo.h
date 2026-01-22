#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_servo.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void servo_init(gpio_num_t gpio);

void servo_set_angle(float angle);

#ifdef __cplusplus
}
#endif