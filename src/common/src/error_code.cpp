// src/common/src/error_code.cpp
#include "openems/common/error_code.h"

namespace openems::common {

std::string error_code_to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:                  return "Ok";
    case ErrorCode::Unknown:             return "Unknown error";
    case ErrorCode::Timeout:             return "Timeout";
    case ErrorCode::ConnectionFailed:    return "Connection failed";
    case ErrorCode::ConnectionClosed:    return "Connection closed";
    case ErrorCode::InvalidArgument:     return "Invalid argument";
    case ErrorCode::InvalidConfig:       return "Invalid config";
    case ErrorCode::DeviceOffline:       return "Device offline";
    case ErrorCode::ModbusError:         return "Modbus error";
    case ErrorCode::ModbusTimeout:       return "Modbus timeout";
    case ErrorCode::ModbusConnectionFailed: return "Modbus connection failed";
    case ErrorCode::ModbusReadFailed:    return "Modbus read failed";
    case ErrorCode::ModbusWriteFailed:   return "Modbus write write failed";
    case ErrorCode::ModbusInvalidFrame:  return "Modbus invalid frame";
    case ErrorCode::ParseError:          return "Parse error";
    case ErrorCode::PointNotFound:       return "Point not found";
    case ErrorCode::DeviceNotFound:      return "Device not found";
    case ErrorCode::SiteNotFound:        return "Site not found";
    case ErrorCode::DataQualityBad:      return "Data quality bad";
    case ErrorCode::NotSupported:        return "Not supported";
    case ErrorCode::SerialPortError:     return "Serial port error";
    case ErrorCode::SerialPortOpenFailed: return "Serial port open failed";
    case ErrorCode::SerialPortConfigFailed: return "Serial port config failed";
    case ErrorCode::SerialPortWriteFailed: return "Serial port write failed";
    case ErrorCode::SerialPortReadFailed: return "Serial port read failed";
    case ErrorCode::ModbusRtuCrcError:  return "Modbus RTU CRC error";
    case ErrorCode::ModbusRtuFrameError: return "Modbus RTU frame error";
    default:                             return "Unknown error code";
  }
}

} // namespace openems::common