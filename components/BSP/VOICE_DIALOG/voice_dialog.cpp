#include "voice_dialog.h"

#include "cloud_chat.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mp3_player.h"
#include "wake_word.h"
#include "websocket_chat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "VoiceDialog";

namespace {
static std::string getDeviceIdFromMac() {
  uint8_t mac[6] = {};
  esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  }
  if (err != ESP_OK) {
    return "esp32-unknown";
  }
  char buf[20];
  snprintf(buf, sizeof(buf), "esp32-%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

static uint32_t meanAbs16(const int16_t *samples, int numSamples) {
  if (samples == nullptr || numSamples <= 0) {
    return 0;
  }
  uint32_t sum = 0;
  for (int i = 0; i < numSamples; i++) {
    int32_t s = samples[i];
    int32_t a = s < 0 ? -s : s;
    if (a > 32767) {
      a = 32767; // handle INT16_MIN
    }
    sum += (uint32_t)a;
  }
  return sum / (uint32_t)numSamples;
}

static uint8_t *buildWav16Mono(const int16_t *pcm, size_t samples,
                               int sampleRate, size_t &outLen) {
  outLen = 0;
  if (pcm == nullptr || samples == 0 || sampleRate <= 0) {
    return nullptr;
  }

  const uint16_t numChannels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate =
      (uint32_t)sampleRate * numChannels * (bitsPerSample / 8);
  const uint16_t blockAlign = numChannels * (bitsPerSample / 8);

  const uint32_t dataBytes = (uint32_t)(samples * sizeof(int16_t));
  const uint32_t riffSize = 36 + dataBytes;

  struct WavHeader {
    char riff[4];
    uint32_t riffSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
  } __attribute__((packed));

  WavHeader h{};
  memcpy(h.riff, "RIFF", 4);
  h.riffSize = riffSize;
  memcpy(h.wave, "WAVE", 4);
  memcpy(h.fmt, "fmt ", 4);
  h.fmtSize = 16;
  h.audioFormat = 1; // PCM
  h.numChannels = numChannels;
  h.sampleRate = (uint32_t)sampleRate;
  h.byteRate = byteRate;
  h.blockAlign = blockAlign;
  h.bitsPerSample = bitsPerSample;
  memcpy(h.data, "data", 4);
  h.dataSize = dataBytes;

  size_t total = sizeof(WavHeader) + dataBytes;
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) {
    return nullptr;
  }
  memcpy(buf, &h, sizeof(WavHeader));
  memcpy(buf + sizeof(WavHeader), pcm, dataBytes);
  outLen = total;
  return buf;
}
} // namespace

esp_err_t VoiceDialog::init(const VoiceDialogConfig &cfg) {
  if (m_inited) {
    return ESP_OK;
  }
  m_cfg = cfg;
  m_deviceId = getDeviceIdFromMac();
  ESP_LOGI(TAG, "Init: deviceId=%s use_websocket=%d", m_deviceId.c_str(),
           m_cfg.use_websocket ? 1 : 0);

  // Pre-reserve capture buffer
  int capMs = std::max(1000, m_cfg.max_pcm_ms);
  size_t capSamples = (size_t)((int64_t)capMs * m_cfg.sample_rate_hz / 1000);
  m_pcm.reserve(capSamples);
  if (m_cfg.use_websocket) {
    constexpr int kPreRollMs = 200;
    size_t preRollSamples =
        (size_t)((int64_t)kPreRollMs * m_cfg.sample_rate_hz / 1000);
    m_wsPreRoll.reserve(preRollSamples);
  }

  // WebSocket mode: init WebSocket client
  if (m_cfg.use_websocket) {
    initWebSocket();
  }

  m_queue = xQueueCreate(4, sizeof(UtteranceEvent));
  if (!m_queue) {
    ESP_LOGE(TAG, "Failed to create queue");
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ok =
      xTaskCreatePinnedToCore(workerTask, "voice_dialog", m_cfg.worker_stack,
                              this, m_cfg.worker_prio, &m_task,
                              m_cfg.worker_core);
  if (ok != pdPASS) {
    vQueueDelete(m_queue);
    m_queue = nullptr;
    ESP_LOGE(TAG, "Failed to create worker task");
    return ESP_FAIL;
  }

  m_inited = true;
  return ESP_OK;
}

void VoiceDialog::onWakeDetected() {
  if (!m_inited) {
    return;
  }
  m_sessionActive = true;
  m_turnBusy.store(false, std::memory_order_relaxed);
  m_ignoreUntilTick = 0;
  resetCapture();
  m_wsPreRoll.clear();
  m_wsLastConnectAttemptTick = 0;
  m_wsTurnBusySinceTick = 0;
  m_wsStopListenTick = 0;

  if (m_queue) {
    UtteranceEvent ev{};
    while (xQueueReceive(m_queue, &ev, 0) == pdTRUE) {
      if (ev.pcm) {
        free(ev.pcm);
      }
    }
  }
  
  // WebSocket mode: connect and start listening
  if (m_cfg.use_websocket) {
    auto& ws = WebSocketChat::instance();
    if (!ws.isReady()) {
      ws.connect();
    }
    m_wsListening = false;
  }
  
  WakeWord::instance().touchDialog();
  ESP_LOGI(TAG, "Session started (websocket=%d)", m_cfg.use_websocket ? 1 : 0);
}

void VoiceDialog::onLocalCommandDetected() {
  // 本地命令被识别：丢弃当前录音，避免同时走云端对话
  m_turnBusy.store(false, std::memory_order_relaxed);
  resetCapture();
  m_wsPreRoll.clear();
  int ignoreMs = std::max(0, m_cfg.local_command_ignore_ms);
  if (ignoreMs > 0) {
    m_ignoreUntilTick =
        (uint32_t)xTaskGetTickCount() + (uint32_t)pdMS_TO_TICKS(ignoreMs);
  } else {
    m_ignoreUntilTick = 0;
  }

  // 尽量清掉队列里尚未处理的语音，避免“命令也执行了、云端也回了一句”
  if (m_queue) {
    UtteranceEvent ev{};
    while (xQueueReceive(m_queue, &ev, 0) == pdTRUE) {
      if (ev.pcm) {
        free(ev.pcm);
      }
    }
  }

  // WebSocket mode: abort current interaction as early as possible.
  if (m_cfg.use_websocket) {
    auto &ws = WebSocketChat::instance();
    if (ws.isReady()) {
      (void)ws.sendAbort();
      (void)ws.stopListening();
    }
    m_wsListening = false;
  }

  WakeWord::instance().touchDialog();
  ESP_LOGI(TAG, "Local command detected, cancel current utterance");
}

void VoiceDialog::resetCapture() {
  m_inSpeech = false;
  m_dropCurrentUtterance = false;
  m_speechMs = 0;
  m_silenceMs = 0;
  m_frameMs = 0;
  m_pcm.clear();
  m_wsPreRoll.clear();
}

bool VoiceDialog::shouldFinalizeOnSilence() const {
  if (!m_inSpeech) {
    return false;
  }
  if (m_speechMs < m_cfg.min_speech_ms) {
    return false;
  }
  return m_silenceMs >= m_cfg.end_silence_ms;
}

void VoiceDialog::onAudioFrame(const int16_t *samples, int numSamples,
                               vad_state_t vad) {
  if (!m_inited || !m_sessionActive) {
    return;
  }
  
  // WebSocket mode: use streaming
  if (m_cfg.use_websocket) {
    handleWsAudioFrame(samples, numSamples, vad);
    return;
  }

  // After local command, ignore a short window to avoid uploading the command
  // utterance to cloud chat.
  if (m_ignoreUntilTick != 0) {
    uint32_t now = (uint32_t)xTaskGetTickCount();
    if ((int32_t)(now - m_ignoreUntilTick) < 0) {
      WakeWord::instance().touchDialog();
      return;
    }
    m_ignoreUntilTick = 0;
  }

  // Single-turn policy: while waiting assistant reply, ignore new speech
  if (m_turnBusy.load(std::memory_order_relaxed)) {
    WakeWord::instance().touchDialog();
    return;
  }

  // 播放器正在播报时：不采集，避免回声把自己“听进去”
  if (Mp3Player::instance().getState() != Mp3PlayerState::Idle) {
    WakeWord::instance().touchDialog(); // keep alive during long response
    return;
  }

  if (samples == nullptr || numSamples <= 0) {
    return;
  }

  if (m_frameMs <= 0) {
    m_frameMs = std::max(1, (numSamples * 1000) / m_cfg.sample_rate_hz);
  }

  uint32_t meanAbs = meanAbs16(samples, numSamples);
  bool speechFrame = (vad == VAD_SPEECH);
  if (m_cfg.energy_gate_mean_abs > 0 &&
      meanAbs < (uint32_t)m_cfg.energy_gate_mean_abs) {
    speechFrame = false;
  }

  if (speechFrame) {
    if (!m_inSpeech) {
      m_inSpeech = true;
      m_dropCurrentUtterance = false;
      m_speechMs = 0;
      m_silenceMs = 0;
      m_pcm.clear();
      ESP_LOGI(TAG, "Speech start (vad=%d meanAbs=%u gate=%d)", (int)vad,
               (unsigned)meanAbs, m_cfg.energy_gate_mean_abs);
    }
    m_speechMs += m_frameMs;
    WakeWord::instance().touchDialog();
  } else {
    if (m_inSpeech) {
      m_silenceMs += m_frameMs;
    }
  }

  if (!m_inSpeech) {
    return;
  }

  // Append frame (include trailing silence for STT robustness)
  size_t oldSize = m_pcm.size();
  m_pcm.resize(oldSize + (size_t)numSamples);
  memcpy(m_pcm.data() + oldSize, samples, (size_t)numSamples * sizeof(int16_t));

  // Hard cap
  bool forcedFinalize = false;
  int totalMs = m_speechMs + m_silenceMs;
  if (totalMs > m_cfg.max_utterance_ms || totalMs > m_cfg.max_pcm_ms) {
    m_silenceMs = m_cfg.end_silence_ms;
    forcedFinalize = true;
  }

  if (!shouldFinalizeOnSilence()) {
    return;
  }

  // finalize utterance
  if (m_dropCurrentUtterance) {
    resetCapture();
    return;
  }

  // Trim excessive tail silence to reduce upload size/latency.
  // Only do this when we truly ended on silence (not forced by hard cap),
  // otherwise we may accidentally cut off real speech.
  if (!forcedFinalize) {
    constexpr int kKeepTailSilenceMs = 200;
    if (m_silenceMs > kKeepTailSilenceMs) {
      int trimMs = m_silenceMs - kKeepTailSilenceMs;
      size_t trimSamples =
          (size_t)((int64_t)trimMs * m_cfg.sample_rate_hz / 1000);
      if (trimSamples > 0 && trimSamples < m_pcm.size()) {
        m_pcm.resize(m_pcm.size() - trimSamples);
      }
    }
  }

  size_t totalSamples = m_pcm.size();
  if (totalSamples == 0) {
    resetCapture();
    return;
  }

  ESP_LOGI(
      TAG, "Utterance finalize: speech=%dms silence=%dms samples=%u forced=%d",
      m_speechMs, m_silenceMs, (unsigned)totalSamples, forcedFinalize ? 1 : 0);

  int16_t *pcmCopy =
      (int16_t *)malloc(totalSamples * sizeof(int16_t));
  if (!pcmCopy) {
    ESP_LOGW(TAG, "No mem for pcm copy");
    resetCapture();
    return;
  }
  memcpy(pcmCopy, m_pcm.data(), totalSamples * sizeof(int16_t));

  UtteranceEvent ev = {
      .pcm = pcmCopy,
      .samples = totalSamples,
      .sample_rate_hz = m_cfg.sample_rate_hz,
  };

  if (xQueueSend(m_queue, &ev, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Queue full, drop utterance");
    free(pcmCopy);
  } else {
    // Now waiting assistant reply; pause listening until it finishes
    m_turnBusy.store(true, std::memory_order_relaxed);
  }

  resetCapture();
}

void VoiceDialog::tick() {
  if (!m_inited || !m_sessionActive) {
    return;
  }

  // WebSocket mode: best-effort reconnect + watchdog to avoid being stuck after
  // a missing "tts stop" / disconnect.
  if (m_cfg.use_websocket) {
    auto &ws = WebSocketChat::instance();
    if (!ws.isReady()) {
      uint32_t now = (uint32_t)xTaskGetTickCount();
      constexpr uint32_t kReconnectIntervalMs = 3000;
      if (m_wsLastConnectAttemptTick == 0 ||
          (uint32_t)((now - m_wsLastConnectAttemptTick) * portTICK_PERIOD_MS) >=
              kReconnectIntervalMs) {
        m_wsLastConnectAttemptTick = now;
        ESP_LOGI(TAG, "WS not ready, reconnecting...");
        (void)ws.connect();
      }
    } else {
      m_wsLastConnectAttemptTick = 0;
    }

    if (m_turnBusy.load(std::memory_order_relaxed)) {
      if (m_wsTurnBusySinceTick == 0) {
        m_wsTurnBusySinceTick = (uint32_t)xTaskGetTickCount();
      } else {
        uint32_t now = (uint32_t)xTaskGetTickCount();
        uint32_t diffTicks =
            (now >= m_wsTurnBusySinceTick) ? (now - m_wsTurnBusySinceTick) : 0;
        uint32_t elapsedMs = diffTicks * portTICK_PERIOD_MS;
        
        // STT timeout: if waiting for response and no TTS started for 10s, reset
        constexpr uint32_t kSttTimeoutMs = 10000;
        if (ws.getState() == WsDialogState::WaitingForResponse && elapsedMs > kSttTimeoutMs) {
          ESP_LOGW(TAG, "WS STT timeout (%u ms), reset to allow new listening",
                   (unsigned)elapsedMs);
          m_turnBusy.store(false, std::memory_order_relaxed);
          m_wsTurnBusySinceTick = 0;
          m_wsStopListenTick = 0;
          m_wsListening = false;
          resetCapture();
        }
        // Global busy timeout (reduced from 30s to 20s)
        constexpr uint32_t kBusyTimeoutMs = 20000;
        if (elapsedMs > kBusyTimeoutMs) {
          ESP_LOGW(TAG, "WS busy timeout (%u ms), reset dialog turn",
                   (unsigned)elapsedMs);
          if (ws.isReady()) {
            (void)ws.sendAbort();
          }
          m_turnBusy.store(false, std::memory_order_relaxed);
          m_wsTurnBusySinceTick = 0;
          m_wsStopListenTick = 0;
          m_wsListening = false;
          resetCapture();
        }
      }
    } else {
      m_wsTurnBusySinceTick = 0;
      m_wsStopListenTick = 0;
    }
  }

  // WakeWord 已退出对话模式 -> 清理本地状态
  if (WakeWord::instance().getState() != WakeWordState::Dialog) {
    m_sessionActive = false;
    m_turnBusy.store(false, std::memory_order_relaxed);
    m_wsTurnBusySinceTick = 0;
    resetCapture();
  }
}

void VoiceDialog::workerTask(void *arg) {
  auto *self = static_cast<VoiceDialog *>(arg);
  if (!self || !self->m_queue) {
    vTaskDelete(nullptr);
    return;
  }

  UtteranceEvent ev{};
  while (true) {
    if (xQueueReceive(self->m_queue, &ev, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    self->handleUtterance(ev);
    if (ev.pcm) {
      free(ev.pcm);
    }
  }
}

void VoiceDialog::handleUtterance(const UtteranceEvent &ev) {
  if (m_cfg.chat_url.empty()) {
    ESP_LOGW(TAG, "chat_url empty, skip");
    m_turnBusy.store(false, std::memory_order_relaxed);
    return;
  }

  size_t wavLen = 0;
  uint8_t *wav = buildWav16Mono(ev.pcm, ev.samples, ev.sample_rate_hz, wavLen);
  if (!wav || wavLen == 0) {
    ESP_LOGW(TAG, "build wav failed");
    if (wav) {
      free(wav);
    }
    m_turnBusy.store(false, std::memory_order_relaxed);
    return;
  }

  ESP_LOGI(TAG, "Upload wav: bytes=%u", (unsigned)wavLen);

  auto &chat = CloudChat::instance();
  if (!chat.isInitialized()) {
    chat.init({
        .url = m_cfg.chat_url,
        .timeout_ms = 60000,
        .max_response_bytes = 2 * 1024 * 1024,
    });
  } else {
    chat.setUrl(m_cfg.chat_url);
  }

  WakeWord::instance().touchDialog();
  esp_err_t err = ESP_OK;
  if (m_cfg.use_pcm_stream) {
    err = chat.chatWavPcmStream(wav, wavLen, m_deviceId);
  } else {
    err = chat.chatWav(wav, wavLen, m_deviceId);
  }
  free(wav);

  // keep dialog alive while assistant is speaking
  if (err == ESP_OK) {
    while (Mp3Player::instance().getState() != Mp3PlayerState::Idle) {
      WakeWord::instance().touchDialog();
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  } else {
    ESP_LOGW(TAG, "chat failed: %s", esp_err_to_name(err));
  }

  m_turnBusy.store(false, std::memory_order_relaxed);
}

void VoiceDialog::initWebSocket() {
  if (m_wsInited) {
    return;
  }
  
  auto& ws = WebSocketChat::instance();
  
  WebSocketChatConfig ws_cfg;
  ws_cfg.url = m_cfg.ws_url;
  ws_cfg.device_id = m_deviceId;
  ws_cfg.sample_rate = m_cfg.sample_rate_hz;
  
  esp_err_t err = ws.init(ws_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init WebSocket: %s", esp_err_to_name(err));
    return;
  }
  
  // STT 回调：识别结果
  ws.setOnStt([this](const std::string& text) {
    ESP_LOGI(TAG, "WS STT: %s", text.c_str());
    // 可以在这里显示文本等
  });
  
  // TTS 状态回调
  ws.setOnTtsState([this](bool started) {
    auto &ws = WebSocketChat::instance();
    auto& player = Mp3Player::instance();
    if (started) {
      ESP_LOGI(TAG, "WS TTS started");
      m_turnBusy.store(true, std::memory_order_relaxed);
      if (m_wsTurnBusySinceTick == 0) {
        m_wsTurnBusySinceTick = (uint32_t)xTaskGetTickCount();
      }

      // 开始 PCM 流播放（S16LE mono）
      int sr = ws.serverSampleRate();
      if (sr < 8000 || sr > 48000) {
        sr = m_cfg.sample_rate_hz;
      }
      esp_err_t err = player.pcmStreamBegin((uint32_t)sr, 100);  // 100ms prebuffer
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PCM stream: %s", esp_err_to_name(err));
        // Avoid being stuck in busy state if we cannot play audio.
        m_turnBusy.store(false, std::memory_order_relaxed);
        m_wsTurnBusySinceTick = 0;
      }
    } else {
      ESP_LOGI(TAG, "WS TTS stopped");
      // 结束 PCM 流
      player.pcmStreamEnd();
      // Full state reset to prepare for next turn
      m_turnBusy.store(false, std::memory_order_relaxed);
      m_wsTurnBusySinceTick = 0;
      m_wsStopListenTick = 0;
      m_wsListening = false;
      // Reset capture state for next utterance
      resetCapture();
    }
  });
  
  // TTS 音频回调：播放收到的 PCM
  ws.setOnTtsAudio([this](const uint8_t* data, size_t len) {
    auto& player = Mp3Player::instance();
    // 写入 PCM 数据到播放流
    esp_err_t err = player.pcmStreamWrite(data, len, 500);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "PCM write failed: %s (len=%u)", esp_err_to_name(err), (unsigned)len);
    }
    WakeWord::instance().touchDialog();
  });
  
  // 连接状态回调
  ws.setOnConnection([this](bool connected) {
    if (connected) {
      ESP_LOGI(TAG, "WS connected");
    } else {
      ESP_LOGI(TAG, "WS disconnected");
      m_wsListening = false;
      m_turnBusy.store(false, std::memory_order_relaxed);
      m_wsTurnBusySinceTick = 0;
      resetCapture();
      // 如果断开时正在播放，结束 PCM 流
      auto& player = Mp3Player::instance();
      if (player.getState() == Mp3PlayerState::Playing) {
        player.pcmStreamEnd();
      }
    }
  });
  
  m_wsInited = true;
  ESP_LOGI(TAG, "WebSocket initialized: %s", ws_cfg.url.c_str());
}

void VoiceDialog::handleWsAudioFrame(const int16_t *samples, int numSamples,
                                      vad_state_t vad) {
  if (!m_inited || !m_sessionActive || !m_cfg.use_websocket) {
    return;
  }
  
  auto& ws = WebSocketChat::instance();

  // After local command, ignore a short window to avoid uploading the command
  // utterance to cloud chat.
  if (m_ignoreUntilTick != 0) {
    uint32_t now = (uint32_t)xTaskGetTickCount();
    if ((int32_t)(now - m_ignoreUntilTick) < 0) {
      WakeWord::instance().touchDialog();
      return;
    }
    m_ignoreUntilTick = 0;
  }

  // One turn at a time: while waiting assistant reply, ignore new speech.
  if (m_turnBusy.load(std::memory_order_relaxed)) {
    WakeWord::instance().touchDialog();
    return;
  }

  // 播放器正在播报时：不采集，避免回声把自己“听进去”
  if (Mp3Player::instance().getState() != Mp3PlayerState::Idle) {
    WakeWord::instance().touchDialog();
    return;
  }
  
  // 等待 WebSocket 连接就绪
  if (!ws.isReady()) {
    WakeWord::instance().touchDialog();
    return;
  }
  
  // 如果 TTS 正在播放，不发送音频
  if (ws.getState() == WsDialogState::Speaking) {
    WakeWord::instance().touchDialog();
    return;
  }

  if (samples == nullptr || numSamples <= 0) {
    return;
  }

  // 计算帧时长
  if (m_frameMs <= 0) {
    m_frameMs = std::max(1, (numSamples * 1000) / m_cfg.sample_rate_hz);
  }

  // 本地 VAD + 能量门限：避免噪声导致误触发
  uint32_t meanAbs = meanAbs16(samples, numSamples);
  bool speechFrame = (vad == VAD_SPEECH);
  if (m_cfg.energy_gate_mean_abs > 0 &&
      meanAbs < (uint32_t)m_cfg.energy_gate_mean_abs) {
    speechFrame = false;
  }

  // Pre-roll (avoid cutting the first syllable when we start listening)
  constexpr int kPreRollMs = 200;
  size_t preRollSamples =
      (size_t)((int64_t)kPreRollMs * m_cfg.sample_rate_hz / 1000);

  // Start a new utterance only when speech is detected (avoid uploading long
  // silence which can confuse the server and waste bandwidth).
  if (!m_inSpeech) {
    if (!speechFrame) {
      if (preRollSamples > 0) {
        size_t old = m_wsPreRoll.size();
        m_wsPreRoll.resize(old + (size_t)numSamples);
        memcpy(m_wsPreRoll.data() + old, samples,
               (size_t)numSamples * sizeof(int16_t));
        if (m_wsPreRoll.size() > preRollSamples) {
          size_t drop = m_wsPreRoll.size() - preRollSamples;
          m_wsPreRoll.erase(m_wsPreRoll.begin(), m_wsPreRoll.begin() + drop);
        }
      }
      return;
    }

    if (ws.getState() == WsDialogState::Connected) {
      esp_err_t err = ws.startListening();
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS startListening failed: %s", esp_err_to_name(err));
        return;
      }
    } else if (ws.getState() != WsDialogState::Listening) {
      // Handshake not complete or in an unexpected state.
      return;
    }

    m_wsListening = true;
    m_inSpeech = true;
    m_speechMs = m_frameMs;
    m_silenceMs = 0;

    ESP_LOGI(TAG, "WS speech start (meanAbs=%u)", (unsigned)meanAbs);

    // Flush pre-roll first (if any)
    if (!m_wsPreRoll.empty()) {
      (void)ws.sendAudio((const uint8_t *)m_wsPreRoll.data(),
                         m_wsPreRoll.size() * sizeof(int16_t));
      m_wsPreRoll.clear();
    }

    (void)ws.sendAudio((const uint8_t *)samples,
                       (size_t)numSamples * sizeof(int16_t));
    WakeWord::instance().touchDialog();
    return;
  }

  // In speech: stream frames (including short trailing silence for STT).
  if (speechFrame) {
    m_speechMs += m_frameMs;
    m_silenceMs = 0;
    WakeWord::instance().touchDialog();
  } else {
    m_silenceMs += m_frameMs;
  }

  if (m_wsListening) {
    (void)ws.sendAudio((const uint8_t *)samples,
                       (size_t)numSamples * sizeof(int16_t));
  }

  bool shouldStop = false;
  if (m_speechMs >= m_cfg.min_speech_ms &&
      m_silenceMs >= m_cfg.end_silence_ms) {
    shouldStop = true;
  }
  if (m_speechMs >= m_cfg.max_utterance_ms) {
    shouldStop = true;
  }

  if (shouldStop) {
    ESP_LOGI(TAG, "WS speech end: speech=%dms silence=%dms", m_speechMs,
             m_silenceMs);
    (void)ws.stopListening();
    m_wsListening = false;
    m_wsPreRoll.clear();
    m_inSpeech = false;
    m_speechMs = 0;
    m_silenceMs = 0;

    // Wait for assistant reply (tts start/stop). Prevent overlapping turns.
    uint32_t now = (uint32_t)xTaskGetTickCount();
    m_wsTurnBusySinceTick = now;
    m_wsStopListenTick = now;  // Track when we stopped for STT timeout
    m_turnBusy.store(true, std::memory_order_relaxed);
  }
}
