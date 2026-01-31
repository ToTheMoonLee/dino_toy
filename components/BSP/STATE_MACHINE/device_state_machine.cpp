#include "device_state_machine.h"
#include "esp_log.h"
#include <algorithm>

static const char* TAG = "StateMachine";

DeviceStateMachine& DeviceStateMachine::instance() {
    static DeviceStateMachine instance;
    return instance;
}

DeviceStateMachine::DeviceStateMachine() {
    current_state_.store(kDeviceStateUnknown);
}

bool DeviceStateMachine::transitionTo(DeviceState new_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    DeviceState old_state = current_state_.load();
    
    if (old_state == new_state) {
        return true; // 已经在目标状态
    }
    
    if (!isValidTransition(old_state, new_state)) {
        ESP_LOGW(TAG, "Invalid transition: %s -> %s",
                 GetDeviceStateName(old_state), GetDeviceStateName(new_state));
        return false;
    }
    
    ESP_LOGI(TAG, "State transition: %s -> %s",
             GetDeviceStateName(old_state), GetDeviceStateName(new_state));
    
    current_state_.store(new_state);
    
    // 通知监听器（在锁外执行以避免死锁）
    auto listeners_copy = listeners_;
    
    // 释放锁后通知
    for (const auto& [id, callback] : listeners_copy) {
        if (callback) {
            callback(old_state, new_state);
        }
    }
    
    return true;
}

bool DeviceStateMachine::canTransitionTo(DeviceState target) const {
    return isValidTransition(current_state_.load(), target);
}

bool DeviceStateMachine::isValidTransition(DeviceState from, DeviceState to) const {
    // 定义有效的状态转换规则
    switch (from) {
        case kDeviceStateUnknown:
            // 初始状态可以转到任何状态
            return true;
            
        case kDeviceStateStarting:
            return to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateError;
                   
        case kDeviceStateWifiConfiguring:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateError;
                   
        case kDeviceStateIdle:
            return to == kDeviceStateListening ||
                   to == kDeviceStateUpgrading ||
                   to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateError;
                   
        case kDeviceStateListening:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateProcessing ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateError;
                   
        case kDeviceStateProcessing:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateError;
                   
        case kDeviceStateSpeaking:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateError;
                   
        case kDeviceStateUpgrading:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateStarting ||
                   to == kDeviceStateError;
                   
        case kDeviceStateError:
            // 错误状态可以恢复到启动或空闲
            return to == kDeviceStateStarting ||
                   to == kDeviceStateIdle;
                   
        default:
            return false;
    }
}

int DeviceStateMachine::addStateChangeListener(StateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(callback));
    ESP_LOGD(TAG, "Added state change listener: %d", id);
    return id;
}

void DeviceStateMachine::removeStateChangeListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                       [listener_id](const auto& pair) {
                           return pair.first == listener_id;
                       }),
        listeners_.end());
    ESP_LOGD(TAG, "Removed state change listener: %d", listener_id);
}

void DeviceStateMachine::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceState old_state = current_state_.load();
    current_state_.store(kDeviceStateUnknown);
    
    if (old_state != kDeviceStateUnknown) {
        for (const auto& [id, callback] : listeners_) {
            if (callback) {
                callback(old_state, kDeviceStateUnknown);
            }
        }
    }
    ESP_LOGI(TAG, "State machine reset");
}

void DeviceStateMachine::notifyStateChange(DeviceState old_state, DeviceState new_state) {
    for (const auto& [id, callback] : listeners_) {
        if (callback) {
            callback(old_state, new_state);
        }
    }
}
