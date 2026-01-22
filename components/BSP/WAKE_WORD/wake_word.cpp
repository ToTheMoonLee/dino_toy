/**
 * @file wake_word.cpp
 * @brief ESP32-S3 语音唤醒与命令识别组件实现
 *
 * 使用 ESP-SR V2.0 框架的 AFE (Audio Front-End)、WakeNet 和 MultiNet
 * 引擎实现语音唤醒检测和命令词识别。
 *
 * 支持的命令词：
 * - "开灯" (ID: 0)
 * - "关灯" (ID: 1)
 * - "前进" (ID: 2)
 * - "后退" (ID: 3)
 * - "神龙摆尾" (ID: 4)
 */

#include "wake_word.h"

#include "driver/i2s_std.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#include "esp_wn_iface.h"
#include "model_path.h"
#include <string.h>

static const char *TAG = "WakeWord";

// 音频配置
static constexpr int AUDIO_SAMPLE_RATE = 16000;

// 命令词定义
static const char *COMMANDS[] = {
    "kai deng",         // 开灯
    "guan deng",        // 关灯
    "qian jin",         // 前进
    "hou tui",          // 后退
    "shen long bai wei" // 神龙摆尾
};
static const char *COMMAND_NAMES[] = {"开灯", "关灯", "前进", "后退",
                                      "神龙摆尾"};
static constexpr int NUM_COMMANDS = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

// ============= 单例实现 =============

WakeWord &WakeWord::instance() {
  static WakeWord instance;
  return instance;
}

// ============= 任务函数 =============

void WakeWord::audioFeedTask(void *arg) {
  auto &self = WakeWord::instance();

  int chunkSize = self.m_afeHandle->get_feed_chunksize(self.m_afeData);
  int16_t *buffer = (int16_t *)malloc(chunkSize * sizeof(int16_t));

  if (buffer == nullptr) {
    ESP_LOGE(TAG, "无法分配音频缓冲区");
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "音频采集任务已启动, chunk size: %d", chunkSize);

  size_t bytesRead = 0;
  uint32_t totalChunks = 0;
  int16_t maxLevel = 0;
  TickType_t lastLogTime = xTaskGetTickCount();

  while (self.m_running) {
    esp_err_t ret = i2s_channel_read(self.m_i2sRxHandle, buffer,
                                     chunkSize * sizeof(int16_t), &bytesRead,
                                     portMAX_DELAY);
    if (ret == ESP_OK && bytesRead > 0) {
      // 计算音频电平（找最大值）
      for (int i = 0; i < chunkSize; i++) {
        int16_t absVal = buffer[i] > 0 ? buffer[i] : -buffer[i];
        if (absVal > maxLevel) {
          maxLevel = absVal;
        }
      }

      self.m_afeHandle->feed(self.m_afeData, buffer);
      totalChunks++;

      // 每 5 秒打印一次调试信息
      TickType_t currentTime = xTaskGetTickCount();
      if ((currentTime - lastLogTime) * portTICK_PERIOD_MS >= 5000) {
        ESP_LOGI(TAG, "📊 音频统计: chunks=%lu, 最大电平=%d, 读取字节=%u",
                 totalChunks, maxLevel, (unsigned)bytesRead);
        maxLevel = 0; // 重置
        lastLogTime = currentTime;
      }
    } else {
      ESP_LOGW(TAG, "I2S 读取失败: ret=%d, bytesRead=%u", ret,
               (unsigned)bytesRead);
    }
  }

  free(buffer);
  ESP_LOGI(TAG, "音频采集任务已退出");
  vTaskDelete(nullptr);
}

void WakeWord::detectTask(void *arg) {
  auto &self = WakeWord::instance();

  ESP_LOGI(TAG, "唤醒词检测任务已启动");

  while (self.m_running) {
    afe_fetch_result_t *res = self.m_afeHandle->fetch(self.m_afeData);

    if (res == nullptr || res->ret_value == ESP_FAIL) {
      continue;
    }

    // 检测到唤醒词
    if (res->wakeup_state == WAKENET_DETECTED) {
      ESP_LOGI(TAG, "🎤 唤醒词检测到! 索引: %d", res->wake_word_index);

      self.m_state = WakeWordState::Detected;

      // 调用用户回调
      if (self.m_callback) {
        self.m_callback(res->wake_word_index);
      }

      // 进入命令词监听模式
      self.m_state = WakeWordState::ListeningCommand;
      self.m_listeningCommand = true;
      self.m_commandStartTime = xTaskGetTickCount();

      ESP_LOGI(TAG, "🎧 开始监听命令词...");
    }

    // 命令词识别模式
    if (self.m_listeningCommand && self.m_mnHandle && self.m_mnData) {
      // 检查超时
      TickType_t currentTime = xTaskGetTickCount();
      TickType_t elapsedMs =
          (currentTime - self.m_commandStartTime) * portTICK_PERIOD_MS;
      if (elapsedMs > (TickType_t)self.m_cmdConfig.timeout_ms) {
        ESP_LOGW(TAG, "⏰ 命令词识别超时");
        self.m_listeningCommand = false;
        self.m_state = WakeWordState::Running;
        continue;
      }

      // 进行命令词识别
      esp_mn_state_t mnState =
          self.m_mnHandle->detect(self.m_mnData, res->data);

      if (mnState == ESP_MN_STATE_DETECTING) {
        // 正在检测中，继续
        continue;
      }

      if (mnState == ESP_MN_STATE_DETECTED) {
        // 识别到命令词
        esp_mn_results_t *mnResult =
            self.m_mnHandle->get_results(self.m_mnData);

        if (mnResult != nullptr && mnResult->num > 0) {
          int commandId = mnResult->command_id[0];
          const char *commandName = COMMAND_NAMES[commandId];

          ESP_LOGI(TAG, "✅ 命令词识别成功: %s (ID: %d, 置信度: %.2f)",
                   commandName, commandId, mnResult->prob[0]);

          // 调用命令回调
          if (self.m_commandCallback) {
            self.m_commandCallback(commandId, commandName);
          }
        }

        // 识别完成，回到等待唤醒状态
        self.m_listeningCommand = false;
        self.m_state = WakeWordState::Running;
      }

      if (mnState == ESP_MN_STATE_TIMEOUT) {
        ESP_LOGW(TAG, "⏰ MultiNet 检测超时");
        self.m_listeningCommand = false;
        self.m_state = WakeWordState::Running;
      }
    }
  }

  ESP_LOGI(TAG, "唤醒词检测任务已退出");
  vTaskDelete(nullptr);
}

// ============= 内部初始化 =============

esp_err_t WakeWord::initI2s(const I2sConfig &config) {
  i2s_chan_config_t chanCfg =
      I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)config.port, I2S_ROLE_MASTER);
  chanCfg.auto_clear = true;

  ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, nullptr, &m_i2sRxHandle));

  // 配置 slot 为左声道（INMP441 L/R 接 GND 时输出左声道数据）
  i2s_std_slot_config_t slotCfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  slotCfg.slot_mask = I2S_STD_SLOT_LEFT; // 明确指定左声道

  i2s_std_config_t stdCfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
      .slot_cfg = slotCfg,
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)config.bck_io,
              .ws = (gpio_num_t)config.ws_io,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)config.din_io,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(m_i2sRxHandle, &stdCfg));
  ESP_ERROR_CHECK(i2s_channel_enable(m_i2sRxHandle));

  ESP_LOGI(TAG, "I2S 初始化完成 (BCK:%d, WS:%d, DIN:%d)", config.bck_io,
           config.ws_io, config.din_io);

  return ESP_OK;
}

esp_err_t WakeWord::initAfe() {
  // 加载语音识别模型
  m_models = esp_srmodel_init("model");
  if (m_models == nullptr) {
    ESP_LOGE(TAG, "模型加载失败,请检查 model 分区");
    return ESP_FAIL;
  }

  // 使用 ESP-SR V2.0 的新 API
  // "M" = 单麦克风通道
  m_afeConfig = afe_config_init("M", m_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (m_afeConfig == nullptr) {
    ESP_LOGE(TAG, "AFE 配置初始化失败");
    return ESP_ERR_NO_MEM;
  }

  afe_config_print(m_afeConfig);

  // 获取唤醒词模型名称
  char *wnName = esp_srmodel_filter(m_models, ESP_WN_PREFIX, nullptr);
  if (wnName == nullptr) {
    ESP_LOGE(TAG, "未找到唤醒词模型,请通过 menuconfig 配置");
    afe_config_free(m_afeConfig);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "使用唤醒词模型: %s", wnName);

  // 创建 AFE handle
  m_afeHandle = esp_afe_handle_from_config(m_afeConfig);
  if (m_afeHandle == nullptr) {
    ESP_LOGE(TAG, "AFE handle 创建失败");
    afe_config_free(m_afeConfig);
    return ESP_ERR_NO_MEM;
  }

  // 创建 AFE 数据
  m_afeData = m_afeHandle->create_from_config(m_afeConfig);
  if (m_afeData == nullptr) {
    ESP_LOGE(TAG, "AFE 数据创建失败");
    afe_config_free(m_afeConfig);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "AFE 初始化完成");
  return ESP_OK;
}

esp_err_t WakeWord::initMultiNet() {
  // 获取 MultiNet 模型名称
  char *mnName = esp_srmodel_filter(m_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  if (mnName == nullptr) {
    ESP_LOGE(TAG, "未找到 MultiNet 模型,请通过 menuconfig 配置");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "使用 MultiNet 模型: %s", mnName);

  // 获取 MultiNet 接口
  m_mnHandle = esp_mn_handle_from_name(mnName);
  if (m_mnHandle == nullptr) {
    ESP_LOGE(TAG, "MultiNet handle 获取失败");
    return ESP_FAIL;
  }

  // 创建 MultiNet 数据
  m_mnData = m_mnHandle->create(mnName, m_cmdConfig.timeout_ms);
  if (m_mnData == nullptr) {
    ESP_LOGE(TAG, "MultiNet 数据创建失败");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "MultiNet 初始化完成");
  return ESP_OK;
}

esp_err_t WakeWord::registerCommands() {
  // 清除现有命令
  esp_mn_commands_clear();

  // 添加命令词
  for (int i = 0; i < NUM_COMMANDS; i++) {
    esp_err_t ret = esp_mn_commands_add(i, (char *)COMMANDS[i]);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "添加命令词失败: %s", COMMANDS[i]);
      return ret;
    }
    ESP_LOGI(TAG, "注册命令词 [%d]: %s (%s)", i, COMMAND_NAMES[i], COMMANDS[i]);
  }

  // 应用命令词更新
  esp_mn_error_t *errors = esp_mn_commands_update();
  if (errors != nullptr) {
    ESP_LOGE(TAG, "命令词更新失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "命令词注册完成，共 %d 个命令", NUM_COMMANDS);
  esp_mn_commands_print();

  return ESP_OK;
}

// ============= 公共 API =============

esp_err_t WakeWord::init(const I2sConfig &i2sConfig,
                         const CommandConfig &cmdConfig) {
  if (m_initialized) {
    ESP_LOGW(TAG, "已经初始化");
    return ESP_OK;
  }

  m_cmdConfig = cmdConfig;

  ESP_LOGI(TAG, "初始化唤醒词与命令识别模块...");

  esp_err_t ret = initI2s(i2sConfig);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2S 初始化失败");
    return ret;
  }

  ret = initAfe();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "AFE 初始化失败");
    return ret;
  }

  ret = initMultiNet();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "MultiNet 初始化失败");
    return ret;
  }

  ret = registerCommands();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "命令词注册失败");
    return ret;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "唤醒词与命令识别模块初始化完成");
  return ESP_OK;
}

esp_err_t WakeWord::start() {
  if (!m_initialized) {
    ESP_LOGE(TAG, "请先调用 init()");
    return ESP_FAIL;
  }

  if (m_running) {
    ESP_LOGW(TAG, "已在运行中");
    return ESP_OK;
  }

  m_running = true;
  m_state = WakeWordState::Running;

  // 创建音频采集任务
  BaseType_t ret = xTaskCreatePinnedToCore(audioFeedTask, "audio_feed", 4096,
                                           nullptr, 5, &m_feedTaskHandle, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "音频采集任务创建失败");
    m_running = false;
    m_state = WakeWordState::Idle;
    return ESP_FAIL;
  }

  // 创建唤醒词检测任务
  ret = xTaskCreatePinnedToCore(detectTask, "wake_detect", 8192, nullptr, 5,
                                &m_detectTaskHandle, 1);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "唤醒词检测任务创建失败");
    m_running = false;
    m_state = WakeWordState::Idle;
    vTaskDelete(m_feedTaskHandle);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "🚀 语音识别已启动，请说 \"小鹿，小鹿\" 唤醒");
  return ESP_OK;
}

esp_err_t WakeWord::stop() {
  if (!m_running) {
    return ESP_OK;
  }

  m_running = false;
  m_listeningCommand = false;
  m_state = WakeWordState::Idle;

  // 等待任务退出
  vTaskDelay(pdMS_TO_TICKS(100));

  ESP_LOGI(TAG, "唤醒词检测已停止");
  return ESP_OK;
}

void WakeWord::disable() {
  if (m_afeHandle && m_afeData) {
    m_afeHandle->disable_wakenet(m_afeData);
    ESP_LOGI(TAG, "唤醒词检测已禁用");
  }
}

void WakeWord::enable() {
  if (m_afeHandle && m_afeData) {
    m_afeHandle->enable_wakenet(m_afeData);
    ESP_LOGI(TAG, "唤醒词检测已启用");
  }
}

void WakeWord::deinit() {
  stop();

  if (m_mnHandle && m_mnData) {
    m_mnHandle->destroy(m_mnData);
    m_mnData = nullptr;
  }

  if (m_afeHandle && m_afeData) {
    m_afeHandle->destroy(m_afeData);
    m_afeData = nullptr;
  }

  if (m_afeConfig) {
    afe_config_free(m_afeConfig);
    m_afeConfig = nullptr;
  }

  if (m_i2sRxHandle) {
    i2s_channel_disable(m_i2sRxHandle);
    i2s_del_channel(m_i2sRxHandle);
    m_i2sRxHandle = nullptr;
  }

  m_initialized = false;
  ESP_LOGI(TAG, "唤醒词模块已释放");
}
