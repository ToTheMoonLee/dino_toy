#include "button.h"

void button_init(gpio_num_t pin) {
  gpio_config_t conf = {.pin_bit_mask = (1ULL << pin),
                        .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE,
                        .pull_down_en =
                            GPIO_PULLDOWN_ENABLE, // Pulling down because VCC is
                                                  // connected to button
                        .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&conf);
}

int button_get_level(gpio_num_t pin) { return gpio_get_level(pin); }
