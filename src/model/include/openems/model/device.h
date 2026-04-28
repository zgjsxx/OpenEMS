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
         uint32_t poll_interval_ms,
         std::string protocol = "modbus-tcp",
         std::string serial_port = "",
         uint32_t baud_rate = 9600,
         char parity = 'N',
         uint8_t data_bits = 8,
         uint8_t stop_bits = 1);

  const common::DeviceId& id() const { return id_; }
  const std::string& name() const { return name_; }
  common::DeviceType type() const { return type_; }
  const std::string& ip() const { return ip_; }
  uint16_t port() const { return port_; }
  uint8_t unit_id() const { return unit_id_; }
  uint32_t poll_interval_ms() const { return poll_interval_ms_; }
  const std::string& protocol() const { return protocol_; }
  const std::string& serial_port() const { return serial_port_; }
  uint32_t baud_rate() const { return baud_rate_; }
  char parity() const { return parity_; }
  uint8_t data_bits() const { return data_bits_; }
  uint8_t stop_bits() const { return stop_bits_; }

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
  std::string protocol_;
  std::string serial_port_;
  uint32_t baud_rate_;
  char parity_;
  uint8_t data_bits_;
  uint8_t stop_bits_;

  common::DeviceStatus status_ = common::DeviceStatus::Offline;
  std::vector<PointPtr> points_;
  mutable std::mutex mutex_;
};

using DevicePtr = std::shared_ptr<Device>;

inline DevicePtr DeviceCreate(common::DeviceId id, std::string name, common::DeviceType type,
                              std::string ip, uint16_t port, uint8_t unit_id,
                              uint32_t poll_interval_ms,
                              std::string protocol = "modbus-tcp",
                              std::string serial_port = "",
                              uint32_t baud_rate = 9600,
                              char parity = 'N',
                              uint8_t data_bits = 8,
                              uint8_t stop_bits = 1) {
  return std::make_shared<Device>(std::move(id), std::move(name), type,
                                   std::move(ip), port, unit_id, poll_interval_ms,
                                   std::move(protocol), std::move(serial_port),
                                   baud_rate, parity, data_bits, stop_bits);
}

} // namespace openems::model