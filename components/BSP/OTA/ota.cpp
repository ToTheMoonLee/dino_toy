#include "ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <chrono>

static const char* TAG = "OTA";

Ota& Ota::instance() {
    static Ota instance;
    return instance;
}

std::string Ota::getCurrentVersion() const {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    return app_desc ? app_desc->version : "unknown";
}

void Ota::markValid() {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Marked current firmware as valid");
}

void Ota::reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t Ota::upgrade(const std::string& firmware_url, OtaProgressCallback callback) {
    if (upgrading_) {
        ESP_LOGW(TAG, "Already upgrading");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (firmware_url.empty()) {
        ESP_LOGE(TAG, "Empty firmware URL");
        return ESP_ERR_INVALID_ARG;
    }
    
    upgrading_ = true;
    callback_ = callback;
    
    ESP_LOGI(TAG, "Starting OTA upgrade from: %s", firmware_url.c_str());
    ESP_LOGI(TAG, "Current version: %s", getCurrentVersion().c_str());
    
    // 配置 HTTP 客户端
    esp_http_client_config_t http_config = {};
    http_config.url = firmware_url.c_str();
    http_config.timeout_ms = 30000;
    http_config.buffer_size = 4096;
    http_config.buffer_size_tx = 1024;
    http_config.keep_alive_enable = true;
    
    // 配置 OTA
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    
    esp_https_ota_handle_t ota_handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        upgrading_ = false;
        return err;
    }
    
    // 获取固件大小
    int image_size = esp_https_ota_get_image_size(ota_handle);
    ESP_LOGI(TAG, "Firmware size: %d bytes", image_size);
    
    // 下载并写入固件
    int downloaded = 0;
    auto start_time = std::chrono::steady_clock::now();
    int last_progress = -1;
    
    while (true) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        downloaded = esp_https_ota_get_image_len_read(ota_handle);
        int progress = (image_size > 0) ? (downloaded * 100 / image_size) : 0;
        
        // 每 5% 报告一次进度
        if (progress != last_progress && progress % 5 == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            size_t speed = (elapsed > 0) ? (downloaded * 1000 / elapsed) : 0;
            
            ESP_LOGI(TAG, "Progress: %d%%, Speed: %d KB/s", progress, (int)(speed / 1024));
            
            if (callback_) {
                callback_(progress, speed);
            }
            last_progress = progress;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        upgrading_ = false;
        return err;
    }
    
    // 验证固件
    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(ota_handle);
        upgrading_ = false;
        return ESP_ERR_INVALID_SIZE;
    }
    
    // 完成 OTA
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        upgrading_ = false;
        return err;
    }
    
    ESP_LOGI(TAG, "OTA upgrade successful!");
    
    if (callback_) {
        callback_(100, 0);
    }
    
    upgrading_ = false;
    return ESP_OK;
}
