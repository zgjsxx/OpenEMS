// src/modbus/include/openems/modbus/modbus_tcp_client.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include "openems/common/result.h"
#include "openems/common/types.h"

namespace openems::modbus {

struct ModbusConfig {
  std::string ip;
  uint16_t port = 502;
  uint8_t unit_id = 1;
  uint32_t timeout_ms = 3000;
  uint32_t reconnect_interval_ms = 5000;
  uint32_t max_reconnect_attempts = 3;
};

// Modbus 读取结果
struct ModbusReadResult {
  std::vector<uint16_t> registers;   // 寄存器读取结果
  std::vector<bool> coils;           // coils / discrete inputs 结果
};

// 连接状态回调
using ConnectionCallback = std::function<void(bool connected, const std::string& ip, uint16_t port)>;

class ModbusTcpClient {
public:
  explicit ModbusTcpClient(const ModbusConfig& config);
  ~ModbusTcpClient();

  // 连接管理
  common::VoidResult connect();
  void disconnect();
  bool is_connected() const;

  // 断线重连
  common::VoidResult reconnect();
  void set_connection_callback(ConnectionCallback cb);

  // Modbus 读取
  common::Result<ModbusReadResult> read_holding_registers(uint16_t address, uint16_t count);
  common::Result<ModbusReadResult> read_input_registers(uint16_t address, uint16_t count);
  common::Result<ModbusReadResult> read_coils(uint16_t address, uint16_t count);
  common::Result<ModbusReadResult> read_discrete_inputs(uint16_t address, uint16_t count);

  // Modbus 写入（预留）
  common::VoidResult write_single_register(uint16_t address, uint16_t value);
  common::VoidResult write_multiple_registers(uint16_t address, const std::vector<uint16_t>& values);
  common::VoidResult write_single_coil(uint16_t address, bool value);

private:
  // 内部 TCP 通信
  common::VoidResult do_connect();
  common::Result<std::vector<uint8_t>> send_and_receive(const std::vector<uint8_t>& request);
  common::Result<ModbusReadResult> read_registers(uint8_t func_code, uint16_t address, uint16_t count);
  common::Result<ModbusReadResult> read_bits(uint8_t func_code, uint16_t address, uint16_t count);

  ModbusConfig config_;
  std::atomic<bool> connected_{false};
  int socket_fd_ = -1;
  mutable std::mutex socket_mutex_;
  ConnectionCallback conn_cb_;
  uint16_t transaction_id_ = 0;
};

using ModbusTcpClientPtr = std::shared_ptr<ModbusTcpClient>;

inline ModbusTcpClientPtr ModbusTcpClientCreate(const ModbusConfig& config) {
  return std::make_shared<ModbusTcpClient>(config);
}

} // namespace openems::modbus