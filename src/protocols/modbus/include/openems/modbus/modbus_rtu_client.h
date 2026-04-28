// src/protocols/modbus/include/openems/modbus/modbus_rtu_client.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include "openems/common/result.h"
#include "openems/common/constants.h"
#include "openems/modbus/imodbus_client.h"
#include "openems/modbus/serial_port.h"

namespace openems::modbus {

struct ModbusRtuConfig {
  std::string serial_port;          // "COM3" 或 "/dev/ttyUSB0"
  uint32_t baud_rate = common::constants::DefaultModbusRtuBaudRate;
  char parity = common::constants::DefaultModbusRtuParity;
  uint8_t data_bits = common::constants::DefaultModbusRtuDataBits;
  uint8_t stop_bits = common::constants::DefaultModbusRtuStopBits;
  uint8_t unit_id = 1;
  uint32_t timeout_ms = common::constants::DefaultModbusTimeoutMs;
  uint32_t reconnect_interval_ms = common::constants::DefaultReconnectIntervalMs;
  uint32_t max_reconnect_attempts = common::constants::DefaultMaxReconnectAttempts;
  uint32_t inter_frame_delay_ms = common::constants::DefaultModbusRtuInterFrameDelayMs;  // 0=自动
};

class ModbusRtuClient : public IModbusClient {
public:
  explicit ModbusRtuClient(const ModbusRtuConfig& config);
  ~ModbusRtuClient();

  // 连接管理
  common::VoidResult connect() override;
  void disconnect() override;
  bool is_connected() const override;
  common::VoidResult reconnect() override;
  std::string connection_info() const override;
  void set_connection_callback(ConnectionCallback cb) override;

  // Modbus 读取
  common::Result<ModbusReadResult> read_holding_registers(uint16_t address, uint16_t count) override;
  common::Result<ModbusReadResult> read_input_registers(uint16_t address, uint16_t count) override;
  common::Result<ModbusReadResult> read_coils(uint16_t address, uint16_t count) override;
  common::Result<ModbusReadResult> read_discrete_inputs(uint16_t address, uint16_t count) override;

  // Modbus 写入
  common::VoidResult write_single_register(uint16_t address, uint16_t value) override;
  common::VoidResult write_multiple_registers(uint16_t address, const std::vector<uint16_t>& values) override;
  common::VoidResult write_single_coil(uint16_t address, bool value) override;

  const ModbusRtuConfig& config() const { return config_; }

private:
  common::VoidResult do_connect();

  // ADU 组帧与拆帧
  common::Result<std::vector<uint8_t>> send_and_receive(const std::vector<uint8_t>& pdu);
  common::Result<ModbusReadResult> read_registers(uint8_t func_code, uint16_t address, uint16_t count);
  common::Result<ModbusReadResult> read_bits(uint8_t func_code, uint16_t address, uint16_t count);

  // 帧间定界：确保发送前后有足够的静默时间
  void ensure_inter_frame_delay();

  // 计算自动帧间延迟 (3.5 字符时间)
  uint32_t calculate_inter_frame_delay_ms() const;

  ModbusRtuConfig config_;
  SerialPortPtr serial_;
  std::mutex serial_mutex_;
  std::atomic<bool> connected_{false};
  ConnectionCallback conn_cb_;
  std::chrono::steady_clock::time_point last_tx_time_;
};

using ModbusRtuClientPtr = std::shared_ptr<ModbusRtuClient>;

inline ModbusRtuClientPtr ModbusRtuClientCreate(const ModbusRtuConfig& config) {
  return std::make_shared<ModbusRtuClient>(config);
}

} // namespace openems::modbus