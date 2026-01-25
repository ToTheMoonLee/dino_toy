#pragma once

#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief 云端对话配置
 *
 * 设备端仅上传用户语音（WAV/PCM），服务端完成 ASR/LLM/TTS，并返回 audio/wav。
 */
struct CloudChatConfig {
  /**
   * @brief Chat Proxy URL，例如: http://192.168.1.10:8000/chat
   *
   * POST audio/wav；响应 audio/wav。
   */
  std::string url;

  int timeout_ms = 60000;
  int max_response_bytes = 1024 * 1024; // 1 MiB
};

class CloudChat {
public:
  static CloudChat &instance();

  CloudChat(const CloudChat &) = delete;
  CloudChat &operator=(const CloudChat &) = delete;
  CloudChat(CloudChat &&) = delete;
  CloudChat &operator=(CloudChat &&) = delete;

  esp_err_t init(const CloudChatConfig &cfg);
  bool isInitialized() const { return m_inited; }

  /**
   * @brief 发送用户语音（WAV）并播放返回语音（audio/wav）
   *
   * @param wavData WAV bytes
   * @param wavLen  WAV length
   * @param deviceId 设备标识，用于服务端维持多轮对话上下文
   */
  esp_err_t chatWav(const uint8_t *wavData, size_t wavLen,
                    const std::string &deviceId);

  /**
   * @brief 发送用户语音（WAV）并以 PCM 流方式播放返回语音（更低延迟）
   *
   * 服务端响应应为 16-bit PCM mono（audio/L16），可选通过响应头传回采样率：
   * - X-Audio-Sample-Rate: 16000
   *
   * @param wavData WAV bytes
   * @param wavLen  WAV length
   * @param deviceId 设备标识，用于服务端维持多轮对话上下文
   */
  esp_err_t chatWavPcmStream(const uint8_t *wavData, size_t wavLen,
                             const std::string &deviceId);

  void setUrl(const std::string &url) { m_cfg.url = url; }
  std::string getUrl() const { return m_cfg.url; }

private:
  CloudChat() = default;
  ~CloudChat() = default;

  CloudChatConfig m_cfg;
  bool m_inited = false;
};
