// src/model/src/point_value.cpp
#include "openems/model/point_value.h"
#include <sstream>

namespace openems::model {

std::string PointValue::to_string() const {
  std::ostringstream oss;
  if (!valid) {
    oss << "[INVALID]";
  } else {
    oss << std::visit([](auto&& v) -> std::string {
      return std::to_string(v);
    }, value);
  }
  oss << " Q=" << static_cast<int>(quality);
  return oss.str();
}

double PointValue::as_double() const {
  return std::visit([](auto&& v) -> double {
    return static_cast<double>(v);
  }, value);
}

float PointValue::as_float() const {
  return std::visit([](auto&& v) -> float {
    return static_cast<float>(v);
  }, value);
}

int32_t PointValue::as_int32() const {
  return std::visit([](auto&& v) -> int32_t {
    return static_cast<int32_t>(v);
  }, value);
}

bool PointValue::as_bool() const {
  if (std::holds_alternative<bool>(value)) return std::get<bool>(value);
  return as_int32() != 0;
}

} // namespace openems::model