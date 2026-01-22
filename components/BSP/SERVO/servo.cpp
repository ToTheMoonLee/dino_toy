#include "servo.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "SERVO";
static gpio_num_t s_servo_gpio = GPIO_NUM_NC;

// 舵机 PWM 参数
#define SERVO_MIN_PULSEWIDTH_US 500  // 0度对应的脉宽
#define SERVO_MAX_PULSEWIDTH_US 2500 // 180度对应的脉宽
#define SERVO_MAX_DEGREE 180

// 角度转换为占空比
static uint32_t angle_to_duty(float angle) {
  // PWM 周期 = 20ms (50Hz)，分辨率 = 13位 (8192)
  // duty = pulse_width_us / 20000 * 8192
  float pulse_us = SERVO_MIN_PULSEWIDTH_US +
                   (angle / SERVO_MAX_DEGREE) *
                       (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US);
  return (uint32_t)(pulse_us / 20000.0f * 8192.0f);
}

void servo_init(gpio_num_t gpio) {
  ESP_LOGI(TAG, "初始化舵机 (直接LEDC), GPIO: %d", gpio);
  s_servo_gpio = gpio;

  // 配置 LEDC 定时器
  ledc_timer_config_t timer_conf = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_13_BIT, // 13位分辨率
      .timer_num = LEDC_TIMER_0,
      .freq_hz = 50, // 50Hz for servo
      .clk_cfg = LEDC_AUTO_CLK,
      .deconfigure = false,
  };
  esp_err_t ret = ledc_timer_config(&timer_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LEDC 定时器配置失败: %s", esp_err_to_name(ret));
    return;
  }

  // 配置 LEDC 通道
  ledc_channel_config_t channel_conf = {
      .gpio_num = gpio,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = angle_to_duty(90), // 初始90度
      .hpoint = 0,
      .flags = {.output_invert = 0},
  };
  ret = ledc_channel_config(&channel_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LEDC 通道配置失败: %s", esp_err_to_name(ret));
    return;
  }

  ESP_LOGI(TAG, "舵机初始化成功 (LEDC直接驱动)");
  ESP_LOGI(TAG, "设置初始角度: 90 度, duty: %lu", angle_to_duty(90));
}

void servo_set_angle(float angle) {
  if (angle < 0)
    angle = 0;
  if (angle > 180)
    angle = 180;

  uint32_t duty = angle_to_duty(angle);
  ESP_LOGI(TAG, "设置舵机角度: %.1f, duty: %lu", angle, duty);

  esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "设置占空比失败: %s", esp_err_to_name(ret));
    return;
  }

  ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "更新占空比失败: %s", esp_err_to_name(ret));
  }
}