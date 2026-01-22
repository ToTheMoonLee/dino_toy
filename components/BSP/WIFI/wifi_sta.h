#pragma once

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string>

/**
 * @brief WiFi连接状态枚举
 */
enum class WifiState { Disconnected = 0, Connecting, Connected, Failed };

/**
 * @brief WiFi Station 模式管理类
 *
 * 使用单例模式，因为 ESP32 只有一个 WiFi 硬件
 *
 * @example
 *   auto& wifi = WifiStation::instance();
 *   wifi.init();
 *   wifi.connect("MySSID", "MyPassword");
 *   if (wifi.isConnected()) {
 *       ESP_LOGI("MAIN", "IP: %s", wifi.getIpAddress().c_str());
 *   }
 */
class WifiStation {
public:
  // 单例访问
  static WifiStation &instance();

  // 禁止拷贝和移动
  WifiStation(const WifiStation &) = delete;
  WifiStation &operator=(const WifiStation &) = delete;
  WifiStation(WifiStation &&) = delete;
  WifiStation &operator=(WifiStation &&) = delete;

  /**
   * @brief 初始化WiFi模块
   * @return esp_err_t ESP_OK成功
   */
  esp_err_t init();

  /**
   * @brief 连接到WiFi（阻塞直到成功或失败）
   * @param ssid WiFi名称
   * @param password WiFi密码
   * @param maxRetry 最大重试次数，默认5次
   * @return esp_err_t ESP_OK成功
   */
  esp_err_t connect(const std::string &ssid, const std::string &password,
                    int maxRetry = 5);

  /**
   * @brief 断开WiFi连接
   * @return esp_err_t ESP_OK成功
   */
  esp_err_t disconnect();

  /**
   * @brief 获取当前连接状态
   */
  WifiState getState() const { return m_state; }

  /**
   * @brief 检查是否已连接
   */
  bool isConnected() const { return m_state == WifiState::Connected; }

  /**
   * @brief 获取IP地址字符串
   */
  std::string getIpAddress() const;

  /**
   * @brief 获取当前连接的SSID
   */
  const std::string &getSsid() const { return m_ssid; }

private:
  // 私有构造函数（单例）
  WifiStation() = default;
  ~WifiStation() = default;

  // 事件处理（静态，用于 ESP-IDF 回调）
  static void eventHandler(void *arg, esp_event_base_t eventBase,
                           int32_t eventId, void *eventData);

  // 处理具体事件
  void handleWifiEvent(int32_t eventId);
  void handleIpEvent(int32_t eventId, void *eventData);

  // 成员变量
  bool m_initialized = false;
  WifiState m_state = WifiState::Disconnected;
  std::string m_ssid;
  int m_maxRetry = 5;
  int m_retryCount = 0;

  esp_netif_t *m_netif = nullptr;
  EventGroupHandle_t m_eventGroup = nullptr;
  esp_ip4_addr_t m_ipAddr = {};

  // 事件位
  static constexpr int CONNECTED_BIT = BIT0;
  static constexpr int FAIL_BIT = BIT1;
};
