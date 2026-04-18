// src/collector/src/polling_service.cpp
#include "openems/collector/polling_service.h"
#include "openems/modbus/modbus_data_parser.h"
#include "openems/utils/logger.h"
#include "openems/utils/time_utils.h"
#include <algorithm>

namespace openems::collector {

DevicePollTask::DevicePollTask(
    model::DevicePtr device,
    modbus::ModbusTcpClientPtr client,
    rt_db::RtDb* rtdb)
    : device_(std::move(device)),
      client_(std::move(client)),
      rtdb_(rtdb) {}

std::vector<DevicePollTask::RegisterGroup>
DevicePollTask::build_register_groups() const {
  std::unordered_map<uint8_t, std::vector<model::PointPtr>> by_fc;
  for (auto& pt : device_->points()) {
    if (pt->has_modbus_mapping()) {
      by_fc[pt->modbus_mapping().function_code].push_back(pt);
    }
  }

  std::vector<RegisterGroup> groups;
  for (auto& [fc, pts] : by_fc) {
    std::sort(pts.begin(), pts.end(),
        [](const auto& a, const auto& b) {
          return a->modbus_mapping().register_address <
                 b->modbus_mapping().register_address;
        });

    uint16_t cur_start = pts.front()->modbus_mapping().register_address;
    uint16_t cur_end = cur_start + pts.front()->modbus_mapping().register_count;
    std::vector<model::PointPtr> cur_pts;
    cur_pts.push_back(pts.front());

    for (size_t i = 1; i < pts.size(); ++i) {
      auto& pt = pts[i];
      auto& m = pt->modbus_mapping();
      uint16_t pt_end = m.register_address + m.register_count;

      if (m.register_address <= cur_end + 5) {
        cur_end = std::max(cur_end, pt_end);
        cur_pts.push_back(pt);
      } else {
        RegisterGroup group;
        group.function_code = fc;
        group.start_address = cur_start;
        group.count = cur_end - cur_start;
        group.points = std::move(cur_pts);
        groups.push_back(std::move(group));

        cur_start = m.register_address;
        cur_end = pt_end;
        cur_pts = {pt};
      }
    }
    RegisterGroup group;
    group.function_code = fc;
    group.start_address = cur_start;
    group.count = cur_end - cur_start;
    group.points = std::move(cur_pts);
    groups.push_back(std::move(group));
  }
  return groups;
}

common::VoidResult DevicePollTask::poll_register_group(const RegisterGroup& group) {
  auto read_result = common::Result<modbus::ModbusReadResult>::Err(
      common::ErrorCode::Unknown, "not initialized");

  switch (group.function_code) {
    case 1:  read_result = client_->read_coils(group.start_address, group.count); break;
    case 2:  read_result = client_->read_discrete_inputs(group.start_address, group.count); break;
    case 3:  read_result = client_->read_holding_registers(group.start_address, group.count); break;
    case 4:  read_result = client_->read_input_registers(group.start_address, group.count); break;
    default: return common::VoidResult::Err(
        common::ErrorCode::NotSupported,
        "Unsupported function code: " + std::to_string(group.function_code));
  }

  if (!read_result.is_ok()) {
    return common::VoidResult::Err(read_result.error_code(), read_result.error_msg());
  }

  auto& modbus_result = read_result.value();

  for (auto& pt : group.points) {
    auto& mapping = pt->modbus_mapping();
    uint16_t offset = mapping.register_address - group.start_address;

    double eng_value = 0.0;
    common::Quality quality = common::Quality::Good;
    bool valid = true;

    if (group.function_code <= 2) {
      auto parse_result = modbus::ModbusDataParser::parse_bits(
          modbus_result.coils, offset, mapping.data_type);
      if (parse_result.is_ok()) {
        eng_value = modbus::ModbusDataParser::apply_scaling(
            parse_result.value(), mapping.scale, mapping.offset);
      } else {
        quality = common::Quality::Bad;
        valid = false;
      }
    } else {
      std::vector<uint16_t> sub_regs(
          modbus_result.registers.begin() + offset,
          modbus_result.registers.begin() + offset + mapping.register_count);
      auto parse_result = modbus::ModbusDataParser::parse_register(sub_regs, mapping);
      if (parse_result.is_ok()) {
        eng_value = modbus::ModbusDataParser::apply_scaling(
            parse_result.value(), mapping.scale, mapping.offset);
      } else {
        quality = common::Quality::Bad;
        valid = false;
        OPENEMS_LOG_W("PollTask",
            "Parse failed for point " + pt->id() + ": " + parse_result.error_msg());
      }
    }

    // Write to RtDb based on point category
    if (pt->category() == common::PointCategory::Teleindication) {
      // Teleindication: store as uint16 state code
      rtdb_->write_teleindication(pt->id(),
          static_cast<uint16_t>(std::round(eng_value)),
          quality, valid);
    } else {
      // Telemetry / Telecontrol / Setting: store as double engineering value
      rtdb_->write_telemetry(pt->id(), eng_value, quality, valid);
    }
  }

  return common::VoidResult::Ok();
}

common::VoidResult DevicePollTask::poll_once() {
  stats_.poll_count++;
  stats_.last_poll_time = openems::utils::now();

  auto groups = build_register_groups();
  bool all_ok = true;

  for (auto& group : groups) {
    auto result = poll_register_group(group);
    if (!result.is_ok()) {
      all_ok = false;
      OPENEMS_LOG_W("PollTask",
          "Poll group failed for device " + device_->id() +
          " (" + client_->config().ip + ":" + std::to_string(client_->config().port) + ")" +
          " FC=" + std::to_string(group.function_code) +
          " addr=" + std::to_string(group.start_address) +
          ": " + result.error_msg());
      // Only mark the points in this failed group as Bad, not all device points
      for (auto& pt : group.points) {
        if (pt->category() == common::PointCategory::Teleindication) {
          rtdb_->write_teleindication(pt->id(), 0, common::Quality::Bad, false);
        } else {
          rtdb_->write_telemetry(pt->id(), 0.0, common::Quality::Bad, false);
        }
      }
    }
  }

  if (all_ok) {
    stats_.success_count++;
    stats_.last_success_time = stats_.last_poll_time;
    device_->set_status(common::DeviceStatus::Online);
  } else {
    stats_.fail_count++;
    device_->set_status(common::DeviceStatus::Offline);
  }

  return all_ok ? common::VoidResult::Ok()
                : common::VoidResult::Err(common::ErrorCode::ModbusReadFailed, "Some groups failed");
}

// ===== PollingService =====

PollingService::PollingService() = default;
PollingService::~PollingService() { stop(); }

bool PollingService::start() {
  if (running_.load()) return true;
  running_ = true;

  std::lock_guard lock(tasks_mutex_);
  for (auto& task : tasks_) {
    poll_threads_.emplace_back(&PollingService::device_poll_thread, this, task);
  }
  OPENEMS_LOG_I("PollingService", "Started polling service with " +
      std::to_string(poll_threads_.size()) + " device threads");
  return true;
}

bool PollingService::stop() {
  if (!running_.load()) return true;
  running_ = false;
  for (auto& t : poll_threads_) {
    if (t.joinable()) t.join();
  }
  poll_threads_.clear();
  OPENEMS_LOG_I("PollingService", "Stopped polling service");
  return true;
}

void PollingService::add_task(DevicePollTaskPtr task) {
  std::lock_guard lock(tasks_mutex_);
  tasks_.push_back(std::move(task));
}

void PollingService::remove_task(const common::DeviceId& device_id) {
  std::lock_guard lock(tasks_mutex_);
  tasks_.erase(
      std::remove_if(tasks_.begin(), tasks_.end(),
          [&device_id](const auto& t) { return t->device()->id() == device_id; }),
      tasks_.end());
}

void PollingService::set_default_interval(uint32_t interval_ms) {
  default_interval_ms_ = interval_ms;
}

void PollingService::device_poll_thread(DevicePollTaskPtr task) {
  OPENEMS_LOG_I("PollingService", "Poll thread started for device " + task->device()->id());
  while (running_.load()) {
    auto start = std::chrono::steady_clock::now();
    auto result = task->poll_once();
    if (!result.is_ok()) {
      OPENEMS_LOG_D("PollingService",
          "Poll failed for " + task->device()->id() + ": " + result.error_msg());
    }
    // Sleep only the remaining time to hit the target interval
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    auto sleep_ms = static_cast<long>(default_interval_ms_) - elapsed;
    if (sleep_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }
  OPENEMS_LOG_I("PollingService", "Poll thread exiting for device " + task->device()->id());
}

} // namespace openems::collector