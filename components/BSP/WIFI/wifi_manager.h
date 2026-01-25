#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <cstdint>
#include <functional>
#include <string>

/**
 * @brief WiFi + Web 配网/控制配置
 */
struct WifiManagerConfig {
  // SoftAP 配网热点（没有保存 WiFi 或 STA 连接失败时启用）
  std::string ap_ssid = "ESP32-Setup";
  std::string ap_password = ""; // 为空表示开放热点；若设置密码需 >= 8
  uint8_t ap_channel = 1;
  uint8_t ap_max_conn = 4;

  // STA 连接参数
  int sta_connect_timeout_ms = 15000; // 配网页面提交后等待连接结果的超时
  int sta_max_retry = 5;

  // 若为 true：STA 连上后依旧保持 AP 不关闭（手机可一直连热点控制）
  bool keep_ap_on_after_sta_connected = false;
};

/**
 * @brief Web 控制命令回调
 * @param command_id 与 WakeWord 命令 ID 一致 (0-4)
 */
using WifiWebCommandCallback = std::function<void(int command_id)>;

/**
 * @brief Web 状态回调（返回 JSON 对象字符串，例如 {"led":true}）
 */
using WifiWebStatusJsonCallback = std::function<std::string(void)>;

/**
 * @brief Web TTS 回调（传入要朗读的文本）
 */
using WifiWebTtsCallback = std::function<void(const std::string &text)>;

/**
 * @brief WiFi 管理器：支持网页配网 + 网页控制
 *
 * - 无 WiFi 配置 / 连接失败：启动 SoftAP，用户连接热点后打开 192.168.4.1 进行配网
 * - 连接成功：通过路由器分配的 IP 访问网页控制（同时可选保留 AP）
 */
class WifiManager {
public:
  static WifiManager &instance();

  WifiManager(const WifiManager &) = delete;
  WifiManager &operator=(const WifiManager &) = delete;
  WifiManager(WifiManager &&) = delete;
  WifiManager &operator=(WifiManager &&) = delete;

  esp_err_t init(const WifiManagerConfig &config = WifiManagerConfig{});

  /**
   * @brief 启动 WiFi 与 Web 服务
   */
  esp_err_t start();

  /**
   * @brief 设置网页控制命令回调
   */
  void setCommandCallback(WifiWebCommandCallback cb) { m_cmdCb = cb; }

  /**
   * @brief 设置网页状态回调（返回 JSON 对象字符串）
   */
  void setStatusCallback(WifiWebStatusJsonCallback cb) { m_statusCb = cb; }

  /**
   * @brief 设置网页 TTS 回调（把文本交给外部去合成/播放）
   */
  void setTtsCallback(WifiWebTtsCallback cb) { m_ttsCb = cb; }

  bool isStaConnected() const;
  std::string getStaIpAddress() const;

  bool isApRunning() const { return m_apRunning; }
  std::string getApIpAddress() const;

private:
  WifiManager() = default;
  ~WifiManager() = default;

  // WiFi/NVS
  esp_err_t initNvs();
  esp_err_t initWifiDriver();
  esp_err_t loadCredentials(std::string &ssidOut, std::string &passOut);
  esp_err_t saveCredentials(const std::string &ssid, const std::string &pass);
  esp_err_t startSta(const std::string &ssid, const std::string &pass,
                     bool withAp);
  esp_err_t startAp();
  void stopAp();

  // 事件处理
  static void eventHandler(void *arg, esp_event_base_t eventBase,
                           int32_t eventId, void *eventData);
  void onWifiEvent(int32_t eventId, void *eventData);
  void onIpEvent(int32_t eventId, void *eventData);

  // HTTP Server
  esp_err_t startWebServer();
  void stopWebServer();

  // HTTP handlers
  static esp_err_t handleRoot(httpd_req_t *req);
  static esp_err_t handleWifiPage(httpd_req_t *req);
  static esp_err_t handleStatus(httpd_req_t *req);
  static esp_err_t handleCmd(httpd_req_t *req);
  static esp_err_t handleTts(httpd_req_t *req);
  static esp_err_t handleWifiSave(httpd_req_t *req);

  // helpers
  static std::string urlDecode(const std::string &in);
  static bool parseFormUrlEncoded(const std::string &body, std::string &ssidOut,
                                  std::string &passOut);

  WifiManagerConfig m_cfg;
  bool m_initialized = false;

  // WiFi runtime
  bool m_apRunning = false;
  bool m_deferStopAp = false; // 用于配网页面提交后，延迟关 AP，避免断开 HTTP 会话
  std::string m_staSsid;
  std::string m_staPass;
  esp_ip4_addr_t m_staIp = {};
  int m_staRetryCount = 0;

  esp_netif_t *m_staNetif = nullptr;
  esp_netif_t *m_apNetif = nullptr;
  EventGroupHandle_t m_eventGroup = nullptr;

  // HTTP server handle
  httpd_handle_t m_httpd = nullptr;

  // Callbacks
  WifiWebCommandCallback m_cmdCb = nullptr;
  WifiWebStatusJsonCallback m_statusCb = nullptr;
  WifiWebTtsCallback m_ttsCb = nullptr;

  // event bits
  static constexpr int STA_CONNECTED_BIT = BIT0;
  static constexpr int STA_FAIL_BIT = BIT1;
};
