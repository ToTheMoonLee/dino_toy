#include "voice_control.h"
#include "esp_log.h"
#include "led.h"
#include "mp3_player.h"
#include "servo.h"
#include "wake_word.h"
#include <algorithm>
#include <cstring>

static const char *TAG = "VoiceControl";

namespace {
enum class VoiceControlEventType : uint8_t { WakeDetected = 0, Command = 1 };

struct VoiceControlEvent {
  VoiceControlEventType type = VoiceControlEventType::WakeDetected;
  int commandId = -1;
  uint32_t token = 0;
};
} // namespace

esp_err_t VoiceControl::init(const VoiceControlConfig &config) {
  if (m_initialized) {
    ESP_LOGW(TAG, "VoiceControl already initialized");
    return ESP_OK;
  }

  m_config = config;

  // 初始化LED
  ESP_LOGI(TAG, "Initializing LED on GPIO %d", m_config.led_gpio);
  led_flash_init(m_config.led_gpio);
  led_set_state(m_config.led_gpio, 0); // 初始关闭
  m_ledOn = false;

  // 初始化舵机
  ESP_LOGI(TAG, "Initializing Servo on GPIO %d", m_config.servo_gpio);
  servo_init(m_config.servo_gpio);
  servo_set_angle(m_config.servo_center_angle); // 初始中间位置
  m_currentAngle = m_config.servo_center_angle;

  // 初始化 MP3 播放器（MAX98357）
  if (m_config.i2s_bck_io != GPIO_NUM_NC && m_config.i2s_ws_io != GPIO_NUM_NC &&
      m_config.i2s_dout_io != GPIO_NUM_NC) {
    ESP_LOGI(TAG, "Initializing MP3 Player (BCK=%d, WS=%d, DOUT=%d)",
             m_config.i2s_bck_io, m_config.i2s_ws_io, m_config.i2s_dout_io);
    auto &mp3Player = Mp3Player::instance();
    esp_err_t mp3Ret = mp3Player.init({
        .bck_io = m_config.i2s_bck_io,
        .ws_io = m_config.i2s_ws_io,
        .dout_io = m_config.i2s_dout_io,
    });
    if (mp3Ret != ESP_OK) {
      ESP_LOGW(TAG, "MP3 Player init failed: %s", esp_err_to_name(mp3Ret));
    }
  } else {
    ESP_LOGW(TAG, "MP3 Player pins not set, skip init");
  }

  // 创建后台执行任务：避免在唤醒/识别线程里执行带 vTaskDelay 的动作，提升丝滑度
  m_eventQueue = xQueueCreate(8, sizeof(VoiceControlEvent));
  if (m_eventQueue == nullptr) {
    ESP_LOGW(TAG, "Failed to create VoiceControl event queue, fallback to sync "
                  "execution");
    // 继续初始化，其它功能仍可用（但会回退到同步执行，可能不够丝滑）
  } else {
    BaseType_t taskOk =
        xTaskCreatePinnedToCore(workerTask, "vc_worker", 4096, this, 4,
                                &m_workerTaskHandle, 0);
    if (taskOk != pdPASS) {
      ESP_LOGW(TAG,
               "Failed to create VoiceControl worker task, fallback to sync "
               "execution");
      vQueueDelete(m_eventQueue);
      m_eventQueue = nullptr;
    }
  }

  m_initialized = true;
  ESP_LOGI(TAG, "VoiceControl initialized successfully");
  return ESP_OK;
}

VoiceCommand VoiceControl::parseCommand(const std::string &text) {
  // 检查包含的关键词
  if (text.find("开灯") != std::string::npos) {
    return VoiceCommand::LightOn;
  }
  if (text.find("关灯") != std::string::npos) {
    return VoiceCommand::LightOff;
  }
  if (text.find("前进") != std::string::npos) {
    return VoiceCommand::Forward;
  }
  if (text.find("后退") != std::string::npos) {
    return VoiceCommand::Backward;
  }
  if (text.find("神龙摆尾") != std::string::npos) {
    return VoiceCommand::DragonTail;
  }

  return VoiceCommand::Unknown;
}

VoiceCommand VoiceControl::parseAndExecute(const std::string &commandText) {
  VoiceCommand cmd = parseCommand(commandText);

  if (cmd != VoiceCommand::Unknown) {
    ESP_LOGI(TAG, "Recognized command: %d from text: %s", static_cast<int>(cmd),
             commandText.c_str());
    executeCommand(cmd);
  } else {
    ESP_LOGW(TAG, "Unknown command: %s", commandText.c_str());
  }

  return cmd;
}

void VoiceControl::executeCommand(VoiceCommand command) {
  executeCommandInternal(command, m_actionToken.load(std::memory_order_relaxed));
}

void VoiceControl::turnOnLight() {
  led_set_state(m_config.led_gpio, 1);
  m_ledOn = true;
  ESP_LOGI(TAG, "LED turned ON");
}

void VoiceControl::turnOffLight() {
  led_set_state(m_config.led_gpio, 0);
  m_ledOn = false;
  ESP_LOGI(TAG, "LED turned OFF");
}

void VoiceControl::moveForward() {
  // 前进 - 舵机向右旋转90度（从中心位置算起）
  float targetAngle = m_config.servo_center_angle + m_config.servo_rotate_angle;

  // 限制角度范围 0-180
  if (targetAngle > 180.0f)
    targetAngle = 180.0f;

  servo_set_angle(targetAngle);
  m_currentAngle = targetAngle;
  ESP_LOGI(TAG, "Servo moved forward to angle: %.1f", targetAngle);
}

void VoiceControl::moveBackward() {
  // 后退 - 舵机向左旋转90度（从中心位置算起）
  float targetAngle = m_config.servo_center_angle - m_config.servo_rotate_angle;

  // 限制角度范围 0-180
  if (targetAngle < 0.0f)
    targetAngle = 0.0f;

  servo_set_angle(targetAngle);
  m_currentAngle = targetAngle;
  ESP_LOGI(TAG, "Servo moved backward to angle: %.1f", targetAngle);
}

void VoiceControl::dragonTailSwing() {
  dragonTailSwing(m_actionToken.load(std::memory_order_relaxed));
}

bool VoiceControl::shouldAbort(uint32_t token) const {
  return m_actionToken.load(std::memory_order_relaxed) != token;
}

void VoiceControl::executeCommandInternal(VoiceCommand command, uint32_t token) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "VoiceControl not initialized");
    return;
  }
  if (shouldAbort(token)) {
    return;
  }

  switch (command) {
  case VoiceCommand::LightOn:
    ESP_LOGI(TAG, "Executing: 开灯");
    turnOnLight();
    break;
  case VoiceCommand::LightOff:
    ESP_LOGI(TAG, "Executing: 关灯");
    turnOffLight();
    break;
  case VoiceCommand::Forward:
    ESP_LOGI(TAG, "Executing: 前进");
    moveForward();
    break;
  case VoiceCommand::Backward:
    ESP_LOGI(TAG, "Executing: 后退");
    moveBackward();
    break;
  case VoiceCommand::DragonTail:
    ESP_LOGI(TAG, "Executing: 神龙摆尾");
    dragonTailSwing(token);
    break;
  default:
    ESP_LOGW(TAG, "Unknown command");
    break;
  }

  // 触发回调
  if (m_callback) {
    m_callback(command);
  }
}

void VoiceControl::dragonTailSwing(uint32_t token) {
  ESP_LOGI(TAG, "Starting Dragon Tail Swing!");

  // 保存LED原始状态
  bool originalLedState = m_ledOn;

  // 播放“神龙摆尾”音效（嵌入的 dinosaur-roar.mp3）
  auto &mp3Player = Mp3Player::instance();
  if (mp3Player.getState() != Mp3PlayerState::Idle) {
    mp3Player.stop();
  }
  mp3Player.playEmbedded(false);

  // 舵机左右摆动和LED闪烁同时进行
  // 舵机摆动3次，LED闪烁5次
  // 为了同步效果，我们交替执行

  int totalSteps =
      std::max(m_config.servo_swing_count * 2, m_config.led_flash_count * 2);
  int servoStep = 0;
  int ledStep = 0;
  bool servoRight = true; // 先向右摆
  bool ledOn = true;      // 先亮

  for (int i = 0; i < totalSteps; i++) {
    if (shouldAbort(token)) {
      break;
    }

    // 舵机控制
    if (servoStep < m_config.servo_swing_count * 2) {
      if (servoRight) {
        float rightAngle =
            m_config.servo_center_angle + m_config.servo_rotate_angle;
        if (rightAngle > 180.0f)
          rightAngle = 180.0f;
        servo_set_angle(rightAngle);
        m_currentAngle = rightAngle;
      } else {
        float leftAngle =
            m_config.servo_center_angle - m_config.servo_rotate_angle;
        if (leftAngle < 0.0f)
          leftAngle = 0.0f;
        servo_set_angle(leftAngle);
        m_currentAngle = leftAngle;
      }
      servoRight = !servoRight;
      servoStep++;
    }

    // LED控制
    if (ledStep < m_config.led_flash_count * 2) {
      led_set_state(m_config.led_gpio, ledOn ? 1 : 0);
      ledOn = !ledOn;
      ledStep++;
    }

    // 延时
    int delayMs = std::max(m_config.swing_delay_ms, m_config.flash_delay_ms);
    // 将长延时切片，提升打断响应速度
    constexpr int kSliceMs = 50;
    int remain = delayMs;
    while (remain > 0 && !shouldAbort(token)) {
      int slice = std::min(remain, kSliceMs);
      vTaskDelay(pdMS_TO_TICKS(slice));
      remain -= slice;
    }
  }

  // 被打断时停止音效，避免“换命令后还在吼”
  if (shouldAbort(token)) {
    mp3Player.stop();
  }

  // 恢复舵机到中间位置
  servo_set_angle(m_config.servo_center_angle);
  m_currentAngle = m_config.servo_center_angle;

  // 恢复LED原始状态
  led_set_state(m_config.led_gpio, originalLedState ? 1 : 0);
  m_ledOn = originalLedState;

  ESP_LOGI(TAG, "Dragon Tail Swing completed!");
}

void VoiceControl::executeCommandById(int commandId) {
  // 命令ID映射：
  // 0 = 开灯
  // 1 = 关灯
  // 2 = 前进
  // 3 = 后退
  // 4 = 神龙摆尾

  VoiceCommand commands[] = {VoiceCommand::LightOn, VoiceCommand::LightOff,
                             VoiceCommand::Forward, VoiceCommand::Backward,
                             VoiceCommand::DragonTail};

  if (commandId >= 0 && commandId < 5) {
    executeCommand(commands[commandId]);
  } else {
    ESP_LOGW(TAG, "Invalid command ID: %d", commandId);
  }
}

void VoiceControl::bindToWakeWord() {
  auto &wakeWord = WakeWord::instance();

  // 保存this指针以在lambda中使用
  VoiceControl *self = this;

  // 设置唤醒词回调
  wakeWord.setCallback([self](int /*index*/) {
    ESP_LOGI("VoiceControl", "🎤 唤醒词检测到! 准备接收命令...");
    if (self) {
      self->postWakeEvent();
    }
  });

  // 设置命令词回调
  wakeWord.setCommandCallback([self](int commandId, const char *commandText) {
    ESP_LOGI("VoiceControl", "📢 收到命令: %s (ID: %d)", commandText,
             commandId);
    if (self) {
      self->postCommandEvent(commandId);
    }
  });

  ESP_LOGI(TAG, "VoiceControl 已绑定到 WakeWord 组件");
}

void VoiceControl::blinkLed(int count, int delay_ms, uint32_t token) {
  if (count <= 0) {
    return;
  }
  if (delay_ms <= 0) {
    delay_ms = 100;
  }

  bool originalLedState = m_ledOn;
  bool ledOn = true;

  for (int i = 0; i < count * 2; i++) {
    if (shouldAbort(token)) {
      break;
    }
    led_set_state(m_config.led_gpio, ledOn ? 1 : 0);
    ledOn = !ledOn;

    constexpr int kSliceMs = 50;
    int remain = delay_ms;
    while (remain > 0 && !shouldAbort(token)) {
      int slice = std::min(remain, kSliceMs);
      vTaskDelay(pdMS_TO_TICKS(slice));
      remain -= slice;
    }
  }

  led_set_state(m_config.led_gpio, originalLedState ? 1 : 0);
}

void VoiceControl::postWakeEvent() {
  if (m_eventQueue == nullptr) {
    blinkLed(2, m_config.flash_delay_ms,
             m_actionToken.load(std::memory_order_relaxed));
    return;
  }

  VoiceControlEvent ev = {
      .type = VoiceControlEventType::WakeDetected,
      .commandId = -1,
      .token = m_actionToken.load(std::memory_order_relaxed),
  };
  if (xQueueSend(m_eventQueue, &ev, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Event queue full, drop wake event");
  }
}

void VoiceControl::postCommandEvent(int commandId) {
  // 新命令到来：提升 token -> 打断正在执行的动作
  uint32_t token = m_actionToken.fetch_add(1, std::memory_order_relaxed) + 1;

  if (m_eventQueue == nullptr) {
    // 兜底：没有队列时仍执行，但会阻塞调用方（不推荐）
    executeCommandById(commandId);
    return;
  }

  VoiceControlEvent ev = {
      .type = VoiceControlEventType::Command,
      .commandId = commandId,
      .token = token,
  };
  if (xQueueSend(m_eventQueue, &ev, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Event queue full, drop command: %d", commandId);
  }
}

void VoiceControl::workerTask(void *arg) {
  auto *self = static_cast<VoiceControl *>(arg);
  if (self == nullptr || self->m_eventQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  VoiceControlEvent ev{};

  while (true) {
    if (xQueueReceive(self->m_eventQueue, &ev, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    // 合并队列中后续的 Command 事件：只执行最后一个，减少“排队造成的迟滞”
    if (ev.type == VoiceControlEventType::Command) {
      VoiceControlEvent next{};
      while (xQueueReceive(self->m_eventQueue, &next, 0) == pdTRUE) {
        if (next.type == VoiceControlEventType::Command) {
          ev = next;
        }
      }
    }

    switch (ev.type) {
    case VoiceControlEventType::WakeDetected:
      self->blinkLed(2, self->m_config.flash_delay_ms, ev.token);
      break;

    case VoiceControlEventType::Command: {
      VoiceCommand commands[] = {VoiceCommand::LightOn, VoiceCommand::LightOff,
                                 VoiceCommand::Forward, VoiceCommand::Backward,
                                 VoiceCommand::DragonTail};
      if (ev.commandId >= 0 && ev.commandId < 5) {
        self->executeCommandInternal(commands[ev.commandId], ev.token);
      } else {
        ESP_LOGW(TAG, "Invalid command ID: %d", ev.commandId);
      }
      break;
    }
    }
  }
}
