#include "cloud_tts.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "mp3_player.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

static const char *TAG = "CloudTts";

namespace {
struct HttpBuf {
  uint8_t *data = nullptr;
  size_t size = 0;
  size_t cap = 0;
};

static void freeBuf(HttpBuf &b) {
  if (b.data) {
    free(b.data);
  }
  b = {};
}

static bool ensureCap(HttpBuf &b, size_t need, size_t maxCap) {
  if (need > maxCap) {
    return false;
  }
  if (b.cap >= need) {
    return true;
  }

  size_t newCap = b.cap == 0 ? 16 * 1024 : b.cap;
  while (newCap < need) {
    newCap = std::min(newCap * 2, maxCap);
    if (newCap < need && newCap == maxCap) {
      return false;
    }
  }

  void *p = realloc(b.data, newCap);
  if (!p) {
    return false;
  }
  b.data = static_cast<uint8_t *>(p);
  b.cap = newCap;
  return true;
}
} // namespace

CloudTts &CloudTts::instance() {
  static CloudTts inst;
  return inst;
}

esp_err_t CloudTts::init(const CloudTtsConfig &cfg) {
  m_cfg = cfg;
  m_inited = true;
  return ESP_OK;
}

esp_err_t CloudTts::speak(const std::string &text) {
  if (!m_inited) {
    ESP_LOGE(TAG, "CloudTts not initialized");
    return ESP_ERR_INVALID_STATE;
  }
  if (m_cfg.url.empty()) {
    ESP_LOGE(TAG, "CloudTts url is empty");
    return ESP_ERR_INVALID_ARG;
  }
  if (text.empty()) {
    return ESP_OK;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = m_cfg.url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = m_cfg.timeout_ms;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(client, "Content-Type",
                             "text/plain; charset=utf-8");
  esp_http_client_set_header(client, "Accept", "audio/wav");

  esp_err_t err = esp_http_client_open(client, (int)text.size());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }

  int w = esp_http_client_write(client, text.data(), (int)text.size());
  if (w < 0 || (size_t)w != text.size()) {
    ESP_LOGE(TAG, "http write failed: wrote=%d", w);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  int contentLen = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    ESP_LOGE(TAG, "TTS server http status=%d, contentLen=%d", status,
             contentLen);

    char errBuf[256];
    int r = esp_http_client_read(client, errBuf, sizeof(errBuf) - 1);
    if (r > 0) {
      errBuf[r] = '\0';
      ESP_LOGE(TAG, "TTS server body: %s", errBuf);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  HttpBuf buf;
  size_t maxBytes = (size_t)std::max(0, m_cfg.max_response_bytes);
  if (maxBytes == 0) {
    maxBytes = 256 * 1024;
  }

  if (contentLen > 0) {
    if (!ensureCap(buf, (size_t)contentLen, maxBytes)) {
      ESP_LOGE(TAG, "response too large: %d", contentLen);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return ESP_ERR_NO_MEM;
    }
  } else {
    if (!ensureCap(buf, 16 * 1024, maxBytes)) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return ESP_ERR_NO_MEM;
    }
  }

  while (true) {
    if (buf.size == buf.cap) {
      if (!ensureCap(buf, buf.cap + 1024, maxBytes)) {
        ESP_LOGE(TAG, "response exceeds max bytes (%u)",
                 (unsigned)maxBytes);
        freeBuf(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
      }
    }

    int r = esp_http_client_read(
        client, reinterpret_cast<char *>(buf.data + buf.size),
        (int)std::min((size_t)4096, buf.cap - buf.size));
    if (r < 0) {
      ESP_LOGE(TAG, "http read failed");
      freeBuf(buf);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return ESP_FAIL;
    }
    if (r == 0) {
      break;
    }
    buf.size += (size_t)r;
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (contentLen > 0 && (int)buf.size < contentLen) {
    ESP_LOGE(TAG, "incomplete download: got=%u expected=%d",
             (unsigned)buf.size, contentLen);
    freeBuf(buf);
    return ESP_FAIL;
  }

  if (buf.size == 0) {
    ESP_LOGE(TAG, "empty audio response");
    freeBuf(buf);
    return ESP_FAIL;
  }

  if (buf.size < 4 || memcmp(buf.data, "RIFF", 4) != 0) {
    ESP_LOGE(TAG, "unexpected audio header (not RIFF), size=%u",
             (unsigned)buf.size);
    freeBuf(buf);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "TTS audio bytes: %u", (unsigned)buf.size);

  // 播放返回的 audio/wav（Mp3Player 基于 audio_player，支持 WAV/MP3 解码）
  auto &player = Mp3Player::instance();
  if (player.getState() != Mp3PlayerState::Idle) {
    player.stop();
  }

  err = player.playOwnedBuffer(buf.data, buf.size, false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "playOwnedBuffer failed: %s", esp_err_to_name(err));
    freeBuf(buf);
    return err;
  }

  // 播放器接管内存
  buf.data = nullptr;
  buf.size = 0;
  buf.cap = 0;
  return ESP_OK;
}
