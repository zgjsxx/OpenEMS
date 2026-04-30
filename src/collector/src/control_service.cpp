// src/collector/src/control_service.cpp
#include "openems/collector/control_service.h"
#include "openems/modbus/modbus_data_parser.h"
#include "openems/utils/logger.h"
#include <algorithm>

namespace openems::collector {

DeviceControlTask::DeviceControlTask(
    model::DevicePtr device,
    modbus::IModbusClientPtr client,
    rt_db::RtDb* rtdb)
    : device_(std::move(device)),
      client_(std::move(client)),
      rtdb_(rtdb) {
  for (const auto& point : device_->points()) {
    if (!point) {
      continue;
    }
    if (!point->writable() || !point->has_modbus_mapping()) {
      continue;
    }
    controlled_points_[point->id()] = point;
    controlled_point_ids_.push_back(point->id());
  }
}

DeviceControlTask::DeviceControlTask(
    model::DevicePtr device,
    modbus::ModbusTcpClientPtr tcp_client,
    rt_db::RtDb* rtdb)
    : device_(std::move(device)),
      client_(std::static_pointer_cast<modbus::IModbusClient>(tcp_client)),
      rtdb_(rtdb) {
  for (const auto& point : device_->points()) {
    if (!point) {
      continue;
    }
    if (!point->writable() || !point->has_modbus_mapping()) {
      continue;
    }
    controlled_points_[point->id()] = point;
    controlled_point_ids_.push_back(point->id());
  }
}

model::PointPtr DeviceControlTask::find_controlled_point(const common::PointId& pid) const {
  const auto it = controlled_points_.find(pid);
  if (it == controlled_points_.end()) {
    return nullptr;
  }
  return it->second;
}

uint8_t DeviceControlTask::read_back_fc(uint8_t write_fc) {
  switch (write_fc) {
    case 5:  return 1;   // WriteSingleCoil → ReadCoils
    case 6:  return 3;   // WriteSingleRegister → ReadHolding
    case 16: return 3;   // WriteMultipleRegisters → ReadHolding
    default: return 3;
  }
}

common::VoidResult DeviceControlTask::execute_pending_commands() {
  common::PointId pid;
  double desired_value;

  while (rtdb_->read_pending_command_for_points(controlled_point_ids_, pid, desired_value)) {
    model::PointPtr target_point = find_controlled_point(pid);

    if (!target_point) {
      OPENEMS_LOG_W("ControlTask", "Command point not found in device: " + pid);
      continue;
    }

    if (!target_point->has_modbus_mapping()) {
      OPENEMS_LOG_W("ControlTask", "No Modbus mapping for point: " + pid);
      rtdb_->complete_command(pid, rt_db::CommandFailed, 0.0, 2);
      continue;
    }

    auto& mapping = target_point->modbus_mapping();
    uint8_t fc = mapping.function_code;

    // Verify it's a write function code (5, 6, or 16)
    if (fc != 5 && fc != 6 && fc != 16) {
      OPENEMS_LOG_W("ControlTask", "Point " + pid + " has non-write FC " + std::to_string(fc));
      rtdb_->complete_command(pid, rt_db::CommandFailed, 0.0, 3);
      continue;
    }

    auto result = execute_write(pid, desired_value, mapping);

    if (result.is_ok()) {
      // Read-back to verify the value was written
      double read_back_value = desired_value;  // default if read-back fails
      uint8_t read_fc = read_back_fc(fc);

      if (fc == 5) {
        // Coil read-back
        auto coil_result = client_->read_coils(mapping.register_address, 1);
        if (coil_result.is_ok() && coil_result.value().coils.size() > 0) {
          read_back_value = coil_result.value().coils[0] ? 1.0 : 0.0;
        }
      } else {
        // Register read-back
        auto reg_result = client_->read_holding_registers(
            mapping.register_address, mapping.register_count);
        if (reg_result.is_ok()) {
          auto parse_result = modbus::ModbusDataParser::parse_register(
              reg_result.value().registers, mapping);
          if (parse_result.is_ok()) {
            read_back_value = modbus::ModbusDataParser::apply_scaling(
                parse_result.value(), mapping.scale, mapping.offset);
          }
        }
      }

      rtdb_->complete_command(pid, rt_db::CommandSuccess, read_back_value, 0);
      OPENEMS_LOG_I("ControlTask", "Command succeeded: " + pid +
          " desired=" + std::to_string(desired_value) +
          " read_back=" + std::to_string(read_back_value));
    } else {
      rtdb_->complete_command(pid, rt_db::CommandFailed, 0.0, 4);
      OPENEMS_LOG_W("ControlTask", "Command write failed: " + pid +
          " " + result.error_msg());
    }
  }

  return common::VoidResult::Ok();
}

common::VoidResult DeviceControlTask::execute_write(
    const common::PointId& pid,
    double desired_value,
    const model::ModbusPointMapping& mapping) {

  uint8_t fc = mapping.function_code;

  if (fc == 5) {
    // Write Single Coil
    bool coil_value = modbus::ModbusDataParser::encode_coil(desired_value);
    return client_->write_single_coil(mapping.register_address, coil_value);
  }

  if (fc == 6) {
    // Write Single Register
    auto encode_result = modbus::ModbusDataParser::encode_register(desired_value, mapping);
    if (!encode_result.is_ok()) {
      return common::VoidResult::Err(encode_result.error_code(), encode_result.error_msg());
    }
    auto& words = encode_result.value();
    if (words.size() == 1) {
      return client_->write_single_register(mapping.register_address, words[0]);
    }
    // Multi-register write with FC6 — fall through to FC16
  }

  if (fc == 16) {
    // Write Multiple Registers
    auto encode_result = modbus::ModbusDataParser::encode_register(desired_value, mapping);
    if (!encode_result.is_ok()) {
      return common::VoidResult::Err(encode_result.error_code(), encode_result.error_msg());
    }
    return client_->write_multiple_registers(mapping.register_address, encode_result.value());
  }

  // FC6 with multi-word: use write_multiple_registers
  auto encode_result = modbus::ModbusDataParser::encode_register(desired_value, mapping);
  if (!encode_result.is_ok()) {
    return common::VoidResult::Err(encode_result.error_code(), encode_result.error_msg());
  }
  return client_->write_multiple_registers(mapping.register_address, encode_result.value());
}

// ===== ControlService =====

ControlService::ControlService() = default;
ControlService::~ControlService() { stop(); }

bool ControlService::start() {
  if (running_.load()) return true;
  running_ = true;

  std::lock_guard lock(tasks_mutex_);
  for (auto& task : tasks_) {
    control_threads_.emplace_back(&ControlService::device_control_thread, this, task);
  }
  OPENEMS_LOG_I("ControlService", "Started control service with " +
      std::to_string(control_threads_.size()) + " device threads");
  return true;
}

bool ControlService::stop() {
  if (!running_.load()) return true;
  running_ = false;
  for (auto& t : control_threads_) {
    if (t.joinable()) t.join();
  }
  control_threads_.clear();
  OPENEMS_LOG_I("ControlService", "Stopped control service");
  return true;
}

void ControlService::add_task(DeviceControlTaskPtr task) {
  std::lock_guard lock(tasks_mutex_);
  tasks_.push_back(std::move(task));
}

void ControlService::set_default_interval(uint32_t interval_ms) {
  default_interval_ms_ = interval_ms;
}

void ControlService::device_control_thread(DeviceControlTaskPtr task) {
  OPENEMS_LOG_I("ControlService", "Control thread started for device " + task->device()->id());
  while (running_.load()) {
    auto start = std::chrono::steady_clock::now();
    auto result = task->execute_pending_commands();
    if (!result.is_ok()) {
      OPENEMS_LOG_D("ControlService",
          "Execute commands failed for " + task->device()->id() + ": " + result.error_msg());
    }
    // Sleep only the remaining time
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    auto sleep_ms = static_cast<long>(default_interval_ms_) - elapsed;
    if (sleep_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }
  OPENEMS_LOG_I("ControlService", "Control thread exiting for device " + task->device()->id());
}

} // namespace openems::collector
