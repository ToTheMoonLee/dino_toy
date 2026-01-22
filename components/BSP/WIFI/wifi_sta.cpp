#include "wifi_sta.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>

static const char *TAG = "WifiStation";

// ============================================================================
// 单例实现
// ============================================================================
WifiStation &WifiStation::instance() {
  static WifiStation instance;
  return instance;
}

// ============================================================================
// 事件处理
// ============================================================================
void WifiStation::eventHandler(void *arg, esp_event_base_t eventBase,
                               int32_t eventId, void *eventData) {
  // 获取单例实例
  auto &self = instance();

  if (eventBase == WIFI_EVENT) {
    self.handleWifiEvent(eventId);
  } else if (eventBase == IP_EVENT) {
    self.handleIpEvent(eventId, eventData);
  }
}

void WifiStation::handleWifiEvent(int32_t eventId) {
  switch (eventId) {
  case WIFI_EVENT_STA_START:
    ESP_LOGI(TAG, "WiFi STA started, connecting to %s...", m_ssid.c_str());
    m_state = WifiState::Connecting;
    esp_wifi_connect();
    break;

  case WIFI_EVENT_STA_DISCONNECTED:
    if (m_retryCount < m_maxRetry) {
      esp_wifi_connect();
      m_retryCount++;
      ESP_LOGI(TAG, "Retry connecting (%d/%d)", m_retryCount, m_maxRetry);
    } else {
      m_state = WifiState::Failed;
      if (m_eventGroup) {
        xEventGroupSetBits(m_eventGroup, FAIL_BIT);
      }
      ESP_LOGE(TAG, "Failed to connect after %d attempts", m_maxRetry);
    }
    break;

  default:
    break;
  }
}

void WifiStation::handleIpEvent(int32_t eventId, void *eventData) {
  if (eventId == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(eventData);
    m_ipAddr = event->ip_info.ip;

    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&m_ipAddr));

    m_retryCount = 0;
    m_state = WifiState::Connected;

    if (m_eventGroup) {
      xEventGroupSetBits(m_eventGroup, CONNECTED_BIT);
    }
  }
}

// ============================================================================
// 公共方法
// ============================================================================
esp_err_t WifiStation::init() {
  if (m_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  // 1. 初始化 NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 2. 创建事件组
  m_eventGroup = xEventGroupCreate();
  if (!m_eventGroup) {
    ESP_LOGE(TAG, "Failed to create event group");
    return ESP_FAIL;
  }

  // 3. 初始化网络接口
  ESP_ERROR_CHECK(esp_netif_init());

  // 4. 创建默认事件循环
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // 5. 创建 WiFi STA 网络接口
  m_netif = esp_netif_create_default_wifi_sta();
  if (!m_netif) {
    ESP_LOGE(TAG, "Failed to create netif");
    return ESP_FAIL;
  }

  // 6. 初始化 WiFi 驱动
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // 7. 注册事件处理
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, nullptr, nullptr));

  m_initialized = true;
  ESP_LOGI(TAG, "WiFi initialized successfully");
  return ESP_OK;
}

esp_err_t WifiStation::connect(const std::string &ssid,
                               const std::string &password, int maxRetry) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "Not initialized, call init() first");
    return ESP_ERR_INVALID_STATE;
  }

  m_ssid = ssid;
  m_maxRetry = maxRetry;
  m_retryCount = 0;

  // 配置 WiFi
  wifi_config_t wifiConfig = {};
  std::memcpy(wifiConfig.sta.ssid, ssid.c_str(),
              std::min(ssid.size(), sizeof(wifiConfig.sta.ssid) - 1));
  std::memcpy(wifiConfig.sta.password, password.c_str(),
              std::min(password.size(), sizeof(wifiConfig.sta.password) - 1));
  wifiConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifiConfig.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Waiting for connection to %s...", ssid.c_str());

  // 等待连接结果
  EventBits_t bits = xEventGroupWaitBits(m_eventGroup, CONNECTED_BIT | FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & CONNECTED_BIT) {
    ESP_LOGI(TAG, "Successfully connected to %s", ssid.c_str());
    return ESP_OK;
  }

  ESP_LOGE(TAG, "Failed to connect to %s", ssid.c_str());
  return ESP_FAIL;
}

esp_err_t WifiStation::disconnect() {
  esp_err_t ret = esp_wifi_disconnect();
  if (ret == ESP_OK) {
    m_state = WifiState::Disconnected;
    ESP_LOGI(TAG, "Disconnected");
  }
  return ret;
}

std::string WifiStation::getIpAddress() const {
  if (!isConnected()) {
    return "";
  }

  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR(&m_ipAddr));
  return std::string(buf);
}
