#pragma once

#include <variant>
#include <chrono>
#include <string>
#include "openems/common/types.h"

namespace openems::model {

using ValueVariant = std::variant<bool, int16_t, uint16_t, int32_t, uint32_t, float, int64_t, uint64_t, double>;

struct PointValue {
  ValueVariant value = uint16_t{0};
  common::Timestamp timestamp = {};
  common::Quality quality = common::Quality::Bad;
  bool valid = false;

  std::string to_string() const;

  double as_double() const;
  float  as_float() const;
  int32_t as_int32() const;
  bool   as_bool() const;
};

} // namespace openems::model