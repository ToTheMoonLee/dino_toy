#pragma once

#include "esp_err.h"
#include "esp_websocket_client.h"
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

/**
 * @brief WebSocket 对话状态
 */
enum class WsDialogState {
    Idle,           ///< 空闲
    Connecting,     ///< 连接中
    Connected,      ///< 已连接，等待 hello
    Listening,      ///< 录音监听中
    WaitingForResponse, ///< 等待服务器 STT/TTS 响应
    Speaking,       ///< TTS 播放中
};

/**
 * @brief WebSocket 对话配置
 */
struct WebSocketChatConfig {
    std::string url;                     ///< WebSocket 服务器 URL (ws:// 或 wss://)
    std::string device_id;               ///< 设备 ID
    int reconnect_timeout_ms = 10000;    ///< 重连超时
    int buffer_size = 4096;              ///< 接收缓冲区大小
    int sample_rate = 16000;             ///< 音频采样率
};

/**
 * @brief WebSocket 流式对话客户端 (xiaozhi 兼容协议)
 * 
 * 支持实时双向流式对话：
 * - 边录音边上传音频
 * - 边接收边播放回复
 * 
 * @example
 *   auto& ws = WebSocketChat::instance();
 *   ws.init({.url = "ws://192.168.1.10:8000/ws", .device_id = "esp32-xxx"});
 *   ws.setOnStt([](const std::string& text) { ... });
 *   ws.setOnTtsAudio([](const uint8_t* data, size_t len) { ... });
 *   ws.connect();
 *   ws.startListening();
 *   ws.sendAudio(pcm_data, pcm_len);
 *   ws.stopListening();
 */
class WebSocketChat {
public:
    static WebSocketChat& instance();
    
    WebSocketChat(const WebSocketChat&) = delete;
    WebSocketChat& operator=(const WebSocketChat&) = delete;
    
    /**
     * @brief 初始化
     */
    esp_err_t init(const WebSocketChatConfig& config);
    
    /**
     * @brief 连接服务器
     */
    esp_err_t connect();
    
    /**
     * @brief 断开连接
     */
    void disconnect();
    
    /**
     * @brief 是否已连接并完成握手
     */
    bool isReady() const { return state_.load() >= WsDialogState::Connected; }
    
    /**
     * @brief 获取当前状态
     */
    WsDialogState getState() const { return state_.load(); }
    
    /**
     * @brief 获取 session ID
     */
    const std::string& sessionId() const { return session_id_; }

    /**
     * @brief 获取服务器协商的音频采样率
     */
    int serverSampleRate() const { return server_sample_rate_; }
    
    /**
     * @brief 开始监听 (发送 listen start)
     */
    esp_err_t startListening();
    
    /**
     * @brief 停止监听 (发送 listen stop)
     */
    esp_err_t stopListening();
    
    /**
     * @brief 发送二进制音频数据
     * @param data PCM 16-bit mono 数据
     * @param len 数据长度 (字节)
     */
    esp_err_t sendAudio(const uint8_t* data, size_t len);
    
    /**
     * @brief 发送打断信号
     */
    esp_err_t sendAbort();
    
    // ========== 回调设置 ==========
    
    /**
     * @brief STT 识别结果回调
     */
    using SttCallback = std::function<void(const std::string& text)>;
    void setOnStt(SttCallback cb) { on_stt_ = cb; }
    
    /**
     * @brief TTS 状态回调 (start/stop)
     */
    using TtsStateCallback = std::function<void(bool started)>;
    void setOnTtsState(TtsStateCallback cb) { on_tts_state_ = cb; }
    
    /**
     * @brief TTS 音频数据回调
     */
    using TtsAudioCallback = std::function<void(const uint8_t* data, size_t len)>;
    void setOnTtsAudio(TtsAudioCallback cb) { on_tts_audio_ = cb; }
    
    /**
     * @brief 连接状态回调
     */
    using ConnectionCallback = std::function<void(bool connected)>;
    void setOnConnection(ConnectionCallback cb) { on_connection_ = cb; }

private:
    WebSocketChat() = default;
    ~WebSocketChat();
    
    WebSocketChatConfig config_;
    esp_websocket_client_handle_t client_ = nullptr;
    std::atomic<WsDialogState> state_{WsDialogState::Idle};
    bool initialized_ = false;
    std::mutex mutex_;
    
    std::string session_id_;
    int server_sample_rate_ = 16000;

    // RX framing helpers (handle continuation / oversized frames)
    uint8_t rx_continuation_opcode_ = 0;
    std::string rx_text_buf_;
    
    // 回调
    SttCallback on_stt_;
    TtsStateCallback on_tts_state_;
    TtsAudioCallback on_tts_audio_;
    ConnectionCallback on_connection_;
    
    // 发送 JSON 文本
    esp_err_t sendText(const std::string& text);
    
    // 事件处理
    static void eventHandler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
    void handleEvent(esp_websocket_event_data_t* data, int32_t event_id);
    void handleTextMessage(const char* data, size_t len);
    void sendHello();
};
