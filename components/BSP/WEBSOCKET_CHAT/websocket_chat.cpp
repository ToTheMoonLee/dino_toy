#include "websocket_chat.h"
#include "esp_log.h"
#include "cJSON.h"

static const char* TAG = "WebSocketChat";

WebSocketChat& WebSocketChat::instance() {
    static WebSocketChat instance;
    return instance;
}

WebSocketChat::~WebSocketChat() {
    disconnect();
    if (client_) {
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
}

esp_err_t WebSocketChat::init(const WebSocketChatConfig& config) {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    if (config.url.empty()) {
        ESP_LOGE(TAG, "Empty WebSocket URL");
        return ESP_ERR_INVALID_ARG;
    }
    
    config_ = config;
    
    // 配置 WebSocket 客户端
    esp_websocket_client_config_t ws_config = {};
    ws_config.uri = config_.url.c_str();
    ws_config.buffer_size = config_.buffer_size;
    ws_config.reconnect_timeout_ms = config_.reconnect_timeout_ms;
    ws_config.network_timeout_ms = 10000;
    ws_config.ping_interval_sec = 30;
    ws_config.pingpong_timeout_sec = 10;
    
    client_ = esp_websocket_client_init(&ws_config);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    // Add a few headers for better xiaozhi compatibility (safe for servers
    // that ignore unknown headers).
    (void)esp_websocket_client_append_header(client_, "Protocol-Version", "1");
    if (!config_.device_id.empty()) {
        (void)esp_websocket_client_append_header(client_, "Device-Id", config_.device_id.c_str());
        (void)esp_websocket_client_append_header(client_, "Client-Id", config_.device_id.c_str());
    }

    // 注册事件处理
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, eventHandler, this);
    
    initialized_ = true;
    state_.store(WsDialogState::Idle);
    ESP_LOGI(TAG, "WebSocket client initialized, URL: %s", config_.url.c_str());
    return ESP_OK;
}

esp_err_t WebSocketChat::connect() {
    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (state_.load() != WsDialogState::Idle) {
        ESP_LOGW(TAG, "Already connected or connecting");
        return ESP_OK;
    }
    
    state_.store(WsDialogState::Connecting);
    ESP_LOGI(TAG, "Connecting to WebSocket server...");
    
    esp_err_t err = esp_websocket_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        state_.store(WsDialogState::Idle);
    }
    return err;
}

void WebSocketChat::disconnect() {
    if (!initialized_ || !client_) {
        return;
    }
    
    if (esp_websocket_client_is_connected(client_)) {
        ESP_LOGI(TAG, "Disconnecting...");
        esp_websocket_client_stop(client_);
    }
    state_.store(WsDialogState::Idle);
    session_id_.clear();
}

esp_err_t WebSocketChat::sendText(const std::string& text) {
    if (state_.load() < WsDialogState::Connected) {
        ESP_LOGW(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    int sent = esp_websocket_client_send_text(client_, text.c_str(), text.length(), portMAX_DELAY);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send text");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Sent: %s", text.c_str());
    return ESP_OK;
}

esp_err_t WebSocketChat::sendAudio(const uint8_t* data, size_t len) {
    if (state_.load() != WsDialogState::Listening) {
        return ESP_ERR_INVALID_STATE;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    int sent = esp_websocket_client_send_bin(client_, (const char*)data, len, portMAX_DELAY);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send audio data");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

void WebSocketChat::sendHello() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);

    // Align with xiaozhi style hello (server may ignore extra fields).
    cJSON_AddStringToObject(root, "transport", "websocket");
    
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "pcm");
    cJSON_AddNumberToObject(audio_params, "sample_rate", config_.sample_rate);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    
    char* str = cJSON_PrintUnformatted(root);
    sendText(str);
    
    free(str);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Sent hello");
}

esp_err_t WebSocketChat::startListening() {
    if (state_.load() != WsDialogState::Connected) {
        ESP_LOGW(TAG, "Cannot start listening: not in Connected state");
        return ESP_ERR_INVALID_STATE;
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    cJSON_AddStringToObject(root, "mode", "auto");
    
    char* str = cJSON_PrintUnformatted(root);
    esp_err_t err = sendText(str);
    
    free(str);
    cJSON_Delete(root);
    
    if (err == ESP_OK) {
        state_.store(WsDialogState::Listening);
        ESP_LOGI(TAG, "Start listening");
    }
    return err;
}

esp_err_t WebSocketChat::stopListening() {
    if (state_.load() != WsDialogState::Listening) {
        return ESP_ERR_INVALID_STATE;
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "stop");
    
    char* str = cJSON_PrintUnformatted(root);
    esp_err_t err = sendText(str);
    
    free(str);
    cJSON_Delete(root);
    
    if (err == ESP_OK) {
        // Transition to WaitingForResponse - waiting for STT/TTS from server
        state_.store(WsDialogState::WaitingForResponse);
        ESP_LOGI(TAG, "Stop listening, waiting for response");
    }
    return err;
}

esp_err_t WebSocketChat::sendAbort() {
    if (state_.load() < WsDialogState::Connected) {
        return ESP_ERR_INVALID_STATE;
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "reason", "user_interrupt");
    
    char* str = cJSON_PrintUnformatted(root);
    esp_err_t err = sendText(str);
    
    free(str);
    cJSON_Delete(root);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent abort");
    }
    return err;
}

void WebSocketChat::eventHandler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
    auto* self = static_cast<WebSocketChat*>(arg);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
    self->handleEvent(data, event_id);
}

void WebSocketChat::handleEvent(esp_websocket_event_data_t* data, int32_t event_id) {
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            // 先设置为 Connected 才能发送 hello
            state_.store(WsDialogState::Connected);
            sendHello();
            // hello 发送后暂时回到 Connecting，等待服务器 hello 响应
            state_.store(WsDialogState::Connecting);
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            state_.store(WsDialogState::Idle);
            session_id_.clear();
            rx_continuation_opcode_ = 0;
            rx_text_buf_.clear();
            if (on_connection_) {
                on_connection_(false);
            }
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->data_ptr && data->data_len > 0) {
                const uint8_t raw_op = data->op_code;
                uint8_t op = raw_op;

                // Handle RFC6455 continuation frames (opcode 0x00) by tracking
                // the last non-continuation opcode.
                if (op == 0x00) {
                    op = rx_continuation_opcode_;
                } else if (op == 0x01 || op == 0x02) {
                    rx_continuation_opcode_ = op;
                }

                const bool frame_done =
                    (data->payload_len <= 0) ? data->fin
                                             : ((data->payload_offset + data->data_len) >= data->payload_len);

                if (op == 0x01) {
                    // Text message (JSON). Buffer until FIN + end-of-frame to
                    // support oversized frames and continuation.
                    if (raw_op == 0x01 && data->payload_offset == 0) {
                        rx_text_buf_.clear();
                        if (data->payload_len > 0) {
                            rx_text_buf_.reserve((size_t)data->payload_len);
                        }
                    }
                    rx_text_buf_.append(data->data_ptr, (size_t)data->data_len);

                    if (data->fin && frame_done) {
                        // Log only a small prefix; frequent INFO logs may cause audio glitches.
                        ESP_LOGD(TAG, "Text msg len=%u", (unsigned)rx_text_buf_.size());
                        if (!rx_text_buf_.empty()) {
                            handleTextMessage(rx_text_buf_.c_str(), rx_text_buf_.size());
                        }
                        rx_text_buf_.clear();
                    }
                } else if (op == 0x02) {
                    // Binary message (audio data)
                    if (state_.load() == WsDialogState::Speaking && on_tts_audio_) {
                        on_tts_audio_((const uint8_t*)data->data_ptr, (size_t)data->data_len);
                    }
                }

                // Clear opcode tracking when the message is finished.
                if (data->fin && frame_done) {
                    rx_continuation_opcode_ = 0;
                }
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            // ERROR may not always be followed by DISCONNECTED; reset state so
            // upper layers can recover.
            state_.store(WsDialogState::Idle);
            session_id_.clear();
            rx_continuation_opcode_ = 0;
            rx_text_buf_.clear();
            if (on_connection_) {
                on_connection_(false);
            }
            break;

        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGI(TAG, "WebSocket closed");
            state_.store(WsDialogState::Idle);
            session_id_.clear();
            rx_continuation_opcode_ = 0;
            rx_text_buf_.clear();
            if (on_connection_) {
                on_connection_(false);
            }
            break;
            
        default:
            break;
    }
}

void WebSocketChat::handleTextMessage(const char* data, size_t len) {
    if (!data || len == 0) {
        return;
    }
    
    // 解析 JSON
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON");
        return;
    }
    
    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return;
    }
    
    const char* type = type_item->valuestring;
    
    if (strcmp(type, "hello") == 0) {
        // 服务器 hello 响应
        cJSON* session_id_item = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(session_id_item)) {
            session_id_ = session_id_item->valuestring;
        }
        
        cJSON* audio_params = cJSON_GetObjectItem(root, "audio_params");
        if (cJSON_IsObject(audio_params)) {
            cJSON* sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
            if (cJSON_IsNumber(sample_rate)) {
                server_sample_rate_ = sample_rate->valueint;
            }
        }
        
        state_.store(WsDialogState::Connected);
        ESP_LOGI(TAG, "Hello handshake complete, session_id=%s, server_sr=%d", 
                 session_id_.c_str(), server_sample_rate_);

        if (on_connection_) {
            on_connection_(true);
        }
        
    } else if (strcmp(type, "stt") == 0) {
        // STT 识别结果
        cJSON* text_item = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text_item) && on_stt_) {
            on_stt_(text_item->valuestring);
        }
        ESP_LOGI(TAG, "STT: %s", cJSON_IsString(text_item) ? text_item->valuestring : "");
        
    } else if (strcmp(type, "tts") == 0) {
        // TTS 状态
        cJSON* state_item = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state_item)) {
            const char* tts_state = state_item->valuestring;
            
            if (strcmp(tts_state, "start") == 0) {
                // Accept TTS start from WaitingForResponse or Connected state
                auto cur_state = state_.load();
                if (cur_state == WsDialogState::WaitingForResponse ||
                    cur_state == WsDialogState::Connected) {
                    state_.store(WsDialogState::Speaking);
                    if (on_tts_state_) {
                        on_tts_state_(true);
                    }
                    ESP_LOGI(TAG, "TTS start");
                } else {
                    ESP_LOGW(TAG, "TTS start in unexpected state: %d", (int)cur_state);
                }
                
            } else if (strcmp(tts_state, "stop") == 0) {
                state_.store(WsDialogState::Connected);
                if (on_tts_state_) {
                    on_tts_state_(false);
                }
                ESP_LOGI(TAG, "TTS stop");
            }
        }
    }
    
    cJSON_Delete(root);
}
