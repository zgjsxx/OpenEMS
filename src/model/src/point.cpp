// src/model/src/point.cpp
#include "openems/model/point.h"
#include "openems/model/iec104_point_mapping.h"
#include <sstream>
#include "openems/utils/time_utils.h"

namespace openems::model {

Point::Point(common::PointId id, std::string name, std::string code,
             common::PointCategory category, common::DataType data_type,
             std::string unit, bool writable)
    : id_(std::move(id)), name_(std::move(name)), code_(std::move(code)),
      category_(category), data_type_(data_type), unit_(std::move(unit)),
      writable_(writable), protocol_("modbus-tcp") {
  current_value_.quality = common::Quality::Bad;
  current_value_.valid = false;
  current_value_.timestamp = openems::utils::now();
}

void Point::set_modbus_mapping(const ModbusPointMapping& mapping) {
  modbus_mapping_ = mapping;
  has_modbus_mapping_ = true;
}

const ModbusPointMapping& Point::modbus_mapping() const {
  return modbus_mapping_;
}

void Point::set_iec104_mapping(const Iec104PointMapping& mapping) {
  iec104_mapping_ = mapping;
  has_iec104_mapping_ = true;
}

const Iec104PointMapping& Point::iec104_mapping() const {
  return iec104_mapping_;
}

void Point::set_value(const PointValue& pv) {
  std::lock_guard lock(value_mutex_);
  current_value_ = pv;
}

PointValue Point::get_value() const {
  std::lock_guard lock(value_mutex_);
  return current_value_;
}

std::string Point::to_string() const {
  std::ostringstream oss;
  oss << "Point[" << id_ << "] " << name_
      << " code=" << code_
      << " unit=" << unit_
      << " writable=" << writable_
      << " protocol=" << protocol_;
  if (has_modbus_mapping_) {
    oss << " modbus=" << modbus_mapping_.to_string();
  }
  if (has_iec104_mapping_) {
    oss << " iec104=" << iec104_mapping_.to_string();
  }
  return oss.str();
}

} // namespace openems::model