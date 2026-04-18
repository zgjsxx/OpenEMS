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
               uint32_t poll_interval_ms)
    : id_(std::move(id)), name_(std::move(name)), type_(type),
      ip_(std::move(ip)), port_(port), unit_id_(unit_id),
      poll_interval_ms_(poll_interval_ms) {}

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
      << " " << ip_ << ":" << port_
      << " unit=" << unit_id_
      << " status=" << device_status_to_string(status_)
      << " poll=" << poll_interval_ms_ << "ms"
      << " points=" << points_.size();
  return oss.str();
}

} // namespace openems::model