#pragma once

#include "esp_err.h"
#include <functional>
#include <string>

/**
 * @brief OTA 升级回调类型
 * @param progress 进度 0-100
 * @param speed 下载速度 bytes/s
 */
using OtaProgressCallback = std::function<void(int progress, size_t speed)>;

/**
 * @brief OTA 固件升级管理类
 * 
 * 支持通过 HTTP URL 下载并升级固件
 * 
 * @example
 *   auto& ota = Ota::instance();
 *   ota.upgrade("http://example.com/firmware.bin", [](int p, size_t s) {
 *       ESP_LOGI("OTA", "Progress: %d%%, Speed: %d KB/s", p, s/1024);
 *   });
 */
class Ota {
public:
    static Ota& instance();
    
    Ota(const Ota&) = delete;
    Ota& operator=(const Ota&) = delete;
    
    /**
     * @brief 从 URL 升级固件
     * @param firmware_url 固件下载地址
     * @param callback 进度回调
     * @return ESP_OK 成功启动升级
     */
    esp_err_t upgrade(const std::string& firmware_url, OtaProgressCallback callback = nullptr);
    
    /**
     * @brief 检查是否正在升级
     */
    bool isUpgrading() const { return upgrading_; }
    
    /**
     * @brief 获取当前固件版本
     */
    std::string getCurrentVersion() const;
    
    /**
     * @brief 标记当前版本有效（防止回滚）
     */
    void markValid();
    
    /**
     * @brief 重启设备
     */
    void reboot();

private:
    Ota() = default;
    ~Ota() = default;
    
    bool upgrading_ = false;
    OtaProgressCallback callback_;
};
