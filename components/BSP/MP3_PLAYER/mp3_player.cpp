/**
 * @file mp3_player.cpp
 * @brief MP3 播放器实现
 *
 * 基于 esp-audio-player 组件实现的 MP3 播放器
 * 支持嵌入的 MP3 文件播放和循环播放
 */

#include "mp3_player.h"

#include "audio_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

// 嵌入的 MP3 文件
extern const uint8_t mp3_start[] asm("_binary_dinosaur_roar_mp3_start");
extern const uint8_t mp3_end[] asm("_binary_dinosaur_roar_mp3_end");

static const char *TAG = "Mp3Player";

// 静态成员用于 I2S 写入回调
static i2s_chan_handle_t s_txHandle = nullptr;

// ============= 单例实现 =============

Mp3Player &Mp3Player::instance() {
  static Mp3Player instance;
  return instance;
}

// ============= 静态回调函数 =============

void Mp3Player::audioCallback(void *ctx) {
  auto *cbCtx = static_cast<audio_player_cb_ctx_t *>(ctx);
  auto &self = Mp3Player::instance();

  switch (cbCtx->audio_event) {
  case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
    ESP_LOGI(TAG, "播放完成");
    self.m_state = Mp3PlayerState::Idle;

    // 如果开启循环播放，重新开始
    if (self.m_loopEnabled) {
      ESP_LOGI(TAG, "循环播放，重新开始...");
      self.startPlayback();
    }
    break;

  case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
  case AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT:
    ESP_LOGI(TAG, "正在播放");
    self.m_state = Mp3PlayerState::Playing;
    break;

  case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
    ESP_LOGI(TAG, "已暂停");
    self.m_state = Mp3PlayerState::Paused;
    break;

  default:
    break;
  }

  // 调用用户回调
  if (self.m_callback) {
    self.m_callback(self.m_state);
  }
}

esp_err_t Mp3Player::i2sWrite(void *audio_buffer, size_t len,
                              size_t *bytes_written, uint32_t timeout_ms) {
  if (s_txHandle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  return i2s_channel_write(s_txHandle, audio_buffer, len, bytes_written,
                           pdMS_TO_TICKS(timeout_ms));
}

esp_err_t Mp3Player::clkSetFn(uint32_t rate, uint32_t bits_cfg,
                              i2s_slot_mode_t ch) {
  if (s_txHandle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  // 需要先禁用通道才能重新配置
  ESP_ERROR_CHECK(i2s_channel_disable(s_txHandle));

  // 更新时钟配置
  i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_txHandle, &clkCfg));

  // 更新槽位配置
  i2s_std_slot_config_t slotCfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
      static_cast<i2s_data_bit_width_t>(bits_cfg), ch);
  ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(s_txHandle, &slotCfg));

  // 重新启用通道
  ESP_ERROR_CHECK(i2s_channel_enable(s_txHandle));

  ESP_LOGI(TAG, "I2S 时钟配置更新: rate=%lu, bits=%lu, ch=%d", rate, bits_cfg,
           ch);
  return ESP_OK;
}

// ============= I2S 初始化 =============

esp_err_t Mp3Player::initI2s(const Mp3I2sConfig &config) {
  // I2S 通道配置
  i2s_chan_config_t chanCfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, &m_txHandle, nullptr));

  // I2S 标准模式配置
  i2s_std_config_t stdCfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                  I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = config.bck_io,
              .ws = config.ws_io,
              .dout = config.dout_io,
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(m_txHandle, &stdCfg));
  ESP_ERROR_CHECK(i2s_channel_enable(m_txHandle));

  // 保存到静态变量供回调使用
  s_txHandle = m_txHandle;

  ESP_LOGI(TAG, "I2S 初始化完成 (BCK:%d, WS:%d, DOUT:%d)", config.bck_io,
           config.ws_io, config.dout_io);

  return ESP_OK;
}

// ============= 公共 API =============

esp_err_t Mp3Player::init(const Mp3I2sConfig &config) {
  if (m_initialized) {
    ESP_LOGW(TAG, "已经初始化");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "初始化 MP3 播放器...");

  // 初始化 I2S
  esp_err_t ret = initI2s(config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2S 初始化失败");
    return ret;
  }

  // 配置 audio_player
  audio_player_config_t playerCfg = {
      .mute_fn = nullptr,
      .clk_set_fn = clkSetFn,
      .write_fn = i2sWrite,
      .priority = 5,
      .coreID = 1,
  };

  ret = audio_player_new(playerCfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "audio_player 初始化失败");
    return ret;
  }

  // 注册回调
  ret = audio_player_callback_register(
      [](audio_player_cb_ctx_t *ctx) { audioCallback(ctx); }, nullptr);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "回调注册失败");
    return ret;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "MP3 播放器初始化完成");
  return ESP_OK;
}

esp_err_t Mp3Player::startPlayback() {
  // 使用 fmemopen 创建内存文件
  size_t mp3Size = mp3_end - mp3_start;
  FILE *fp = fmemopen((void *)mp3_start, mp3Size, "rb");
  if (fp == nullptr) {
    ESP_LOGE(TAG, "无法打开嵌入的 MP3 数据");
    return ESP_FAIL;
  }

  return audio_player_play(fp);
}

esp_err_t Mp3Player::playEmbedded(bool loop) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "请先调用 init()");
    return ESP_ERR_INVALID_STATE;
  }

  m_loopEnabled = loop;

  esp_err_t ret = startPlayback();
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "开始播放 (循环=%d)", loop);
  }

  return ret;
}

esp_err_t Mp3Player::pause() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return audio_player_pause();
}

esp_err_t Mp3Player::resume() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return audio_player_resume();
}

esp_err_t Mp3Player::stop() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  m_loopEnabled = false;
  return audio_player_stop();
}

void Mp3Player::deinit() {
  if (!m_initialized) {
    return;
  }

  stop();
  audio_player_delete();

  if (m_txHandle) {
    i2s_channel_disable(m_txHandle);
    i2s_del_channel(m_txHandle);
    m_txHandle = nullptr;
    s_txHandle = nullptr;
  }

  m_initialized = false;
  ESP_LOGI(TAG, "MP3 播放器已释放");
}
