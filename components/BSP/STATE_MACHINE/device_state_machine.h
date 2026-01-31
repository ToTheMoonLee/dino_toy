#ifndef _DEVICE_STATE_MACHINE_H_
#define _DEVICE_STATE_MACHINE_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "device_state.h"

/**
 * @brief 设备状态机
 * 
 * 管理设备状态转换，支持状态变化回调通知
 * 
 * @example
 *   auto& sm = DeviceStateMachine::instance();
 *   sm.addStateChangeListener([](DeviceState old, DeviceState new_state) {
 *       ESP_LOGI("SM", "State: %s -> %s", 
 *                GetDeviceStateName(old), GetDeviceStateName(new_state));
 *   });
 *   sm.transitionTo(kDeviceStateListening);
 */
class DeviceStateMachine {
public:
    /**
     * @brief 获取单例实例
     */
    static DeviceStateMachine& instance();

    // 禁止拷贝
    DeviceStateMachine(const DeviceStateMachine&) = delete;
    DeviceStateMachine& operator=(const DeviceStateMachine&) = delete;

    /**
     * @brief 获取当前状态
     */
    DeviceState getState() const { return current_state_.load(); }

    /**
     * @brief 尝试转换到新状态
     * @param new_state 目标状态
     * @return true 转换成功, false 转换无效
     */
    bool transitionTo(DeviceState new_state);

    /**
     * @brief 检查是否可以转换到目标状态
     */
    bool canTransitionTo(DeviceState target) const;

    /**
     * @brief 状态变化回调类型
     * 参数: (旧状态, 新状态)
     */
    using StateCallback = std::function<void(DeviceState, DeviceState)>;

    /**
     * @brief 添加状态变化监听器
     * @param callback 回调函数
     * @return 监听器ID，用于移除
     */
    int addStateChangeListener(StateCallback callback);

    /**
     * @brief 移除状态变化监听器
     * @param listener_id 监听器ID
     */
    void removeStateChangeListener(int listener_id);

    /**
     * @brief 重置状态机到初始状态
     */
    void reset();

private:
    DeviceStateMachine();
    ~DeviceStateMachine() = default;

    std::atomic<DeviceState> current_state_{kDeviceStateUnknown};
    std::vector<std::pair<int, StateCallback>> listeners_;
    int next_listener_id_{0};
    mutable std::mutex mutex_;

    /**
     * @brief 验证状态转换是否有效
     */
    bool isValidTransition(DeviceState from, DeviceState to) const;

    /**
     * @brief 通知所有监听器状态变化
     */
    void notifyStateChange(DeviceState old_state, DeviceState new_state);
};

#endif // _DEVICE_STATE_MACHINE_H_
