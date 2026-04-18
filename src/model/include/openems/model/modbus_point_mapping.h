// src/model/include/openems/model/modbus_point_mapping.h
#pragma once

#include <cstdint>
#include <string>
#include "openems/common/types.h"

namespace openems::model {

struct ModbusPointMapping {
  uint8_t  function_code = 0;     // 1=Coils, 2=DI, 3=Holding, 4=Input
  uint16_t register_address = 0;  // 寄存器起始地址
  uint16_t register_count = 1;    // 寄存器数量
  common::DataType data_type = common::DataType::Uint16;
  double   scale = 1.0;           // 缩放系数
  double   offset = 0.0;          // 偏移
  bool     byte_swap = false;     // 字节序交换
  bool     word_swap = false;     // 字序交换

  std::string to_string() const;
};

} // namespace openems::model