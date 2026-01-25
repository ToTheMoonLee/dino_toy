#pragma once

#include "driver/i2s_std.h"
#include "esp_afe_sr_iface.h"
#include "esp_err.h"
#include "esp_mn_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <functional>

/**
 * @brief 唤醒词检测状态枚举
 */
enum class WakeWordState {
  Idle = 0,        /*!< 空闲状态 */
  Running,         /*!< 等待唤醒词 */
  Detected,        /*!< 唤醒词已检测到 */
  ListeningCommand, /*!< 正在监听命令词 */
  Dialog            /*!< 对话模式：连续语音交互 */
};

/**
 * @brief 唤醒词检测回调类型
 * @param wake_word_index 检测到的唤醒词索引 (从1开始)
 */
using WakeWordCallback = std::function<void(int wake_word_index)>;

/**
 * @brief 命令词识别回调类型
 * @param command_id 识别到的命令ID
 * @param command_text 识别到的命令文本
 */
using CommandCallback =
    std::function<void(int command_id, const char *command_text)>;

/**
 * @brief AFE 音频帧回调（对话时用于录音/上传）
 *
 * @param samples    单通道 16bit PCM
 * @param numSamples 样本数
 * @param vadState   VAD 状态（VAD_SPEECH / VAD_SILENCE）
 */
using AudioFrameCallback =
    std::function<void(const int16_t *samples, int numSamples,
                       vad_state_t vadState)>;

/**
 * @brief I2S 麦克风配置
 */
struct I2sConfig {
  int port = 0;    /*!< I2S 端口号 */
  int bck_io = 41; /*!< I2S BCK GPIO */
  int ws_io = 42;  /*!< I2S WS GPIO */
  int din_io = 2;  /*!< I2S DIN GPIO */
};

/**
 * @brief 命令词配置
 */
struct CommandConfig {
  int timeout_ms = 6000; /*!< 命令词识别超时时间 (毫秒) */
};

/**
 * @brief 对话模式配置
 */
struct DialogConfig {
  bool enabled = false;           /*!< 唤醒后进入对话模式 */
  int session_timeout_ms = 20000; /*!< 多少毫秒无语音则退出对话 */
};

/**
 * @brief 语音唤醒与命令识别管理类
 *
 * 使用单例模式，因为 ESP32 通常只有一个麦克风输入
 * 基于 ESP-SR V2.0 框架的 AFE、WakeNet 和 MultiNet 引擎
 *
 * 工作流程：
 * 1. 等待唤醒词 "小鹿，小鹿"
 * 2. 唤醒后开始监听命令词
 * 3. 识别到命令词后执行回调
 * 4. 超时或识别完成后回到等待唤醒状态
 *
 * @example
 *   auto& wakeWord = WakeWord::instance();
 *   wakeWord.init({.bck_io = 41, .ws_io = 42, .din_io = 2});
 *   wakeWord.setCallback([](int index) {
 *       ESP_LOGI("MAIN", "唤醒词检测到!");
 *   });
 *   wakeWord.setCommandCallback([](int id, const char* text) {
 *       ESP_LOGI("MAIN", "命令词: %s (ID: %d)", text, id);
 *   });
 *   wakeWord.start();
 */
class WakeWord {
public:
  // 单例访问
  static WakeWord &instance();

  // 禁止拷贝和移动
  WakeWord(const WakeWord &) = delete;
  WakeWord &operator=(const WakeWord &) = delete;
  WakeWord(WakeWord &&) = delete;
  WakeWord &operator=(WakeWord &&) = delete;

  /**
   * @brief 初始化唤醒词模块（包含命令词识别）
   * @param i2sConfig I2S 麦克风配置
   * @param cmdConfig 命令词配置
   * @return ESP_OK 成功
   */
  esp_err_t init(const I2sConfig &i2sConfig = I2sConfig{},
                 const CommandConfig &cmdConfig = CommandConfig{});

  /**
   * @brief 设置唤醒词检测回调
   * @param callback 回调函数
   */
  void setCallback(WakeWordCallback callback) { m_callback = callback; }

  /**
   * @brief 设置命令词识别回调
   * @param callback 回调函数
   */
  void setCommandCallback(CommandCallback callback) {
    m_commandCallback = callback;
  }

  /**
   * @brief 设置音频帧回调（对话模式用）
   */
  void setAudioFrameCallback(AudioFrameCallback callback) {
    m_audioFrameCallback = callback;
  }

  /**
   * @brief 设置对话模式配置
   */
  void setDialogConfig(const DialogConfig &cfg);

  /**
   * @brief 对话模式下：触碰 keep-alive，避免长回复时被 session timeout 退出
   */
  void touchDialog();

  /**
   * @brief 请求退出对话模式（异步；由内部检测任务执行实际切换）
   */
  void requestExitDialog();

  /**
   * @brief 启动唤醒词检测
   * @return ESP_OK 成功
   */
  esp_err_t start();

  /**
   * @brief 停止唤醒词检测
   * @return ESP_OK 成功
   */
  esp_err_t stop();

  /**
   * @brief 临时禁用唤醒检测（对话时使用）
   */
  void disable();

  /**
   * @brief 重新启用唤醒检测
   */
  void enable();

  /**
   * @brief 释放资源
   */
  void deinit();

  /**
   * @brief 获取当前状态
   */
  WakeWordState getState() const { return m_state; }

  /**
   * @brief 检查是否正在运行
   */
  bool isRunning() const { return m_state != WakeWordState::Idle; }

  /**
   * @brief 检查是否正在监听命令
   */
  bool isListeningCommand() const {
    return m_state == WakeWordState::ListeningCommand;
  }

private:
  // 私有构造函数（单例）
  WakeWord() = default;
  ~WakeWord() = default;

  // 内部初始化
  esp_err_t initI2s(const I2sConfig &config);
  esp_err_t initAfe();
  esp_err_t initMultiNet();
  esp_err_t registerCommands();

  // 任务函数（静态，用于 FreeRTOS）
  static void audioFeedTask(void *arg);
  static void detectTask(void *arg);

  // 成员变量
  bool m_initialized = false;
  volatile WakeWordState m_state = WakeWordState::Idle;
  WakeWordCallback m_callback = nullptr;
  CommandCallback m_commandCallback = nullptr;
  CommandConfig m_cmdConfig;

  // ESP-SR 相关
  const esp_afe_sr_iface_t *m_afeHandle = nullptr;
  esp_afe_sr_data_t *m_afeData = nullptr;
  afe_config_t *m_afeConfig = nullptr;
  srmodel_list_t *m_models = nullptr;

  // MultiNet 相关
  const esp_mn_iface_t *m_mnHandle = nullptr;
  model_iface_data_t *m_mnData = nullptr;

  // I2S
  i2s_chan_handle_t m_i2sRxHandle = nullptr;

  // FreeRTOS 任务
  TaskHandle_t m_feedTaskHandle = nullptr;
  TaskHandle_t m_detectTaskHandle = nullptr;
  volatile bool m_running = false;

  // 命令词监听状态
  volatile bool m_listeningCommand = false;
  TickType_t m_commandStartTime = 0;

  // 对话模式
  DialogConfig m_dialogCfg;
  std::atomic<uint32_t> m_dialogLastActivityTick{0};
  std::atomic<bool> m_exitDialogRequested{false};
  bool m_prevVadSpeech = false;
  bool m_prevSpeakerPlaying = false;
  AudioFrameCallback m_audioFrameCallback = nullptr;
};
