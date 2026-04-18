// src/collector/include/openems/collector/polling_service.h
#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include <cstdint>
#include <string>
#include "openems/common/types.h"
#include "openems/common/result.h"
#include "openems/common/base_interface.h"
#include "openems/model/device.h"
#include "openems/modbus/modbus_tcp_client.h"
#include "openems/rt_db/rt_db.h"

namespace openems::collector {

struct PollingStats {
  uint32_t poll_count = 0;
  uint32_t success_count = 0;
  uint32_t fail_count = 0;
  common::Timestamp last_poll_time = {};
  common::Timestamp last_success_time = {};
};

class DevicePollTask {
public:
  DevicePollTask(model::DevicePtr device,
                 modbus::ModbusTcpClientPtr client,
                 rt_db::RtDb* rtdb);

  common::VoidResult poll_once();

  const model::DevicePtr& device() const { return device_; }
  const PollingStats& stats() const { return stats_; }

private:
  struct RegisterGroup {
    uint8_t function_code;
    uint16_t start_address;
    uint16_t count;
    std::vector<model::PointPtr> points;
  };

  std::vector<RegisterGroup> build_register_groups() const;
  common::VoidResult poll_register_group(const RegisterGroup& group);

  model::DevicePtr device_;
  modbus::ModbusTcpClientPtr client_;
  rt_db::RtDb* rtdb_;
  PollingStats stats_;
};

using DevicePollTaskPtr = std::shared_ptr<DevicePollTask>;

inline DevicePollTaskPtr DevicePollTaskCreate(
    model::DevicePtr device,
    modbus::ModbusTcpClientPtr client,
    rt_db::RtDb* rtdb) {
  return std::make_shared<DevicePollTask>(std::move(device), std::move(client), rtdb);
}

class PollingService : public common::IModule {
public:
  PollingService();
  ~PollingService();

  std::string name() const override { return "PollingService"; }
  bool start() override;
  bool stop() override;

  void add_task(DevicePollTaskPtr task);
  void remove_task(const common::DeviceId& device_id);
  void set_default_interval(uint32_t interval_ms);

private:
  void device_poll_thread(DevicePollTaskPtr task);

  std::vector<DevicePollTaskPtr> tasks_;
  std::atomic<bool> running_{false};
  std::vector<std::thread> poll_threads_;
  mutable std::mutex tasks_mutex_;
  uint32_t default_interval_ms_ = 1000;
};

using PollingServicePtr = std::shared_ptr<PollingService>;

inline PollingServicePtr PollingServiceCreate() {
  return std::make_shared<PollingService>();
}

} // namespace openems::collector