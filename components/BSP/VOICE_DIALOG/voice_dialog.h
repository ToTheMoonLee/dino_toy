#pragma once

#include "esp_err.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct VoiceDialogConfig {
  std::string chat_url; // e.g. http://192.168.1.10:8000/chat
  int sample_rate_hz = 16000;
  bool use_pcm_stream = false; // server returns streaming PCM instead of WAV

  int min_speech_ms = 300;
  int end_silence_ms = 450;
  int max_utterance_ms = 8000;
  int max_pcm_ms = 10000; // hard cap to avoid OOM

  int worker_stack = 8192;
  int worker_prio = 4;
  int worker_core = 0;
};

class VoiceDialog {
public:
  esp_err_t init(const VoiceDialogConfig &cfg);

  void onWakeDetected();
  void onLocalCommandDetected();

  /**
   * @brief 对话模式音频帧输入（从 WakeWord::setAudioFrameCallback 喂入）
   */
  void onAudioFrame(const int16_t *samples, int numSamples, vad_state_t vad);

  /**
   * @brief 在主循环里周期调用，用于检测对话模式退出后清理状态
   */
  void tick();

private:
  struct UtteranceEvent {
    int16_t *pcm = nullptr; // malloc owned; worker will free
    size_t samples = 0;
    int sample_rate_hz = 16000;
  };

  static void workerTask(void *arg);
  void handleUtterance(const UtteranceEvent &ev);
  void resetCapture();
  bool shouldFinalizeOnSilence() const;

  VoiceDialogConfig m_cfg;
  bool m_inited = false;

  // capture state (called from WakeWord detect task)
  bool m_sessionActive = false;
  std::atomic<bool> m_turnBusy{false}; // one user turn at a time
  bool m_inSpeech = false;
  bool m_dropCurrentUtterance = false;
  int m_speechMs = 0;
  int m_silenceMs = 0;
  int m_frameMs = 0;
  std::vector<int16_t> m_pcm;

  // worker
  QueueHandle_t m_queue = nullptr;
  TaskHandle_t m_task = nullptr;

  std::string m_deviceId;
};
