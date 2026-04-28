// src/protocols/modbus/include/openems/modbus/serial_port.h
#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include "openems/common/result.h"
#include "openems/common/macros.h"

namespace openems::modbus {

struct SerialPortConfig {
  std::string port_name;     // "COM3" 或 "/dev/ttyUSB0"
  uint32_t baud_rate = 9600;
  char parity = 'N';         // N/E/O
  uint8_t data_bits = 8;
  uint8_t stop_bits = 1;
  uint32_t timeout_ms = 3000;
  uint32_t inter_char_timeout_ms = 0;  // RTU: 帧内字符间隔超时 (1.5字符时间), 0=自动
};

class SerialPort {
public:
  explicit SerialPort(const SerialPortConfig& config);
  ~SerialPort();

  common::VoidResult open();
  void close();
  bool is_open() const;

  common::VoidResult write(const uint8_t* data, size_t len);
  common::Result<std::vector<uint8_t>> read_with_timeout(size_t min_bytes);

  void drain();  // 清空缓冲区

  const SerialPortConfig& config() const { return config_; }

private:
  SerialPortConfig config_;

#ifdef _WIN32
  void* handle_ = nullptr;  // HANDLE
#else
  int fd_ = -1;
#endif
};

using SerialPortPtr = std::unique_ptr<SerialPort>;

} // namespace openems::modbus