// src/modbus/include/openems/modbus/modbus_data_parser.h
#pragma once

#include <vector>
#include <cstdint>
#include <variant>
#include "openems/common/types.h"
#include "openems/common/result.h"
#include "openems/model/modbus_point_mapping.h"
#include "openems/model/point_value.h"

namespace openems::modbus {

// 将原始寄存器数据按 ModbusPointMapping 解析为工程值
class ModbusDataParser {
public:
  // 从寄存器数组中解析单个点位
  static common::Result<model::ValueVariant> parse_register(
      const std::vector<uint16_t>& registers,
      const model::ModbusPointMapping& mapping);

  // 从 coils/discrete inputs 解析
  static common::Result<model::ValueVariant> parse_bits(
      const std::vector<bool>& bits,
      uint16_t bit_index,
      common::DataType data_type);

  // 将原始值应用缩放和偏移，转为工程值
  static double apply_scaling(model::ValueVariant raw, double scale, double offset);

  // 将解析后的工程值根据 DataType 封装回 ValueVariant
  static model::ValueVariant to_value_variant(double eng_value, common::DataType target_type);

  // 将工程值反向编码为原始寄存器值 (engineering → raw)
  static common::Result<std::vector<uint16_t>> encode_register(
      double eng_value, const model::ModbusPointMapping& mapping);

  // 将工程值反向编码为线圈值 (engineering → coil)
  static bool encode_coil(double eng_value);
};

} // namespace openems::modbus