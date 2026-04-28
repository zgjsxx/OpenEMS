// src/modbus/src/modbus_data_parser.cpp
#include "openems/modbus/modbus_data_parser.h"
#include "openems/common/result.h"
#include <cstring>
#include <cmath>

namespace openems::modbus {

common::Result<model::ValueVariant> ModbusDataParser::parse_register(
    const std::vector<uint16_t>& registers,
    const model::ModbusPointMapping& mapping) {
  if (mapping.register_address + mapping.register_count > registers.size() + mapping.register_address) {
    // 检查数据是否足够——这里用偏移方式: registers 数组是按地址读取的，
    // 假设 registers[0] 对应 mapping.register_address
    // 即需要的范围是 0 ~ register_count-1
  }
  if (registers.size() < mapping.register_count) {
    return common::Result<model::ValueVariant>::Err(
        common::ErrorCode::ParseError,
        "Not enough registers: need " + std::to_string(mapping.register_count) +
        " have " + std::to_string(registers.size()));
  }

  switch (mapping.data_type) {
    case common::DataType::Bool:
      return common::Result<model::ValueVariant>::Ok(
          model::ValueVariant(registers[0] != 0));

    case common::DataType::Uint16:
      return common::Result<model::ValueVariant>::Ok(
          model::ValueVariant(registers[0]));

    case common::DataType::Int16:
      return common::Result<model::ValueVariant>::Ok(
          model::ValueVariant(static_cast<int16_t>(registers[0])));

    case common::DataType::Uint32: {
      uint32_t val;
      if (mapping.word_swap) {
        val = (static_cast<uint32_t>(registers[1]) << 16) | registers[0];
      } else {
        val = (static_cast<uint32_t>(registers[0]) << 16) | registers[1];
      }
      return common::Result<model::ValueVariant>::Ok(model::ValueVariant(val));
    }

    case common::DataType::Int32: {
      uint32_t raw;
      if (mapping.word_swap) {
        raw = (static_cast<uint32_t>(registers[1]) << 16) | registers[0];
      } else {
        raw = (static_cast<uint32_t>(registers[0]) << 16) | registers[1];
      }
      return common::Result<model::ValueVariant>::Ok(
          model::ValueVariant(static_cast<int32_t>(raw)));
    }

    case common::DataType::Float32: {
      uint32_t raw;
      if (mapping.word_swap) {
        raw = (static_cast<uint32_t>(registers[1]) << 16) | registers[0];
      } else {
        raw = (static_cast<uint32_t>(registers[0]) << 16) | registers[1];
      }
      float f;
      std::memcpy(&f, &raw, sizeof(f));
      return common::Result<model::ValueVariant>::Ok(model::ValueVariant(f));
    }

    case common::DataType::Int64: {
      uint64_t raw = 0;
      if (mapping.word_swap) {
        raw = (static_cast<uint64_t>(registers[3]) << 48) |
              (static_cast<uint64_t>(registers[2]) << 32) |
              (static_cast<uint64_t>(registers[1]) << 16) |
              registers[0];
      } else {
        raw = (static_cast<uint64_t>(registers[0]) << 48) |
              (static_cast<uint64_t>(registers[1]) << 32) |
              (static_cast<uint64_t>(registers[2]) << 16) |
              registers[3];
      }
      return common::Result<model::ValueVariant>::Ok(
          model::ValueVariant(static_cast<int64_t>(raw)));
    }

    case common::DataType::Uint64: {
      uint64_t raw = 0;
      if (mapping.word_swap) {
        raw = (static_cast<uint64_t>(registers[3]) << 48) |
              (static_cast<uint64_t>(registers[2]) << 32) |
              (static_cast<uint64_t>(registers[1]) << 16) |
              registers[0];
      } else {
        raw = (static_cast<uint64_t>(registers[0]) << 48) |
              (static_cast<uint64_t>(registers[1]) << 32) |
              (static_cast<uint64_t>(registers[2]) << 16) |
              registers[3];
      }
      return common::Result<model::ValueVariant>::Ok(model::ValueVariant(raw));
    }

    case common::DataType::Double: {
      uint64_t raw = 0;
      if (mapping.word_swap) {
        raw = (static_cast<uint64_t>(registers[3]) << 48) |
              (static_cast<uint64_t>(registers[2]) << 32) |
              (static_cast<uint64_t>(registers[1]) << 16) |
              registers[0];
      } else {
        raw = (static_cast<uint64_t>(registers[0]) << 48) |
              (static_cast<uint64_t>(registers[1]) << 32) |
              (static_cast<uint64_t>(registers[2]) << 16) |
              registers[3];
      }
      double d;
      std::memcpy(&d, &raw, sizeof(d));
      return common::Result<model::ValueVariant>::Ok(model::ValueVariant(d));
    }

    default:
      return common::Result<model::ValueVariant>::Err(
          common::ErrorCode::ParseError, "Unsupported data type");
  }
}

common::Result<model::ValueVariant> ModbusDataParser::parse_bits(
    const std::vector<bool>& bits,
    uint16_t bit_index,
    common::DataType data_type) {
  if (bit_index >= bits.size()) {
    return common::Result<model::ValueVariant>::Err(
        common::ErrorCode::ParseError, "Bit index out of range");
  }
  if (data_type == common::DataType::Bool) {
    return common::Result<model::ValueVariant>::Ok(
        model::ValueVariant(bits[bit_index]));
  }
  return common::Result<model::ValueVariant>::Err(
      common::ErrorCode::NotSupported, "Only Bool type supported for bits");
}

double ModbusDataParser::apply_scaling(
    model::ValueVariant raw, double scale, double offset) {
  double raw_val = std::visit([](auto&& v) -> double {
    return static_cast<double>(v);
  }, raw);
  return raw_val * scale + offset;
}

model::ValueVariant ModbusDataParser::to_value_variant(
    double eng_value, common::DataType target_type) {
  switch (target_type) {
    case common::DataType::Bool:    return model::ValueVariant(eng_value != 0.0);
    case common::DataType::Int16:   return model::ValueVariant(static_cast<int16_t>(std::round(eng_value)));
    case common::DataType::Uint16:  return model::ValueVariant(static_cast<uint16_t>(std::round(eng_value)));
    case common::DataType::Int32:   return model::ValueVariant(static_cast<int32_t>(std::round(eng_value)));
    case common::DataType::Uint32:  return model::ValueVariant(static_cast<uint32_t>(std::round(eng_value)));
    case common::DataType::Float32: return model::ValueVariant(static_cast<float>(eng_value));
    case common::DataType::Int64:   return model::ValueVariant(static_cast<int64_t>(std::round(eng_value)));
    case common::DataType::Uint64:  return model::ValueVariant(static_cast<uint64_t>(std::round(eng_value)));
    case common::DataType::Double:  return model::ValueVariant(eng_value);
    default:                        return model::ValueVariant(static_cast<uint16_t>(std::round(eng_value)));
  }
}

bool ModbusDataParser::encode_coil(double eng_value) {
  return eng_value != 0.0;
}

common::Result<std::vector<uint16_t>> ModbusDataParser::encode_register(
    double eng_value, const model::ModbusPointMapping& mapping) {
  // Reverse scaling: eng_value = raw * scale + offset → raw = (eng_value - offset) / scale
  double raw_double = (eng_value - mapping.offset) / mapping.scale;

  std::vector<uint16_t> words;

  switch (mapping.data_type) {
    case common::DataType::Bool:
    case common::DataType::Uint16:
      words.push_back(static_cast<uint16_t>(std::round(raw_double)));
      break;

    case common::DataType::Int16:
      words.push_back(static_cast<uint16_t>(static_cast<int16_t>(std::round(raw_double))));
      break;

    case common::DataType::Uint32: {
      uint32_t raw = static_cast<uint32_t>(std::round(raw_double));
      if (mapping.word_swap) {
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
      } else {
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
      }
      break;
    }

    case common::DataType::Int32: {
      uint32_t raw = static_cast<uint32_t>(static_cast<int32_t>(std::round(raw_double)));
      if (mapping.word_swap) {
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
      } else {
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
      }
      break;
    }

    case common::DataType::Float32: {
      float f = static_cast<float>(raw_double);
      uint32_t raw;
      std::memcpy(&raw, &f, sizeof(f));
      if (mapping.word_swap) {
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
      } else {
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
      }
      break;
    }

    case common::DataType::Int64: {
      uint64_t raw = static_cast<uint64_t>(static_cast<int64_t>(std::round(raw_double)));
      if (mapping.word_swap) {
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 32) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 48) & 0xFFFF));
      } else {
        words.push_back(static_cast<uint16_t>((raw >> 48) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 32) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
      }
      break;
    }

    case common::DataType::Uint64: {
      uint64_t raw = static_cast<uint64_t>(std::round(raw_double));
      if (mapping.word_swap) {
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 32) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 48) & 0xFFFF));
      } else {
        words.push_back(static_cast<uint16_t>((raw >> 48) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 32) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
      }
      break;
    }

    case common::DataType::Double: {
      uint64_t raw;
      std::memcpy(&raw, &raw_double, sizeof(raw_double));
      if (mapping.word_swap) {
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 32) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 48) & 0xFFFF));
      } else {
        words.push_back(static_cast<uint16_t>((raw >> 48) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 32) & 0xFFFF));
        words.push_back(static_cast<uint16_t>((raw >> 16) & 0xFFFF));
        words.push_back(static_cast<uint16_t>(raw & 0xFFFF));
      }
      break;
    }

    default:
      return common::Result<std::vector<uint16_t>>::Err(
          common::ErrorCode::ParseError, "Unsupported data type for encoding");
  }

  return common::Result<std::vector<uint16_t>>::Ok(words);
}

} // namespace openems::modbus