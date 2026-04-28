// src/protocols/modbus/include/openems/modbus/imodbus_client.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include "openems/common/result.h"
#include "openems/common/macros.h"

namespace openems::modbus {

// Modbus 读取结果（TCP 和 RTU 共享）
struct ModbusReadResult {
  std::vector<uint16_t> registers;   // 寄存器读取结果
  std::vector<bool> coils;           // coils / discrete inputs 结果
};

// 统一连接回调：(bool connected, string info)
using ConnectionCallback = std::function<void(bool, const std::string&)>;

class IModbusClient {
public:
  virtual ~IModbusClient() = default;

  // 连接管理
  virtual common::VoidResult connect() = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;
  virtual common::VoidResult reconnect() = 0;
  virtual std::string connection_info() const = 0;  // "192.168.1.1:502" 或 "COM3:9600-8N1"
  virtual void set_connection_callback(ConnectionCallback cb) = 0;

  // Modbus 读取
  virtual common::Result<ModbusReadResult> read_holding_registers(uint16_t address, uint16_t count) = 0;
  virtual common::Result<ModbusReadResult> read_input_registers(uint16_t address, uint16_t count) = 0;
  virtual common::Result<ModbusReadResult> read_coils(uint16_t address, uint16_t count) = 0;
  virtual common::Result<ModbusReadResult> read_discrete_inputs(uint16_t address, uint16_t count) = 0;

  // Modbus 写入
  virtual common::VoidResult write_single_register(uint16_t address, uint16_t value) = 0;
  virtual common::VoidResult write_multiple_registers(uint16_t address, const std::vector<uint16_t>& values) = 0;
  virtual common::VoidResult write_single_coil(uint16_t address, bool value) = 0;
};

OPENEMS_DECLARE_PTR(IModbusClient)

} // namespace openems::modbus