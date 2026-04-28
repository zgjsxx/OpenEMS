#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "openems/common/types.h"
#include "openems/model/modbus_point_mapping.h"
#include "openems/model/iec104_point_mapping.h"

namespace openems::config {

struct PointConfig {
  common::PointId id;
  std::string name;
  std::string code;
  common::PointCategory category;
  common::DataType data_type;
  std::string unit;
  bool writable = false;
  bool has_modbus_mapping = false;
  bool has_iec104_mapping = false;
  model::ModbusPointMapping modbus_mapping;
  model::Iec104PointMapping iec104_mapping;
};

struct DeviceConfig {
  common::DeviceId id;
  std::string name;
  common::DeviceType type;
  std::string ip;
  uint16_t port;
  uint8_t unit_id;
  uint32_t poll_interval_ms;
  std::string protocol = "modbus-tcp";  // "modbus-tcp", "modbus-rtu", or "iec104"
  uint16_t iec104_common_address = 1;
  // RTU 串口参数
  std::string serial_port;    // 默认空
  uint32_t baud_rate = 9600;
  char parity = 'N';
  uint8_t data_bits = 8;
  uint8_t stop_bits = 1;
  std::vector<PointConfig> points;
};

struct SiteConfig {
  common::SiteId id;
  std::string name;
  std::string description;
  std::vector<DeviceConfig> devices;
};

struct EmsConfig {
  std::string log_level = "info";
  uint32_t default_poll_interval_ms = 1000;
  SiteConfig site;
};

} // namespace openems::config