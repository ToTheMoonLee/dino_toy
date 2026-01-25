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
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

// 嵌入的 MP3 文件
extern const uint8_t mp3_start[] asm("_binary_dinosaur_roar_mp3_start");
extern const uint8_t mp3_end[] asm("_binary_dinosaur_roar_mp3_end");

static const char *TAG = "Mp3Player";

// 静态成员用于 I2S 写入回调
static i2s_chan_handle_t s_txHandle = nullptr;

// MAX98357 等 I2S 功放通常没有硬件静音脚；提供一个空实现避免 audio_player 解引用空函数指针
static esp_err_t muteNoopFn(AUDIO_PLAYER_MUTE_SETTING /*setting*/) {
  return ESP_OK;
}

// ============= 单例实现 =============

Mp3Player &Mp3Player::instance() {
  static Mp3Player instance;
  return instance;
}

// ============= 静态回调函数 =============

void Mp3Player::audioCallback(void *ctx) {
  auto *cbCtx = static_cast<audio_player_cb_ctx_t *>(ctx);
  auto &self = Mp3Player::instance();

  // While PCM streaming, ignore audio_player events to avoid state corruption
  // (audio_player_stop may still emit IDLE later).
  if (self.m_pcmTask != nullptr) {
    return;
  }

  switch (cbCtx->audio_event) {
  case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
    ESP_LOGI(TAG, "播放完成");
    self.m_state = Mp3PlayerState::Idle;

    // 如果开启循环播放，重新开始
    if (self.m_loopEnabled) {
      ESP_LOGI(TAG, "循环播放，重新开始...");
      self.startPlayback();
    } else {
      // 非循环：若播放的是动态 buffer，则在播放结束后释放
      self.freeActiveBuffer();
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
  // 防止 TX underflow 时输出旧数据/噪声（MAX98357 上会表现为“哒哒哒”）
  chanCfg.auto_clear = true;
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
      .mute_fn = muteNoopFn,
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

void Mp3Player::freeActiveBuffer() {
  if (m_source != Source::OwnedBuffer) {
    return;
  }
  if (m_activeBuf) {
    free(m_activeBuf);
  }
  m_activeBuf = nullptr;
  m_activeBufLen = 0;
  m_source = Source::EmbeddedMp3;
}

void Mp3Player::waitForIdle(uint32_t timeout_ms) {
  if (m_state == Mp3PlayerState::Idle) {
    return;
  }
  TickType_t start = xTaskGetTickCount();
  TickType_t timeoutTicks = pdMS_TO_TICKS(timeout_ms);
  while (m_state != Mp3PlayerState::Idle) {
    vTaskDelay(pdMS_TO_TICKS(20));
    if ((xTaskGetTickCount() - start) > timeoutTicks) {
      break;
    }
  }
}

esp_err_t Mp3Player::startPlayback() {
  const void *data = nullptr;
  size_t size = 0;

  switch (m_source) {
  case Source::EmbeddedMp3:
    data = mp3_start;
    size = (size_t)(mp3_end - mp3_start);
    break;
  case Source::OwnedBuffer:
    data = m_activeBuf;
    size = m_activeBufLen;
    break;
  }

  if (data == nullptr || size == 0) {
    ESP_LOGE(TAG, "no audio data to play");
    return ESP_ERR_INVALID_STATE;
  }

  // 使用 fmemopen 创建内存文件
  FILE *fp = fmemopen((void *)data, size, "rb");
  if (fp == nullptr) {
    ESP_LOGE(TAG, "无法打开音频数据");
    return ESP_FAIL;
  }

  esp_err_t ret = audio_player_play(fp);
  if (ret != ESP_OK) {
    // audio_player_play 文档说明：非 ESP_OK 时由调用方关闭 fp
    fclose(fp);
  }
  return ret;
}

esp_err_t Mp3Player::playEmbedded(bool loop) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "请先调用 init()");
    return ESP_ERR_INVALID_STATE;
  }

  // Stop PCM stream if active (and wait it to end).
  stopPcmStreamInternal(true);

  // 确保先停止当前播放，等待进入 IDLE（避免上一次 IDLE 回调释放到新的 buffer）
  if (m_state != Mp3PlayerState::Idle) {
    stop();
    waitForIdle(3000);
    if (m_state != Mp3PlayerState::Idle) {
      ESP_LOGW(TAG, "stop timeout, skip playEmbedded");
      return ESP_ERR_TIMEOUT;
    }
  }
  m_source = Source::EmbeddedMp3;
  m_loopEnabled = loop;

  esp_err_t ret = startPlayback();
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "开始播放 (循环=%d)", loop);
  }

  return ret;
}

esp_err_t Mp3Player::playOwnedBuffer(uint8_t *data, size_t len, bool loop) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "请先调用 init()");
    if (data) {
      free(data);
    }
    return ESP_ERR_INVALID_STATE;
  }
  if (data == nullptr || len == 0) {
    if (data) {
      free(data);
    }
    return ESP_ERR_INVALID_ARG;
  }

  // Stop PCM stream if active (and wait it to end).
  stopPcmStreamInternal(true);

  // 停止当前播放，等待进入 IDLE（确保上一次 IDLE 回调已完成）
  if (m_state != Mp3PlayerState::Idle) {
    stop();
    waitForIdle(3000);
    if (m_state != Mp3PlayerState::Idle) {
      ESP_LOGW(TAG, "stop timeout, drop new audio buffer");
      free(data);
      return ESP_ERR_TIMEOUT;
    }
  }

  m_source = Source::OwnedBuffer;
  m_activeBuf = data;
  m_activeBufLen = len;
  m_loopEnabled = loop;

  esp_err_t ret = startPlayback();
  if (ret != ESP_OK) {
    freeActiveBuffer();
    return ret;
  }
  ESP_LOGI(TAG, "开始播放内存音频 (len=%u, 循环=%d)", (unsigned)len, loop);
  return ESP_OK;
}

void Mp3Player::pcmStreamTask(void *arg) {
  auto *self = static_cast<Mp3Player *>(arg);
  if (!self) {
    vTaskDelete(nullptr);
    return;
  }

  // Small prebuffer to reduce underflow/clicking on LAN jitter.
  TickType_t start = xTaskGetTickCount();
  while (!self->m_pcmStop && self->m_pcmStream &&
         xStreamBufferBytesAvailable(self->m_pcmStream) <
             self->m_pcmPrebufferBytes) {
    vTaskDelay(pdMS_TO_TICKS(10));
    // Don't wait forever; start anyway after ~2s
    if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(2000)) {
      break;
    }
  }

  static constexpr size_t kInChunkBytes = 1024; // mono bytes
  static constexpr size_t kOutChunkBytes = kInChunkBytes * 2; // stereo bytes
  uint8_t *inBuf = (uint8_t *)malloc(kInChunkBytes);
  uint8_t *outBuf = (uint8_t *)malloc(kOutChunkBytes);
  if (!inBuf || !outBuf) {
    ESP_LOGE(TAG, "pcm stream malloc failed");
  }

  while (true) {
    if (!inBuf || !outBuf) {
      break;
    }
    if (!self->m_pcmStream) {
      break;
    }
    size_t avail = xStreamBufferBytesAvailable(self->m_pcmStream);
    if (self->m_pcmStop && avail == 0) {
      break;
    }

    size_t got = xStreamBufferReceive(self->m_pcmStream, inBuf, kInChunkBytes,
                                      pdMS_TO_TICKS(100));
    if (got == 0) {
      continue;
    }
    if (got % 2 == 1) {
      got -= 1;
    }
    if (got == 0) {
      continue;
    }

    // mono S16LE -> stereo S16LE (duplicate samples)
    int samples = (int)(got / 2);
    auto *in = reinterpret_cast<const int16_t *>(inBuf);
    auto *out = reinterpret_cast<int16_t *>(outBuf);
    for (int i = 0; i < samples; i++) {
      int16_t s = in[i];
      out[i * 2] = s;
      out[i * 2 + 1] = s;
    }

    size_t outBytes = (size_t)samples * 2 * sizeof(int16_t);
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_txHandle, outBuf, outBytes, &written,
                                      pdMS_TO_TICKS(2000));
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "pcm i2s write failed: %s", esp_err_to_name(err));
      // Keep draining so we can exit cleanly.
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // Cleanup stream resources
  if (self->m_pcmStream) {
    vStreamBufferDelete(self->m_pcmStream);
    self->m_pcmStream = nullptr;
  }
  self->m_pcmStop = false;

  self->m_state = Mp3PlayerState::Idle;
  if (self->m_callback) {
    self->m_callback(self->m_state);
  }

  self->m_pcmTask = nullptr;
  if (inBuf) {
    free(inBuf);
  }
  if (outBuf) {
    free(outBuf);
  }
  ESP_LOGI(TAG, "PCM stream finished");
  vTaskDelete(nullptr);
}

void Mp3Player::stopPcmStreamInternal(bool waitIdle) {
  if (!m_pcmTask) {
    if (m_pcmStream) {
      vStreamBufferDelete(m_pcmStream);
      m_pcmStream = nullptr;
    }
    m_pcmStop = false;
    return;
  }

  m_pcmStop = true;
  if (waitIdle) {
    waitForIdle(3000);
  }
}

esp_err_t Mp3Player::pcmStreamBegin(uint32_t sample_rate_hz,
                                   uint32_t prebuffer_ms) {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (sample_rate_hz == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // Stop any existing stream first
  stopPcmStreamInternal(true);

  // Stop current audio_player playback
  if (m_state != Mp3PlayerState::Idle) {
    stop();
    waitForIdle(3000);
    if (m_state != Mp3PlayerState::Idle) {
      ESP_LOGW(TAG, "stop timeout, skip pcmStreamBegin");
      return ESP_ERR_TIMEOUT;
    }
  }

  // Release owned buffer (if any) now; streaming doesn't need it.
  m_loopEnabled = false;
  freeActiveBuffer();

  // Reconfigure I2S clock to match PCM stream
  (void)clkSetFn(sample_rate_hz, (uint32_t)I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO);

  // Create stream buffer (~1s for 16k mono; scale with sample rate)
  size_t bufBytes = std::max((size_t)32 * 1024,
                             (size_t)(sample_rate_hz * 2)); // mono bytes/sec
  bufBytes = std::min(bufBytes, (size_t)96 * 1024);
  m_pcmStream = xStreamBufferCreate(bufBytes, 1);
  if (!m_pcmStream) {
    ESP_LOGE(TAG, "pcm stream buffer alloc failed");
    return ESP_ERR_NO_MEM;
  }

  m_pcmSampleRate = sample_rate_hz;
  m_pcmPrebufferBytes =
      (size_t)((uint64_t)sample_rate_hz * 2 * (uint64_t)prebuffer_ms / 1000);
  if (m_pcmPrebufferBytes > bufBytes) {
    m_pcmPrebufferBytes = bufBytes / 2;
  }
  m_pcmStop = false;

  // Mark as playing so other modules can mute/ignore mic during streaming
  m_state = Mp3PlayerState::Playing;
  if (m_callback) {
    m_callback(m_state);
  }

  BaseType_t ok = xTaskCreatePinnedToCore(pcmStreamTask, "pcm_stream", 6144,
                                         this, 5, &m_pcmTask, 1);
  if (ok != pdPASS) {
    vStreamBufferDelete(m_pcmStream);
    m_pcmStream = nullptr;
    m_state = Mp3PlayerState::Idle;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "PCM stream begin: rate=%lu, prebuffer=%lu ms, buf=%u",
           (unsigned long)sample_rate_hz, (unsigned long)prebuffer_ms,
           (unsigned)bufBytes);
  return ESP_OK;
}

esp_err_t Mp3Player::pcmStreamWrite(const uint8_t *data, size_t len,
                                   uint32_t timeout_ms) {
  if (!m_initialized || !m_pcmStream || !m_pcmTask) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!data || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  TickType_t timeoutTicks = pdMS_TO_TICKS(timeout_ms);
  size_t sentTotal = 0;
  while (sentTotal < len) {
    size_t sent =
        xStreamBufferSend(m_pcmStream, data + sentTotal, len - sentTotal,
                          timeoutTicks);
    if (sent == 0) {
      return ESP_ERR_TIMEOUT;
    }
    sentTotal += sent;
  }
  return ESP_OK;
}

esp_err_t Mp3Player::pcmStreamEnd() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!m_pcmTask) {
    return ESP_OK;
  }
  m_pcmStop = true;
  return ESP_OK;
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
  // Also stop any active PCM stream. Keep it non-blocking to match existing
  // stop() behavior; callers that require synchronization can waitForIdle().
  stopPcmStreamInternal(false);
  return audio_player_stop();
}

void Mp3Player::deinit() {
  if (!m_initialized) {
    return;
  }

  stopPcmStreamInternal(true);
  stop();
  waitForIdle(3000);
  freeActiveBuffer();
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
