#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include <cstddef>
#include <cstdint>
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
   * @brief 播放内存中的音频数据（WAV/MP3）
   *
   * @note data 必须来自 malloc/realloc 等可 free 的内存；播放器会接管并在播放结束/stop 时释放。
   */
  esp_err_t playOwnedBuffer(uint8_t *data, size_t len, bool loop = false);

  /**
   * @brief 开始播放 PCM 流（16-bit little-endian mono），用于低延迟语音对话
   *
   * @note 会停止当前播放（MP3/WAV），并切换 I2S 时钟到指定采样率。
   */
  esp_err_t pcmStreamBegin(uint32_t sample_rate_hz, uint32_t prebuffer_ms = 80);

  /**
   * @brief 写入 PCM 数据（16-bit little-endian mono）
   *
   * @return ESP_OK 成功；超时/失败会返回错误码
   */
  esp_err_t pcmStreamWrite(const uint8_t *data, size_t len,
                           uint32_t timeout_ms = 1000);

  /**
   * @brief 结束 PCM 流（会播放完缓冲区后回到 Idle）
   */
  esp_err_t pcmStreamEnd();

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

  void waitForIdle(uint32_t timeout_ms);
  void freeActiveBuffer();

  // 静态回调函数（用于 audio_player）
  static void audioCallback(void *ctx);

  // I2S 写入函数
  static esp_err_t i2sWrite(void *audio_buffer, size_t len,
                            size_t *bytes_written, uint32_t timeout_ms);

  // I2S 时钟设置函数
  static esp_err_t clkSetFn(uint32_t rate, uint32_t bits_cfg,
                            i2s_slot_mode_t ch);

  // PCM stream task
  static void pcmStreamTask(void *arg);
  void stopPcmStreamInternal(bool waitIdle);

  // 成员变量
  bool m_initialized = false;
  volatile Mp3PlayerState m_state = Mp3PlayerState::Idle;
  Mp3PlayerCallback m_callback = nullptr;
  i2s_chan_handle_t m_txHandle = nullptr;
  bool m_loopEnabled = false;

  enum class Source : uint8_t { EmbeddedMp3 = 0, OwnedBuffer = 1 };
  Source m_source = Source::EmbeddedMp3;
  uint8_t *m_activeBuf = nullptr;
  size_t m_activeBufLen = 0;

  // PCM streaming (input is mono S16LE)
  StreamBufferHandle_t m_pcmStream = nullptr;
  TaskHandle_t m_pcmTask = nullptr;
  volatile bool m_pcmStop = false;
  size_t m_pcmPrebufferBytes = 0;
  uint32_t m_pcmSampleRate = 16000;
};
