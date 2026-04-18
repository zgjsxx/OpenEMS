// src/common/include/openems/common/error_code.h
#pragma once

#include <string>
#include <cstdint>

namespace openems::common {

enum class ErrorCode : uint32_t {
  Ok                  = 0,
  Unknown             = 1,
  Timeout             = 2,
  ConnectionFailed    = 3,
  ConnectionClosed    = 4,
  InvalidArgument     = 5,
  InvalidConfig       = 6,
  DeviceOffline       = 7,
  ModbusError         = 8,
  ModbusTimeout       = 9,
  ModbusConnectionFailed = 10,
  ModbusReadFailed    = 11,
  ModbusWriteFailed   = 12,
  ModbusInvalidFrame  = 13,
  ParseError          = 14,
  PointNotFound       = 15,
  DeviceNotFound      = 16,
  SiteNotFound        = 17,
  DataQualityBad      = 18,
  NotSupported        = 19,
};

std::string error_code_to_string(ErrorCode code);

} // namespace openems::common