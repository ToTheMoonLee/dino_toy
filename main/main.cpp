#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "cloud_chat.h"
#include "cloud_tts.h"
#include "voice_dialog.h"
#include "voice_control.h"
#include "wake_word.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

// 语音控制实例
static VoiceControl voiceCtrl;
static VoiceDialog voiceDialog;

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

  // 启用对话模式：唤醒后可连续多轮对话（本地命令仍保留）
  // 优先使用 WebSocket 模式（延迟最低），否则回退到 HTTP 模式
  const bool useWebSocket = (strlen(CONFIG_CLOUD_WEBSOCKET_URL) > 0);
  const bool usePcmStream = !useWebSocket && (strlen(CONFIG_CLOUD_CHAT_PCM_PROXY_URL) > 0);
  const char *chatUrl =
      usePcmStream ? CONFIG_CLOUD_CHAT_PCM_PROXY_URL : CONFIG_CLOUD_CHAT_PROXY_URL;
  const bool dialogEnabled = useWebSocket || (strlen(chatUrl) > 0);
  wakeWord.setDialogConfig({
      .enabled = dialogEnabled,
      .session_timeout_ms = CONFIG_DIALOG_SESSION_TIMEOUT_MS,
  });

  // 初始化对话模块（语音分段 + 上云对话 + 播报）
  voiceDialog.init({
      .chat_url = chatUrl,
      .ws_url = CONFIG_CLOUD_WEBSOCKET_URL,
      .use_websocket = useWebSocket,
      .sample_rate_hz = 16000,
      .use_pcm_stream = usePcmStream,
      .min_speech_ms = 300,
      .end_silence_ms = CONFIG_DIALOG_END_SILENCE_MS,
      .max_utterance_ms = CONFIG_DIALOG_MAX_UTTERANCE_MS,
      .max_pcm_ms = CONFIG_DIALOG_MAX_UTTERANCE_MS + CONFIG_DIALOG_END_SILENCE_MS +
                    2000,
      .energy_gate_mean_abs = CONFIG_DIALOG_ENERGY_GATE_MEAN_ABS,
      .local_command_ignore_ms = CONFIG_DIALOG_LOCAL_COMMAND_IGNORE_MS,
      .worker_stack = 8192,
      .worker_prio = 4,
      .worker_core = 0,
  });

  // 统一在 main 分发 WakeWord 回调：保留原命令功能，同时接入对话
  wakeWord.setCallback([](int /*index*/) {
    voiceCtrl.onWakeDetected();
    voiceDialog.onWakeDetected();
  });
  wakeWord.setCommandCallback([](int commandId, const char * /*commandText*/) {
    voiceCtrl.executeCommandById(commandId);
    voiceDialog.onLocalCommandDetected();
  });
  wakeWord.setAudioFrameCallback([](const int16_t *samples, int numSamples,
                                    vad_state_t vad) {
    voiceDialog.onAudioFrame(samples, numSamples, vad);
  });

  // 启动语音识别
  ret = wakeWord.start();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "语音识别启动失败!");
    return;
  }

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  系统已就绪!");
  ESP_LOGI(TAG, "  唤醒词: \"小鹿，小鹿\"");
  ESP_LOGI(TAG, "  支持的本地命令:");
  ESP_LOGI(TAG, "    - 开灯 / 关灯 / 前进 / 后退 / 神龙摆尾");
  if (useWebSocket) {
    ESP_LOGI(TAG, "  对话模式: WebSocket 实时流式 (延迟最低)");
    ESP_LOGI(TAG, "    URL: %s", CONFIG_CLOUD_WEBSOCKET_URL);
  } else if (dialogEnabled) {
    ESP_LOGI(TAG, "  对话模式: HTTP %s", usePcmStream ? "(PCM Stream)" : "(WAV)");
  } else {
    ESP_LOGW(TAG, "  对话模式: 未启用 (请在 menuconfig 设置 Cloud WebSocket/Chat URL)");
  }
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
    // 初始化 Cloud TTS（建议配合局域网 proxy，避免在固件里放 API Key）
    CloudTts::instance().init({
        .url = CONFIG_CLOUD_TTS_PROXY_URL,
        .timeout_ms = 15000,
        .max_response_bytes = 1024 * 1024,
    });

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
    wifiMgr.setTtsCallback([](const std::string &text) {
      auto &tts = CloudTts::instance();
      if (tts.getUrl().empty()) {
        ESP_LOGW(TAG, "CONFIG_CLOUD_TTS_PROXY_URL is empty, skip tts");
        return;
      }
      (void)tts.speak(text);
    });
    wifiMgr.start();
  }

  // 主循环保持运行
  while (1) {
    voiceDialog.tick();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
