#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_control.h"
#include "wake_word.h"
#include "wifi_manager.h"
#include <stdio.h>

static const char *TAG = "main";

// 语音控制实例
static VoiceControl voiceCtrl;

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "    语音控制示例程序");
  ESP_LOGI(TAG, "========================================");

  // 初始化语音控制组件
  ESP_LOGI(TAG, "正在初始化语音控制组件...");
  esp_err_t ret = voiceCtrl.init({
      .led_gpio = GPIO_NUM_18,
      .servo_gpio = GPIO_NUM_7,
      .i2s_bck_io = GPIO_NUM_15,  // MAX98357 BCK
      .i2s_ws_io = GPIO_NUM_16,   // MAX98357 LRC
      .i2s_dout_io = GPIO_NUM_17, // MAX98357 DIN
      .servo_center_angle = 90.0f,
      .servo_rotate_angle = 90.0f,
      .led_flash_count = 5,
      .servo_swing_count = 3,
      .flash_delay_ms = 200,
      .swing_delay_ms = 300,
  });
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "语音控制组件初始化失败!");
    return;
  }

  // 初始化唤醒词与命令识别模块
  ESP_LOGI(TAG, "正在初始化语音识别模块...");
  auto &wakeWord = WakeWord::instance();
  ret = wakeWord.init(
      {.port = 0, .bck_io = 41, .ws_io = 42, .din_io = 2}, // I2S 配置
      {.timeout_ms = 6000}                                 // 命令识别超时
  );
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "语音识别模块初始化失败!");
    return;
  }

  // 绑定语音控制到唤醒词模块
  voiceCtrl.bindToWakeWord();

  // 启动语音识别
  ret = wakeWord.start();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "语音识别启动失败!");
    return;
  }

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  系统已就绪!");
  ESP_LOGI(TAG, "  唤醒词: \"小鹿，小鹿\"");
  ESP_LOGI(TAG, "  支持的命令:");
  ESP_LOGI(TAG, "    - 开灯");
  ESP_LOGI(TAG, "    - 关灯");
  ESP_LOGI(TAG, "    - 前进");
  ESP_LOGI(TAG, "    - 后退");
  ESP_LOGI(TAG, "    - 神龙摆尾");
  ESP_LOGI(TAG, "========================================");

  // 启动 WiFi + Web 配网/控制
  auto &wifiMgr = WifiManager::instance();
  ret = wifiMgr.init({
      .ap_ssid = "ESP32-Setup",
      .ap_password = "", // 为空=开放热点；如需密码请设置 >= 8 位
      .sta_connect_timeout_ms = 15000,
      .sta_max_retry = 5,
      .keep_ap_on_after_sta_connected = false,
  });
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "WiFi manager init failed: %s", esp_err_to_name(ret));
  } else {
    wifiMgr.setCommandCallback([](int commandId) {
      // 与语音命令 ID 对齐：0-4
      voiceCtrl.executeCommandById(commandId);
    });
    wifiMgr.setStatusCallback([]() -> std::string {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "{\"led_on\":%s,\"servo_angle\":%.1f}",
               voiceCtrl.isLightOn() ? "true" : "false",
               (double)voiceCtrl.getCurrentServoAngle());
      return std::string(buf);
    });
    wifiMgr.start();
  }

  // 主循环保持运行
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
