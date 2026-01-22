#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include <functional>

/**
 * @brief MP3 播放器状态枚举
 */
enum class Mp3PlayerState {
  Idle = 0, /*!< 空闲状态 */
  Playing,  /*!< 播放中 */
  Paused    /*!< 暂停 */
};

/**
 * @brief 播放事件回调类型
 */
using Mp3PlayerCallback = std::function<void(Mp3PlayerState state)>;

/**
 * @brief I2S 配置
 */
struct Mp3I2sConfig {
  gpio_num_t bck_io = GPIO_NUM_NC;  /*!< I2S BCK 引脚 */
  gpio_num_t ws_io = GPIO_NUM_NC;   /*!< I2S WS/LRCK 引脚 */
  gpio_num_t dout_io = GPIO_NUM_NC; /*!< I2S DOUT 引脚 */
};

/**
 * @brief MP3 播放器类（单例模式）
 *
 * 基于 esp-audio-player 组件封装的 C++ 接口
 *
 * @example
 *   auto& player = Mp3Player::instance();
 *   player.init({.bck_io = GPIO_NUM_15, .ws_io = GPIO_NUM_16, .dout_io =
 * GPIO_NUM_17}); player.playEmbedded(true);  // 循环播放嵌入的 MP3
 */
class Mp3Player {
public:
  static Mp3Player &instance();

  // 禁止拷贝和移动
  Mp3Player(const Mp3Player &) = delete;
  Mp3Player &operator=(const Mp3Player &) = delete;
  Mp3Player(Mp3Player &&) = delete;
  Mp3Player &operator=(Mp3Player &&) = delete;

  /**
   * @brief 初始化播放器
   * @param config I2S 配置
   * @return ESP_OK 成功
   */
  esp_err_t init(const Mp3I2sConfig &config);

  /**
   * @brief 播放嵌入的 MP3 文件
   * @param loop 是否循环播放
   * @return ESP_OK 成功
   */
  esp_err_t playEmbedded(bool loop = false);

  /**
   * @brief 暂停播放
   * @return ESP_OK 成功
   */
  esp_err_t pause();

  /**
   * @brief 恢复播放
   * @return ESP_OK 成功
   */
  esp_err_t resume();

  /**
   * @brief 停止播放
   * @return ESP_OK 成功
   */
  esp_err_t stop();

  /**
   * @brief 获取当前状态
   * @return 当前播放状态
   */
  Mp3PlayerState getState() const { return m_state; }

  /**
   * @brief 检查是否正在播放
   * @return true 正在播放
   */
  bool isPlaying() const { return m_state == Mp3PlayerState::Playing; }

  /**
   * @brief 设置状态回调
   * @param callback 回调函数
   */
  void setCallback(Mp3PlayerCallback callback) { m_callback = callback; }

  /**
   * @brief 释放资源
   */
  void deinit();

private:
  Mp3Player() = default;
  ~Mp3Player() = default;

  // I2S 初始化
  esp_err_t initI2s(const Mp3I2sConfig &config);

  // 启动播放
  esp_err_t startPlayback();

  // 静态回调函数（用于 audio_player）
  static void audioCallback(void *ctx);

  // I2S 写入函数
  static esp_err_t i2sWrite(void *audio_buffer, size_t len,
                            size_t *bytes_written, uint32_t timeout_ms);

  // I2S 时钟设置函数
  static esp_err_t clkSetFn(uint32_t rate, uint32_t bits_cfg,
                            i2s_slot_mode_t ch);

  // 成员变量
  bool m_initialized = false;
  volatile Mp3PlayerState m_state = Mp3PlayerState::Idle;
  Mp3PlayerCallback m_callback = nullptr;
  i2s_chan_handle_t m_txHandle = nullptr;
  bool m_loopEnabled = false;
};
