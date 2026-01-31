#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "device_state.h"
#include <atomic>
#include <mutex>

/**
 * @brief LED 抽象接口
 */
class Led {
public:
    virtual ~Led() = default;
    
    /**
     * @brief 状态变化时更新 LED 表现
     * @param state 新的设备状态
     */
    virtual void onStateChanged(DeviceState state) = 0;
    
    /**
     * @brief 设置亮度
     * @param brightness 0-100
     */
    virtual void setBrightness(uint8_t brightness) = 0;
    
    /**
     * @brief 开灯
     */
    virtual void turnOn() = 0;
    
    /**
     * @brief 关灯
     */
    virtual void turnOff() = 0;
};

/**
 * @brief 无 LED 实现（用于无 LED 的板子）
 */
class NoLed : public Led {
public:
    void onStateChanged(DeviceState) override {}
    void setBrightness(uint8_t) override {}
    void turnOn() override {}
    void turnOff() override {}
};

/**
 * @brief GPIO LED 实现
 * 
 * 使用 LEDC PWM 控制 LED，支持：
 * - 呼吸灯效果（空闲状态）
 * - 快速闪烁（监听状态）
 * - 常亮（播放状态）
 * - 慢闪（配网状态）
 */
class GpioLed : public Led {
public:
    /**
     * @brief 构造函数
     * @param gpio LED GPIO 引脚
     * @param output_invert 是否反转输出（某些 LED 低电平亮）
     */
    GpioLed(gpio_num_t gpio, bool output_invert = false);
    ~GpioLed();

    void onStateChanged(DeviceState state) override;
    void setBrightness(uint8_t brightness) override;
    void turnOn() override;
    void turnOff() override;

    /**
     * @brief 闪烁指定次数
     * @param times 次数，-1 表示无限
     * @param interval_ms 间隔毫秒
     */
    void blink(int times, int interval_ms);
    
    /**
     * @brief 开始呼吸灯
     */
    void startBreathing();
    
    /**
     * @brief 停止所有效果
     */
    void stop();

private:
    gpio_num_t gpio_;
    bool initialized_ = false;
    std::mutex mutex_;
    
    // LEDC 配置
    ledc_channel_config_t ledc_channel_ = {};
    uint32_t duty_ = 0;
    static constexpr uint32_t MAX_DUTY = 8191; // 13-bit
    
    // 闪烁控制
    esp_timer_handle_t blink_timer_ = nullptr;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    static constexpr int BLINK_INFINITE = -1;
    
    // 呼吸灯控制
    bool fade_up_ = true;
    TaskHandle_t fade_task_handle_ = nullptr;
    
    void onBlinkTimer();
    void onFadeEnd();
    static bool IRAM_ATTR fadeCallback(const ledc_cb_param_t* param, void* user_arg);
    static void fadeTask(void* arg);
};

// =============================================================================
// Backward Compatibility C-style API
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LED GPIO (兼容旧 API)
 * @deprecated 请使用 GpioLed 类
 */
void led_flash_init(int gpio);

/**
 * @brief 设置 LED 状态 (兼容旧 API)
 * @deprecated 请使用 GpioLed 类
 * @param gpio LED GPIO
 * @param state 1=开, 0=关
 */
void led_set_state(int gpio, int state);

#ifdef __cplusplus
}
#endif