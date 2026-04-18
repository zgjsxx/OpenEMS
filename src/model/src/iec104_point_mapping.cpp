// src/model/src/iec104_point_mapping.cpp
#include "openems/model/iec104_point_mapping.h"
#include <sstream>

namespace openems::model {

std::string Iec104PointMapping::to_string() const {
  std::ostringstream oss;
  oss << "TypeID=" << static_cast<int>(type_id)
      << " IOA=" << ioa
      << " CA=" << common_address
      << " Scale=" << scale
      << " COT=" << static_cast<int>(cot);
  return oss.str();
}

} // namespace openems::model