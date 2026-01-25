#pragma once

#include "esp_err.h"
#include <string>

/**
 * @brief 云端 TTS 配置
 *
 * 推荐在局域网内跑一个轻量 proxy（负责调用 Qwen TTS，并返回 audio/wav），
 * ESP32 只做 HTTP 拉取 + 播放，避免在固件里处理鉴权/流式/大 JSON，维护成本更低。
 */
struct CloudTtsConfig {
  /**
   * @brief TTS Proxy URL，例如: http://192.168.1.10:8000/tts
   *
   * POST 纯文本，请求体为待合成文本；响应为 audio/wav。
   */
  std::string url;

  int timeout_ms = 15000;
  int max_response_bytes = 1024 * 1024; // 1 MiB，按需调整
};

/**
 * @brief 云端 TTS 客户端（单例）
 */
class CloudTts {
public:
  static CloudTts &instance();

  CloudTts(const CloudTts &) = delete;
  CloudTts &operator=(const CloudTts &) = delete;
  CloudTts(CloudTts &&) = delete;
  CloudTts &operator=(CloudTts &&) = delete;

  esp_err_t init(const CloudTtsConfig &cfg);

  /**
   * @brief 合成并播放语音（阻塞：下载完成后开始播放）
   */
  esp_err_t speak(const std::string &text);

  void setUrl(const std::string &url) { m_cfg.url = url; }
  std::string getUrl() const { return m_cfg.url; }

private:
  CloudTts() = default;
  ~CloudTts() = default;

  CloudTtsConfig m_cfg;
  bool m_inited = false;
};

