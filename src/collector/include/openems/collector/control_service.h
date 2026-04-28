// src/collector/include/openems/collector/control_service.h
#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <chrono>
#include <cstdint>
#include <string>
#include "openems/common/types.h"
#include "openems/common/result.h"
#include "openems/common/base_interface.h"
#include "openems/model/device.h"
#include "openems/modbus/imodbus_client.h"
#include "openems/modbus/modbus_tcp_client.h"
#include "openems/rt_db/rt_db.h"

namespace openems::collector {

class DeviceControlTask {
public:
  // IModbusClientPtr 构造（支持 TCP 和 RTU）
  DeviceControlTask(model::DevicePtr device,
                    modbus::IModbusClientPtr client,
                    rt_db::RtDb* rtdb);

  // ModbusTcpClientPtr 构造（向后兼容）
  DeviceControlTask(model::DevicePtr device,
                    modbus::ModbusTcpClientPtr tcp_client,
                    rt_db::RtDb* rtdb);

  common::VoidResult execute_pending_commands();

  const model::DevicePtr& device() const { return device_; }

  // Map write FC → read-back FC
  static uint8_t read_back_fc(uint8_t write_fc);

private:
  common::VoidResult execute_write(const common::PointId& pid,
                                    double desired_value,
                                    const model::ModbusPointMapping& mapping);

  model::DevicePtr device_;
  modbus::IModbusClientPtr client_;
  rt_db::RtDb* rtdb_;
};

using DeviceControlTaskPtr = std::shared_ptr<DeviceControlTask>;

inline DeviceControlTaskPtr DeviceControlTaskCreate(
    model::DevicePtr device,
    modbus::IModbusClientPtr client,
    rt_db::RtDb* rtdb) {
  return std::make_shared<DeviceControlTask>(std::move(device), std::move(client), rtdb);
}

class ControlService : public common::IModule {
public:
  ControlService();
  ~ControlService();

  std::string name() const override { return "ControlService"; }
  bool start() override;
  bool stop() override;

  void add_task(DeviceControlTaskPtr task);
  void set_default_interval(uint32_t interval_ms);

private:
  void device_control_thread(DeviceControlTaskPtr task);

  std::vector<DeviceControlTaskPtr> tasks_;
  std::atomic<bool> running_{false};
  std::vector<std::thread> control_threads_;
  mutable std::mutex tasks_mutex_;
  uint32_t default_interval_ms_ = 100;
};

using ControlServicePtr = std::shared_ptr<ControlService>;

inline ControlServicePtr ControlServiceCreate() {
  return std::make_shared<ControlService>();
}

} // namespace openems::collector