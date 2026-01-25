#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "WifiManager";

// NVS keys
static constexpr const char *kNvsNamespace = "wifi";
static constexpr const char *kNvsKeySsid = "ssid";
static constexpr const char *kNvsKeyPass = "pass";

WifiManager &WifiManager::instance() {
  static WifiManager inst;
  return inst;
}

// ============================================================================
// Init
// ============================================================================

esp_err_t WifiManager::init(const WifiManagerConfig &config) {
  if (m_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  m_cfg = config;

  esp_err_t ret = initNvs();
  if (ret != ESP_OK) {
    return ret;
  }

  m_eventGroup = xEventGroupCreate();
  if (m_eventGroup == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    return ESP_ERR_NO_MEM;
  }

  ret = initWifiDriver();
  if (ret != ESP_OK) {
    return ret;
  }

  m_initialized = true;
  return ESP_OK;
}

esp_err_t WifiManager::initNvs() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  return ESP_OK;
}

esp_err_t WifiManager::initWifiDriver() {
  // These APIs may return ESP_ERR_INVALID_STATE if already called elsewhere.
  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s",
             esp_err_to_name(ret));
    return ret;
  }

  // Create netifs once
  if (m_staNetif == nullptr) {
    m_staNetif = esp_netif_create_default_wifi_sta();
    if (m_staNetif == nullptr) {
      ESP_LOGE(TAG, "Failed to create STA netif");
      return ESP_FAIL;
    }
  }
  if (m_apNetif == nullptr) {
    m_apNetif = esp_netif_create_default_wifi_ap();
    if (m_apNetif == nullptr) {
      ESP_LOGE(TAG, "Failed to create AP netif");
      return ESP_FAIL;
    }
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ret = esp_wifi_init(&cfg);
  if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::eventHandler, this, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::eventHandler, this, nullptr));

  return ESP_OK;
}

// ============================================================================
// Public start
// ============================================================================

esp_err_t WifiManager::start() {
  if (!m_initialized) {
    ESP_LOGE(TAG, "Not initialized, call init() first");
    return ESP_ERR_INVALID_STATE;
  }

  // Always start web server; it will be reachable via STA or AP IP.
  esp_err_t ret = startWebServer();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
  }

  std::string ssid;
  std::string pass;
  loadCredentials(ssid, pass);

  if (!ssid.empty()) {
    ESP_LOGI(TAG, "Found saved WiFi SSID: %s, trying STA connect...", ssid.c_str());
    ret = startSta(ssid, pass, false);
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "STA connected, IP: %s", getStaIpAddress().c_str());
      return ESP_OK;
    }
    ESP_LOGW(TAG, "STA connect failed, fallback to AP config mode");
  } else {
    ESP_LOGI(TAG, "No saved WiFi, start AP config mode");
  }

  ret = startAp();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start AP: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "AP started, connect to SSID: %s, open http://%s/",
           m_cfg.ap_ssid.c_str(), getApIpAddress().c_str());
  return ESP_OK;
}

// ============================================================================
// WiFi state helpers
// ============================================================================

bool WifiManager::isStaConnected() const { return (m_staIp.addr != 0); }

std::string WifiManager::getStaIpAddress() const {
  if (!isStaConnected()) {
    return "";
  }
  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR(&m_staIp));
  return std::string(buf);
}

std::string WifiManager::getApIpAddress() const {
  if (m_apNetif == nullptr) {
    return "";
  }

  esp_netif_ip_info_t ip{};
  if (esp_netif_get_ip_info(m_apNetif, &ip) != ESP_OK) {
    return "";
  }

  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
  return std::string(buf);
}

// ============================================================================
// NVS credentials
// ============================================================================

esp_err_t WifiManager::loadCredentials(std::string &ssidOut,
                                      std::string &passOut) {
  ssidOut.clear();
  passOut.clear();

  nvs_handle_t h;
  esp_err_t ret = nvs_open(kNvsNamespace, NVS_READONLY, &h);
  if (ret != ESP_OK) {
    return ret;
  }

  size_t ssidLen = 0;
  ret = nvs_get_str(h, kNvsKeySsid, nullptr, &ssidLen);
  if (ret == ESP_OK && ssidLen > 1) {
    std::string ssid(ssidLen, '\0');
    if (nvs_get_str(h, kNvsKeySsid, &ssid[0], &ssidLen) == ESP_OK) {
      // nvs_get_str writes trailing '\0'
      ssid.resize(std::strlen(ssid.c_str()));
      ssidOut = ssid;
    }
  }

  size_t passLen = 0;
  ret = nvs_get_str(h, kNvsKeyPass, nullptr, &passLen);
  if (ret == ESP_OK && passLen > 0) {
    std::string pass(passLen, '\0');
    if (nvs_get_str(h, kNvsKeyPass, &pass[0], &passLen) == ESP_OK) {
      pass.resize(std::strlen(pass.c_str()));
      passOut = pass;
    }
  }

  nvs_close(h);
  return ESP_OK;
}

esp_err_t WifiManager::saveCredentials(const std::string &ssid,
                                      const std::string &pass) {
  nvs_handle_t h;
  esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = nvs_set_str(h, kNvsKeySsid, ssid.c_str());
  if (ret != ESP_OK) {
    nvs_close(h);
    return ret;
  }

  ret = nvs_set_str(h, kNvsKeyPass, pass.c_str());
  if (ret != ESP_OK) {
    nvs_close(h);
    return ret;
  }

  ret = nvs_commit(h);
  nvs_close(h);
  return ret;
}

// ============================================================================
// Start STA/AP
// ============================================================================

esp_err_t WifiManager::startSta(const std::string &ssid, const std::string &pass,
                               bool withAp) {
  m_staSsid = ssid;
  m_staPass = pass;
  m_staIp = {};
  m_staRetryCount = 0;
  m_deferStopAp = withAp;

  if (m_eventGroup) {
    xEventGroupClearBits(m_eventGroup, STA_CONNECTED_BIT | STA_FAIL_BIT);
  }

  // Reconfigure WiFi
  esp_wifi_stop(); // ignore errors

  wifi_config_t staCfg = {};
  std::memcpy(staCfg.sta.ssid, ssid.c_str(),
              std::min(ssid.size(), sizeof(staCfg.sta.ssid) - 1));
  std::memcpy(staCfg.sta.password, pass.c_str(),
              std::min(pass.size(), sizeof(staCfg.sta.password) - 1));
  staCfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  staCfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  wifi_mode_t mode = withAp ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &staCfg));

  if (withAp) {
    // Ensure AP config present
    wifi_config_t apCfg = {};
    std::memcpy(apCfg.ap.ssid, m_cfg.ap_ssid.c_str(),
                std::min(m_cfg.ap_ssid.size(), sizeof(apCfg.ap.ssid) - 1));
    apCfg.ap.ssid_len = m_cfg.ap_ssid.size();
    apCfg.ap.channel = m_cfg.ap_channel;
    apCfg.ap.max_connection = m_cfg.ap_max_conn;
    if (m_cfg.ap_password.empty()) {
      apCfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
      std::memcpy(apCfg.ap.password, m_cfg.ap_password.c_str(),
                  std::min(m_cfg.ap_password.size(),
                           sizeof(apCfg.ap.password) - 1));
      apCfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apCfg));
    m_apRunning = true;
  } else {
    m_apRunning = false;
  }

  ESP_ERROR_CHECK(esp_wifi_start());

  // Connect explicitly (avoid relying on WIFI_EVENT_STA_START timing)
  esp_wifi_connect();

  if (m_eventGroup == nullptr) {
    return ESP_OK;
  }

  TickType_t ticks = pdMS_TO_TICKS(m_cfg.sta_connect_timeout_ms);
  EventBits_t bits = xEventGroupWaitBits(m_eventGroup,
                                         STA_CONNECTED_BIT | STA_FAIL_BIT,
                                         pdFALSE, pdFALSE, ticks);

  if (bits & STA_CONNECTED_BIT) {
    return ESP_OK;
  }
  return ESP_FAIL;
}

esp_err_t WifiManager::startAp() {
  // Validate password length for WPA2
  if (!m_cfg.ap_password.empty() && m_cfg.ap_password.size() < 8) {
    ESP_LOGW(TAG, "AP password too short (<8), fallback to open AP");
    m_cfg.ap_password.clear();
  }

  esp_wifi_stop(); // ignore errors

  wifi_config_t apCfg = {};
  std::memcpy(apCfg.ap.ssid, m_cfg.ap_ssid.c_str(),
              std::min(m_cfg.ap_ssid.size(), sizeof(apCfg.ap.ssid) - 1));
  apCfg.ap.ssid_len = m_cfg.ap_ssid.size();
  apCfg.ap.channel = m_cfg.ap_channel;
  apCfg.ap.max_connection = m_cfg.ap_max_conn;
  if (m_cfg.ap_password.empty()) {
    apCfg.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    std::memcpy(apCfg.ap.password, m_cfg.ap_password.c_str(),
                std::min(m_cfg.ap_password.size(), sizeof(apCfg.ap.password) - 1));
    apCfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apCfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  m_apRunning = true;
  m_staIp = {};
  return ESP_OK;
}

void WifiManager::stopAp() {
  if (!m_apRunning) {
    m_deferStopAp = false;
    return;
  }
  // Switch to STA only if already connected; otherwise just leave it.
  esp_wifi_set_mode(WIFI_MODE_STA);
  m_apRunning = false;
  m_deferStopAp = false;
}

// ============================================================================
// Events
// ============================================================================

void WifiManager::eventHandler(void *arg, esp_event_base_t eventBase,
                               int32_t eventId, void *eventData) {
  auto *self = static_cast<WifiManager *>(arg);
  if (self == nullptr) {
    return;
  }

  if (eventBase == WIFI_EVENT) {
    self->onWifiEvent(eventId, eventData);
  } else if (eventBase == IP_EVENT) {
    self->onIpEvent(eventId, eventData);
  }
}

void WifiManager::onWifiEvent(int32_t eventId, void * /*eventData*/) {
  switch (eventId) {
  case WIFI_EVENT_STA_DISCONNECTED:
    // If we are trying STA connect, retry a few times to avoid transient issues.
    if (!m_staSsid.empty() && m_staRetryCount < m_cfg.sta_max_retry) {
      m_staRetryCount++;
      ESP_LOGI(TAG, "Retry connecting to %s (%d/%d)", m_staSsid.c_str(),
               m_staRetryCount, m_cfg.sta_max_retry);
      esp_wifi_connect();
    } else {
      if (m_eventGroup) {
        xEventGroupSetBits(m_eventGroup, STA_FAIL_BIT);
      }
      m_staIp = {};
    }
    break;
  default:
    break;
  }
}

void WifiManager::onIpEvent(int32_t eventId, void *eventData) {
  if (eventId != IP_EVENT_STA_GOT_IP) {
    return;
  }

  auto *event = static_cast<ip_event_got_ip_t *>(eventData);
  m_staIp = event->ip_info.ip;
  m_staRetryCount = 0;

  ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&m_staIp));
  if (m_eventGroup) {
    xEventGroupSetBits(m_eventGroup, STA_CONNECTED_BIT);
  }

  if (!m_cfg.keep_ap_on_after_sta_connected && !m_deferStopAp) {
    stopAp();
  }
}

// ============================================================================
// Web server
// ============================================================================

static const char *kHtmlRoot = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>ESP32 Control</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; margin: 18px; }
    .row { display: flex; flex-wrap: wrap; gap: 10px; margin: 14px 0; }
    button { padding: 12px 14px; border: 1px solid #222; background: #fff; border-radius: 10px; }
    button:active { background: #eee; }
    .card { border: 1px solid #ddd; border-radius: 12px; padding: 12px; }
    input[type="text"] { width: 100%; padding: 10px; border: 1px solid #aaa; border-radius: 10px; margin: 6px 0 12px; }
    pre { white-space: pre-wrap; word-break: break-word; }
    a { color: #0366d6; }
  </style>
</head>
<body>
  <h2>ESP32 Web Control</h2>
  <div class="card">
    <div><a href="/wifi">WiFi 配网</a></div>
    <div class="row">
      <button onclick="cmd(0)">开灯</button>
      <button onclick="cmd(1)">关灯</button>
      <button onclick="cmd(2)">前进</button>
      <button onclick="cmd(3)">后退</button>
      <button onclick="cmd(4)">神龙摆尾</button>
    </div>
  </div>

  <h3>TTS</h3>
  <div class="card">
    <input id="ttsText" type="text" placeholder="输入要朗读的文本，例如：你好，我是ESP32" />
    <div class="row">
      <button onclick="tts()">朗读</button>
    </div>
    <pre id="ttsRet"></pre>
  </div>

  <h3>状态</h3>
  <div class="card">
    <pre id="status">loading...</pre>
  </div>

<script>
async function cmd(id) {
  try {
    await fetch('/api/cmd?id=' + id, { method: 'GET' });
    await refresh();
  } catch (e) {
    console.log(e);
  }
}

async function tts() {
  const text = document.getElementById('ttsText').value || '';
  if (!text.trim()) return;
  try {
    document.getElementById('ttsRet').textContent = 'requesting...';
    const r = await fetch('/api/tts', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      body: text
    });
    document.getElementById('ttsRet').textContent = await r.text();
  } catch (e) {
    document.getElementById('ttsRet').textContent = 'tts error: ' + e;
  }
}

async function refresh() {
  try {
    const r = await fetch('/api/status');
    const j = await r.json();
    document.getElementById('status').textContent = JSON.stringify(j, null, 2);
  } catch (e) {
    document.getElementById('status').textContent = 'status error: ' + e;
  }
}

refresh();
setInterval(refresh, 1200);
</script>
</body>
</html>
)HTML";

static const char *kHtmlWifi = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>WiFi Setup</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; margin: 18px; }
    input { width: 100%; padding: 10px; border: 1px solid #aaa; border-radius: 10px; margin: 6px 0 12px; }
    button { padding: 12px 14px; border: 1px solid #222; background: #fff; border-radius: 10px; }
    .card { border: 1px solid #ddd; border-radius: 12px; padding: 12px; }
    a { color: #0366d6; }
  </style>
</head>
<body>
  <h2>WiFi 配网</h2>
  <div class="card">
    <div><a href="/">返回控制页</a></div>
    <form action="/api/wifi/save" method="post">
      <label>SSID</label>
      <input name="ssid" placeholder="Your WiFi SSID" required />
      <label>密码</label>
      <input name="pass" type="password" placeholder="Password" />
      <button type="submit">保存并连接</button>
    </form>
  </div>
</body>
</html>
)HTML";

esp_err_t WifiManager::startWebServer() {
  if (m_httpd != nullptr) {
    return ESP_OK;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.max_uri_handlers = 12;

  esp_err_t ret = httpd_start(&m_httpd, &cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
    m_httpd = nullptr;
    return ret;
  }

  httpd_uri_t root = {.uri = "/",
                      .method = HTTP_GET,
                      .handler = &WifiManager::handleRoot,
                      .user_ctx = this};
  httpd_register_uri_handler(m_httpd, &root);

  httpd_uri_t wifi = {.uri = "/wifi",
                      .method = HTTP_GET,
                      .handler = &WifiManager::handleWifiPage,
                      .user_ctx = this};
  httpd_register_uri_handler(m_httpd, &wifi);

  httpd_uri_t status = {.uri = "/api/status",
                        .method = HTTP_GET,
                        .handler = &WifiManager::handleStatus,
                        .user_ctx = this};
  httpd_register_uri_handler(m_httpd, &status);

  httpd_uri_t cmd = {.uri = "/api/cmd",
                     .method = HTTP_GET,
                     .handler = &WifiManager::handleCmd,
                     .user_ctx = this};
  httpd_register_uri_handler(m_httpd, &cmd);

  httpd_uri_t tts = {.uri = "/api/tts",
                     .method = HTTP_POST,
                     .handler = &WifiManager::handleTts,
                     .user_ctx = this};
  httpd_register_uri_handler(m_httpd, &tts);

  httpd_uri_t wifiSave = {.uri = "/api/wifi/save",
                          .method = HTTP_POST,
                          .handler = &WifiManager::handleWifiSave,
                          .user_ctx = this};
  httpd_register_uri_handler(m_httpd, &wifiSave);

  ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
  return ESP_OK;
}

void WifiManager::stopWebServer() {
  if (m_httpd) {
    httpd_stop(m_httpd);
    m_httpd = nullptr;
  }
}

// ============================================================================
// HTTP handlers
// ============================================================================

esp_err_t WifiManager::handleRoot(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, kHtmlRoot, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiManager::handleWifiPage(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, kHtmlWifi, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiManager::handleStatus(httpd_req_t *req) {
  auto *self = static_cast<WifiManager *>(req->user_ctx);
  if (self == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
    return ESP_FAIL;
  }

  std::string app = "{}";
  if (self->m_statusCb) {
    app = self->m_statusCb();
    if (app.empty()) {
      app = "{}";
    }
  }

  std::string body;
  body.reserve(256 + app.size());
  body += "{";
  body += "\"sta\":{";
  body += "\"connected\":";
  body += (self->isStaConnected() ? "true" : "false");
  body += ",\"ssid\":\"";
  body += self->m_staSsid;
  body += "\",\"ip\":\"";
  body += self->getStaIpAddress();
  body += "\"},";
  body += "\"ap\":{";
  body += "\"running\":";
  body += (self->m_apRunning ? "true" : "false");
  body += ",\"ssid\":\"";
  body += self->m_cfg.ap_ssid;
  body += "\",\"ip\":\"";
  body += self->getApIpAddress();
  body += "\"},";
  body += "\"app\":";
  body += app;
  body += "}";

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t WifiManager::handleCmd(httpd_req_t *req) {
  auto *self = static_cast<WifiManager *>(req->user_ctx);
  if (self == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
    return ESP_FAIL;
  }

  char query[64];
  char idStr[8];
  int id = -1;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "id", idStr, sizeof(idStr)) == ESP_OK) {
      id = atoi(idStr);
    }
  }

  if (id >= 0 && id <= 4 && self->m_cmdCb) {
    self->m_cmdCb(id);
  }

  httpd_resp_set_type(req, "application/json");
  const char *ok = "{\"ok\":true}";
  return httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
}

static std::string readReqBody(httpd_req_t *req);

namespace {
struct TtsTaskCtx {
  WifiWebTtsCallback cb;
  std::string text;
};

static void ttsTask(void *arg) {
  auto *ctx = static_cast<TtsTaskCtx *>(arg);
  if (ctx && ctx->cb) {
    ctx->cb(ctx->text);
  }
  delete ctx;
  vTaskDelete(nullptr);
}
} // namespace

esp_err_t WifiManager::handleTts(httpd_req_t *req) {
  auto *self = static_cast<WifiManager *>(req->user_ctx);
  if (self == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
    return ESP_FAIL;
  }
  if (!self->m_ttsCb) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tts not configured");
    return ESP_FAIL;
  }

  std::string text = readReqBody(req);
  // 简单限流：避免一次性塞入超长文本导致内存压力
  if (text.empty() || text.size() > 512) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad text");
    return ESP_FAIL;
  }

  // 放到后台任务执行，避免阻塞 httpd 线程
  auto *ctx = new TtsTaskCtx();
  if (!ctx) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    return ESP_FAIL;
  }
  ctx->cb = self->m_ttsCb;
  ctx->text = std::move(text);
  BaseType_t ok = xTaskCreatePinnedToCore(ttsTask, "tts_task", 8192, ctx, 4,
                                         nullptr, 0);
  if (ok != pdPASS) {
    delete ctx;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task fail");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  const char *body = "{\"ok\":true}";
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static std::string readReqBody(httpd_req_t *req) {
  std::string out;
  size_t total = req->content_len;
  if (total == 0) {
    return out;
  }
  out.resize(total);

  size_t received = 0;
  while (received < total) {
    int r = httpd_req_recv(req, &out[0] + received, total - received);
    if (r <= 0) {
      break;
    }
    received += (size_t)r;
  }
  out.resize(received);
  return out;
}

esp_err_t WifiManager::handleWifiSave(httpd_req_t *req) {
  auto *self = static_cast<WifiManager *>(req->user_ctx);
  if (self == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
    return ESP_FAIL;
  }

  std::string body = readReqBody(req);
  std::string ssid;
  std::string pass;
  if (!parseFormUrlEncoded(body, ssid, pass) || ssid.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Save WiFi SSID: %s", ssid.c_str());
  self->saveCredentials(ssid, pass);

  // During provisioning, keep AP on while trying STA (avoid breaking HTTP session).
  esp_err_t ret = self->startSta(ssid, pass, true);

  // Small delayed task to stop AP after responding, if needed.
  bool shouldStopAp = (ret == ESP_OK) && (!self->m_cfg.keep_ap_on_after_sta_connected);

  std::string resp;
  if (ret == ESP_OK) {
    resp = "<html><body><h3>Connected!</h3><p>IP: ";
    resp += self->getStaIpAddress();
    resp += "</p><p>Now you can open <a href=\"/\">Control</a>.</p>";
    if (shouldStopAp) {
      resp += "<p>AP will be turned off shortly.</p>";
    }
    resp += "</body></html>";
  } else {
    resp = "<html><body><h3>Connect failed</h3><p>Please check SSID/password.</p>"
           "<p><a href=\"/wifi\">Back</a></p></body></html>";
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, resp.c_str(), resp.size());

  if (shouldStopAp) {
    xTaskCreate(
        [](void *p) {
          auto *mgr = static_cast<WifiManager *>(p);
          vTaskDelay(pdMS_TO_TICKS(2000));
          mgr->stopAp();
          vTaskDelete(nullptr);
        },
        "stop_ap_later", 2048, self, 3, nullptr);
  }

  return ESP_OK;
}

// ============================================================================
// Helpers
// ============================================================================

std::string WifiManager::urlDecode(const std::string &in) {
  std::string out;
  out.reserve(in.size());

  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '+') {
      out.push_back(' ');
      continue;
    }
    if (c == '%' && i + 2 < in.size()) {
      auto hex = [](char hc) -> int {
        if (hc >= '0' && hc <= '9')
          return hc - '0';
        if (hc >= 'a' && hc <= 'f')
          return 10 + (hc - 'a');
        if (hc >= 'A' && hc <= 'F')
          return 10 + (hc - 'A');
        return -1;
      };
      int hi = hex(in[i + 1]);
      int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

bool WifiManager::parseFormUrlEncoded(const std::string &body,
                                      std::string &ssidOut,
                                      std::string &passOut) {
  ssidOut.clear();
  passOut.clear();

  size_t pos = 0;
  while (pos < body.size()) {
    size_t amp = body.find('&', pos);
    if (amp == std::string::npos) {
      amp = body.size();
    }
    std::string pair = body.substr(pos, amp - pos);
    size_t eq = pair.find('=');
    std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
    std::string v = (eq == std::string::npos) ? "" : pair.substr(eq + 1);

    k = urlDecode(k);
    v = urlDecode(v);

    if (k == "ssid") {
      ssidOut = v;
    } else if (k == "pass") {
      passOut = v;
    }

    pos = amp + 1;
  }
  return true;
}
