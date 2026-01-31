#include "led.h"
#include "esp_log.h"

static const char* TAG = "GpioLed";

// 亮度常量
static constexpr uint8_t IDLE_BRIGHTNESS = 5;
static constexpr uint8_t LISTENING_BRIGHTNESS = 80;
static constexpr uint8_t SPEAKING_BRIGHTNESS = 60;
static constexpr uint8_t DEFAULT_BRIGHTNESS = 50;

// LEDC 配置
static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_1;
static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_0;
static constexpr int FADE_TIME_MS = 1000;

GpioLed::GpioLed(gpio_num_t gpio, bool output_invert) : gpio_(gpio) {
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "LED GPIO not configured");
        return;
    }

    // 配置 LEDC 定时器
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    // 配置 LEDC 通道
    ledc_channel_ = {
        .gpio_num = gpio,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert ? 1u : 0u,
        },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_));

    // 安装渐变服务
    ledc_fade_func_install(0);

    // 注册渐变回调
    ledc_cbs_t callbacks = {
        .fade_cb = fadeCallback,
    };
    ledc_cb_register(LEDC_MODE, LEDC_CHANNEL, &callbacks, this);

    // 创建闪烁定时器
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<GpioLed*>(arg)->onBlinkTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &blink_timer_));

    // 创建渐变任务
    xTaskCreate(fadeTask, "led_fade", 2048, this, tskIDLE_PRIORITY + 2, &fade_task_handle_);

    initialized_ = true;
    ESP_LOGI(TAG, "LED initialized on GPIO %d", gpio);
}

GpioLed::~GpioLed() {
    if (blink_timer_) {
        esp_timer_stop(blink_timer_);
        esp_timer_delete(blink_timer_);
    }
    if (fade_task_handle_) {
        vTaskDelete(fade_task_handle_);
    }
    if (initialized_) {
        ledc_fade_stop(LEDC_MODE, LEDC_CHANNEL);
        ledc_fade_func_uninstall();
    }
}

void GpioLed::setBrightness(uint8_t brightness) {
    if (brightness >= 100) {
        duty_ = MAX_DUTY;
    } else {
        duty_ = brightness * MAX_DUTY / 100;
    }
}

void GpioLed::turnOn() {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(LEDC_MODE, LEDC_CHANNEL);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty_);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void GpioLed::turnOff() {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(LEDC_MODE, LEDC_CHANNEL);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void GpioLed::stop() {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(LEDC_MODE, LEDC_CHANNEL);
}

void GpioLed::blink(int times, int interval_ms) {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(LEDC_MODE, LEDC_CHANNEL);
    
    blink_counter_ = (times == BLINK_INFINITE) ? BLINK_INFINITE : times * 2;
    blink_interval_ms_ = interval_ms;
    esp_timer_start_periodic(blink_timer_, interval_ms * 1000);
}

void GpioLed::onBlinkTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (blink_counter_ != BLINK_INFINITE) {
        blink_counter_--;
    }
    
    // 交替亮灭
    if ((blink_counter_ & 1) || blink_counter_ == BLINK_INFINITE) {
        static bool on = false;
        on = !on;
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, on ? duty_ : 0);
    } else {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
        if (blink_counter_ == 0) {
            esp_timer_stop(blink_timer_);
        }
    }
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void GpioLed::startBreathing() {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    ledc_fade_stop(LEDC_MODE, LEDC_CHANNEL);
    
    fade_up_ = true;
    ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL, MAX_DUTY, FADE_TIME_MS);
    ledc_fade_start(LEDC_MODE, LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
}

void GpioLed::onFadeEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    fade_up_ = !fade_up_;
    ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL, fade_up_ ? MAX_DUTY : 0, FADE_TIME_MS);
    ledc_fade_start(LEDC_MODE, LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
}

bool IRAM_ATTR GpioLed::fadeCallback(const ledc_cb_param_t* param, void* user_arg) {
    if (param->event == LEDC_FADE_END_EVT) {
        auto led = static_cast<GpioLed*>(user_arg);
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTaskNotifyFromISR(led->fade_task_handle_, 0x01, eSetValueWithOverwrite,
                           &xHigherPriorityTaskWoken);
        return xHigherPriorityTaskWoken == pdTRUE;
    }
    return false;
}

void GpioLed::fadeTask(void* arg) {
    GpioLed* led = static_cast<GpioLed*>(arg);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        led->onFadeEnd();
    }
}

void GpioLed::onStateChanged(DeviceState state) {
    ESP_LOGD(TAG, "State changed to: %s", GetDeviceStateName(state));
    
    switch (state) {
        case kDeviceStateStarting:
            setBrightness(DEFAULT_BRIGHTNESS);
            blink(BLINK_INFINITE, 100);  // 快闪
            break;
            
        case kDeviceStateWifiConfiguring:
            setBrightness(DEFAULT_BRIGHTNESS);
            blink(BLINK_INFINITE, 500);  // 慢闪
            break;
            
        case kDeviceStateIdle:
            setBrightness(IDLE_BRIGHTNESS);
            startBreathing();  // 呼吸灯
            break;
            
        case kDeviceStateListening:
            setBrightness(LISTENING_BRIGHTNESS);
            startBreathing();  // 呼吸灯（较亮）
            break;
            
        case kDeviceStateProcessing:
            setBrightness(DEFAULT_BRIGHTNESS);
            blink(BLINK_INFINITE, 200);  // 中速闪烁
            break;
            
        case kDeviceStateSpeaking:
            setBrightness(SPEAKING_BRIGHTNESS);
            turnOn();  // 常亮
            break;
            
        case kDeviceStateUpgrading:
            setBrightness(DEFAULT_BRIGHTNESS);
            blink(BLINK_INFINITE, 100);  // 快闪
            break;
            
        case kDeviceStateError:
            setBrightness(100);
            blink(BLINK_INFINITE, 200);  // 高亮快闪
            break;
            
        default:
            turnOff();
            break;
    }
}

// =============================================================================
// Backward Compatibility C-style API Implementation
// =============================================================================

static gpio_num_t s_legacy_led_gpio = GPIO_NUM_NC;
static bool s_legacy_led_initialized = false;

extern "C" void led_flash_init(int gpio) {
    s_legacy_led_gpio = static_cast<gpio_num_t>(gpio);
    
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << gpio);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    
    s_legacy_led_initialized = true;
    ESP_LOGI(TAG, "Legacy LED initialized on GPIO %d", gpio);
}

extern "C" void led_set_state(int gpio, int state) {
    if (gpio >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(gpio), state);
    } else if (s_legacy_led_initialized) {
        gpio_set_level(s_legacy_led_gpio, state);
    }
}