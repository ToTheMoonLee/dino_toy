#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

/**
 * @brief 设备状态枚举
 * 
 * 定义设备的各种运行状态，用于状态机管理
 */
enum DeviceState {
    kDeviceStateUnknown = 0,       ///< 未知状态
    kDeviceStateStarting,          ///< 启动中
    kDeviceStateWifiConfiguring,   ///< WiFi配网中
    kDeviceStateIdle,              ///< 空闲待机
    kDeviceStateListening,         ///< 正在聆听（唤醒后等待命令）
    kDeviceStateProcessing,        ///< 处理中（语音识别/云端对话）
    kDeviceStateSpeaking,          ///< 播放语音中
    kDeviceStateUpgrading,         ///< OTA升级中
    kDeviceStateError              ///< 错误状态
};

/**
 * @brief 获取状态名称字符串
 * @param state 设备状态
 * @return 状态名称
 */
inline const char* GetDeviceStateName(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown:         return "Unknown";
        case kDeviceStateStarting:        return "Starting";
        case kDeviceStateWifiConfiguring: return "WifiConfiguring";
        case kDeviceStateIdle:            return "Idle";
        case kDeviceStateListening:       return "Listening";
        case kDeviceStateProcessing:      return "Processing";
        case kDeviceStateSpeaking:        return "Speaking";
        case kDeviceStateUpgrading:       return "Upgrading";
        case kDeviceStateError:           return "Error";
        default:                          return "Invalid";
    }
}

#endif // _DEVICE_STATE_H_
