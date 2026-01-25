#include "cloud_chat.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "mp3_player.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

static const char *TAG = "CloudChat";

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

CloudChat &CloudChat::instance() {
  static CloudChat inst;
  return inst;
}

esp_err_t CloudChat::init(const CloudChatConfig &cfg) {
  m_cfg = cfg;
  m_inited = true;
  return ESP_OK;
}

esp_err_t CloudChat::chatWav(const uint8_t *wavData, size_t wavLen,
                             const std::string &deviceId) {
  if (!m_inited) {
    return ESP_ERR_INVALID_STATE;
  }
  if (m_cfg.url.empty() || wavData == nullptr || wavLen == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "POST %s (wav=%u bytes, deviceId=%s)", m_cfg.url.c_str(),
           (unsigned)wavLen, deviceId.c_str());

  esp_http_client_config_t cfg = {};
  cfg.url = m_cfg.url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = m_cfg.timeout_ms;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(client, "Content-Type", "audio/wav");
  esp_http_client_set_header(client, "Accept", "audio/wav");
  if (!deviceId.empty()) {
    esp_http_client_set_header(client, "X-Device-Id", deviceId.c_str());
  }

  esp_err_t err = esp_http_client_open(client, (int)wavLen);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }

  int w = esp_http_client_write(client, (const char *)wavData, (int)wavLen);
  if (w < 0 || (size_t)w != wavLen) {
    ESP_LOGE(TAG, "http write failed: wrote=%d", w);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  int contentLen = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    ESP_LOGE(TAG, "chat http status=%d, contentLen=%d", status, contentLen);

    char errBuf[256];
    int r = esp_http_client_read(client, errBuf, sizeof(errBuf) - 1);
    if (r > 0) {
      errBuf[r] = '\0';
      ESP_LOGE(TAG, "chat body: %s", errBuf);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  HttpBuf buf;
  size_t maxBytes = (size_t)std::max(0, m_cfg.max_response_bytes);
  if (maxBytes == 0) {
    maxBytes = 1024 * 1024;
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
    // Print a small prefix for debug
    char prefix[2 * 32 + 1] = {0};
    size_t n = std::min((size_t)32, buf.size);
    for (size_t i = 0; i < n; i++) {
      snprintf(prefix + i * 2, sizeof(prefix) - i * 2, "%02X", buf.data[i]);
    }
    ESP_LOGE(TAG, "prefix(hex): %s", prefix);
    freeBuf(buf);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "assistant audio bytes: %u", (unsigned)buf.size);

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

esp_err_t CloudChat::chatWavPcmStream(const uint8_t *wavData, size_t wavLen,
                                      const std::string &deviceId) {
  if (!m_inited) {
    return ESP_ERR_INVALID_STATE;
  }
  if (m_cfg.url.empty() || wavData == nullptr || wavLen == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "POST %s (wav=%u bytes, deviceId=%s) [pcm stream]",
           m_cfg.url.c_str(), (unsigned)wavLen, deviceId.c_str());

  esp_http_client_config_t cfg = {};
  cfg.url = m_cfg.url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = m_cfg.timeout_ms;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(client, "Content-Type", "audio/wav");
  // Expect raw PCM stream (audio/L16)
  esp_http_client_set_header(client, "Accept", "audio/L16");
  if (!deviceId.empty()) {
    esp_http_client_set_header(client, "X-Device-Id", deviceId.c_str());
  }

  esp_err_t err = esp_http_client_open(client, (int)wavLen);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }

  int w = esp_http_client_write(client, (const char *)wavData, (int)wavLen);
  if (w < 0 || (size_t)w != wavLen) {
    ESP_LOGE(TAG, "http write failed: wrote=%d", w);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  (void)esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    ESP_LOGE(TAG, "chat_pcm http status=%d", status);

    char errBuf[256];
    int r = esp_http_client_read(client, errBuf, sizeof(errBuf) - 1);
    if (r > 0) {
      errBuf[r] = '\0';
      ESP_LOGE(TAG, "chat_pcm body: %s", errBuf);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  // Read sample rate hint (optional)
  int sampleRate = 16000;
  char *srVal = nullptr;
  if (esp_http_client_get_header(client, "X-Audio-Sample-Rate", &srVal) ==
          ESP_OK &&
      srVal && *srVal) {
    int v = atoi(srVal);
    if (v >= 8000 && v <= 48000) {
      sampleRate = v;
    }
  }

  auto &player = Mp3Player::instance();
  if (player.getState() != Mp3PlayerState::Idle) {
    player.stop();
  }
  // Lower prebuffer for faster "first audio" latency. If your LAN is unstable
  // and you hear dropouts, increase this value (e.g. 60-100ms).
  err = player.pcmStreamBegin((uint32_t)sampleRate, 40);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "pcmStreamBegin failed: %s", esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
  }

  uint8_t buf[2048];
  bool hasTail = false;
  uint8_t tail = 0;
  while (true) {
    int r = esp_http_client_read(client, (char *)buf, sizeof(buf));
    if (r < 0) {
      ESP_LOGE(TAG, "http read failed");
      err = ESP_FAIL;
      break;
    }
    if (r == 0) {
      err = ESP_OK;
      break;
    }

    const uint8_t *p = buf;
    size_t n = (size_t)r;
    if (hasTail) {
      uint8_t two[2] = {tail, p[0]};
      (void)player.pcmStreamWrite(two, sizeof(two), 1000);
      p += 1;
      n -= 1;
      hasTail = false;
    }
    if (n % 2 == 1) {
      tail = p[n - 1];
      hasTail = true;
      n -= 1;
    }
    if (n == 0) {
      continue;
    }

    esp_err_t we = player.pcmStreamWrite(p, n, 2000);
    if (we != ESP_OK) {
      ESP_LOGE(TAG, "pcmStreamWrite failed: %s", esp_err_to_name(we));
      err = we;
      break;
    }
  }

  player.pcmStreamEnd();
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return err;
}
