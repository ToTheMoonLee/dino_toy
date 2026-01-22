#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <functional>
#include <string>

/**
 * @brief 语音命令类型枚举
 */
enum class VoiceCommand {
  Unknown = 0,
  LightOn,   // 开灯
  LightOff,  // 关灯
  Forward,   // 前进
  Backward,  // 后退
  DragonTail // 神龙摆尾
};

/**
 * @brief 语音命令回调类型
 * @param command 识别到的命令
 */
using VoiceCommandCallback = std::function<void(VoiceCommand command)>;

/**
 * @brief 语音控制配置
 */
struct VoiceControlConfig {
  gpio_num_t led_gpio = GPIO_NUM_18;      /*!< LED GPIO */
  gpio_num_t servo_gpio = GPIO_NUM_4;     /*!< 舵机 GPIO */
  gpio_num_t i2s_bck_io = GPIO_NUM_15;    /*!< I2S BCK引脚 (MAX98357) */
  gpio_num_t i2s_ws_io = GPIO_NUM_16;     /*!< I2S WS引脚 (MAX98357) */
  gpio_num_t i2s_dout_io = GPIO_NUM_17;   /*!< I2S DOUT引脚 (MAX98357) */
  float servo_center_angle = 90.0f;       /*!< 舵机中心角度 */
  float servo_rotate_angle = 90.0f;       /*!< 舵机旋转角度 */
  int led_flash_count = 5;                /*!< 神龙摆尾LED闪烁次数 */
  int servo_swing_count = 3;              /*!< 神龙摆尾舵机摆动次数 */
  int flash_delay_ms = 200;               /*!< 闪烁延时 (ms) */
  int swing_delay_ms = 300;               /*!< 摆动延时 (ms) */
};

/**
 * @brief 语音控制组件类
 *
 * 根据语音命令控制LED和舵机
 * 支持的命令：
 * - "开灯": 点亮LED
 * - "关灯": 关闭LED
 * - "前进": 舵机右转90度
 * - "后退": 舵机左转90度
 * - "神龙摆尾": 舵机左右摆动3次，LED闪烁5次
 *
 * @example
 *   VoiceControl voiceCtrl;
 *   voiceCtrl.init({.led_gpio = GPIO_NUM_18, .servo_gpio = GPIO_NUM_4});
 *   voiceCtrl.executeCommand(VoiceCommand::LightOn);
 */
class VoiceControl {
public:
  VoiceControl() = default;
  ~VoiceControl() = default;

  /**
   * @brief 初始化语音控制组件
   * @param config 配置参数
   * @return ESP_OK 成功
   */
  esp_err_t init(const VoiceControlConfig &config = VoiceControlConfig{});

  /**
   * @brief 设置命令执行回调（可选，用于通知外部命令已执行）
   * @param callback 回调函数
   */
  void setCallback(VoiceCommandCallback callback) { m_callback = callback; }

  /**
   * @brief 根据命令字符串执行对应操作
   * @param commandText 命令文本
   * @return 识别到的命令类型
   */
  VoiceCommand parseAndExecute(const std::string &commandText);

  /**
   * @brief 执行指定命令
   * @param command 命令类型
   */
  void executeCommand(VoiceCommand command);

  /**
   * @brief 开灯
   */
  void turnOnLight();

  /**
   * @brief 关灯
   */
  void turnOffLight();

  /**
   * @brief 前进 - 舵机右转90度
   */
  void moveForward();

  /**
   * @brief 后退 - 舵机左转90度
   */
  void moveBackward();

  /**
   * @brief 神龙摆尾 - 舵机左右摆动3次，LED闪烁5次
   */
  void dragonTailSwing();

  /**
   * @brief 获取LED当前状态
   * @return true 亮, false 灭
   */
  bool isLightOn() const { return m_ledOn; }

  /**
   * @brief 获取当前舵机角度
   * @return 当前角度
   */
  float getCurrentServoAngle() const { return m_currentAngle; }

  /**
   * @brief 绑定到 WakeWord 组件
   * 自动设置命令回调，将语音识别结果映射到执行动作
   */
  void bindToWakeWord();

  /**
   * @brief 根据命令ID执行对应操作
   * @param commandId 命令ID (0-4)
   */
  void executeCommandById(int commandId);

private:
  // 解析命令文本
  VoiceCommand parseCommand(const std::string &text);

  // 配置
  VoiceControlConfig m_config;

  // 状态
  bool m_initialized = false;
  bool m_ledOn = false;
  float m_currentAngle = 90.0f;

  // 回调
  VoiceCommandCallback m_callback = nullptr;
};
