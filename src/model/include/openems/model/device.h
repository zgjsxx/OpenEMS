// src/model/include/openems/model/device.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include "openems/common/types.h"
#include "openems/model/point.h"

namespace openems::model {

class Device {
public:
  Device(common::DeviceId id, std::string name, common::DeviceType type,
         std::string ip, uint16_t port, uint8_t unit_id,
         uint32_t poll_interval_ms);

  const common::DeviceId& id() const { return id_; }
  const std::string& name() const { return name_; }
  common::DeviceType type() const { return type_; }
  const std::string& ip() const { return ip_; }
  uint16_t port() const { return port_; }
  uint8_t unit_id() const { return unit_id_; }
  uint32_t poll_interval_ms() const { return poll_interval_ms_; }

  void set_status(common::DeviceStatus status);
  common::DeviceStatus status() const;

  void add_point(PointPtr point);
  const std::vector<PointPtr>& points() const { return points_; }
  PointPtr find_point(const common::PointId& pid) const;

  std::string to_string() const;

private:
  common::DeviceId id_;
  std::string name_;
  common::DeviceType type_;
  std::string ip_;
  uint16_t port_;
  uint8_t unit_id_;
  uint32_t poll_interval_ms_;

  common::DeviceStatus status_ = common::DeviceStatus::Offline;
  std::vector<PointPtr> points_;
  mutable std::mutex mutex_;
};

using DevicePtr = std::shared_ptr<Device>;

inline DevicePtr DeviceCreate(common::DeviceId id, std::string name, common::DeviceType type,
                              std::string ip, uint16_t port, uint8_t unit_id,
                              uint32_t poll_interval_ms) {
  return std::make_shared<Device>(std::move(id), std::move(name), type,
                                   std::move(ip), port, unit_id, poll_interval_ms);
}

} // namespace openems::model