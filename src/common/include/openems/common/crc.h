// src/common/include/openems/common/crc.h
#pragma once

#include <cstdint>
#include <cstddef>

namespace openems::common {

// Modbus RTU CRC-16
uint16_t crc16_modbus(const uint8_t* data, size_t len);

// 通用 CRC-32（数据校验、文件完整性）
uint32_t crc32(const uint8_t* data, size_t len);

} // namespace openems::common