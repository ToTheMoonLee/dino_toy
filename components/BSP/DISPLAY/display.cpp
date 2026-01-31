#include "display.h"
#include "esp_log.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "ST7789Display";

// 简单字体数据 (5x7 ASCII, 简化版本)
// 完整实现应使用 LVGL 或自定义字体库
static const uint8_t FONT_5X7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // Space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    // ... 可扩展更多字符
};

// 表情图案 (简化的像素表情)
struct EmotionPattern {
    const char* name;
    uint16_t eye_color;
    uint16_t mouth_color;
    bool eyes_open;
    int mouth_type;  // 0=neutral, 1=smile, 2=sad, 3=open
};

static const EmotionPattern EMOTIONS[] = {
    {"neutral",   0xFFFF, 0xFFFF, true,  0},
    {"happy",     0xFFE0, 0xFFE0, true,  1},  // 黄色
    {"sad",       0x001F, 0x001F, true,  2},  // 蓝色
    {"thinking",  0x07FF, 0x07FF, false, 0},  // 青色
    {"listening", 0x07E0, 0x07E0, true,  3},  // 绿色
    {"speaking",  0xF81F, 0xF81F, true,  3},  // 紫色
    {"error",     0xF800, 0xF800, true,  2},  // 红色
};

ST7789Display& ST7789Display::instance() {
    static ST7789Display instance;
    return instance;
}

ST7789Display::~ST7789Display() {
    if (panel_) {
        esp_lcd_panel_del(panel_);
    }
    if (io_handle_) {
        esp_lcd_panel_io_del(io_handle_);
    }
}

esp_err_t ST7789Display::init(const ST7789Config& config) {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    config_ = config;
    width_ = config.width;
    height_ = config.height;
    
    ESP_LOGI(TAG, "Initializing ST7789 display %dx%d", width_, height_);
    
    // 初始化 SPI 总线
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = config.pin_mosi;
    bus_config.miso_io_num = GPIO_NUM_NC;
    bus_config.sclk_io_num = config.pin_sclk;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = width_ * height_ * 2;
    
    esp_err_t ret = spi_bus_initialize(config.spi_host, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置 LCD IO
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = config.pin_cs;
    io_config.dc_gpio_num = config.pin_dc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = config.spi_freq_hz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config.spi_host, 
                                    &io_config, &io_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD IO init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置 LCD 面板
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = config.pin_rst;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    
    ret = esp_lcd_new_panel_st7789(io_handle_, &panel_config, &panel_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 复位和初始化面板
    esp_lcd_panel_reset(panel_);
    esp_lcd_panel_init(panel_);
    
    // 设置方向
    esp_lcd_panel_mirror(panel_, config.mirror_x, config.mirror_y);
    esp_lcd_panel_swap_xy(panel_, config.swap_xy);
    
    // 颜色反转
    if (config.invert_color) {
        esp_lcd_panel_invert_color(panel_, true);
    }
    
    // 设置显示偏移
    esp_lcd_panel_set_gap(panel_, config.offset_x, config.offset_y);
    
    // 开启显示
    esp_lcd_panel_disp_on_off(panel_, true);
    
    // 初始化背光
    initBacklight();
    setBacklight(100);
    
    // 清屏
    clear(0x0000);
    
    initialized_ = true;
    ESP_LOGI(TAG, "ST7789 display initialized");
    
    // 显示默认表情
    setEmotion("neutral");
    
    return ESP_OK;
}

void ST7789Display::initBacklight() {
    if (config_.pin_bl == GPIO_NUM_NC) {
        return;
    }
    
    // 使用 LEDC 控制背光
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ledc_timer_config(&timer_config);
    
    ledc_channel_config_t channel_config = {
        .gpio_num = config_.pin_bl,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = 0},
    };
    ledc_channel_config(&channel_config);
}

void ST7789Display::setBacklight(uint8_t level) {
    if (config_.pin_bl == GPIO_NUM_NC) {
        return;
    }
    
    uint32_t duty = (level * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void ST7789Display::clear(uint16_t color) {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 分块填充以节省内存
    const int block_height = 20;
    size_t buf_size = width_ * block_height * sizeof(uint16_t);
    uint16_t* buf = (uint16_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return;
    }
    
    // 填充颜色
    for (int i = 0; i < width_ * block_height; i++) {
        buf[i] = __builtin_bswap16(color);  // 字节序转换
    }
    
    // 分块绘制
    for (int y = 0; y < height_; y += block_height) {
        int h = (y + block_height <= height_) ? block_height : (height_ - y);
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + h, buf);
    }
    
    free(buf);
}

void ST7789Display::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 边界检查
    if (x >= width_ || y >= height_) return;
    if (x + w > width_) w = width_ - x;
    if (y + h > height_) h = height_ - y;
    
    size_t buf_size = w * h * sizeof(uint16_t);
    uint16_t* buf = (uint16_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!buf) {
        return;
    }
    
    uint16_t swapped = __builtin_bswap16(color);
    for (int i = 0; i < w * h; i++) {
        buf[i] = swapped;
    }
    
    esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, buf);
    free(buf);
}

void ST7789Display::drawText(int x, int y, const char* text, uint16_t color) {
    if (!initialized_ || !text) return;
    
    // 简化实现：仅绘制占位矩形
    // 完整实现需要字体渲染库
    int text_len = strlen(text);
    int text_width = text_len * 8;  // 假设每字符8像素宽
    int text_height = 16;
    
    fillRect(x, y, text_width, text_height, color);
}

void ST7789Display::setStatus(const char* status) {
    if (!status) return;
    current_status_ = status;
    drawStatusBar();
}

void ST7789Display::drawStatusBar() {
    if (!initialized_) return;
    
    // 清除状态栏区域
    fillRect(0, height_ - 30, width_, 30, 0x0000);
    
    // 绘制状态文字背景
    if (!current_status_.empty()) {
        fillRect(10, height_ - 25, width_ - 20, 20, 0x2104);  // 深灰背景
    }
}

void ST7789Display::setEmotion(const char* emotion) {
    if (!emotion) return;
    current_emotion_ = emotion;
    drawEmotion(emotion);
}

void ST7789Display::drawEmotion(const char* emotion) {
    if (!initialized_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 查找表情
    const EmotionPattern* pattern = &EMOTIONS[0];  // 默认 neutral
    for (const auto& e : EMOTIONS) {
        if (strcmp(e.name, emotion) == 0) {
            pattern = &e;
            break;
        }
    }
    
    // 清除表情区域
    int face_y = 20;
    int face_h = height_ - 60;
    fillRect(0, face_y, width_, face_h, 0x0000);
    
    // 绘制眼睛
    int eye_y = face_y + face_h / 3;
    int eye_size = 30;
    int eye_gap = 60;
    int left_eye_x = width_ / 2 - eye_gap / 2 - eye_size / 2;
    int right_eye_x = width_ / 2 + eye_gap / 2 - eye_size / 2;
    
    if (pattern->eyes_open) {
        // 圆形眼睛
        fillRect(left_eye_x, eye_y, eye_size, eye_size, pattern->eye_color);
        fillRect(right_eye_x, eye_y, eye_size, eye_size, pattern->eye_color);
    } else {
        // 闭眼 (横线)
        fillRect(left_eye_x, eye_y + eye_size / 2 - 3, eye_size, 6, pattern->eye_color);
        fillRect(right_eye_x, eye_y + eye_size / 2 - 3, eye_size, 6, pattern->eye_color);
    }
    
    // 绘制嘴巴
    int mouth_y = eye_y + eye_size + 30;
    int mouth_x = width_ / 2 - 30;
    int mouth_w = 60;
    int mouth_h = 15;
    
    switch (pattern->mouth_type) {
        case 0:  // Neutral
            fillRect(mouth_x, mouth_y, mouth_w, mouth_h / 3, pattern->mouth_color);
            break;
        case 1:  // Smile (弧形用矩形近似)
            fillRect(mouth_x, mouth_y, mouth_w, mouth_h / 3, pattern->mouth_color);
            fillRect(mouth_x + 5, mouth_y - 5, 10, 5, pattern->mouth_color);
            fillRect(mouth_x + mouth_w - 15, mouth_y - 5, 10, 5, pattern->mouth_color);
            break;
        case 2:  // Sad
            fillRect(mouth_x, mouth_y, mouth_w, mouth_h / 3, pattern->mouth_color);
            fillRect(mouth_x + 5, mouth_y + 5, 10, 5, pattern->mouth_color);
            fillRect(mouth_x + mouth_w - 15, mouth_y + 5, 10, 5, pattern->mouth_color);
            break;
        case 3:  // Open
            fillRect(mouth_x + 10, mouth_y - 10, mouth_w - 20, mouth_h + 10, pattern->mouth_color);
            break;
    }
    
    ESP_LOGD(TAG, "Drew emotion: %s", emotion);
}

void ST7789Display::showNotification(const char* msg, int duration_ms) {
    if (!msg) return;
    
    // 显示通知
    fillRect(10, height_ / 2 - 20, width_ - 20, 40, 0x4208);  // 灰色背景
    
    // 简化：延时后清除
    // 完整实现应使用定时器
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    fillRect(10, height_ / 2 - 20, width_ - 20, 40, 0x0000);
    
    // 重绘表情
    drawEmotion(current_emotion_.c_str());
}

void ST7789Display::setChatMessage(const char* role, const char* content) {
    if (!content) return;
    
    // 在底部显示聊天消息
    fillRect(5, height_ - 60, width_ - 10, 25, 0x2104);
    // 完整实现需要字体渲染
}

void ST7789Display::onStateChanged(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            setEmotion("neutral");
            setStatus("待机");
            break;
        case kDeviceStateListening:
            setEmotion("listening");
            setStatus("聆听中...");
            break;
        case kDeviceStateProcessing:
            setEmotion("thinking");
            setStatus("思考中...");
            break;
        case kDeviceStateSpeaking:
            setEmotion("speaking");
            setStatus("说话中...");
            break;
        case kDeviceStateWifiConfiguring:
            setEmotion("thinking");
            setStatus("配网中...");
            break;
        case kDeviceStateError:
            setEmotion("error");
            setStatus("错误");
            break;
        default:
            break;
    }
}
