#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "device_state.h"
#include <string>
#include <mutex>

/**
 * @brief 显示屏抽象接口
 */
class Display {
public:
    virtual ~Display() = default;
    
    /**
     * @brief 设置状态文字
     */
    virtual void setStatus(const char* status) = 0;
    
    /**
     * @brief 设置表情
     * @param emotion 表情名称: "neutral", "happy", "sad", "thinking", "listening"
     */
    virtual void setEmotion(const char* emotion) = 0;
    
    /**
     * @brief 显示通知
     */
    virtual void showNotification(const char* msg, int duration_ms = 3000) = 0;
    
    /**
     * @brief 设置聊天消息
     */
    virtual void setChatMessage(const char* role, const char* content) = 0;
    
    /**
     * @brief 状态变化时更新显示
     */
    virtual void onStateChanged(DeviceState state) = 0;
    
    int width() const { return width_; }
    int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;
};

/**
 * @brief 无显示屏实现
 */
class NoDisplay : public Display {
public:
    void setStatus(const char*) override {}
    void setEmotion(const char*) override {}
    void showNotification(const char*, int) override {}
    void setChatMessage(const char*, const char*) override {}
    void onStateChanged(DeviceState) override {}
};

/**
 * @brief ST7789 LCD 显示配置
 */
struct ST7789Config {
    // SPI 配置
    spi_host_device_t spi_host = SPI2_HOST;
    gpio_num_t pin_mosi = GPIO_NUM_NC;
    gpio_num_t pin_sclk = GPIO_NUM_NC;
    gpio_num_t pin_cs = GPIO_NUM_NC;
    gpio_num_t pin_dc = GPIO_NUM_NC;
    gpio_num_t pin_rst = GPIO_NUM_NC;
    gpio_num_t pin_bl = GPIO_NUM_NC;  // 背光引脚
    
    // 显示配置
    int width = 240;
    int height = 240;
    int offset_x = 0;
    int offset_y = 0;
    bool mirror_x = false;
    bool mirror_y = false;
    bool swap_xy = false;
    bool invert_color = true;  // ST7789 通常需要颜色反转
    
    int spi_freq_hz = 40 * 1000 * 1000;  // 40MHz
};

/**
 * @brief ST7789 LCD 显示实现
 * 
 * 支持功能：
 * - 表情动画显示
 * - 状态文字显示
 * - 聊天消息显示
 */
class ST7789Display : public Display {
public:
    static ST7789Display& instance();
    
    ST7789Display(const ST7789Display&) = delete;
    ST7789Display& operator=(const ST7789Display&) = delete;
    
    /**
     * @brief 初始化显示屏
     */
    esp_err_t init(const ST7789Config& config);
    
    void setStatus(const char* status) override;
    void setEmotion(const char* emotion) override;
    void showNotification(const char* msg, int duration_ms = 3000) override;
    void setChatMessage(const char* role, const char* content) override;
    void onStateChanged(DeviceState state) override;
    
    /**
     * @brief 设置背光亮度
     * @param level 0-100
     */
    void setBacklight(uint8_t level);
    
    /**
     * @brief 清屏
     */
    void clear(uint16_t color = 0x0000);
    
    /**
     * @brief 绘制文字
     */
    void drawText(int x, int y, const char* text, uint16_t color = 0xFFFF);
    
    /**
     * @brief 填充矩形
     */
    void fillRect(int x, int y, int w, int h, uint16_t color);

private:
    ST7789Display() = default;
    ~ST7789Display();
    
    ST7789Config config_;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t io_handle_ = nullptr;
    bool initialized_ = false;
    std::mutex mutex_;
    
    std::string current_emotion_;
    std::string current_status_;
    
    void drawEmotion(const char* emotion);
    void drawStatusBar();
    void initBacklight();
};
