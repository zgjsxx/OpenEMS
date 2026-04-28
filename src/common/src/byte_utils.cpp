// src/common/src/byte_utils.cpp
#include "openems/common/byte_utils.h"

#include <iomanip>
#include <sstream>

namespace openems::common {

std::string bytes_to_hex(const uint8_t* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    if (i > 0) oss << ' ';
    oss << std::setw(2) << static_cast<int>(data[i]);
  }
  return oss.str();
}

std::string bytes_to_hex(const std::vector<uint8_t>& data) {
  return bytes_to_hex(data.data(), data.size());
}

bool hex_to_bytes(const std::string& hex_str, std::vector<uint8_t>& out) {
  out.clear();
  // 支持空格分隔或连续十六进制
  std::string clean;
  for (char c : hex_str) {
    if (c == ' ') continue;
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    clean += c;
  }
  if (clean.size() % 2 != 0) return false;
  for (size_t i = 0; i < clean.size(); i += 2) {
    unsigned int byte_val;
    std::istringstream iss(clean.substr(i, 2));
    iss >> std::hex >> byte_val;
    out.push_back(static_cast<uint8_t>(byte_val));
  }
  return true;
}

void put_uint16_be(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint16_t get_uint16_be(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

void put_uint32_be(std::vector<uint8_t>& buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>(val >> 24));
  buf.push_back(static_cast<uint8_t>(val >> 16));
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint32_t get_uint32_be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         p[3];
}

void put_uint16_le(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
  buf.push_back(static_cast<uint8_t>(val >> 8));
}

uint16_t get_uint16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

int16_t get_int16_le(const uint8_t* p) {
  return static_cast<int16_t>(get_uint16_le(p));
}

void put_uint32_le(std::vector<uint8_t>& buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val >> 16));
  buf.push_back(static_cast<uint8_t>(val >> 24));
}

uint32_t get_uint32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int32_t get_int32_le(const uint8_t* p) {
  return static_cast<int32_t>(get_uint32_le(p));
}

uint32_t swap_words(uint16_t reg0, uint16_t reg1) {
  return (static_cast<uint32_t>(reg1) << 16) | reg0;
}

} // namespace openems::common