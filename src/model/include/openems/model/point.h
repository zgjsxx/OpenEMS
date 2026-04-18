#pragma once

#include <string>
#include <memory>
#include <mutex>
#include "openems/common/types.h"
#include "openems/model/point_value.h"
#include "openems/model/modbus_point_mapping.h"
#include "openems/model/iec104_point_mapping.h"

namespace openems::model {

class Point {
public:
  Point(common::PointId id, std::string name, std::string code,
        common::PointCategory category, common::DataType data_type,
        std::string unit, bool writable);

  const common::PointId& id() const { return id_; }
  const std::string& name() const { return name_; }
  const std::string& code() const { return code_; }
  common::PointCategory category() const { return category_; }
  common::DataType data_type() const { return data_type_; }
  const std::string& unit() const { return unit_; }
  bool writable() const { return writable_; }

  // Protocol: which protocol this point uses
  std::string protocol() const { return protocol_; }
  void set_protocol(const std::string& proto) { protocol_ = proto; }

  // Modbus mapping
  void set_modbus_mapping(const ModbusPointMapping& mapping);
  const ModbusPointMapping& modbus_mapping() const;
  bool has_modbus_mapping() const { return has_modbus_mapping_; }

  // IEC104 mapping
  void set_iec104_mapping(const Iec104PointMapping& mapping);
  const Iec104PointMapping& iec104_mapping() const;
  bool has_iec104_mapping() const { return has_iec104_mapping_; }

  void set_value(const PointValue& pv);
  PointValue get_value() const;

  std::string to_string() const;

private:
  common::PointId id_;
  std::string name_;
  std::string code_;
  common::PointCategory category_;
  common::DataType data_type_;
  std::string unit_;
  bool writable_;
  std::string protocol_;  // "modbus-tcp" or "iec104"

  ModbusPointMapping modbus_mapping_{};
  bool has_modbus_mapping_ = false;

  Iec104PointMapping iec104_mapping_{};
  bool has_iec104_mapping_ = false;

  PointValue current_value_;
  mutable std::mutex value_mutex_;
};

using PointPtr = std::shared_ptr<Point>;

inline PointPtr PointCreate(common::PointId id, std::string name, std::string code,
                             common::PointCategory category, common::DataType data_type,
                             std::string unit, bool writable) {
  return std::make_shared<Point>(std::move(id), std::move(name), std::move(code),
                                  category, data_type, std::move(unit), writable);
}

} // namespace openems::model