// src/model/src/modbus_point_mapping.cpp
#include "openems/model/modbus_point_mapping.h"
#include <sstream>

namespace openems::model {

std::string ModbusPointMapping::to_string() const {
  std::ostringstream oss;
  oss << "FC=" << static_cast<int>(function_code)
      << " Addr=" << register_address
      << " Count=" << register_count
      << " Scale=" << scale
      << " Offset=" << offset;
  return oss.str();
}

} // namespace openems::model