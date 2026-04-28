// src/model/src/device.cpp
#include "openems/model/device.h"
#include <sstream>

namespace openems::model {

static std::string device_type_to_string(common::DeviceType t) {
  switch (t) {
    case common::DeviceType::PV:         return "PV";
    case common::DeviceType::BESS:       return "BESS";
    case common::DeviceType::Meter:      return "Meter";
    case common::DeviceType::Inverter:   return "Inverter";
    case common::DeviceType::Grid:       return "Grid";
    case common::DeviceType::Transformer: return "Transformer";
    default:                             return "Unknown";
  }
}

static std::string device_status_to_string(common::DeviceStatus s) {
  switch (s) {
    case common::DeviceStatus::Online:  return "Online";
    case common::DeviceStatus::Offline: return "Offline";
    case common::DeviceStatus::Fault:   return "Fault";
    case common::DeviceStatus::Standby: return "Standby";
    default:                            return "Unknown";
  }
}

Device::Device(common::DeviceId id, std::string name, common::DeviceType type,
               std::string ip, uint16_t port, uint8_t unit_id,
               uint32_t poll_interval_ms,
               std::string protocol,
               std::string serial_port,
               uint32_t baud_rate,
               char parity,
               uint8_t data_bits,
               uint8_t stop_bits)
    : id_(std::move(id)), name_(std::move(name)), type_(type),
      ip_(std::move(ip)), port_(port), unit_id_(unit_id),
      poll_interval_ms_(poll_interval_ms),
      protocol_(std::move(protocol)), serial_port_(std::move(serial_port)),
      baud_rate_(baud_rate), parity_(parity),
      data_bits_(data_bits), stop_bits_(stop_bits) {}

void Device::set_status(common::DeviceStatus status) {
  std::lock_guard lock(mutex_);
  status_ = status;
}

common::DeviceStatus Device::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

void Device::add_point(PointPtr point) {
  std::lock_guard lock(mutex_);
  points_.push_back(std::move(point));
}

PointPtr Device::find_point(const common::PointId& pid) const {
  std::lock_guard lock(mutex_);
  for (auto& p : points_) {
    if (p->id() == pid) return p;
  }
  return nullptr;
}

std::string Device::to_string() const {
  std::ostringstream oss;
  oss << "Device[" << id_ << "] " << name_
      << " type=" << device_type_to_string(type_)
      << " proto=" << protocol_;
  if (protocol_ == "modbus-rtu" && !serial_port_.empty()) {
    oss << " " << serial_port_ << ":" << baud_rate_
        << "-" << data_bits_ << parity_ << stop_bits_;
  } else {
    oss << " " << ip_ << ":" << port_;
  }
  oss << " unit=" << unit_id_
      << " status=" << device_status_to_string(status_)
      << " poll=" << poll_interval_ms_ << "ms"
      << " points=" << points_.size();
  return oss.str();
}

} // namespace openems::model