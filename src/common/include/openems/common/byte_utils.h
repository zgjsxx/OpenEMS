// src/common/include/openems/common/byte_utils.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openems::common {

// 字节数组转十六进制字符串（用于调试输出）
std::string bytes_to_hex(const uint8_t* data, size_t len);
std::string bytes_to_hex(const std::vector<uint8_t>& data);

// 十六进制字符串转字节数组
bool hex_to_bytes(const std::string& hex_str, std::vector<uint8_t>& out);

// 大端序读写（Modbus 等协议）
void put_uint16_be(std::vector<uint8_t>& buf, uint16_t val);
uint16_t get_uint16_be(const uint8_t* p);

void put_uint32_be(std::vector<uint8_t>& buf, uint32_t val);
uint32_t get_uint32_be(const uint8_t* p);

// 小端序读写（IEC104 等协议）
void put_uint16_le(std::vector<uint8_t>& buf, uint16_t val);
uint16_t get_uint16_le(const uint8_t* p);
int16_t get_int16_le(const uint8_t* p);

void put_uint32_le(std::vector<uint8_t>& buf, uint32_t val);
uint32_t get_uint32_le(const uint8_t* p);
int32_t get_int32_le(const uint8_t* p);

// Modbus 字交换（两个 16 位寄存器组合为 32 位值）
uint32_t swap_words(uint16_t reg0, uint16_t reg1);

} // namespace openems::common