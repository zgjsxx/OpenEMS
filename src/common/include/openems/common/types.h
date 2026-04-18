// src/common/include/openems/common/types.h
#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <functional>
#include <memory>

namespace openems::common {

using Timestamp = std::chrono::system_clock::time_point;
using Duration = std::chrono::milliseconds;

using DeviceId = std::string;
using PointId = std::string;
using SiteId = std::string;

enum class Quality : uint8_t {
  Good       = 0,
  Questionable = 1,
  Bad        = 2,
  Invalid    = 3,
};

enum class PointCategory : uint8_t {
  Telemetry  = 0,  // 遥测
  Teleindication = 1,  // 遥信
  Telecontrol = 2,  // 遥控
  Setting    = 3,   // 遥调
};

enum class DataType : uint8_t {
  Bool    = 0,
  Int16   = 1,
  Uint16  = 2,
  Int32   = 3,
  Uint32  = 4,
  Float32 = 5,
  Int64   = 6,
  Uint64  = 7,
  Double  = 8,
};

enum class DeviceType : uint8_t {
  Unknown = 0,
  PV      = 1,
  BESS    = 2,
  Meter   = 3,
  Inverter = 4,
  Grid    = 5,
  Transformer = 6,
};

enum class DeviceStatus : uint8_t {
  Offline = 0,
  Online  = 1,
  Fault   = 2,
  Standby = 3,
};

enum class GridState : uint8_t {
  OnGrid     = 0,   // 并网
  OffGrid    = 1,   // 离网
  Switching  = 2,   // 切换中
  Unknown    = 3,
};

enum class RunMode : uint8_t {
  Manual      = 0,
  Auto        = 1,
  Remote      = 2,
  Emergency   = 3,
  Maintenance = 4,
};

} // namespace openems::common