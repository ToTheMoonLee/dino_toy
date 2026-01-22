#include "led.h"

void led_flash_init(gpio_num_t gpio) {
  gpio_config_t gpio_cfg = {
      .pin_bit_mask = 1ULL << gpio,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&gpio_cfg);
}

void led_set_state(gpio_num_t gpio, uint32_t level) {
  gpio_set_level(gpio, level);
}