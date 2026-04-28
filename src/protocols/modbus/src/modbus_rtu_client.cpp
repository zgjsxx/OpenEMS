// src/protocols/modbus/src/modbus_rtu_client.cpp
#include "openems/modbus/modbus_rtu_client.h"
#include "openems/common/crc.h"
#include "openems/common/constants.h"
#include "openems/utils/logger.h"
#include "openems/utils/time_utils.h"

#include <cstring>
#include <chrono>
#include <thread>

namespace openems::modbus {

// ===== PDU 构建辅助 =====

static void put_uint16_be(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

static uint16_t get_uint16_be(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

// 构建读 PDU (unit_id + func_code + address + count)
static std::vector<uint8_t> build_read_pdu(uint8_t unit_id, uint8_t func_code,
                                            uint16_t address, uint16_t count) {
  std::vector<uint8_t> pdu;
  pdu.push_back(unit_id);
  pdu.push_back(func_code);
  put_uint16_be(pdu, address);
  put_uint16_be(pdu, count);
  return pdu;
}

// 构建写单寄存器 PDU
static std::vector<uint8_t> build_write_single_reg_pdu(
    uint8_t unit_id, uint16_t address, uint16_t value) {
  std::vector<uint8_t> pdu;
  pdu.push_back(unit_id);
  pdu.push_back(common::constants::ModbusWriteSingleRegister);  // FC=6
  put_uint16_be(pdu, address);
  put_uint16_be(pdu, value);
  return pdu;
}

// 构建写多寄存器 PDU
static std::vector<uint8_t> build_write_multi_reg_pdu(
    uint8_t unit_id, uint16_t address, const std::vector<uint16_t>& values) {
  std::vector<uint8_t> pdu;
  pdu.push_back(unit_id);
  pdu.push_back(common::constants::ModbusWriteMultipleRegisters);  // FC=16
  put_uint16_be(pdu, address);
  put_uint16_be(pdu, static_cast<uint16_t>(values.size()));
  pdu.push_back(static_cast<uint8_t>(values.size() * 2));  // Byte count
  for (auto v : values) {
    put_uint16_be(pdu, v);
  }
  return pdu;
}

// 构建写单线圈 PDU
static std::vector<uint8_t> build_write_single_coil_pdu(
    uint8_t unit_id, uint16_t address, bool value) {
  std::vector<uint8_t> pdu;
  pdu.push_back(unit_id);
  pdu.push_back(common::constants::ModbusWriteSingleCoil);  // FC=5
  put_uint16_be(pdu, address);
  put_uint16_be(pdu, value ? 0xFF00 : 0x0000);
  return pdu;
}

// ===== ModbusRtuClient 实现 =====

ModbusRtuClient::ModbusRtuClient(const ModbusRtuConfig& config) : config_(config) {}

ModbusRtuClient::~ModbusRtuClient() { disconnect(); }

std::string ModbusRtuClient::connection_info() const {
  return config_.serial_port + ":" +
      std::to_string(config_.baud_rate) + "-" +
      std::to_string(config_.data_bits) +
      config_.parity +
      std::to_string(config_.stop_bits);
}

common::VoidResult ModbusRtuClient::do_connect() {
  SerialPortConfig sp_cfg;
  sp_cfg.port_name = config_.serial_port;
  sp_cfg.baud_rate = config_.baud_rate;
  sp_cfg.parity = config_.parity;
  sp_cfg.data_bits = config_.data_bits;
  sp_cfg.stop_bits = config_.stop_bits;
  sp_cfg.timeout_ms = config_.timeout_ms;

  serial_ = std::make_unique<SerialPort>(sp_cfg);
  auto result = serial_->open();
  if (!result.is_ok()) {
    serial_.reset();
    return result;
  }

  connected_ = true;
  OPENEMS_LOG_I("ModbusRtuClient", "Connected to " + connection_info());
  if (conn_cb_) conn_cb_(true, connection_info());
  return common::VoidResult::Ok();
}

common::VoidResult ModbusRtuClient::connect() {
  return do_connect();
}

void ModbusRtuClient::disconnect() {
  serial_.reset();
  connected_ = false;
  OPENEMS_LOG_I("ModbusRtuClient", "Disconnected from " + connection_info());
  if (conn_cb_) conn_cb_(false, connection_info());
}

bool ModbusRtuClient::is_connected() const {
  return connected_.load();
}

common::VoidResult ModbusRtuClient::reconnect() {
  disconnect();
  for (uint32_t i = 0; i < config_.max_reconnect_attempts; ++i) {
    OPENEMS_LOG_W("ModbusRtuClient",
        "Reconnect attempt " + std::to_string(i + 1) +
        "/" + std::to_string(config_.max_reconnect_attempts) +
        " to " + connection_info());
    auto result = do_connect();
    if (result.is_ok()) return result;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.reconnect_interval_ms));
  }
  return common::VoidResult::Err(
      common::ErrorCode::ModbusConnectionFailed,
      "Max reconnect attempts reached for " + connection_info());
}

void ModbusRtuClient::set_connection_callback(ConnectionCallback cb) {
  conn_cb_ = std::move(cb);
}

// ===== 帧间定界 =====

uint32_t ModbusRtuClient::calculate_inter_frame_delay_ms() const {
  if (config_.inter_frame_delay_ms > 0) return config_.inter_frame_delay_ms;

  // 3.5 字符时间
  // 1 字符 = (1 start + data_bits + parity_bit + stop_bits) bits
  uint32_t char_bits = 1 + config_.data_bits +
      (config_.parity != 'N' ? 1 : 0) + config_.stop_bits;
  // 3.5 字符 = 3.5 * char_bits / baud_rate * 1000 ms
  uint32_t delay_ms = static_cast<uint32_t>(3500.0 * char_bits / config_.baud_rate);
  if (delay_ms < 2) delay_ms = 2;  // 最小 2ms
  return delay_ms;
}

void ModbusRtuClient::ensure_inter_frame_delay() {
  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last_tx_time_).count();
  auto required_ms = calculate_inter_frame_delay_ms();

  if (elapsed_ms < static_cast<long>(required_ms)) {
    auto sleep_ms = static_cast<long>(required_ms) - elapsed_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
}

// ===== ADU 组帧/拆帧 =====

common::Result<std::vector<uint8_t>> ModbusRtuClient::send_and_receive(
    const std::vector<uint8_t>& pdu) {
  std::lock_guard lock(serial_mutex_);
  if (!serial_ || !serial_->is_open()) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusConnectionFailed, "Not connected");
  }

  // 帧间定界：发送前确保 ≥3.5 字符时间的静默
  ensure_inter_frame_delay();

  // 组 RTU ADU: [unit_id][PDU][CRC16_LO][CRC16_HI]
  std::vector<uint8_t> adu;
  adu.insert(adu.end(), pdu.begin(), pdu.end());
  uint16_t crc = common::crc16_modbus(adu.data(), adu.size());
  adu.push_back(static_cast<uint8_t>(crc & 0xFF));       // CRC 低字节先发
  adu.push_back(static_cast<uint8_t>(crc >> 8));          // CRC 高字节

  // 清空串口缓冲区
  serial_->drain();

  // 发送 ADU
  auto write_result = serial_->write(adu.data(), adu.size());
  if (!write_result.is_ok()) {
    connected_ = false;
    return common::Result<std::vector<uint8_t>>::Err(
        write_result.error_code(), write_result.error_msg());
  }

  // 记录发送完成时间（用于下次帧间定界）
  last_tx_time_ = std::chrono::steady_clock::now();

  // 接收响应：至少 4 字节 (unit_id + func_code + CRC[2])
  // 异常响应: unit_id(1) + func_code(1) + exc_code(1) + crc(2) = 5
  auto read_result = serial_->read_with_timeout(4);
  if (!read_result.is_ok()) {
    connected_ = false;
    return common::Result<std::vector<uint8_t>>::Err(
        read_result.error_code(), read_result.error_msg());
  }

  auto& response = read_result.value();

  // 验证最小长度
  if (response.size() < 4) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusRtuFrameError,
        "Response too short: " + std::to_string(response.size()) + " bytes");
  }

  // 验证 CRC：去掉最后 2 字节(CRC)，计算剩余部分的 CRC
  size_t data_len = response.size() - 2;
  uint16_t recv_crc = static_cast<uint16_t>(response[data_len]) |
      (static_cast<uint16_t>(response[data_len + 1]) << 8);
  uint16_t calc_crc = common::crc16_modbus(response.data(), data_len);

  if (recv_crc != calc_crc) {
    OPENEMS_LOG_W("ModbusRtuClient",
        "CRC mismatch: recv=0x" + std::to_string(recv_crc) +
        " calc=0x" + std::to_string(calc_crc));
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusRtuCrcError,
        "CRC mismatch");
  }

  // 验证 unit_id
  if (response[0] != config_.unit_id) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusRtuFrameError,
        "Unit ID mismatch: expected " + std::to_string(config_.unit_id) +
        " got " + std::to_string(response[0]));
  }

  // 检查异常响应（func_code 最高位为 1）
  uint8_t func = response[1];
  if (func & 0x80) {
    uint8_t exc_code = response.size() > 2 ? response[2] : 0;
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusError,
        "Modbus exception code " + std::to_string(exc_code));
  }

  // 返回去掉 unit_id + CRC 的 PDU 部分
  std::vector<uint8_t> response_pdu;
  // response: [unit_id][PDU_data][CRC_lo][CRC_hi]
  // PDU_data = response[1..data_len-1]  (去掉 unit_id[0] 和 CRC[data_len..data_len+1])
  response_pdu.assign(response.begin() + 1, response.begin() + data_len);

  return common::Result<std::vector<uint8_t>>::Ok(std::move(response_pdu));
}

// ===== 读取方法 =====

common::Result<ModbusReadResult> ModbusRtuClient::read_registers(
    uint8_t func_code, uint16_t address, uint16_t count) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) {
      return common::Result<ModbusReadResult>::Err(rc.error_code(), rc.error_msg());
    }
  }

  auto pdu = build_read_pdu(config_.unit_id, func_code, address, count);
  auto resp_result = send_and_receive(pdu);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::Result<ModbusReadResult>::Err(
        resp_result.error_code(), resp_result.error_msg());
  }

  auto& response = resp_result.value();
  // PDU: func_code(1) + byte_count(1) + data(N*2)
  if (response.size() < 2) {
    return common::Result<ModbusReadResult>::Err(
        common::ErrorCode::ModbusInvalidFrame, "Response PDU too short");
  }

  uint8_t byte_count = response[1];
  ModbusReadResult result;
  for (uint8_t i = 0; i < byte_count; i += 2) {
    if (2 + i + 1 < response.size()) {
      uint16_t reg_val = get_uint16_be(response.data() + 2 + i);
      result.registers.push_back(reg_val);
    }
  }
  return common::Result<ModbusReadResult>::Ok(std::move(result));
}

common::Result<ModbusReadResult> ModbusRtuClient::read_bits(
    uint8_t func_code, uint16_t address, uint16_t count) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) {
      return common::Result<ModbusReadResult>::Err(rc.error_code(), rc.error_msg());
    }
  }

  auto pdu = build_read_pdu(config_.unit_id, func_code, address, count);
  auto resp_result = send_and_receive(pdu);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::Result<ModbusReadResult>::Err(
        resp_result.error_code(), resp_result.error_msg());
  }

  auto& response = resp_result.value();
  if (response.size() < 2) {
    return common::Result<ModbusReadResult>::Err(
        common::ErrorCode::ModbusInvalidFrame, "Response PDU too short");
  }

  uint8_t byte_count = response[1];
  ModbusReadResult result;
  for (uint8_t byte_idx = 0; byte_idx < byte_count; ++byte_idx) {
    if (2 + byte_idx < response.size()) {
      uint8_t byte_val = response[2 + byte_idx];
      for (uint8_t bit_idx = 0; bit_idx < 8 && result.coils.size() < count; ++bit_idx) {
        result.coils.push_back((byte_val & (1 << bit_idx)) != 0);
      }
    }
  }
  return common::Result<ModbusReadResult>::Ok(std::move(result));
}

common::Result<ModbusReadResult> ModbusRtuClient::read_holding_registers(
    uint16_t address, uint16_t count) {
  return read_registers(common::constants::ModbusReadHoldingRegisters, address, count);
}

common::Result<ModbusReadResult> ModbusRtuClient::read_input_registers(
    uint16_t address, uint16_t count) {
  return read_registers(common::constants::ModbusReadInputRegisters, address, count);
}

common::Result<ModbusReadResult> ModbusRtuClient::read_coils(
    uint16_t address, uint16_t count) {
  return read_bits(common::constants::ModbusReadCoils, address, count);
}

common::Result<ModbusReadResult> ModbusRtuClient::read_discrete_inputs(
    uint16_t address, uint16_t count) {
  return read_bits(common::constants::ModbusReadDiscreteInputs, address, count);
}

// ===== 写入方法 =====

common::VoidResult ModbusRtuClient::write_single_register(
    uint16_t address, uint16_t value) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) return rc;
  }
  auto pdu = build_write_single_reg_pdu(config_.unit_id, address, value);
  auto resp_result = send_and_receive(pdu);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::VoidResult::Err(resp_result.error_code(), resp_result.error_msg());
  }
  return common::VoidResult::Ok();
}

common::VoidResult ModbusRtuClient::write_multiple_registers(
    uint16_t address, const std::vector<uint16_t>& values) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) return rc;
  }
  auto pdu = build_write_multi_reg_pdu(config_.unit_id, address, values);
  auto resp_result = send_and_receive(pdu);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::VoidResult::Err(resp_result.error_code(), resp_result.error_msg());
  }
  return common::VoidResult::Ok();
}

common::VoidResult ModbusRtuClient::write_single_coil(
    uint16_t address, bool value) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) return rc;
  }
  auto pdu = build_write_single_coil_pdu(config_.unit_id, address, value);
  auto resp_result = send_and_receive(pdu);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::VoidResult::Err(resp_result.error_code(), resp_result.error_msg());
  }
  return common::VoidResult::Ok();
}

} // namespace openems::modbus